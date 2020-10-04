#include <string.h>
#include "gwion_util.h"
#include "gwion_ast.h"
#include "gwion_env.h"
#include "vm.h"
#include "traverse.h"
#include "template.h"
#include "parser.h"
#include "parse.h"
#include "object.h"
#include "instr.h"
#include "operator.h"
#include "import.h"

static inline void add_type(const Env env, const Nspc nspc, const Type t) {
  nspc_add_type_front(nspc, insert_symbol(t->name), t);
}

static inline void context_global(const Env env) {
  if(env->context)
    env->context->global = 1;
}

static inline Type scan0_type(const Env env, const m_uint xid,
    const m_str name, const Type t) {
  const Type type = new_type(env->gwion->mp, xid, name, t);
  type->e->ctx = env->context;
  return type;
}

ANN static inline m_bool scan0_defined(const Env env, const Symbol s, const loc_t pos) {
  if(nspc_lookup_type1(env->curr, s))
    ERR_B(pos, _("type '%s' already defined"), s_name(s));
  return already_defined(env, s, pos);
}

ANN static void fptr_assign(const Env env, const Fptr_Def fptr) {
  const Func_Def def = fptr->type->e->d.func->def;
  if(GET_FLAG(fptr->base, global)) {
    context_global(env);
    SET_FLAG(fptr->value, global);
    SET_FLAG(fptr->base->func, global);
    SET_FLAG(def->base, global);
  } else if(!GET_FLAG(fptr->base, static)) {
    SET_FLAG(fptr->value, member);
    SET_FLAG(fptr->base->func, member);
    SET_FLAG(def->base, member);
    def->stack_depth += SZ_INT;
  } else {
    SET_FLAG(fptr->value, static);
    SET_FLAG(fptr->base->func, static);
    SET_FLAG(def->base, static);
  }
  if(GET_FLAG(def->base, variadic))
    def->stack_depth += SZ_INT;
  fptr->value->from->owner_class = env->class_def;
}

static void fptr_def(const Env env, const Fptr_Def fptr) {
  const Func_Def def = new_func_def(env->gwion->mp,
      cpy_func_base(env->gwion->mp, fptr->base),
    NULL, loc_cpy(env->gwion->mp, td_pos(fptr->base->td)));
  fptr->base->func = new_func(env->gwion->mp, s_name(fptr->base->xid), def);
  fptr->value->d.func_ref = fptr->base->func;
  fptr->base->func->value_ref = fptr->value;
  fptr->type->e->d.func = fptr->base->func;
  def->base->func = fptr->base->func;
}

ANN m_bool scan0_fptr_def(const Env env, const Fptr_Def fptr) {
  CHECK_BB(env_access(env, fptr->base->flag, td_pos(fptr->base->td)))
  CHECK_BB(scan0_defined(env, fptr->base->xid, td_pos(fptr->base->td)));
  const m_str name = s_name(fptr->base->xid);
  const Type t = scan0_type(env, env->gwion->type[et_fptr]->xid, name, env->gwion->type[et_fptr]);
  t->e->owner = !(!env->class_def && GET_FLAG(fptr->base, global)) ?
    env->curr : env->global_nspc;
  t->e->owner_class = env->class_def;
  if(GET_FLAG(fptr->base, global))
    context_global(env);
  t->nspc = new_nspc(env->gwion->mp, name);
  t->flag = fptr->base->flag;
  fptr->type = t;
  fptr->value = mk_class(env, t);
  valuefrom(env, fptr->value->from);
  fptr_def(env, fptr);
  if(env->class_def)
    fptr_assign(env, fptr);
  SET_FLAG(fptr->value, func);
  add_type(env, t->e->owner, t);
  mk_class(env, t);
  ADD_REF(t);
  return GW_OK;
}

static OP_CHECK(opck_implicit_similar) {
  const struct Implicit *imp = (struct Implicit*)data;
  return imp->e->info->type;
}

static OP_CHECK(opck_cast_similar) {
  const Exp_Cast *cast = (Exp_Cast*)data;
  return exp_self(cast)->info->type;
}

ANN static void scan0_implicit_similar(const Env env, const Type lhs, const Type rhs) {
  struct Op_Func opfunc = { .ck=opck_cast_similar };
  struct Op_Import opi = { .op=insert_symbol("$"), .lhs=lhs, .rhs=rhs, .func=&opfunc };
  add_op(env->gwion, &opi);
  opi.lhs=rhs;
  opi.rhs=lhs;
  add_op(env->gwion, &opi);
  opfunc.ck = opck_implicit_similar;
  opi.op=insert_symbol("@implicit");
  add_op(env->gwion, &opi);
}

ANN static void typedef_simple(const Env env, const Type_Def tdef, const Type base) {
  const Type t = scan0_type(env, ++env->scope->type_xid, s_name(tdef->xid), base);
  t->size = base->size;
  const Nspc nspc = (!env->class_def && GET_FLAG(tdef->ext, global)) ?
  env->global_nspc : env->curr;
  if(GET_FLAG(tdef->ext, global))
    context_global(env);
  add_type(env, nspc, t);
  t->e->owner = nspc;
  t->e->owner_class = env->class_def;
  tdef->type = t;
  if(base->nspc)
    ADD_REF((t->nspc = base->nspc));
  t->flag = tdef->ext->flag | ae_flag_valid;
  scan0_implicit_similar(env, t, base);
  if(tdef->ext->array && !tdef->ext->array->exp)
    SET_FLAG(t, empty);
}

ANN static m_bool typedef_complex(const Env env, const Type_Def tdef, const Type base) {
  const ae_flag flag = base->e->def ? base->e->def->flag : 0;
  const Class_Def cdef = new_class_def(env->gwion->mp, flag, tdef->xid,
       cpy_type_decl(env->gwion->mp, tdef->ext), NULL,
       loc_cpy(env->gwion->mp, td_pos(tdef->ext)));
  CHECK_BB(scan0_class_def(env, cdef))
  tdef->type = cdef->base.type;
  cdef->base.tmpl = tdef->tmpl;// check cpy
  return GW_OK;
}

ANN static void typedef_fptr(const Env env, const Type_Def tdef, const Type base) {
  tdef->type = type_copy(env->gwion->mp, base);
  ADD_REF(tdef->type->nspc)
  tdef->type->name = s_name(tdef->xid);
  tdef->type->e->parent = base;
  add_type(env, env->curr, tdef->type);
  mk_class(env, tdef->type);
  if(base->e->d.func->def->base->tmpl)
    SET_FLAG(tdef->type, func);
}

ANN m_bool scan0_type_def(const Env env, const Type_Def tdef) {
  CHECK_BB(env_access(env, tdef->ext->flag, td_pos(tdef->ext)))
  DECL_OB(const Type, base, = tdef->tmpl ? find_type(env, tdef->ext) : known_type(env, tdef->ext))
  CHECK_BB(scan0_defined(env, tdef->xid, td_pos(tdef->ext)))
  if(isa(base, env->gwion->type[et_function]) < 0) {
    if(!tdef->ext->types && (!tdef->ext->array || !tdef->ext->array->exp))
      typedef_simple(env, tdef, base);
    else
      CHECK_BB(typedef_complex(env, tdef, base))
  } else
    typedef_fptr(env, tdef, base);
  SET_FLAG(tdef->type, typedef);
  return GW_OK;
}

ANN static Symbol scan0_sym(const Env env, const m_str name, const loc_t pos) {
  const size_t line_len = num_digit(pos->first.line);
  const size_t col_len = num_digit(pos->first.column);
  char c[strlen(env->curr->name) + strlen(env->name) + line_len + col_len + strlen(name) + 6];
  sprintf(c, "@%s:%s:%s:%u:%u", name, env->name, env->curr->name,
      pos->first.line, pos->first.column);
  return insert_symbol(c);
}

#define scan0_nspc(env, a) \
  GET_FLAG(a, global) ? !env->class_def ? env->global_nspc : NULL : env->curr

ANN static Type enum_type(const Env env, const Enum_Def edef) {
  const Type t = type_copy(env->gwion->mp, env->gwion->type[et_int]);
  t->xid = ++env->scope->type_xid;
  const Symbol sym = scan0_sym(env, "enum", edef->pos);
  t->name = edef->xid ? s_name(edef->xid) : s_name(sym);
  t->e->parent = env->gwion->type[et_int];
  const Nspc nspc = GET_FLAG(edef, global) ? env->global_nspc : env->curr;
  t->e->owner = nspc;
  t->e->owner_class = env->class_def;
  add_type(env, nspc, t);
  mk_class(env, t);
  scan0_implicit_similar(env, t, env->gwion->type[et_int]);
  return t;
}

ANN static inline m_bool scan0_global(const Env env, const ae_flag flag, const loc_t pos) {
  if(!env->class_def || !((flag & ae_flag_global) == ae_flag_global))
    return GW_OK;
  ERR_B(pos, _("can't declare as global in class def"))
}

ANN m_bool scan0_enum_def(const Env env, const Enum_Def edef) {
  CHECK_BB(env_storage(env, edef->flag, edef->pos))
  CHECK_BB(scan0_defined(env, edef->xid, edef->pos))
  CHECK_BB(scan0_global(env, edef->flag, edef->pos))
  edef->t = enum_type(env, edef);
  if(GET_FLAG(edef, global))
    context_global(env);
  return GW_OK;
}

ANN static Type union_type(const Env env, const Symbol s, const m_bool add) {
  const m_str name = s_name(s);
  const Type t = type_copy(env->gwion->mp, env->gwion->type[et_union]);
  t->xid = ++env->scope->type_xid;
  t->name = name;
  t->nspc = new_nspc(env->gwion->mp, name);
  t->e->owner = t->nspc->parent = env->curr;
  t->e->owner_class = env->class_def;
  t->e->parent = env->gwion->type[et_union];
  t->e->tuple = new_tupleform(env->gwion->mp, NULL);
  add_type(env, env->curr, t);
  if(add)
    mk_class(env, t);
  SET_FLAG(t, union);
  return t;
}

ANN static void union_tmpl(const Env env, const Union_Def udef) {
  if(tmpl_base(udef->tmpl)) {
    assert(udef->type_xid);
    const Class_Def cdef = new_class_def(env->gwion->mp, udef->flag, udef->type_xid,
        NULL, (Ast)cpy_decl_list(env->gwion->mp, udef->l), loc_cpy(env->gwion->mp, udef->pos));
    udef->type->e->def = cdef;
    cdef->base.tmpl = cpy_tmpl(env->gwion->mp, udef->tmpl);
    cdef->base.type = udef->type;
    SET_FLAG(cdef, union);
    SET_FLAG(udef->type, pure);
    SET_FLAG(udef, template);
    SET_FLAG(udef->type, template);
  }
  if(GET_FLAG(udef, global))
    SET_FLAG(udef->type, global);
  SET_FLAG(udef->type, union);
}

ANN static Value union_value(const Env env, const Type t, const Symbol sym) {
  const Value v = new_value(env->gwion->mp, t, s_name(sym));
  valuefrom(env, v->from);
  nspc_add_value(env->curr, sym, v);
  SET_FLAG(v, valid | ae_flag_pure);
  return v;
}

ANN m_bool scan0_union_def(const Env env, const Union_Def udef) {
  CHECK_BB(env_storage(env, udef->flag, udef->pos))
  CHECK_BB(scan0_global(env, udef->flag, udef->pos))
  const m_uint scope = !GET_FLAG(udef, global) ? env->scope->depth :
      env_push_global(env);
  if(GET_FLAG(udef, global))
    context_global(env);
  if(udef->xid) {
    CHECK_BB(scan0_defined(env, udef->xid, udef->pos))
    const Symbol sym = udef->type_xid ?: scan0_sym(env, "union", udef->pos);
    const Type t = union_type(env, sym, !!udef->type_xid);
    udef->value = union_value(env, t, udef->xid);
    udef->value->flag |= udef->flag;
    SET_ACCESS(udef, t);
    if(env->class_def && !GET_FLAG(udef, static)) {
      SET_FLAG(udef->value, member);
      SET_FLAG(udef, member);
    }
  } else if(udef->type_xid) {
    CHECK_BB(scan0_defined(env, udef->type_xid, udef->pos))
    udef->type = union_type(env, udef->type_xid, 1);
    SET_ACCESS(udef, udef->type);
    SET_FLAG(udef->type, valid);
  } else {
    const Symbol sym = scan0_sym(env, "union", udef->pos);
    CHECK_BB(scan0_defined(env, sym, udef->pos))
    const Type t = union_type(env, sym, 1);
    SET_ACCESS(udef, t);
    udef->value = union_value(env, t, sym);
    udef->value->flag |= udef->flag;
  }
  if(udef->tmpl)
    union_tmpl(env, udef);
  if(GET_FLAG(udef, global))
    env_pop(env, scope);
  union_flag(udef, ae_flag_scan0);
  return GW_OK;
}

ANN static m_bool scan0_class_def_pre(const Env env, const Class_Def cdef) {
  CHECK_BB(env_storage(env, cdef->flag, cdef->pos))
  CHECK_BB(isres(env, cdef->base.xid, cdef->pos))
  CHECK_BB(scan0_global(env, cdef->flag, cdef->pos))
  return GW_OK;
}

ANN static void cdef_flag(const Class_Def cdef, const Type t) {
  if(cdef->base.tmpl) {
    SET_FLAG(t, template);
    SET_FLAG(cdef, template);
  }
  if(cdef->base.ext && cdef->base.ext->array)
    SET_FLAG(t, typedef);
}

ANN static Type get_parent_base(const Env env, Type_Decl *td) {
  DECL_OO(const Type, t, = find_type(env, td))
  Type owner = env->class_def;
  while(owner) {
    if(t == owner)
      ERR_O(td_pos(td), _("'%s' as parent inside itself\n."), owner->name);
    owner = owner->e->owner_class;
  }
  return t;
}

ANN static Type get_parent(const Env env, const Class_Def cdef) {
  if(GET_FLAG(cdef, struct))
    return env->gwion->type[et_compound];
  if(!cdef->base.ext)
    return env->gwion->type[et_object];
  if(tmpl_base(cdef->base.tmpl))
    return get_parent_base(env, cdef->base.ext);
  if(cdef->base.tmpl)
    template_push_types(env, cdef->base.tmpl);
  const Type t = known_type(env, cdef->base.ext);
  if(cdef->base.tmpl)
    nspc_pop_type(env->gwion->mp, env->curr);
  return t ?: (Type)GW_ERROR;
}

ANN static Type scan0_class_def_init(const Env env, const Class_Def cdef) {
  CHECK_BO(scan0_defined(env, cdef->base.xid, cdef->pos))
  const Type parent = get_parent(env, cdef);
  if(parent == (Type)GW_ERROR)
    return NULL;
  const Type t = scan0_type(env, ++env->scope->type_xid, s_name(cdef->base.xid), parent);
  if(GET_FLAG(cdef, struct))
    SET_FLAG(t, struct);
  t->e->tuple = new_tupleform(env->gwion->mp, parent);
  t->e->owner = env->curr;
  t->e->owner_class = env->class_def;
  t->nspc = new_nspc(env->gwion->mp, t->name);
  t->nspc->parent = env->curr;
  t->e->def = cdef;
  t->flag = cdef->flag;
  add_type(env, t->e->owner, t);
  cdef_flag(cdef, t);
  if(cdef->base.ext && cdef->base.ext->array)
    SET_FLAG(t, typedef);
  return t;
}

ANN static m_bool scan0_stmt_list(const Env env, Stmt_List list) {
  do if(list->stmt->stmt_type == ae_stmt_pp && list->stmt->d.stmt_pp.pp_type == ae_pp_include)
    env->name = list->stmt->d.stmt_pp.data;
  while((list = list->next));
  return GW_OK;
}

#define scan0_func_def dummy_func
HANDLE_SECTION_FUNC(scan0, m_bool, Env)

ANN static m_bool scan0_class_def_inner(const Env env, const Class_Def cdef) {
  CHECK_OB((cdef->base.type = scan0_class_def_init(env, cdef)))
  SET_FLAG(cdef->base.type, scan0);
  if(cdef->body)
    CHECK_BB(env_body(env, cdef, scan0_section))
  (void)mk_class(env, cdef->base.type);
  return GW_OK;
}

ANN m_bool scan0_class_def(const Env env, const Class_Def c) {
  const Class_Def cdef = !tmpl_base(c->base.tmpl) ?
    c : cpy_class_def(env->gwion->mp, c);
  if(GET_FLAG(cdef, global)) {
    vector_add(&env->scope->nspc_stack, (vtype)env->curr);
    env->curr = env->global_nspc;
    context_global(env);
  }
  const m_bool ret = scan0_class_def_pre(env, cdef) > 0 ?
    scan0_class_def_inner(env, cdef) : GW_ERROR;
  if(GET_FLAG(cdef, global))
    env->curr = (Nspc)vector_pop(&env->scope->nspc_stack);
  CHECK_BB(ret)
  if(GET_FLAG(cdef, global) || (cdef->base.tmpl && !cdef->base.tmpl->call))
    c->base.type = cdef->base.type;
  SET_FLAG(cdef->base.type, scan0);
  return GW_OK;
}

ANN m_bool scan0_ast(const Env env, Ast ast) {
  do CHECK_BB(scan0_section(env, ast->section))
  while((ast = ast->next));
  return GW_OK;
}

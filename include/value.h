struct Value_ {
  Type m_type;
  m_str name;
  Nspc owner;
  Type owner_class;
  m_uint offset;
  m_uint* ptr;
  Func func_ref;
  m_uint func_num_overloads;
  ae_flag flag;
  struct VM_Object_ obj;
};

Value new_value(const Type type, const m_str name);
void free_value(Value a);

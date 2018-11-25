#include "gwion_util.h"
#include "gwion_ast.h"
#include "oo.h"
#include "vm.h"
#include "env.h"
#include "instr.h"
#include "emit.h"
#include "gwion.h"

ANN void gwion_init(const Gwion gwion) {
  gwion->vm = new_vm();
  gwion->emit = new_emitter();
  gwion->env = new_env();
  gwion->scan = new_scanner(127); // !!! magic number
  gwion->emit->env = gwion->env;
  gwion->vm->gwion = gwion;
}

ANN void gwion_release(const Gwion gwion) {
  free_env(gwion->env);
  free_emitter(gwion->emit);
  free_vm(gwion->vm);
  free_scanner(gwion->scan);
  free_symbols();
}
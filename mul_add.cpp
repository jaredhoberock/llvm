// g++ -std=c++11 `llvm-config --cxxflags` mul_add.cpp `llvm-config --ldflags` `llvm-config --libs` -ldl

#include <string>
#include <memory>
#include <iostream>
#include <vector>
#include <algorithm>
#include <iterator>

#include <llvm/Support/raw_ostream.h>
#include <llvm/LLVMContext.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/IRReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ExecutionEngine/JIT.h>
#include <llvm/Linker.h>
#include <llvm/DerivedTypes.h>
#include <llvm/Function.h>
#include <llvm/GlobalValue.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Module.h>
#include <llvm/Instructions.h>

const char *int_math_ir =
"%T = type i32\n"
"define i32 @multiply(i32 %x, i32 %y) {\n"
"entry:\n"
"  %tmp = mul i32 %x, %y\n"
"  ret i32 %tmp\n"
"}\n"
"define i32 @plus(i32 %x, i32 %y) {\n"
"entry:\n"
"  %tmp = add i32 %x, %y\n"
"  ret i32 %tmp\n"
"}\n"
;

const char *mul_add_ir = 
"%T = type opaque\n"
"declare %T @multiply(%T %x, %T %y)\n"
"declare %T @plus(%T %x, %T %y)\n"
"define %T @mul_add(%T %x, %T %y, %T %z) {\n"
"entry:\n"
"  %tmp  = call %T @multiply(%T %x, %T %y)\n"
"  %tmp2 = call %T @plus(%T %tmp, %T %z)\n"
"  ret %T %tmp2\n"
"}\n"
;


struct type_mapper
  : public llvm::ValueMapTypeRemapper
{
  llvm::Type *replace_me;
  llvm::Type *replace_me_with;

  type_mapper(llvm::Type *replace_me, llvm::Type *replace_me_with)
    : replace_me(replace_me),
      replace_me_with(replace_me_with)
  {}

  virtual llvm::Type *remapType(llvm::Type *SrcTy)
  {
    if(SrcTy == replace_me)
    {
      return replace_me_with;
    }

    return SrcTy;
  }
};


void specialize_declarations(llvm::LLVMContext &context, llvm::Module *generic_module, llvm::ValueToValueMapTy &vmap, llvm::Module *module_i32)
{
  using namespace llvm;

  Type *i32_type = Type::getInt32Ty(context);

  std::vector<Function*> specialize_me;

  // collect function declarations to specialize
  for(auto fun = generic_module->begin();
      fun != generic_module->end();
      ++fun)
  {
    std::string name = fun->getName();

    if(fun->empty())
    {
      specialize_me.push_back(&*fun);
    }
  }

  // specialize the declarations
  for(auto fun : specialize_me)
  {
    auto &args = fun->getArgumentList();

    std::vector<Type*> i32_args(args.size(), i32_type);

    FunctionType *specialized_signature = FunctionType::get(i32_type, ArrayRef<Type*>(i32_args.data(), i32_args.size()), false);

    std::string name = fun->getName();
    vmap[fun] = module_i32->getOrInsertFunction(StringRef(name.c_str()), specialized_signature);
  }
}


void specialize_definitions(llvm::LLVMContext &context, llvm::Module *generic_module, llvm::ValueToValueMapTy &vmap, llvm::Module *module_i32)
{
  using namespace llvm;

  Type *i32_type = Type::getInt32Ty(context);

  std::vector<Function*> specialize_me;

  // collect function definitions to specialize
  for(auto fun = generic_module->begin();
      fun != generic_module->end();
      ++fun)
  {
    std::string name = fun->getName();

    if(!fun->empty())
    {
      specialize_me.push_back(&*fun);
    }
  }

  for(auto fun : specialize_me)
  {
    // make a copy of vmap local to this function 
    llvm::ValueToValueMapTy local_vmap;
    for(auto key = vmap.begin();
        key != vmap.end();
        ++key)
    {
      local_vmap[key->first] = key->second;
    }

    auto &args = fun->getArgumentList();

    std::vector<Type*> i32_args(args.size(), i32_type);

    FunctionType *specialized_signature = FunctionType::get(i32_type, ArrayRef<Type*>(i32_args.data(), i32_args.size()), false);

    std::string name = fun->getName();
    module_i32->getOrInsertFunction(StringRef(name.c_str()), specialized_signature);
    Function *specialization = module_i32->getFunction(name); 

    // copy the names of parameters into the specialization
    for(auto parm_i32 = specialization->arg_begin(), parm = fun->arg_begin();
        parm_i32 != specialization->arg_end();
        ++parm_i32, ++parm)
    {
      parm_i32->setName(parm->getName());

      local_vmap[parm] = parm_i32;
    }

    // clone the body of the original into the specialization
    // get the type variable
    Type *type_variable = fun->getReturnType();
  
    // replace instances of the type variable with i32 in the clone
    SmallVector<ReturnInst*,16> returns;

    // map T to i32 and old function parameters to their new copies
    type_mapper type_mapper(type_variable, i32_type);
    CloneFunctionInto(specialization, fun,
                      local_vmap,
                      false,
                      returns,
                      "",
                      0,
                      &type_mapper);
  }
}


int main(int argc, char **argv)
{
  using namespace llvm;

  InitializeNativeTarget();

  LLVMContext context;
  SMDiagnostic error;

  Module *generic_module = ParseIR(MemoryBuffer::getMemBuffer(StringRef(mul_add_ir)), error, context);
  if(!generic_module)
  {
    std::cerr << "Error after first ParseIR" << std::endl;
    error.print(argv[0], errs());
    std::exit(-1);
  }

  // create a new module to hold the i32 specialization of generic_module
  Module module_i32(StringRef("module_i32"), context);

  // maps generic forms to their specializations
  ValueToValueMapTy vmap;

  specialize_declarations(context, generic_module, vmap, &module_i32);

  specialize_definitions(context, generic_module, vmap, &module_i32);

  Module *user_module = ParseIR(MemoryBuffer::getMemBuffer(StringRef(int_math_ir)), error, context);
  if(!user_module)
  {
    std::cerr << "Error after second ParseIR" << std::endl;
    error.print(argv[0], errs());
    std::exit(-1);
  }

  Linker ld(StringRef(argv[0]), StringRef("mul_add_prog"), context);

  std::string error_msg;
  if(ld.LinkInModule(&module_i32, &error_msg))
  {
    std::cerr << "Error after linkInModule(m1)" << std::endl;
    std::cerr << error_msg << std::endl;
    std::exit(-1);
  }

  if(ld.LinkInModule(user_module, &error_msg))
  {
    std::cerr << "Error after linkInModule(user_module)" << std::endl;
    std::cerr << error_msg << std::endl;
    std::exit(-1);
  }

  Module *composite = ld.releaseModule();

  composite->dump();

  ExecutionEngine *ee = ExecutionEngine::create(composite);

  Function *func = ee->FindFunctionNamed("mul_add");
  if(!func)
  {
    std::cerr << "Couldn't find mul_add" << std::endl;
    std::exit(-1);
  }

  std::cout << std::endl;

  typedef int (*fcn_ptr)(int,int,int);
  fcn_ptr mul_add = reinterpret_cast<fcn_ptr>(ee->getPointerToFunction(func));
  std::cout << "mul_add(1,2,3): " << mul_add(1,2,3) << std::endl;
  delete ee;

  return 0;
}


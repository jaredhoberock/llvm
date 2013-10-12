// g++ -std=c++11 -c ir_jit_hello_world.cpp `llvm-config --cxxflags` -o ir_jit_hello_world.o
// g++ `llvm-config --ldflags` -o ir_jit_hello_world ir_jit_hello_world.o `llvm-config --libs` -lpthread -ldl

#include <string>
#include <memory>
#include <iostream>

#include <llvm/Support/raw_ostream.h>
#include <llvm/LLVMContext.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/IRReader.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/ExecutionEngine/JIT.h>

// // hello_world.c:
// extern int printf(const char *, ...);
// void hello_world()
// {
//   printf("Hello, world!\n");
// }
//
// clang -S -emit-llvm hello_world.c

const char *hello_world_ir =
"; ModuleID = 'hello_world.c'\n"
"target datalayout = \"e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:64:64-v128:128:128-a0:0:64-f80:32:32-n8:16:32-S128\"\n"
"target triple = \"i386-pc-linux-gnu\"\n"
"@.str = private unnamed_addr constant [15 x i8] c\"Hello, world!\\0A\\00\", align 1\n"
"define void @hello_world() nounwind {\n"
"  %1 = call i32 (i8*, ...)* @printf(i8* getelementptr inbounds ([15 x i8]* @.str, i32 0, i32 0))\n"
"  ret void\n"
"}\n"
"declare i32 @printf(i8*, ...)"
;

int main(int argc, char **argv)
{
  using namespace llvm;

  std::cout << "hello_world_ir: " << std::endl;
  std::cout << std::endl;
  std::cout << hello_world_ir << std::endl;
  std::cout << std::endl;

  InitializeNativeTarget();

  LLVMContext context;
  SMDiagnostic error;

  Module *m = ParseIR(MemoryBuffer::getMemBuffer(StringRef(hello_world_ir)), error, context);
  if(!m)
  {
    error.print(argv[0], errs());
  }

  ExecutionEngine *ee = ExecutionEngine::create(m);

  Function *func = ee->FindFunctionNamed("hello_world");

  typedef void (*fcn_ptr)();
  fcn_ptr hello_world = reinterpret_cast<fcn_ptr>(ee->getPointerToFunction(func));
  hello_world();
  delete ee;

  return 0;
}


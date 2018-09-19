// HeadersAnalyzer.cpp : Defines the entry point for the console application.
//

#include "ClangHelper.hpp"
#include "Config.hpp"
#include "HAContext.hpp"
#include "LLDBHelper.hpp"
#include "LLVMHelper.hpp"
#include "TapiHelper.hpp"

#include <Plugins/SymbolFile/PDB/PDBASTParser.h>
#include <Plugins/SymbolFile/PDB/SymbolFilePDB.h>
#include <lldb/Core/Debugger.h>
#include <lldb/Core/Module.h>
#include <lldb/Symbol/ClangASTContext.h>
#include <lldb/Symbol/ClangUtil.h>
#include <lldb/Symbol/Type.h>

#include <CodeGen/CodeGenModule.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Type.h>
#include <clang/CodeGen/CodeGenABITypes.h>
#include <clang/CodeGen/CodeGenAction.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Parse/ParseAST.h>

#include <llvm/DebugInfo/PDB/PDBSymbolFunc.h>
#include <llvm/DebugInfo/PDB/PDBSymbolPublicSymbol.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Mangler.h>
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/FunctionComparator.h>

#include <filesystem>
#include <iostream>
#include <vector>

using namespace clang;
using namespace frontend;
using namespace std;
using namespace filesystem;
using namespace tapi::internal;

class HeadersAnalyzer {
public:
  HeadersAnalyzer() : LLVM(LLVMInit) {}

  void discoverTBDs() {
    TBDHandler TH(HAC);
    vector<string> Dirs{
        "./deps/apple-headers/iPhoneOS11.1.sdk/usr/lib/",
        "./deps/apple-headers/iPhoneOS11.1.sdk/System/Library/TextInput/"};
    for (const string &Dir : Dirs)
      for (auto &File : directory_iterator(Dir))
        TH.HandleFile(File.path().string());
    // Discover `.tbd` files inside frameworks.
    string FrameworksDir =
        "./deps/apple-headers/iPhoneOS11.1.sdk/System/Library/Frameworks/";
    for (auto &File : directory_iterator(FrameworksDir))
      if (File.status().type() == file_type::directory &&
          !File.path().extension().compare(".framework"))
        TH.HandleFile(
            (File.path() / File.path().filename().replace_extension(".tbd"))
                .string());
    llvm::outs() << '\n';
  }
  void parseAppleHeaders() {
    compileAppleHeaders();

    for (const llvm::Function &Func : *LLVM.getModule())
      analyzeAppleFunction(Func);

    reportUnimplementedFunctions();
  }
  void loadDLLs() {
    using namespace llvm::pdb;
    LLDBHelper LLDB;
    ClangHelper Clang(LLVM);

    // Create `clang::CodeGen::CodeGenModule` needed in our `TypeComparer`.
    Clang.Args.add("-target");
    Clang.Args.add(IRHelper::Windows32);
    // Note that this file is not really analyzed, but it still needs to exist
    // (because it's opened) and also its extension is important (to set
    // language options - Objective-C for `.mm`).
    Clang.Args.add("./src/HeadersAnalyzer/iOSHeaders.mm");
    Clang.initFromInvocation();
    Clang.executeAction<InitOnlyAction>();
    auto CGM(Clang.createCodeGenModule());

    // Load DLLs and PDBs.
    for (auto [GroupPtr, DLLGroup] : withPtrs(HAC.DLLGroups)) {
      for (auto [DLLPtr, DLL] : withPtrs(DLLGroup.DLLs)) {
        path DLLPath(DLLGroup.Dir / DLL.Name);
        path PDBPath(DLLPath);
        PDBPath.replace_extension(".pdb");

        LLDB.load(DLLPath.string().c_str(), PDBPath.string().c_str());
        TypeComparer TC(*CGM, LLVM.getModule(), LLDB.getSymbolFile());

        // Analyze functions.
        auto Analyzer = [&](auto &&Func, bool IgnoreDuplicates = false) {
          string Name(LLDBHelper::mangleName(Func));

          // Find the corresponding export info from TBD files.
          ExportPtr Exp;
          if (!HAC.isInterestingForWindows(Name, Exp, IgnoreDuplicates))
            return;

          // Update status accordingly.
          Exp->Status = ExportStatus::FoundInDLL;
          Exp->RVA = Func.getRelativeVirtualAddress();
          DLL.Exports.push_back(Exp);
          Exp->DLLGroup = GroupPtr;
          Exp->DLL = DLLPtr;

          // Save function that will serve as a reference for computing
          // addresses of Objective-C methods.
          if (!DLL.ReferenceFunc && !Exp->ObjCMethod)
            DLL.ReferenceFunc = Exp;

          auto IsStretSetter = [&]() {
            // If it's a normal messenger, it has two parameters (`id` and
            // `SEL`, both actually `void *`). If it's a `stret` messenger, it
            // has one more parameter at the front (a `void *` for struct
            // return).
            Exp->Stret = !Exp->Name.compare(
                Exp->Name.size() - HAContext::StretLength,
                HAContext::StretLength, HAContext::StretPostfix);
          };

          // Find Objective-C messengers. Note that they used to be variadic,
          // but that's deprecated and so we cannot rely on that.
          if (!Exp->Name.compare(0, HAContext::MsgSendLength,
                                 HAContext::MsgSendPrefix)) {
            Exp->Messenger = true;
            IsStretSetter();

            // Don't verify their types.
            return;
          }

          // Also, change type of the lookup functions. In Apple headers, they
          // are declared as `void -> void`, but we need them to have the few
          // first arguments they base their lookup on, so that we transfer them
          // correctly.
          if (!Exp->Name.compare(0, HAContext::MsgLookupLength,
                                 HAContext::MsgLookupPrefix)) {
            IsStretSetter();
            Exp->Type = Exp->Stret ? LookupStretTy : LookupTy;

            // Don't verify their types.
            return;
          }

          // Skip type verification of vararg functions. It doesn't work well -
          // at least for `_NSLog`. There is a weird bug that happens randomly -
          // sometimes everything works fine, sometimes there is an assertion
          // failure `Assertion failed: isValidArgumentType(Params[i]) && "Not a
          // valid type for function argument!", file ..\..\lib\IR\Type.cpp,
          // line 288`.
          // TODO: Investigate and fix this bug.
          if (Exp->Type->isVarArg())
            return;

          // Verify that the function has the same signature as the iOS one.
          if (!TC.areEquivalent(Exp->Type, Func))
            reportError("functions' signatures are not equivalent (" +
                        Exp->Name + ")");
        };
        for (auto &Func : LLDB.enumerate<PDBSymbolFunc>())
          Analyzer(Func);
        for (auto &Func : LLDB.enumerate<PDBSymbolPublicSymbol>())
          Analyzer(Func, /* IgnoreDuplicates */ true);
      }
    }
  }
  void createDirs() {
    OutputDir = createOutputDir("./src/HeadersAnalyzer/Debug/CG/");
    WrappersDir = createOutputDir("./out/Wrappers/");
    DylibsDir = createOutputDir("./out/Dylibs/");
  }
  void generateDLLs() {
    // Generate DLL wrappers and also stub Dylibs for them.
    for (const DLLGroup &DLLGroup : HAC.DLLGroups) {
      for (const DLLEntry &DLL : DLLGroup.DLLs) {
        path DLLPath(DLLGroup.Dir / DLL.Name);

        IRHelper IR(LLVM, DLL.Name, DLLPath.string(), IRHelper::Windows32);
        IRHelper DylibIR(LLVM, DLL.Name, DLLPath.string(), IRHelper::Apple);

        // Since we are transferring data in memory across architectures, they
        // must have the same endianness for that to work.
        if (IR.isLittleEndian() != DylibIR.isLittleEndian())
          reportError("target platforms don't have the same endianness");
        else
          assert(IR.isBigEndian() == DylibIR.isBigEndian() &&
                 "Inconsistency in endianness.");

        // Declare reference function.
        // TODO: What if there are no non-Objective-C functions?
        llvm::Function *RefFunc =
            !DLL.ReferenceFunc ? nullptr : IR.declareFunc(*DLL.ReferenceFunc);

        // Generate function wrappers.
        for (const ExportEntry &Exp : deref(DLL.Exports)) {
          assert(Exp.Status == ExportStatus::FoundInDLL &&
                 "Unexpected status of `ExportEntry`.");

          // Don't generate wrappers for Objective-C messengers. We handle those
          // specially.
          if (Exp.Messenger)
            continue;

          // TODO: Handle variadic functions specially.
          if (Exp.Type->isVarArg())
            reportError(Twine("unhandled variadic function (") + Exp.Name +
                        ")");

          // Declarations.
          llvm::Function *Func = Exp.ObjCMethod ? nullptr : IR.declareFunc(Exp);
          llvm::Function *Wrapper = IR.declareFunc(Exp, /* Wrapper */ true);
          llvm::Function *Stub = DylibIR.declareFunc(Exp, /* Wrapper */ true);

          // Export the wrapper and import the original function.
          Wrapper->setDLLStorageClass(llvm::Function::DLLExportStorageClass);
          if (Func)
            Func->setDLLStorageClass(llvm::Function::DLLImportStorageClass);

          // Generate the Dylib stub.
          DylibIR.defineFunc(Stub);
          DylibIR.Builder.CreateRetVoid();

          FunctionGuard WrapperGuard(IR, Wrapper);

          llvm::Value *UP;
          vector<llvm::Value *> Args;
          if (Exp.isTrivial()) {
            // Trivial functions (`void -> void`) have no arguments, so no union
            // pointer exists - we set it to `nullptr` to check that we don't
            // use it anywhere in the following code.
            UP = nullptr;
          } else {
            auto [Struct, Union] = IR.createParamStruct(Exp);

            // The union pointer is in the first argument.
            UP = Wrapper->args().begin();

            // Get pointer to the structure inside the union.
            llvm::Value *SP =
                IR.Builder.CreateBitCast(UP, Struct->getPointerTo(), "sp");

            // Process arguments.
            Args.reserve(Exp.Type->getNumParams());
            size_t ArgIdx = 0;
            for (llvm::Type *ArgTy : Exp.Type->params()) {
              string ArgNo = to_string(ArgIdx);

              // Load argument from the structure.
              llvm::Value *APP =
                  IR.Builder.CreateStructGEP(Struct, SP, ArgIdx, "app" + ArgNo);
              llvm::Value *AP = IR.Builder.CreateLoad(APP, "ap" + ArgNo);
              llvm::Value *A = IR.Builder.CreateLoad(AP, "a" + ArgNo);

              // Save the argument.
              Args.push_back(A);
              ++ArgIdx;
            }
          }

          llvm::Value *R;
          if (Exp.ObjCMethod) {
            // Objective-C methods are not exported, so we call them by
            // computing their address using their RVA.
            if (!DLL.ReferenceFunc) {
              reportError("no reference function, cannot emit Objective-C "
                          "method DLL wrappers (" +
                          DLL.Name + ")");
              continue;
            }

            // Add RVA to the reference function's address.
            llvm::Value *Addr =
                llvm::ConstantInt::getSigned(llvm::Type::getInt32Ty(LLVM.Ctx),
                                             Exp.RVA - DLL.ReferenceFunc->RVA);
            llvm::Value *RefPtr = IR.Builder.CreateBitCast(
                RefFunc, llvm::Type::getInt8PtrTy(LLVM.Ctx));
            llvm::Value *ComputedPtr = IR.Builder.CreateInBoundsGEP(
                llvm::Type::getInt8Ty(LLVM.Ctx), RefPtr, Addr);
            llvm::Value *FP = IR.Builder.CreateBitCast(
                ComputedPtr, Exp.Type->getPointerTo(), "fp");

            // Call the original DLL function.
            R = IR.createCall(Exp.Type, FP, Args, "r");
          } else
            R = IR.createCall(Func, Args, "r");

          if (R) {
            // Get pointer to the return value inside the union.
            llvm::Value *RP = IR.Builder.CreateBitCast(
                UP, Exp.Type->getReturnType()->getPointerTo(), "rp");

            // Save return value back into the structure.
            IR.Builder.CreateStore(R, RP);
          }

          // Finish.
          IR.Builder.CreateRetVoid();
        }

        // Emit `.obj` file.
        string ObjectFile(
            (OutputDir / DLL.Name).replace_extension(".obj").string());
        IR.emitObj(ObjectFile);

        // Create the wrapper DLL.
        ClangHelper(LLVM).linkDLL(
            (WrappersDir / DLL.Name).string(), ObjectFile,
            path(DLLPath).replace_extension(".lib").string());

        // Emit `.o` file.
        string DylibObjectFile(
            (OutputDir / DLL.Name).replace_extension(".o").string());
        DylibIR.emitObj(DylibObjectFile);

        // Create the stub Dylib.
        ClangHelper(LLVM).linkDylib((OutputDir / ("lib" + DLL.Name))
                                        .replace_extension(".dll.dylib")
                                        .string(),
                                    DylibObjectFile, "/Wrappers/" + DLL.Name);
      }
    }
  }
  void generateDylibs() {
    // Some common types.
    llvm::FunctionType *VoidToVoidTy =
        llvm::FunctionType::get(VoidTy, /* isVarArg */ false);
    llvm::FunctionType *SimpleLookupTy = llvm::FunctionType::get(
        VoidToVoidTy->getPointerTo(), /* isVarArg */ false);
    llvm::Type *SimpleLookupPtrTy = SimpleLookupTy->getPointerTo();

    size_t LibIdx = 0;
    for (const Dylib &Lib : HAC.iOSLibs) {
      string LibNo = to_string(LibIdx++);

      IRHelper IR(LLVM, LibNo, Lib.Name, IRHelper::Apple);

      // Generate function wrappers.
      // TODO: Shouldn't we use aligned instructions?
      for (const ExportEntry &Exp : deref(Lib.Exports)) {

        // Ignore functions that haven't been found in any DLL.
        if (Exp.Status != ExportStatus::FoundInDLL) {
          if constexpr (ErrorUnimplementedFunctions & LibType::DLL) {
            if (Exp.Status == ExportStatus::Found)
              reportError(
                  Twine("function found in Dylib wasn't found in any DLL (") +
                  Exp.Name + ")");
          }
          continue;
        }

        // Handle Objective-C messengers specially.
        if (Exp.Messenger) {
          // Now here comes the trick. We actually declare the `msgSend`
          // function as `void -> void`, then call `msgLookup` as `void ->
          // void(*)(void)` inside of it and tail-call the result. That way, the
          // generated machine instructions shouldn't touch any registers (other
          // than the one for return address), so it should work correctly.
          // TODO: Ideally, we would like to use `PreserveMost` CC (see commit
          // `eeae6dc2`), but it's only for `x86_64` right now.

          // Declare the messenger.
          llvm::Function *MessengerFunc =
              IR.declareFunc(VoidToVoidTy, Exp.Name);

          // And define it, too.
          FunctionGuard MessengerGuard(IR, MessengerFunc);

          // Construct name of the corresponding lookup function.
          Twine LookupName(Twine(HAContext::MsgLookupPrefix) +
                           (Exp.Name.c_str() + HAContext::MsgSendLength));

          // Declare the lookup function.
          llvm::Function *LookupFunc =
              IR.declareFunc(Exp.Stret ? LookupStretTy : LookupTy, LookupName);

          // And bitcast it to `void -> void(*)(void)`.
          llvm::Value *FP =
              IR.Builder.CreateBitCast(LookupFunc, SimpleLookupPtrTy, "fp");

          // Call the lookup function and jump to its result.
          llvm::Value *IMP =
              IR.Builder.CreateCall(SimpleLookupTy, FP, {}, "imp");
          llvm::CallInst *Call = IR.Builder.CreateCall(VoidToVoidTy, IMP, {});
          Call->setTailCallKind(llvm::CallInst::TCK_MustTail);
          IR.Builder.CreateRetVoid();

          continue;
        }

        // Declarations.
        llvm::Function *Func = IR.declareFunc(Exp);
        llvm::Function *Wrapper = IR.declareFunc(Exp, /* Wrapper */ true);

        FunctionGuard FuncGuard(IR, Func);

        // Handle trivial `void -> void` functions specially.
        if (Exp.isTrivial()) {
          IR.Builder.CreateCall(Wrapper);
          IR.Builder.CreateRetVoid();
          continue;
        }

        auto [Struct, Union] = IR.createParamStruct(Exp);

        // Allocate the union.
        llvm::Value *S = IR.Builder.CreateAlloca(Union, nullptr, "s");

        // Get pointer to the structure inside it.
        llvm::Value *SP =
            IR.Builder.CreateBitCast(S, Struct->getPointerTo(), "sp");

        // Process arguments.
        for (llvm::Argument &Arg : Func->args()) {
          string ArgNo = to_string(Arg.getArgNo());

          // Load the argument.
          llvm::Value *AP =
              IR.Builder.CreateAlloca(Arg.getType(), nullptr, "ap" + ArgNo);
          IR.Builder.CreateStore(&Arg, AP);

          // Get pointer to the corresponding structure's element.
          llvm::Value *EP = IR.Builder.CreateStructGEP(
              Struct, SP, Arg.getArgNo(), "ep" + ArgNo);

          // Store argument address in it.
          IR.Builder.CreateStore(AP, EP);
        }

        // Call the DLL wrapper function.
        llvm::Value *VP = IR.Builder.CreateBitCast(
            SP, llvm::Type::getInt8PtrTy(LLVM.Ctx), "vp");
        IR.Builder.CreateCall(Wrapper, {VP});

        // Return.
        llvm::Type *RetTy = Exp.Type->getReturnType();
        if (!RetTy->isVoidTy()) {

          // Get pointer to the return value inside the union.
          llvm::Value *RP =
              IR.Builder.CreateBitCast(S, RetTy->getPointerTo(), "rp");

          // Load and return it.
          llvm::Value *R = IR.Builder.CreateLoad(RP, "r");
          IR.Builder.CreateRet(R);
        } else
          IR.Builder.CreateRetVoid();
      }

      // Emit `.o` file.
      string ObjectFile((OutputDir / (LibNo + ".o")).string());
      IR.emitObj(ObjectFile);

      // We add `./` to the library name to convert it to a relative path.
      path DylibPath(DylibsDir / ("./" + Lib.Name));

      // Initialize Clang args to create the Dylib.
      ClangHelper Clang(LLVM);
      Clang.addDylibArgs(DylibPath.string(), ObjectFile, Lib.Name);
      Clang.Args.add("-L");
      Clang.Args.add(OutputDir.string().c_str());

      // Add DLLs to link.
      set<DLLPtr> DLLs;
      for (const ExportEntry &Exp : deref(Lib.Exports))
        if (Exp.DLL && DLLs.insert(Exp.DLL).second) {
          Clang.Args.add("-l");
          Clang.Args.add(
              path(Exp.DLL->Name).replace_extension(".dll").string().c_str());
        }

      // Create output directory.
      createOutputDir(DylibPath.parent_path().string().c_str());

      // Link the Dylib.
      Clang.executeArgs();
    }
  }

private:
  HAContext HAC;
  LLVMInitializer LLVMInit;
  LLVMHelper LLVM;
  path OutputDir, WrappersDir, DylibsDir;

  // Some common types.
  llvm::Type *VoidTy = llvm::Type::getVoidTy(LLVM.Ctx);
  llvm::Type *VoidPtrTy = llvm::Type::getInt8PtrTy(LLVM.Ctx);
  llvm::FunctionType *SendTy = llvm::FunctionType::get(
      VoidTy, {VoidPtrTy, VoidPtrTy}, /* isVarArg */ false);
  llvm::FunctionType *SendStretTy = llvm::FunctionType::get(
      VoidTy, {VoidPtrTy, VoidPtrTy, VoidPtrTy}, /* isVarArg */ false);
  llvm::FunctionType *LookupTy = llvm::FunctionType::get(
      SendTy->getPointerTo(), {VoidPtrTy, VoidPtrTy}, /* isVarArg */ false);
  llvm::FunctionType *LookupStretTy = llvm::FunctionType::get(
      SendStretTy->getPointerTo(), {VoidPtrTy, VoidPtrTy, VoidPtrTy},
      /* isVarArg */ false);

  void analyzeAppleFunction(const llvm::Function &Func) {
    // We use mangled names to uniquely identify functions.
    string Name(LLVM.mangleName(Func));

    // Find the corresponding export info from TBD files.
    ExportPtr Exp;
    if (!HAC.isInteresting(Name, Exp))
      return;

    // Update status accordingly.
    switch (Exp->Status) {
    case ExportStatus::Found:
      Exp->Status = ExportStatus::Overloaded;
      reportError("function overloaded (" + Name + ")");
      return;
    case ExportStatus::Overloaded:
      return;
    case ExportStatus::NotFound:
      Exp->Status = ExportStatus::Found;
      break;
    default:
      reportFatalError("unexpected status of `ExportEntry`");
    }

    // Save the function's signature.
    Exp->Type = Func.getFunctionType();
  }
  void compileAppleHeaders() {
    ClangHelper Clang(LLVM);
    Clang.Args.loadConfigFile("./src/HeadersAnalyzer/analyze_ios_headers.cfg");
    Clang.initFromInvocation();

    // Include all declarations in the result. See [emit-all-decls].
    // TODO: Maybe filter them (include only those exported from iOS Dylibs).
    Clang.CI.getLangOpts().EmitAllDecls = true;

    // But don't emit bodies, we don't need them. See [emit-bodies].
    Clang.CI.getLangOpts().EmitBodies = false;

    // Compile to LLVM IR.
    Clang.executeCodeGenAction<EmitLLVMOnlyAction>();
  }
  void reportUnimplementedFunctions() {
    if constexpr (ErrorUnimplementedFunctions & LibType::Dylib) {
      for (const ExportEntry &Exp : HAC.iOSExps) {
        if (Exp.Status == ExportStatus::NotFound)
          reportError(
              "function found in TBD files wasn't found in any Apple header (" +
              Exp.Name + ")");
      }
    }
  }
};

int main() {
  try {
    HeadersAnalyzer HA;
    HA.discoverTBDs();
    HA.parseAppleHeaders();
    HA.loadDLLs();
    HA.createDirs();
    HA.generateDLLs();
    HA.generateDylibs();
  } catch (const FatalError &) {
    return 1;
  }

  return 0;
}

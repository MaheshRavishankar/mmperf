#include "llvm/Support/CommandLine.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "mlir/Conversion/SCFToStandard/SCFToStandard.h"
#include "mlir/Conversion/ShapeToStandard/ShapeToStandard.h"
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVM.h"
#include "mlir/Conversion/StandardToLLVM/ConvertStandardToLLVMPass.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"
#include "mlir/Dialect/Linalg/Transforms/CodegenStrategy.h"
#include "mlir/Dialect/Linalg/Transforms/Transforms.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/StandardOps/Transforms/Passes.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/IR/AsmState.h"
#include "mlir/IR/Dialect.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/InitAllDialects.h"
#include "mlir/InitAllPasses.h"
#include "mlir/Parser.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/FileUtilities.h"
#include "mlir/Support/MlirOptMain.h"
#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Target/LLVMIR/ModuleTranslation.h"
#include "mlir/Transforms/Passes.h"

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <iostream>

#include "flatbuffers/flatbuffers.h"
#include "compile_options_generated.h"

using namespace mlir;
using namespace mlir::linalg;

using llvm::Error;
using llvm::Expected;
using llvm::StringError;
using llvm::Twine;

namespace mlir {

namespace {
namespace cl = llvm::cl;
struct Options {
  cl::opt<std::string> inputFile{cl::Positional,
    cl::desc("the input .mlir file"),
    cl::init("")};

  // CPU info
  cl::opt<std::string> targetCPU{"target-cpu", cl::Required,
    cl::desc("Target CPU for codegen"),
    cl::init("skylake-avx512")};
  cl::opt<std::string> vectorWidth{"vector-width", cl::Required,
    cl::desc("Target vector width for codegen"),
    cl::init("512")};

  // Codegen info
  cl::opt<std::string> compileOptions{"compile-options", cl::Required,
    cl::desc("Flatbuffer file describing compile options configurations"),
    cl::init("empty_file_name")};
};
}

namespace {
struct LinalgCodegenPass : public PassWrapper<LinalgCodegenPass, FunctionPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<linalg::LinalgDialect, AffineDialect, scf::SCFDialect>();
  }
  LinalgCodegenPass() = default;
  LinalgCodegenPass(Options &options) {
    params.targetCPU = options.targetCPU;
    params.vectorWidth = options.vectorWidth;
    params.compileOptions = options.compileOptions;
  }
  LinalgCodegenPass(const LinalgCodegenPass &pass) {
    params = pass.params;
  }
  void runOnFunction() override;

  struct Parameters {
    std::string vectorWidth, targetCPU;
    std::string compileOptions;
  };
  Parameters params;
  Nod::CompileOptionsT config;
};
}  // namespace

template<typename LinalgOp>
void performCompileOptions(Nod::OptionsT& options, mlir::FuncOp func) {
  CodegenStrategy strategy;

  const auto& tileOptions = options.tile_options;
  // Tile Codegen
  if (tileOptions != NULL) {
    linalg::LinalgTilingLoopType loop_type = linalg::LinalgTilingLoopType::Loops;
    switch(options.tile_options->loop_type) {
      case Nod::LinalgTilingLoopType_loops:
        loop_type = linalg::LinalgTilingLoopType::Loops;
        break;
        case Nod::LinalgTilingLoopType_affine_loops:
        loop_type = linalg::LinalgTilingLoopType::AffineLoops;
        break;
        case Nod::LinalgTilingLoopType_parallel_loops:
        loop_type = linalg::LinalgTilingLoopType::ParallelLoops;
        break;
    }

    llvm::SmallVector<int64_t, 4> tileSizes;
    llvm::SmallVector<unsigned int, 4> interchangeVector;
    llvm::SmallVector<int64_t, 4> promoteOperands;

    for (int i = 0; i < tileOptions->tile_sizes.size(); i++) {
      tileSizes.push_back(tileOptions->tile_sizes[i]);
    }
    for (int i = 0; i < tileOptions->interchange_vector.size(); i++) {
      interchangeVector.push_back(tileOptions->interchange_vector[i]);
    }
    for (int i = 0; i < tileOptions->promote_operands.size(); i++) {
      promoteOperands.push_back(tileOptions->promote_operands[i]);
    }

    strategy.tileIf<LinalgOp>(!tileSizes.empty(), LinalgTilingOptions()
                      .setTileSizes(tileSizes)
                      .setInterchange(interchangeVector)
                      .setLoopType(loop_type));
    strategy.promoteIf<LinalgOp>(!promoteOperands.empty(), LinalgPromotionOptions()
                      .setOperandsToPromote(tileOptions->promote_operands)
                      .setUseFullTileBuffersByDefault(tileOptions->promote_full_tile)
                      .setAlignment(getpagesize()));
  }

  const auto& vectorizeOptions = options.vectorize_options;
  // Vectorize Codegen
  if (vectorizeOptions != NULL) {
    vector::VectorContractLowering vectorContractLowering = vector::VectorContractLowering::Dot;
    switch(vectorizeOptions->vectorize_to) {
      case Nod::VectorContractLowering_dot:
        vectorContractLowering = vector::VectorContractLowering::Dot;
        break;
      case Nod::VectorContractLowering_matmul:
        vectorContractLowering = vector::VectorContractLowering::Matmul;
        break;
      case Nod::VectorContractLowering_outer_product:
        vectorContractLowering = vector::VectorContractLowering::OuterProduct;
        break;
    }

    vector::VectorTransferSplit vectorTransferSplit = vector::VectorTransferSplit::None;
    switch(vectorizeOptions->vector_transfer_split) {
      case Nod::VectorTransferSplit_none:
        vectorTransferSplit = vector::VectorTransferSplit::None;
        break;
      case Nod::VectorTransferSplit_linalg_copy:
        vectorTransferSplit = vector::VectorTransferSplit::LinalgCopy;
        break;
      case Nod::VectorTransferSplit_vector_transfer:
        vectorTransferSplit = vector::VectorTransferSplit::VectorTransfer;
        break;
    }

    strategy.vectorizeIf<LinalgOp>(vectorizeOptions != NULL)
                      .setVectorTransformsOptions(
                          vector::VectorTransformsOptions()
                              .setVectorTransformsOptions(vectorContractLowering)
                              .setVectorTransferSplit(vectorTransferSplit))
                      .setVectorTransferToSCFOptions(
                          VectorTransferToSCFOptions()
                            .setUnroll(vectorizeOptions->unroll_vector_transfers));
  }

  strategy.transform(func);
}

void LinalgCodegenPass::runOnFunction() {
  MLIRContext *ctx = getFunction().getContext();
  SmallVector<Attribute, 4> attrs;
  attrs.push_back(ArrayAttr::get(ctx,
                                 {StringAttr::get(ctx, "prefer-vector-width"),
                                  StringAttr::get(ctx, params.vectorWidth)}
                                ));
  attrs.push_back(ArrayAttr::get(ctx,
                                 {StringAttr::get(ctx, "target-cpu"),
                                  StringAttr::get(ctx, params.targetCPU)}
                                ));
  getFunction()->setAttr("passthrough", ArrayAttr::get(ctx, attrs));

  // Load CompileOptions from file
  std::ifstream infile;
  infile.open(params.compileOptions, std::ios::binary | std::ios::in);
  infile.seekg(0,std::ios::end);
  int length = infile.tellg();
  if (length < 0) {
    std::cout << "File not found!" << std::endl;
    std::cout << params.compileOptions << std::endl;
    exit(1);
  }
  infile.seekg(0,std::ios::beg);
  char *data = new char[length];
  infile.read(data, length);
  infile.close();
  Nod::GetCompileOptions(data)->UnPackTo(&config);

  // Dynamic Codegen
  const auto& options = config.options;
  for (unsigned int option_index = 0; option_index < options.size(); option_index++) {
    const auto& option = options[option_index];
    // TODO: add matmul column major op
    switch (option->op) {
      case Nod::LinalgOperator_matmul:
        performCompileOptions<MatmulOp>(*option, getFunction());
        break;
      case Nod::LinalgOperator_fill:
        performCompileOptions<FillOp>(*option, getFunction());
        break;
      case Nod::LinalgOperator_copy:
        performCompileOptions<CopyOp>(*option, getFunction());
        break;
      case Nod::LinalgOperator_unknown:
        std::cout << "Must define operator in compile options" << std::endl;
        exit(1);
    }
  }
}

std::unique_ptr<OperationPass<FuncOp>> createLinalgCodegenPass(Options &options) {
  return std::make_unique<LinalgCodegenPass>(options);
}
}

//===----------------------------------------------------------------------===//

/// Wrap a string into an llvm::StringError.
static Error make_string_error(const Twine &message) {
  return llvm::make_error<StringError>(message.str(), llvm::inconvertibleErrorCode());
}

Error compile(Options &options, mlir::DialectRegistry &registry) {
  MLIRContext context(registry);
  context.loadAllAvailableDialects();
  llvm::errs() << "Read file: " << options.inputFile << "\n";
  OwningModuleRef moduleRef = parseSourceFile(options.inputFile, &context);
  if (!moduleRef)
    return make_string_error(Twine("could not open ") + options.inputFile);

  ModuleOp module = *moduleRef;
  PassManager pm(module.getContext(), OpPassManager::Nesting::Implicit);
  mlir::applyPassManagerCLOptions(pm);
  pm.addPass(createCanonicalizerPass());
  pm.addPass(createLinalgCodegenPass(options));
  pm.addPass(createLinalgComprehensiveModuleBufferizePass());

  // Lower to LLVM
  pm.addPass(createConvertVectorToSCFPass());
  pm.addPass(createLowerAffinePass());
  pm.addPass(createConvertLinalgToLoopsPass());
  pm.addPass(createLowerToCFGPass());
  pm.addPass(createConvertVectorToLLVMPass());
  pm.addPass(createMemRefToLLVMPass());
  pm.addPass(createLowerToLLVMPass());

  if (failed(pm.run(module))) {
    return make_string_error(Twine("error compiling to llvm backend"));
  }

  // Convert from MLIR to LLVMIR
  mlir::registerLLVMDialectTranslation(*module->getContext());
  llvm::LLVMContext llvmContext;
  auto llvmModule = mlir::translateModuleToLLVMIR(module, llvmContext);
  if (!llvmModule) {
    return make_string_error(Twine("error translating to llvm ir"));
  }

  std::string moduleStr;
  llvm::raw_string_ostream ss(moduleStr);
  ss << *llvmModule;

  std::string name = std::filesystem::path(std::string(options.inputFile)).stem();
  name += ".ll";
  std::ofstream output(name);
  output << moduleStr;
  output.close();

  return Error::success();
}

int main(int argc, char **argv) {
  mlir::DialectRegistry registry;
  mlir::registerAllDialects(registry);
  mlir::registerAllPasses();

  llvm::InitLLVM y(argc, argv);
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();
  mlir::initializeLLVMPasses();
  mlir::registerAsmPrinterCLOptions();
  mlir::registerPassManagerCLOptions();

  Options options;
  llvm::cl::ParseCommandLineOptions(argc, argv, "matmul-compile\n");

  auto error = compile(options, registry);
  int exitCode = EXIT_SUCCESS;
  llvm::handleAllErrors(std::move(error), [&exitCode](const llvm::ErrorInfoBase &info) {
    llvm::errs() << "Error: ";
    info.log(llvm::errs());
    llvm::errs() << '\n';
    exitCode = EXIT_FAILURE;
  });
  return exitCode;
}

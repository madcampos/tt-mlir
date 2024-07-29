// SPDX-FileCopyrightText: (c) 2024 Tenstorrent AI ULC
//
// SPDX-License-Identifier: Apache-2.0

#include "ttmlir/Dialect/TTMetal/Transforms/Passes.h"

#include "mlir/Analysis/Liveness.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Bufferization/Transforms/Bufferize.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/MLProgram/IR/MLProgram.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Rewrite/FrozenRewritePatternSet.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "ttmlir/Dialect/TT/IR/TT.h"
#include "ttmlir/Dialect/TT/IR/TTOpsTypes.h"
#include "ttmlir/Dialect/TTIR/IR/TTIR.h"
#include "ttmlir/Dialect/TTIR/IR/TTIROps.h"
#include "ttmlir/Dialect/TTIR/Transforms/Passes.h"

#include "ttmlir/Dialect/TTKernel/IR/TTKernel.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernelOps.h"
#include "ttmlir/Dialect/TTKernel/IR/TTKernelOpsTypes.h"
#include "ttmlir/Dialect/TTMetal/IR/TTMetalOpsTypes.h"

namespace mlir::tt::ttmetal {
struct CoreCoord {
  std::int64_t d = 0;
  std::int64_t r = 0;
  std::int64_t c = 0;

  std::int64_t &operator[](std::size_t i) {
    assert(i < 3);
    return i == 0 ? d : i == 1 ? r : c;
  }

  std::int64_t operator[](std::size_t i) const {
    assert(i < 3);
    return i == 0 ? d : i == 1 ? r : c;
  }

  bool operator==(CoreCoord const &other) const {
    return d == other.d && r == other.r && c == other.c;
  }
};
} // namespace mlir::tt::ttmetal

namespace llvm {
template <> struct DenseMapInfo<mlir::tt::ttmetal::CoreCoord> {
  static mlir::tt::ttmetal::CoreCoord getEmptyKey() {
    return mlir::tt::ttmetal::CoreCoord{-1, -1, -1};
  }

  static mlir::tt::ttmetal::CoreCoord getTombstoneKey() {
    return mlir::tt::ttmetal::CoreCoord{-2, -2, -2};
  }

  static unsigned getHashValue(mlir::tt::ttmetal::CoreCoord coord) {
    return llvm::hash_combine(coord.d, coord.r, coord.c);
  }

  static bool isEqual(mlir::tt::ttmetal::CoreCoord lhs,
                      mlir::tt::ttmetal::CoreCoord rhs) {
    return lhs == rhs;
  }
};
} // namespace llvm

namespace mlir::tt::ttmetal {

#define GEN_PASS_DEF_CONVERTTTIRTOTTMETAL
#include "ttmlir/Dialect/TTMetal/Transforms/Passes.h.inc"

class TTIRToTTMetalLayoutRewriter : public OpRewritePattern<ttir::LayoutOp> {
public:
  using OpRewritePattern<ttir::LayoutOp>::OpRewritePattern;

  struct NocRead {
    CoreCoord srcCoord;
    std::int64_t srcOffset = 0;
    std::int64_t dstOffset = 0;
    std::int64_t size = 0;

    bool isContiguous(CoreCoord nextCoord, std::int64_t nextSrcOffset,
                      std::int64_t nextDstOffset) const {
      return (nextCoord == srcCoord) && (nextSrcOffset == srcOffset + size) &&
             (nextDstOffset == dstOffset + size);
    }
  };

  mlir::DenseMap<CoreCoord, mlir::SmallVector<NocRead>>
  calculateDataMovement(ArrayRef<std::int64_t> tensorShape,
                        std::int64_t elemSize, AffineMap src,
                        AffineMap dst) const {
    // For now it's just a simple pull model, but eventually we want to leverage
    // both NoCs and the both read and write
    SmallVector<std::int64_t> strides;
    int64_t stride = 1;
    for (int i = tensorShape.size() - 1; i >= 0; --i) {
      strides.push_back(stride);
      stride *= tensorShape[i];
    }

    int64_t volume = stride;
    mlir::SmallVector<mlir::AffineExpr, 8> exprs(tensorShape.size());
    mlir::DenseMap<CoreCoord, mlir::SmallVector<NocRead>> dst2srcMap;
    assert(3 == src.getNumResults() - 1);
    assert(3 == dst.getNumResults() - 1);
    for (int i = 0; i < volume; ++i) {
      for (unsigned j = 0; j < tensorShape.size(); ++j) {
        exprs[j] = getAffineConstantExpr((i / strides[j]) % tensorShape[j],
                                         src.getContext());
      }

      CoreCoord srcCoord;
      CoreCoord dstCoord;
      for (unsigned j = 0; j < src.getNumResults() - 1; ++j) {
        mlir::AffineExpr srcCoordExpr = src.getResult(j).replaceDims(exprs);
        srcCoord[j] =
            llvm::cast<mlir::AffineConstantExpr>(srcCoordExpr).getValue();
        assert(j != 0 || srcCoord[j] == 0);
        mlir::AffineExpr dstCoordExpr = dst.getResult(j).replaceDims(exprs);
        dstCoord[j] =
            llvm::cast<mlir::AffineConstantExpr>(dstCoordExpr).getValue();
        assert(j != 0 || dstCoord[j] == 0);
      }

      mlir::AffineExpr srcIndexExpr =
          src.getResult(src.getNumResults() - 1).replaceDims(exprs);
      std::int64_t srcIndex =
          llvm::cast<mlir::AffineConstantExpr>(srcIndexExpr).getValue();
      mlir::AffineExpr dstIndexExpr =
          dst.getResult(dst.getNumResults() - 1).replaceDims(exprs);
      std::int64_t dstIndex =
          llvm::cast<mlir::AffineConstantExpr>(dstIndexExpr).getValue();
      std::int64_t srcOffset = srcIndex * elemSize;
      std::int64_t dstOffset = dstIndex * elemSize;
      mlir::SmallVector<NocRead> &srcs = dst2srcMap[dstCoord];
      if (srcs.empty() ||
          not srcs.back().isContiguous(srcCoord, srcOffset, dstOffset)) {
        if (not srcs.empty()) {
          llvm::outs() << "asdfasdfasdf\n";
          llvm::outs() << "srcCoord: " << srcs.back().srcCoord.d << " "
                       << srcs.back().srcCoord.r << " "
                       << srcs.back().srcCoord.c << "\n";
          llvm::outs() << "dstoffset: " << srcs.back().dstOffset << " "
                       << dstOffset << "\n";
          llvm::outs() << "srcOffset: " << srcs.back().srcOffset << " "
                       << srcOffset << "\n";
        }
        srcs.push_back(NocRead{srcCoord, srcOffset, dstOffset, elemSize});
      } else {
        srcs.back().size += elemSize;
      }
    }

    return dst2srcMap;
  }

  LogicalResult matchAndRewrite(ttir::LayoutOp op,
                                PatternRewriter &rewriter) const final {
    auto inputTy = op.getInput().getType().template cast<RankedTensorType>();
    auto outputTy = op.getType().template cast<RankedTensorType>();
    if (not inputTy.getEncoding() || not outputTy.getEncoding()) {
      return failure();
    }
    assert(inputTy.getShape() == outputTy.getShape());
    assert(inputTy.getEncoding().isa<tt::LayoutAttr>());
    assert(outputTy.getEncoding().isa<tt::LayoutAttr>());
    auto inputLayout = inputTy.getEncoding().template cast<tt::LayoutAttr>();
    auto outputLayout = outputTy.getEncoding().template cast<tt::LayoutAttr>();
    if (inputLayout.isSystemMemorySpace()) {
      assert(outputLayout.isDeviceMemorySpace());
      rewriter.replaceOpWithNewOp<ttmetal::HostWriteOp>(
          op, outputTy, op.getInput(), op.getOutput());
    } else if (outputLayout.isSystemMemorySpace()) {
      assert(inputLayout.isDeviceMemorySpace());
      rewriter.replaceOpWithNewOp<ttmetal::HostReadOp>(
          op, outputTy, op.getInput(), op.getOutput());
    } else {
      SmallVector<Attribute> threadTypes = {
          rewriter.getAttr<ttkernel::ThreadTypeAttr>(
              ttkernel::ThreadType::Noc0),
      };
      SmallVector<Attribute> coreRanges = {
          rewriter.getAttr<ttmetal::CoreRangeAttr>(outputLayout.getGrid()),
      };
      SmallVector<Attribute> operand_cb_port_mapping;

      auto metalDispatch = rewriter.create<ttmetal::DispatchOp>(
          op.getLoc(), SmallVector<Type>({outputTy}),
          SmallVector<Value>({op.getInput()}),
          SmallVector<Value>({op.getOutput()}),
          rewriter.getArrayAttr(coreRanges), rewriter.getArrayAttr(threadTypes),
          rewriter.getArrayAttr(operand_cb_port_mapping), threadTypes.size());

      Block *noc0Block = rewriter.createBlock(&metalDispatch.getRegion(0));

      {
        OpBuilder noc0Builder(noc0Block, noc0Block->begin());
        noc0Builder.create<ttkernel::ReturnOp>(op.getLoc(), ValueRange());
      }
      tt::DeviceAttr device = op.getDevice();
      assert(inputLayout.getPhysicalShape(inputTy.getShape()) ==
                 outputLayout.getPhysicalShape(outputTy.getShape()) &&
             "Physical shapes must match for now");
      assert(device);
      AffineMap src =
          inputLayout.projectOnto(inputTy.getShape(), device.getGrid());
      AffineMap dst =
          outputLayout.projectOnto(outputTy.getShape(), device.getGrid());

      auto dm = calculateDataMovement(
          inputTy.getShape(), inputTy.getElementTypeBitWidth() / 8, src, dst);

      for (auto [dstCoord, srcs] : dm) {
        for (auto s : srcs) {
          auto srcCoord = s.srcCoord;
          auto srcOffset = s.srcOffset;
          auto dstOffset = s.dstOffset;
          auto size = srcs[0].size;
          llvm::outs() << "asdf\n";
          llvm::outs() << "srcCoord: " << srcCoord.d << " " << srcCoord.r << " "
            << srcCoord.c << "\n";
          llvm::outs() << "dstCoord: " << dstCoord.d << " " << dstCoord.r << " "
            << dstCoord.c << "\n";
          llvm::outs() << "srcOffset: " << srcOffset << "\n";
          llvm::outs() << "dstOffset: " << dstOffset << "\n";
          llvm::outs() << "size: " << size << "\n";
        }
      }

      rewriter.replaceOp(op, metalDispatch);
    }
    return success();
  }
};

class TTIRToTTMetalKernelRewriter : public OpRewritePattern<ttir::KernelOp> {
public:
  using OpRewritePattern<ttir::KernelOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ttir::KernelOp op,
                                PatternRewriter &rewriter) const final {
    if (not op->use_empty()) {
      return failure();
    }
    rewriter.create<ttkernel::BuiltinOp>(op.getLoc(), op.getOpAttr(),
                                         op.getKindAttr(), op.getOperands());
    op->dropAllUses();
    rewriter.eraseOp(op);
    return success();
  }
};

class TTIRToTTMetalReturnRewriter : public OpRewritePattern<ttir::YieldOp> {
public:
  using OpRewritePattern<ttir::YieldOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ttir::YieldOp op,
                                PatternRewriter &rewriter) const final {
    rewriter.replaceOpWithNewOp<ttkernel::ReturnOp>(op);
    return success();
  }
};

class TTIRToTTMetalDispatchRewriter : public OpRewritePattern<ttir::GenericOp> {
public:
  using OpRewritePattern<ttir::GenericOp>::OpRewritePattern;

  bool hasUnloweredTTIRKernel(ttir::GenericOp op) const {
    bool exists = false;
    op->getRegion(0).walk([&exists](Operation *op) {
      if (isa<ttir::KernelOp>(op)) {
        exists = true;
      }
    });
    return exists;
  }

  uint64_t lookupAddress(Value value) const {
    auto *op = value.getDefiningOp();
    if (!op) {
      return 0;
    }
    auto allocOp = dyn_cast<ttir::AllocOp>(op);
    if (!allocOp) {
      return 0;
    }
    return allocOp.getAddress();
  }

  SmallVector<Type> getBlockArgumentTypesAsCBs(
      mlir::Block::BlockArgListType blockArguments,
      SmallVector<Attribute> const &operand_cb_port_mapping,
      PatternRewriter &rewriter) const {
    SmallVector<Type> rewrittenBlockArgumentTypes;
    for (auto arg : blockArguments) {
      auto address = lookupAddress(arg);
      auto port = operand_cb_port_mapping[arg.getArgNumber()]
                      .cast<IntegerAttr>()
                      .getInt();
      auto memref = arg.getType().cast<MemRefType>();
      rewrittenBlockArgumentTypes.push_back(
          rewriter.getType<ttkernel::CBType>(address, port, memref));
    }
    return rewrittenBlockArgumentTypes;
  }

  LogicalResult matchAndRewrite(ttir::GenericOp op,
                                PatternRewriter &rewriter) const final {
    if (hasUnloweredTTIRKernel(op)) {
      return failure();
    }

    SmallVector<Attribute> threadTypes = {
        rewriter.getAttr<ttkernel::ThreadTypeAttr>(ttkernel::ThreadType::Noc0),
        rewriter.getAttr<ttkernel::ThreadTypeAttr>(ttkernel::ThreadType::Noc1),
        rewriter.getAttr<ttkernel::ThreadTypeAttr>(
            ttkernel::ThreadType::Tensix),
    };
    SmallVector<Attribute> coreRanges = {
        rewriter.getAttr<ttmetal::CoreRangeAttr>(op.getGrid()),
        rewriter.getAttr<ttmetal::CoreRangeAttr>(op.getGrid()),
        rewriter.getAttr<ttmetal::CoreRangeAttr>(op.getGrid()),
    };
    SmallVector<Attribute> operand_cb_port_mapping;
    for (auto &operand : op->getOpOperands()) {
      operand_cb_port_mapping.push_back(
          rewriter.getI64IntegerAttr(operand.getOperandNumber()));
    }
    auto metalDispatch = rewriter.create<ttmetal::DispatchOp>(
        op.getLoc(), op.getResults().getTypes(), op.getInputs(),
        op.getOutputs(), rewriter.getArrayAttr(coreRanges),
        rewriter.getArrayAttr(threadTypes),
        rewriter.getArrayAttr(operand_cb_port_mapping), threadTypes.size());

    auto rewrittenBlockArgumentTypes = getBlockArgumentTypesAsCBs(
        op->getRegion(0).getArguments(), operand_cb_port_mapping, rewriter);

    metalDispatch.getRegion(2).takeBody(op->getRegion(0));
    Block *tensixBlock = &metalDispatch.getRegion(2).front();
    Block *noc0Block = rewriter.createBlock(&metalDispatch.getRegion(0));
    Block *noc1Block = rewriter.createBlock(&metalDispatch.getRegion(1));

    int i = 0;
    for (auto ty : rewrittenBlockArgumentTypes) {
      noc0Block->addArgument(ty, op.getLoc());
      noc1Block->addArgument(ty, op.getLoc());
      auto arg = tensixBlock->getArgument(i++);
      arg.setType(ty);
    }

    {
      OpBuilder noc0Builder(noc0Block, noc0Block->begin());
      auto one = noc0Builder.create<arith::ConstantOp>(
          op.getLoc(), noc0Builder.getI32Type(),
          noc0Builder.getI32IntegerAttr(1));
      noc0Builder.create<ttkernel::CBReserveBackOp>(
          op.getLoc(), noc0Block->getArgument(0), one);
      noc0Builder.create<ttkernel::CBPushBackOp>(
          op.getLoc(), noc0Block->getArgument(0), one);
      noc0Builder.create<ttkernel::ReturnOp>(op.getLoc(), ValueRange());
    }

    {
      OpBuilder noc1Builder(noc1Block, noc1Block->begin());
      auto one = noc1Builder.create<arith::ConstantOp>(
          op.getLoc(), noc1Builder.getI32Type(),
          noc1Builder.getI32IntegerAttr(1));
      noc1Builder.create<ttkernel::CBReserveBackOp>(
          op.getLoc(), noc1Block->getArgument(0), one);
      noc1Builder.create<ttkernel::CBPushBackOp>(
          op.getLoc(), noc1Block->getArgument(0), one);
      noc1Builder.create<ttkernel::ReturnOp>(op.getLoc(), ValueRange());
    }

    rewriter.replaceOp(op, metalDispatch);

    return success();
  }
};

class TTIRToTTMetalAllocRewriter : public OpRewritePattern<ttir::AllocOp> {
public:
  using OpRewritePattern<ttir::AllocOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ttir::AllocOp op,
                                PatternRewriter &rewriter) const final {
    rewriter.replaceOpWithNewOp<ttmetal::AllocOp>(
        op, op.getType(), op.getAddress(), op.getSize(), op.getMemorySpace());
    return success();
  }
};

class TTIRToTTMetalDeallocRewriter : public OpRewritePattern<ttir::DeallocOp> {
public:
  using OpRewritePattern<ttir::DeallocOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ttir::DeallocOp op,
                                PatternRewriter &rewriter) const final {
    rewriter.replaceOpWithNewOp<ttmetal::DeallocOp>(op, op.getResult());
    return success();
  }
};

class ConvertTTIRToTTMetal
    : public impl::ConvertTTIRToTTMetalBase<ConvertTTIRToTTMetal> {
public:
  using impl::ConvertTTIRToTTMetalBase<
      ConvertTTIRToTTMetal>::ConvertTTIRToTTMetalBase;

  void runOnOperation() final {
    RewritePatternSet patterns(&getContext());
    patterns.add<TTIRToTTMetalLayoutRewriter, TTIRToTTMetalKernelRewriter,
                 TTIRToTTMetalReturnRewriter, TTIRToTTMetalDispatchRewriter,
                 TTIRToTTMetalAllocRewriter, TTIRToTTMetalDeallocRewriter>(
        &getContext());
    FrozenRewritePatternSet patternSet(std::move(patterns));
    if (failed(applyPatternsAndFoldGreedily(getOperation(), patternSet))) {
      signalPassFailure();
    }
  }

  void getDependentDialects(mlir::DialectRegistry &registry) const override {
    registry.insert<mlir::tt::ttir::TTIRDialect>();
    registry.insert<mlir::tt::ttmetal::TTMetalDialect>();
    registry.insert<mlir::tt::ttkernel::TTKernelDialect>();
    registry.insert<mlir::arith::ArithDialect>();
  }
};

void createTTIRToTTMetalBackendPipeline(OpPassManager &pm) {
  pm.addPass(mlir::tt::ttir::createTTIRGeneric());
  pm.addPass(mlir::tt::ttir::createTTIRLayout());
  pm.addPass(mlir::tt::ttir::createTTIRGenericRegionOperandsToMemref());
  pm.addPass(mlir::tt::ttir::createTTIRAllocate());
  pm.addPass(createConvertTTIRToTTMetal());
}

} // namespace mlir::tt::ttmetal

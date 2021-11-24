#include <c10/util/variant.h>
#include <torch/csrc/jit/tensorexpr/kernel.h>

#include <ATen/ExpandUtils.h>
#include <ATen/Parallel.h>
#include <ATen/TensorGeometry.h>
#include <c10/core/ScalarTypeToTypeMeta.h>
#include <c10/util/irange.h>
#include <c10/util/string_utils.h>
#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/tensorexpr/analysis.h>
#include <torch/csrc/jit/tensorexpr/graph_opt.h>
#include <torch/csrc/jit/tensorexpr/ir_printer.h>
#include <torch/csrc/jit/tensorexpr/ir_simplifier.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/operators/operators.h>

using namespace torch::jit;
using namespace torch::jit::tensorexpr;

namespace torch {
namespace jit {
namespace tensorexpr {

std::string buildErrorMessage(const std::string& s) {
  static const std::string generic_error_message =
      "This error occured in the fuser. You can turn off the fuser with "
      "torch.jit.enable_fusion(False).";
  if (s.empty()) {
    return generic_error_message;
  }
  if (s.back() == '.') {
    return s + " " + generic_error_message;
  }
  return s + ". " + generic_error_message;
}

static int te_cuda_pointwise_loop_levels = -1;
static int te_cuda_pointwise_block_count = -1;
static int te_cuda_pointwise_block_size = -1;
static bool fallback_allowed = false;
static bool te_generate_block_code = false;
static bool te_must_use_llvm_on_cpu = true;
static bool cat_wo_conditionals = true; // NOLINT
static bool opt_conditionals = false; // NOLINT

bool setFallbackAllowed(bool value) {
  bool old_value = fallback_allowed;
  fallback_allowed = value;
  return old_value;
}

bool fallbackAllowed() {
  static const char* enable_c_str = std::getenv("PYTORCH_TENSOREXPR_FALLBACK");
  if (!enable_c_str) {
    return fallback_allowed;
  }
  if (std::string(enable_c_str) == "0") {
    return false;
  }
  return true;
}

bool fallbackEnforced() {
  static const char* enable_c_str = std::getenv("PYTORCH_TENSOREXPR_FALLBACK");
  if (tensorexpr::getTEGenerateBlockCode()) {
    return false;
  }
  if (!enable_c_str) {
    return fallback_allowed;
  }
  if (std::string(enable_c_str) == "2") {
    return true;
  }
  return false;
}

bool dontUseLLVMFlag() {
  static const char* enable_c_str =
      std::getenv("PYTORCH_TENSOREXPR_DONT_USE_LLVM");
  if (!enable_c_str) {
    return false;
  }
  return std::string(enable_c_str) == "1";
}

int& getTECudaPointwiseLoopLevels() {
  return te_cuda_pointwise_loop_levels;
}

int& getTECudaPointwiseBlockCount() {
  return te_cuda_pointwise_block_count;
}

int& getTECudaPointwiseBlockSize() {
  return te_cuda_pointwise_block_size;
}

// TODO: Remove this global var
// Ideally Block code gen should be decided
// based on device type in tensor.
bool& getTEGenerateBlockCode() {
  return te_generate_block_code;
}

bool& getTEMustUseLLVMOnCPU() {
  return te_must_use_llvm_on_cpu;
}

bool& getCatWoConditionals() {
  return cat_wo_conditionals;
}

bool& getOptConditionals() {
  return opt_conditionals;
}

c10::optional<at::Device> pickDeviceType(
    const at::ArrayRef<torch::jit::Value*>& inputs) {
  c10::optional<at::Device> device = c10::nullopt;
  for (auto const& input : inputs) {
    auto tt = input->type()->cast<TensorType>();
    if (tt && tt->device()) {
      if (device && *device != *tt->device()) {
        return c10::nullopt;
      }
      device = *tt->device();
    }
  }
  return device;
}

c10::optional<at::Device> pickDeviceType(const std::shared_ptr<Graph>& graph) {
  c10::optional<at::Device> device = c10::nullopt;
  for (auto const& node : graph->nodes()) {
    for (auto const& input : node->inputs()) {
      if (auto tt = input->type()->cast<TensorType>()) {
        if (auto inputDevice = tt->device()) {
          TORCH_INTERNAL_ASSERT(
              !device || *device == *inputDevice,
              buildErrorMessage(
                  "Different devices specified for inputs to the fuser."));
          device = inputDevice;
        }
      }
    }
  }
  TORCH_INTERNAL_ASSERT(
      device,
      buildErrorMessage("Could not find device in fuser graph inputs."));
  return device;
}

// If v is a Tensor with concretely-known sizes and dtype, return them, else
// nullopt.
c10::optional<TensorInfo> getTensorInfoJit(torch::jit::Value* v) {
  auto const& it = v->type()->cast<TensorType>();

  c10::ScalarType dtype = c10::ScalarType::Float;

  if (!it) {
    return c10::nullopt;
  }
  if (!it->isComplete()) {
    return c10::nullopt;
  }
  if (it->scalarType()) {
    // TODO: ideally we should be strict here and return nullopt if the dtype is
    // absent in the JIT IR. We're assuming a default Float dtype for now, until
    // dtype propagation is implemented.
    dtype = *it->scalarType();
  }
  auto concrete_sizes = it->sizes().concrete_sizes();
  if (!concrete_sizes) {
    return c10::nullopt;
  }
  return TensorInfo{*concrete_sizes, dtype};
}
std::vector<int64_t> _pair_int(IValue v) {
  if (v.isIntList()) {
    return v.toIntVector();
  } else {
    return {v.toInt(), v.toInt()};
  }
}

static bool isContiguous(const torch::jit::Value* v) {
  auto const& tt = v->type()->cast<TensorType>();
  if (!tt) {
    return false;
  }
  if (!tt->isComplete()) {
    return false;
  }
  auto const& sizes = tt->sizes().concrete_sizes();
  auto const& strides = tt->strides().concrete_sizes();
  if (!sizes || !strides) {
    return false;
  }
  return *strides == TensorType::contiguousStridesOf(*sizes);
}

// The fuser only supports conv2d with very specific properties:
// - Static shapes: 4-d input and filter, 1-d bias.
// - Constant strides/padding/dilation/groups
// - Equal padding and strides, dilation == 1.
// - Depthwise (groups == in_channels == out_channels)
// - 3x3 kernel
bool conv2dIsSupportedJit(const torch::jit::Node* node) {
  auto const& input = getTensorInfoJit(node->input(0));
  auto const& weight = getTensorInfoJit(node->input(1));
  auto const& bias = getTensorInfoJit(node->input(2));
  auto const& stride = toIValue(node->input(3));
  auto const& pad = toIValue(node->input(4));
  auto const& dilation = toIValue(node->input(5));
  auto const& groups = toIValue(node->input(6));

  // Everything should be statically known.
  if (!input || !weight || !bias || !stride || !pad || !dilation || !groups) {
    GRAPH_DEBUG("some params aren't static");
    return false;
  }

  // All inputs should be contiguous so no transposition is required.
  if (!isContiguous(node->input(0)) || !isContiguous(node->input(1)) ||
      !isContiguous(node->input(2))) {
    GRAPH_DEBUG("conv2dIsSupported: some inputs are not contiguous");
    return false;
  }

  return conv2dIsSupported(
      *input,
      *weight,
      *bias,
      _pair_int(*stride),
      _pair_int(*pad),
      _pair_int(*dilation),
      groups->toInt());
}

// The fuser currently only supports matmul of 2D x 2D matrices
bool matmulIsSupported(const torch::jit::Node* node) {
  auto const& input0 = getTensorInfoJit(node->input(0));
  auto const& input1 = getTensorInfoJit(node->input(1));

  // Everything should be statically known.
  if (!input0 || !input1) {
    GRAPH_DEBUG("matmulIsSupported: Input shapes aren't static");
    return false;
  }

  // Proper ndim for tensor inputs.
  if (input0->dims.size() != 2 || input1->dims.size() != 2) {
    GRAPH_DEBUG("matmulIsSupported: Unsupported input sizes");
    return false;
  }

  // Inputs should be contiguous, or the TE will needlessly transpose them.
  if (!isContiguous(node->input(0)) || !isContiguous(node->input(1))) {
    GRAPH_DEBUG("matmulIsSupported: Input shapes are not contiguous");
    return false;
  }

  return true;
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch

static at::ScalarType tensorType(BufPtr b) {
  return static_cast<at::ScalarType>(b->dtype().scalar_type());
}

ExprHandle TensorExprKernel::constant(const torch::jit::Value* v) {
  if (v->node()->kind() == prim::Constant) {
    auto val = toIValue(v).value();
    if (val.isDouble()) {
      return DoubleImm::make(val.toDouble());
    } else if (val.isInt()) {
      return LongImm::make(val.toInt());
    } else if (val.isBool()) {
      return BoolImm::make(val.toBool());
    } else if (val.isNone()) {
      // This is just a placeholder so we don't throw.  None-handling
      // is operator-specific and should be handled properly in
      // the operator-specific lowering code.
      return IntImm::make(0);
    } else {
      throw unsupported_dtype();
    }
  }

  if (!scalars_.count(v)) {
    throw malformed_input("no scalar in Constant");
  }

  return scalars_.at(v);
}

ExprHandle TensorExprKernel::tensorOrConstant(
    const torch::jit::Value* v,
    const std::vector<ExprHandle>& axes) {
  auto ti = bufs_.find(v);
  if (ti != bufs_.end()) {
    return broadcast(BufHandle(ti->second), axes);
  }
  return constant(v);
}

ArgValue TensorExprKernel::toArg(const torch::jit::Value* v) const {
  auto ti = bufs_.find(v);
  if (ti != bufs_.end()) {
    return BufHandle(ti->second);
  }
  if (v->node()->kind() == prim::ListConstruct) {
    std::vector<ArgValue> vec;
    for (auto el : v->node()->inputs()) {
      vec.push_back(toArg(el));
    }
    if (vec.size() == 0) {
      return BufList(); // Return arbitrarily typed vector
    } else if (c10::get_if<BufHandle>(&vec[0])) {
      return convertVecArgValue<BufHandle>(vec);
    } else if (c10::get_if<int64_t>(&vec[0])) {
      return convertVecArgValue<int64_t>(vec);
    }
    throw unsupported_dtype();
  }
  if (v->node()->kind() == prim::Constant) {
    auto val = toIValue(v).value();
    if (val.isDouble()) {
      return val.toDouble();
    } else if (val.isInt()) {
      return val.toInt();
    } else if (val.isBool()) {
      return val.toBool();
    } else if (val.isNone()) {
      // This is just a placeholder so we don't throw.  None-handling
      // is operator-specific and should be handled properly in
      // the operator-specific lowering code.
      return ArgNone();
    } else if (val.isIntList()) {
      return val.toIntVector();
    } else if (val.isDoubleList()) {
      return val.toDoubleVector();
    } else {
      throw unsupported_dtype(val.type()->str());
    }
  }

  if (!scalars_.count(v)) {
    throw malformed_input("no scalar in Constant");
  }
  return scalars_.at(v);
}

ExprHandle TensorExprKernel::getVarForShape(const c10::ShapeSymbol& ss) {
  if (ss.is_static()) {
    return LongImm::make(ss.static_size());
  }
  auto value = ss.value();
  auto it = shapeSymbolToVar_.find(value);
  if (it == shapeSymbolToVar_.end()) {
    VarHandle var("ss" + std::to_string(-value), kLong);
    shapeSymbolToVar_.emplace(value, var);
    return std::move(var);
  }
  return it->second;
}

std::vector<ExprHandle> TensorExprKernel::sizesFromSymbolicShape(
    const c10::SymbolicShape& shape) {
  std::vector<ExprHandle> dims;
  auto maybe_rank = shape.rank();
  TORCH_INTERNAL_ASSERT(maybe_rank);
  auto rank = *maybe_rank;
  for (const auto i : c10::irange(rank)) {
    dims.push_back(getVarForShape(shape[i]));
  }
  return dims;
}

std::vector<ExprHandle> TensorExprKernel::sizesForValue(
    const torch::jit::Value* v) {
  if (known_sizes_.count(v)) {
    return known_sizes_.at(v);
  }

  // If the shape is present in the type info, just extract it from here. No
  // need to infer it.
  if (v->type()->kind() == TypeKind::TensorType) {
    auto tt = v->type()->cast<TensorType>();
    return sizesFromSymbolicShape(tt->symbolic_sizes());
  }

  if (v->type()->isSubtypeOf(*FloatType::get()) ||
      v->type()->isSubtypeOf(*IntType::get())) {
    return {int64_t{1}};
  }
  if (v->type()->isSubtypeOf(*NoneType::get())) {
    return {};
  }
  GRAPH_DEBUG("Unknown sizes for the node: ", *v->node());
  GRAPH_DEBUG("Full fusion group graph:\n", *v->node()->owningGraph());
  std::string msg = std::string("Unhandled node kind (in sizesForValue): ") +
      v->node()->kind().toQualString();
  throw malformed_input(msg);
}

c10::optional<ScalarType> findDtypeForValue(const torch::jit::Value* v) {
  if (v->type()->kind() == TypeKind::TensorType) {
    auto tt = v->type()->cast<TensorType>();
    if (tt->scalarType()) {
      return static_cast<ScalarType>(*tt->scalarType());
    }
  }
  return c10::nullopt;
}

bool constZeroDimTensorAsScalarArg(
    const Value* v,
    std::vector<ArgValue>& args) {
  if (v->node()->kind() != prim::Constant || !v->type()->cast<TensorType>()) {
    return false;
  }

  const auto t = toIValue(v)->toTensor();
  if (t.sizes().size() != 0) {
    return false;
  }

  c10::ScalarType dtype = c10::typeMetaToScalarType(t.dtype());
  switch (dtype) {
    case ScalarType::Float:
      args.emplace_back(t.item().toFloat());
      return true;
    case ScalarType::Long:
      args.emplace_back(t.item().toLong());
      return true;
    default:
      std::stringstream ss;
      ss << "Unsupported tensor dtype:" << dtype
         << " for converting constant 0-dim Tensor to scalar" << std::endl;
      throw unsupported_dtype(ss.str());
  }
}

Tensor TensorExprKernel::computeValue(const torch::jit::Value* v) {
  auto inputs = v->node()->inputs();
  auto op = v->node()->kind();

  if (op == aten::rand_like) {
    hasRandom_ = true;
  }

  auto outputType = findDtypeForValue(v);
  std::vector<ExprHandle> outputShape = sizesForValue(v);

  std::vector<ArgValue> argInputs;
  if (op == prim::ConstantChunk) {
    auto const& n = v->node();
    argInputs.emplace_back(toArg(inputs[0]));
    argInputs.emplace_back(static_cast<int64_t>(v->offset()));
    argInputs.emplace_back(n->i(attr::dim));
    argInputs.emplace_back(n->i(attr::chunks));
  } else if (op == aten::to) {
    argInputs.emplace_back(toArg(inputs[0]));
  } else if (op == aten::quantize_per_tensor) {
    argInputs.emplace_back(toArg(inputs[0]));
    if (!constZeroDimTensorAsScalarArg(inputs[1], argInputs)) {
      argInputs.emplace_back(toArg(inputs[1]));
    }
    if (!constZeroDimTensorAsScalarArg(inputs[2], argInputs)) {
      argInputs.emplace_back(toArg(inputs[2]));
    }
    argInputs.emplace_back(toArg(inputs[3]));
  } else if (op == aten::conv2d) {
    for (auto inp : inputs) {
      argInputs.emplace_back(toArg(inp));
    }
    // handle optional bias
    if (c10::get_if<ArgNone>(&argInputs[2])) {
      Dtype dtype = outputType ? Dtype(*outputType) : kFloat;
      std::vector<ExprHandle> biasShape;
      biasShape.push_back(outputShape[1]);
      auto bias_tensor = at::zeros({outputShape[1].AsNode<LongImm>()->value()});
      unpacked_constant_tensors_.push_back(bias_tensor);
      BufPtr buf = alloc<Buf>(
          "conv2d_bias_opt_" + sanitizeName(v->debugName()),
          ExprHandleVectorToExprVector(biasShape),
          dtype);
      constants_.push_back({buf, bias_tensor.data_ptr()});
      argInputs[2] = BufHandle(buf);
    }
  } else {
    for (auto inp : inputs) {
      argInputs.emplace_back(toArg(inp));
    }
  }

  if (NNCLoweringFunction custom_lowering = getCustomLoweringFor(op)) {
    return custom_lowering(argInputs, outputShape, outputType, device_);
  }
  if (v->node()->maybeSchema()) {
    if (NNCLoweringFunction lowering =
            getStandardLoweringFor(c10::toString(v->node()->schema()))) {
      return lowering(argInputs, outputShape, outputType, device_);
    }
  }
  std::string msg = std::string("Unhandled node kind (in computeValue): ") +
      op.toQualString();
  if (v->node()->maybeSchema()) {
    msg += std::string("\nSchema: ") + c10::toString(v->node()->schema());
  }
  throw malformed_input(msg);
}

// Return the (lower, upper) loop bounds if they are constants, else nullopt.
c10::optional<std::pair<int64_t, int64_t>> loopBounds(ForPtr loop) {
  auto start = IRSimplifier::simplify(loop->start());
  auto stop = IRSimplifier::simplify(loop->stop());
  if (!start->isConstant() || !stop->isConstant()) {
    return c10::nullopt;
  }
  return c10::make_optional(
      std::make_pair(immediateAs<int64_t>(start), immediateAs<int64_t>(stop)));
}

// True if all the loops in this vector have equal bounds.
bool loopBoundsAllEqual(const std::vector<ForPtr>& loops) {
  auto bounds = loopBounds(loops[0]);
  if (!bounds) {
    return false;
  }
  for (auto const& loop : loops) {
    auto next = loopBounds(loop);
    if (!next) {
      return false;
    }
    if (bounds->first != next->first || bounds->second != next->second) {
      return false;
    }
  }
  return true;
}

// Recursively fuse all the loops with matching bounds in `st`.  Stops fusing
// at any level containing non-loops or non-matching bounds.  The restriction
// on matching bounds exists to avoid inserting conditionals on the loop
// indices where none would be needed, which would significantly complicate
// vectorization.
void fuseAllLoops(StmtPtr st) {
  if (auto block = to<tensorexpr::Block>(st)) {
    std::vector<ForPtr> loopsToFuse;
    for (auto stmt : *block) {
      auto loop = to<For>(stmt);
      if (!loop) {
        // Block contains something that's not a loop.  Quit.
        return;
      }
      loopsToFuse.push_back(loop);
    }
    if (loopsToFuse.empty()) {
      return;
    }
    if (!loopBoundsAllEqual(loopsToFuse)) {
      return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    ForPtr fusedLoop;
    if (!LoopNest::fuseLoops(loopsToFuse, &fusedLoop)) {
      return;
    }
    fuseAllLoops(fusedLoop->body());
  }
}

// Compute the trip count of a loop if it is a constant.
c10::optional<int64_t> tripCount(ForPtr loop) {
  auto tc = IRSimplifier::simplify(
      cast<int64_t>(ExprHandle(loop->stop()) - ExprHandle(loop->start())));
  if (auto val = to<LongImm>(tc.node())) {
    return val->value();
  }
  return c10::nullopt;
}

// Prune innermost loops until iterations satisfies a minimum grain size.
static void pruneByGrainSize(std::vector<ForPtr>& loops) {
  constexpr int64_t minGrainSize = 32768;
  int64_t grainSize = 1;
  for (int64_t i = loops.size(); i > 0; i--) {
    auto tc = tripCount(loops[i - 1]);
    if (!tc) {
      break;
    }
    grainSize *= *tc;
    if (grainSize < minGrainSize) {
      loops.pop_back();
    }
  }
}

// Retain enough outermost loops to fill the number of threads.
static void pruneByThreadCount(std::vector<ForPtr>& loops) {
  int64_t trips = 1;
  auto threads = at::get_num_threads();
  auto it = loops.begin();
  for (; it != loops.end(); it++) {
    if (trips >= threads) {
      break;
    }
    auto tc = tripCount(*it);
    if (!tc) {
      break;
    }
    trips *= *tc;
  }
  loops.erase(it, loops.end());
}

// Flatten and parallelize outer loops, subject to a minimum number of elements
// in the inner loop, and a maximum level of thread-level parallelism in the
// outer loops.
template <typename Bufs>
static void parallelizeOuterLoops(LoopNest& l, Bufs&& bufs) {
  for (auto const& buf : bufs) {
    auto loops = l.getLoopStmtsFor(buf);
    pruneByGrainSize(loops);
    pruneByThreadCount(loops);

    // There are no loops to parallelize; give up.
    if (loops.size() == 0) {
      continue;
    }
    // The loop nest contains a reduction; give up.
    auto reductions = NodeFinder<ReduceOp>::find(loops[0]);
    if (reductions.size() > 0) {
      continue;
    }
    // The loop nest has loop carried dependences; give up.
    if (LoopNest::hasLoopCarriedDependence(loops[0])) {
      continue;
    }
    // Try to flatten the outer loops and parallelize them if successful.
    ForPtr flattened = nullptr;
    if (loops.size() == 1) {
      flattened = loops[0];
    } else {
      LoopNest::flatten(loops, &flattened);
    }
    if (flattened) {
      flattened->set_parallel();
    }
  }
}

StmtPtr TensorExprKernel::transformLoops(BackendType backendType, StmtPtr st) {
  torch::jit::tensorexpr::LoopNest l(st, bufOutputs_);
  LoopNest::sanitizeNames(l.root_stmt());
  GRAPH_DEBUG("Original Stmt:\n", std::to_string(l.root_stmt()), "\n");

  bool hasReduction = NodeFinder<ReduceOp>::find(l.root_stmt()).size() != 0;

  // For Block codegen we create a map of tensor dims before
  // inlining. Like GPU codegen we need to inline. But the order
  // where this analysis is run matters.
  auto block_analysis = std::make_unique<CreateBufferMap>();
  if (backendType == kBlockCodeGen) {
    // Run Block analysis to get multi dim buffer info
    auto root_stmt = l.root_stmt();
    root_stmt->accept(block_analysis.get());
  }
  l.simplify();
  GRAPH_DEBUG("after simplify", *l.root_stmt());

  // Inlining output & intermediate buffers can duplicate computation.
  // Duplicating work can slow down the program if it's not ameliorated in some
  // way, but we've empirically found that:
  // - On CPU, LLVM's CSE does a good job as long as you horizontally fuse
  //   output loops.
  // - On GPU, there's enough compute to hide the extra work, and inlining
  //   avoids synchronizing between kernels.
  l.inlineIntermediateBufs(/*allow_duplicated_work=*/true);
  GRAPH_DEBUG("after inline", *l.root_stmt());

  // Optimizing conditionals needs to be performed after inlining because
  // inlining wouldn't work once the loops are split. Also, it has to be
  // performed before loop fusion because loop fusion introduces cases where
  // multiple conditionals are in the same loop and this optimization does not
  // handle such cases yet.
  if (getOptConditionals()) {
    l.optimizeConditionals();
    GRAPH_DEBUG("after optimizing conditionals: ", *l.root_stmt());
  }

  // Fuse loops "horizontally".  This pass allows us to combine loops that
  // write to different output buffers, as long as they have the same bounds.
  if (backendType == kLLVMCodeGen) {
    fuseAllLoops(l.root_stmt());
    GRAPH_DEBUG("after fuse", *l.root_stmt());
    parallelizeOuterLoops(l, bufOutputs_);
    GRAPH_DEBUG("after parallelize", *l.root_stmt());
  }

  if (backendType == kCudaCodeGen) {
    for (auto buf : bufOutputs_) {
      std::vector<ForPtr> loops = l.getLoopStmtsFor(buf);
      if (loops.empty()) {
        // This happens when Buf is 0-dim
        continue;
      }
      ForPtr flattened = nullptr;
      LoopNest::flatten(loops, &flattened);
      assert(flattened);

      int loopLevels = getTECudaPointwiseLoopLevels();
      const int kDefaultLoopLevels = 2;
      loopLevels = (loopLevels > 0) ? loopLevels : kDefaultLoopLevels;
      int blockCount = getTECudaPointwiseBlockCount();
      int blockSize = getTECudaPointwiseBlockSize();

      if (loopLevels == 2) {
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        ForPtr inner;
        const int kDefaultBlockSize = 512;
        if (blockSize < 0) {
          blockSize = kDefaultBlockSize;
        }
        LoopNest::splitWithMask(flattened, blockSize, &inner);
        flattened->set_gpu_block_index(0);
        inner->set_gpu_thread_index(0);
      } else if (loopLevels == 3) {
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        ForPtr inner;
        // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
        ForPtr inner1;
        // TODO: change the number of microprocessors
        const int kDefaultBlockCount = 1280;
        const int kDefaultBlockSize = 256;
        blockCount = (blockCount > 0) ? blockCount : kDefaultBlockCount;
        blockSize = (blockSize > 0) ? blockSize : kDefaultBlockSize;
        LoopNest::splitWithMask(flattened, blockCount * blockSize, &inner);
        LoopNest::splitWithMask(inner, blockSize, &inner1);
        inner->set_gpu_block_index(0);
        inner1->set_gpu_thread_index(0);
      } else {
        throw std::runtime_error(
            "Invalid loop-level: " + c10::to_string(loopLevels));
      }
    }
  }

  if (backendType == kBlockCodeGen) {
    for (auto buf : bufOutputs_) {
      const int default_fp16_blocksize = 16;
      const int default_uint8_blocksize = 32;
      int blockSize = default_fp16_blocksize;
      // We only handle looplevels == 2 for now
      if (buf->dtype().scalar_type() == ScalarType::Byte) {
        blockSize = default_uint8_blocksize;
      }
      std::vector<ForPtr> loops = l.getLoopStmtsFor(buf);
      TORCH_INTERNAL_ASSERT(
          !loops.empty(),
          buildErrorMessage(
              "No loops found for the buffer " + buf->name_hint() +
              " in the fuser."));
      ForPtr flattened = nullptr;
      LoopNest::flatten(loops, &flattened);
      assert(flattened);

      ForPtr inner = nullptr;
      LoopNest::splitWithMask(flattened, blockSize, &inner);
      flattened->set_gpu_block_index(0);
      inner->set_gpu_thread_index(0);
      flattened->set_buffer_map(block_analysis->getBufferMap());
    }
  }

  if (pre_alloc_) {
    auto interm_bufs = l.getIntermediateBufs();
    interm_bufs = preAllocIntermediateBufs(interm_bufs);
    l.prepareForCodegen(interm_bufs);
  } else {
    l.prepareForCodegen();
  }

  GRAPH_DEBUG("after prepareForCodegen", *l.root_stmt());
  l.simplify();
  GRAPH_DEBUG("after simplification", *l.root_stmt());

  if (backendType == kLLVMCodeGen && !hasReduction) {
    l.vectorizeInnerLoops();
    GRAPH_DEBUG("after vectorization", *l.root_stmt());
  }

  StmtPtr stmt = l.root_stmt();
  // Arithmetic Simplification.
  stmt = IRSimplifier::simplify(stmt);
  GRAPH_DEBUG("Final Stmt:\n", std::to_string(stmt), "\n");
  return stmt;
}

std::string TensorExprKernel::getCodeGenName(BackendType backendType) {
  switch (backendType) {
    case kCudaCodeGen:
      return "cuda_codegen";
    case kLLVMCodeGen:
      return "llvm_codegen";
    case kSimpleIREval:
      return "simple_ir_eval";
    case kBlockCodeGen:
      return "block_codegen";
    default:
      throw std::runtime_error(
          "invalid backend type: " +
          c10::to_string(static_cast<int>(backendType)));
  }
}

template <typename T>
static bool isValidPrimProperty(const c10::optional<T>& a, T b) {
  return !a.has_value() || *a == b;
}

TensorExprKernel::BackendType TensorExprKernel::inferBackendTypeFromDevice(
    at::Device device) {
  BackendType backendType = BackendType::kUninitialized;
  if (device.type() == at::kCUDA) {
    backendType = kCudaCodeGen;
  } else if (device.type() == at::kCPU && getTEGenerateBlockCode()) {
    backendType = kBlockCodeGen;
  } else if (device.type() == at::kCPU) {
#ifdef TORCH_ENABLE_LLVM
    backendType = dontUseLLVMFlag() ? kSimpleIREval : kLLVMCodeGen;
#else
    backendType = kSimpleIREval;
#endif
    if (getTEMustUseLLVMOnCPU() && backendType == kSimpleIREval) {
      throw std::runtime_error("LLVM Backend not found");
    }
  } else {
    throw std::runtime_error("Invalid device type");
  }
  return backendType;
}

// we use the debug names in printing cuda code, they need to be removed
// of characters that can't be used in a variable identifier
void TensorExprKernel::genInputDebugNames() {
  std::unordered_map<std::string, const torch::jit::Value*> name_to_value;
  std::unordered_set<std::string> name_set;
  std::unordered_map<const torch::jit::Value*, std::string> value_to_name;
  for (const torch::jit::Value* input : graph_->inputs()) {
    std::string sanitized_name = sanitizeName(input->debugName());
    // we could get fancier here, but name conflict is extremely unlikely
    while (name_set.count(sanitized_name)) {
      sanitized_name.append("_");
    }
    value_to_name[input] = sanitized_name;
    name_set.insert(sanitized_name);
  }
  input_name_map_ = std::move(value_to_name);
}

template <typename T>
static std::vector<ExprHandle> toExprHandles(const std::vector<T>& sizes) {
  std::vector<ExprHandle> dims;
  dims.reserve(sizes.size());
  for (auto const& size : sizes) {
    dims.emplace_back(size);
  }
  return dims;
}

std::vector<ExprHandle>& TensorExprKernel::getStridesForValue(
    const torch::jit::Value* v) {
  auto it = inputToStrides_.find(v);
  if (it != inputToStrides_.end()) {
    return it->second;
  }
  std::vector<ExprHandle> strides;
  auto tt = v->type()->cast<TensorType>();
  auto rank = tt->symbolic_sizes().rank();
  auto concrete_strides = tt->strides().concrete_sizes();
  TORCH_INTERNAL_ASSERT(concrete_strides, "Only concrete strides are handled");
  for (auto cs : *concrete_strides) {
    strides.push_back(LongImm::make(cs));
  }
  inputToStrides_.emplace(v, std::move(strides));
  return inputToStrides_[v];
}

BufHandle TensorExprKernel::bindSymbolicShapeInput(
    const torch::jit::Value* input,
    const std::string& name) {
  auto tt = input->type()->expect<TensorType>();
  auto const& symbolicShape = tt->symbolic_sizes();
  auto rank = symbolicShape.rank();
  if (!rank) {
    throw std::runtime_error("Symbolic shapes must have static ranks.");
  }
  // We only handle symbolic shape input tensors that are contiguous.
  // TODO: Handle strided tensors with symbolic shapes.
  std::vector<ExprHandle> inputTensorDims;
  for (const auto i : c10::irange(*rank)) {
    inputTensorDims.emplace_back(getVarForShape(symbolicShape[i]));
  }
  BufHandle inBuffer(
      name,
      inputTensorDims,
      ToDtype(static_cast<ScalarType>(*tt->scalarType())));
  return inBuffer;
}

Tensor TensorExprKernel::bindInput(const torch::jit::Value* input) {
  auto const& t = input->type();
  Tensor result(nullptr, nullptr);
  switch (t->kind()) {
    case TypeKind::TensorType: {
      auto tt = input->type()->cast<TensorType>();
      if (!input->isCompleteTensor()) {
        auto bufHandle =
            bindSymbolicShapeInput(input, "t" + input_name_map_[input]);
        bufs_.emplace(input, bufHandle.node());
        bufferArgs_.emplace_back(bufHandle);
        break;
      }
      if (isContiguous(input)) {
        BufHandle inBuffer(
            "t" + input_name_map_[input],
            toExprHandles(*tt->sizes().concrete_sizes()),
            ToDtype(static_cast<ScalarType>(*tt->scalarType())));
        bufs_.emplace(input, inBuffer.node());
        bufferArgs_.emplace_back(inBuffer);
        break;
      }
      BufHandle inBuffer(
          "t" + input_name_map_[input],
          {0},
          ToDtype(static_cast<ScalarType>(*tt->scalarType())));
      std::vector<DimArg> inputTensorDims;
      for (size_t i = 0; i < *tt->sizes().size(); i++) {
        auto const size = *tt->sizes()[i];
        inputTensorDims.emplace_back(DimArg(size, "i" + c10::to_string(i)));
      }
      auto const strides = tt->strides();
      result = Compute(
          "input" + c10::to_string(bufs_.size() + 1),
          inputTensorDims,
          [&](const std::vector<VarHandle>& axes) {
            ExprHandle idx = 0;
            for (size_t i = 0; i < axes.size(); i++) {
              idx = idx + axes[i] * *strides[i];
            }
            return inBuffer.load(idx);
          });
      bufs_.emplace(input, result.buf());
      bufferArgs_.emplace_back(inBuffer);
      break;
    }
    case TypeKind::FloatType: {
      VarHandle v("v" + input_name_map_[input], kDouble);
      bufferArgs_.emplace_back(v);
      scalars_.emplace(input, v);
      break;
    }
    case TypeKind::BoolType: {
      VarHandle v("v" + input_name_map_[input], kBool);
      bufferArgs_.emplace_back(v);
      scalars_.emplace(input, v);
      break;
    }
    case TypeKind::IntType: {
      VarHandle v("v" + input_name_map_[input], kLong);
      bufferArgs_.emplace_back(v);
      scalars_.emplace(input, v);
      break;
    }
    default: {
      throw unsupported_dtype(t->repr_str());
      break;
    }
  }
  return result;
}

NNCLoweringFunction TensorExprKernel::getCustomLoweringFor(
    c10::Symbol op) const {
  if (custom_lowerings_.count(op))
    return custom_lowerings_.at(op);
  return nullptr;
}

template <typename T>
std::vector<size_t> reverse_sort_indices(const std::vector<T>& v) {
  // initialize original index locations
  std::vector<size_t> idx(v.size());
  iota(idx.begin(), idx.end(), 0);

  std::sort(idx.begin(), idx.end(), [&v](size_t i1, size_t i2) {
    return v[i1] > v[i2];
  });
  return idx;
}

bool denseAndNonOverlapping(
    at::ArrayRef<int64_t> sizes,
    at::ArrayRef<int64_t> strides) {
  return (strides == at::infer_dense_strides(sizes, strides));
}

Tensor TensorExprKernel::convertOutputToCorrectStrides(torch::jit::Value* v) {
  const TensorTypePtr& tt = v->type()->expect<TensorType>();
  TORCH_INTERNAL_ASSERT(
      bufs_.count(v),
      buildErrorMessage(
          "Ouput tensor has no corresponding bufs in the fuser."));
  BufPtr buf = bufs_.at(v);

  // No shape info is present in the graph
  if (!tt->sizes().concrete_sizes()) {
    std::string msg =
        std::string("Shapes for output '%") + v->debugName() + "' are unknown";
    throw malformed_input(msg);
  }

  TORCH_INTERNAL_ASSERT(
      tt->sizes().concrete_sizes(),
      buildErrorMessage("Output shapes are unknown."));
  auto sizes = *tt->sizes().concrete_sizes();
  std::vector<int64_t> default_strides = TensorType::contiguousStridesOf(sizes);
  if (!tt->strides().concrete_sizes()) {
    return Tensor(buf, nullptr);
  }
  TORCH_INTERNAL_ASSERT(
      tt->strides().concrete_sizes(),
      buildErrorMessage("Output strides are unknown."));
  const std::vector<int64_t> strides = *tt->strides().concrete_sizes();
  // All Tensors in NNC are layed out in default, contiguous layout.
  // If the output is also default contiguous we don't need to do anything
  if (strides == default_strides) {
    return Tensor(buf, nullptr);
  }
  // If the tensor is not dense or overlaps, we have
  // no way of matching the profiled striding
  if (!denseAndNonOverlapping(sizes, strides)) {
    return Tensor(buf, nullptr);
  }

  auto dims = c10::fmap<DimArg>(sizesForValue(v));
  // We need to convert the output tensor so that its values are layed
  // so that when viewed from the output strides the values are correct.
  // A contiguous Tensor of size(2, 3) with values 0-5 is layed out as:
  // [0] [1] [2] [3] [4] [5]
  // The same valued tensor with strides (2, 1) would be layed out like
  // [0] [3] [1] [4] [2] [5]
  // When we are doing the re-ordering of values into the output tensor,
  // we are iterating per-element of the input, and we are fixed
  // in indexing in to the output tensor at [i, j] = val
  // `val` we want here is equal to the indices for the output
  // tensor that would have given the same position as the output
  // The position is equal to the sum of stride[i] * index[i],
  // and we can can calculate the equivalent indices in the
  // output tensor strides by iteratively computing the index of
  // the biggest stride:
  // absolute = ...
  // for stride in strides_from_largest_to_smallest:
  //     cur_idx = absolute // stride
  //     absolute = absolute % stride

  auto zero = LongImm::make(0);
  return Compute(
      "output_1", dims, [&](const std::vector<VarHandle>& axes_input) {
        std::vector<ExprHandle> axes(axes_input.begin(), axes_input.end());
        auto absolute_position = ExprHandle(immLike(axes[0], 0));
        for (size_t i = 0; i < axes.size(); ++i) {
          absolute_position = absolute_position +
              (ExprHandle(immLike(axes[i], default_strides[i])) * axes[i]);
        }
        std::vector<size_t> sorted_stride_indices =
            reverse_sort_indices(strides);
        std::vector<ExprHandle> new_axes(sorted_stride_indices.size());
        for (size_t stride_index : sorted_stride_indices) {
          auto size = sizes[stride_index];
          auto index = zero;
          if (size != 1) {
            auto stride = strides[stride_index];
            index = absolute_position /
                ExprHandle(immLike(absolute_position, stride));
            absolute_position = absolute_position %
                ExprHandle(immLike(absolute_position, stride));
          }
          new_axes[stride_index] = index;
        }
        return BufHandle(buf).load(new_axes);
      });
}

void TensorExprKernel::bindConstant(const torch::jit::Value* v) {
  auto val = toIValue(v).value();
  if (torch::isCustomClass(val)) {
    auto name_hint = "const_" + sanitizeName(v->debugName());
    auto dtype = Dtype(ScalarType::Float);
    std::vector<ExprPtr> dims;
    BufPtr buf = alloc<Buf>(name_hint, dims, dtype);
    auto dataPtr = val.toObjectRef().getSlot(0).toCapsule().get();
    // NOLINTNEXTLINE
    constants_.push_back({buf, nullptr, const_cast<Node*>(v->node())});
    bufs_[v] = buf;
    return;
  }
  if (!v->type()->cast<TensorType>()) {
    // Only Tensor constants need to be bound, scalar constants will be turned
    // into immediates in TE IR
    return;
  }
  auto const_tensor = toIValue(v)->toTensor();
  auto scalar_type = c10::typeMetaToScalarType(const_tensor.options().dtype());
  const auto& tt = v->type()->expect<TensorType>();
  auto sizes = const_tensor.sizes();
  std::vector<ExprHandle> te_sizes;
  te_sizes.reserve(sizes.size());
  for (auto s : sizes) {
    te_sizes.push_back(s);
  }
  BufPtr buf = alloc<Buf>(
      "const_" + sanitizeName(v->debugName()),
      ExprHandleVectorToExprVector(te_sizes),
      ToDtype(scalar_type));

  if (!const_tensor.is_contiguous()) {
    const_tensor = const_tensor.clone().contiguous();
    unpacked_constant_tensors_.push_back(const_tensor);
  }

  constants_.push_back({buf, const_tensor.data_ptr()});
  bufs_[v] = buf;
}

std::vector<BufPtr> TensorExprKernel::preAllocIntermediateBufs(
    const std::vector<BufPtr>& interm_bufs) {
  std::vector<BufPtr> remaining_interm_bufs;
  std::vector<std::pair<BufPtr, void*>> allocated_bufs;
  for (auto buf : interm_bufs) {
    // Check if buf shape is static and compute its size if static.
    bool is_static = true;
    size_t size =
        elementSize(buf->dtype().scalar_type()) * buf->dtype().lanes();
    for (auto& d : buf->dims()) {
      if (!d->isConstant()) {
        is_static = false;
        break;
      }
      size = size * (*intValue(d));
    }
    // Only allocate memory for static bufs.
    if (!is_static) {
      remaining_interm_bufs.push_back(buf);
      continue;
    }
    auto bp = (void*)malloc(size);
    if (!bp) {
      remaining_interm_bufs.push_back(buf);
      continue;
    }
    constants_.push_back({buf, bp});
  }
  return remaining_interm_bufs;
}

BlockPtr TensorExprKernel::bindAllInputs() {
  std::vector<CodeGen::BufferArg> symbolic_shape_args;
  auto symbolic_shape_inputs_start_pos =
      nInputs_ - symbolic_shape_inputs_.size();
  if (has_symbolic_shapes_) {
    // The graph is supposed to have input params that represent the symbolic
    // dims at the end of the list of inputs. The number of such symbolic input
    // params is defined by the size of the `symbolic_shape_inputs_` vector.
    //
    // TODO: Check if the tensors with symbolic shapes are contiguous.
    TORCH_CHECK(
        nInputs_ > symbolic_shape_inputs_.size(),
        "Symbolic dims not provided as inputs to the graph");

    // First, process the symbolic input params and create a new variable for
    // each of them.
    // NOTE: This has to be done before processing the tensor inputs, because
    // their symbolic sizes needs to be associated with these variables we
    // create for the symbolic input params.
    symbolic_shape_args.reserve(symbolic_shape_inputs_.size());
    for (size_t i = symbolic_shape_inputs_start_pos; i < nInputs_; ++i) {
      auto input = graph_->inputs()[i];
      if (input->type()->kind() != TypeKind::IntType) {
        throw std::runtime_error(
            "Expected integer type input to graph for symbolic dims.");
      }
      VarHandle v("v" + input_name_map_[input], kLong);
      symbolic_shape_args.emplace_back(v);
      scalars_.emplace(input, v);
      shapeSymbolInputPos_[scalars_[input].node()] = i;
    }
    // For every shape symbol, store a map to the corresponding var.
    for (size_t i = 0; i < symbolic_shape_inputs_.size(); ++i) {
      shapeSymbolToVar_[symbolic_shape_inputs_[i]] =
          scalars_[graph_->inputs()[symbolic_shape_inputs_start_pos + i]];
    }
  }

  // Block to collect the Stmts corresponding to all tensors.
  auto block = alloc<Block>(std::vector<StmtPtr>({}));

  // Process the inputs before the symbolic input params.
  for (const auto i : c10::irange(symbolic_shape_inputs_start_pos)) {
    auto input = graph_->inputs()[i];
    Tensor t = bindInput(input);
    if (t.stmt()) {
      block->append_stmt(t.stmt());
    }
  }
  // Now, add all the variables corresponding to the symbolic input params.
  bufferArgs_.insert(
      bufferArgs_.end(),
      symbolic_shape_args.begin(),
      symbolic_shape_args.end());
  return block;
}

void TensorExprKernel::compile() {
  graph_ = torch::jit::tensorexpr::removeGraphOutput(graph_, 1);
  graph_ = torch::jit::tensorexpr::replaceListOutputWithTuple(graph_);
  GRAPH_DUMP("TensorExprKernel graph:", graph_);

  device_ = *pickDeviceType(graph_);
  OptimizeCat(graph_);

  has_symbolic_shapes_ = !symbolic_shape_inputs_.empty();
  nInputs_ = graph_->inputs().size();
  genInputDebugNames();

  // Bind inputs to buffers.
  auto block = bindAllInputs();

  // Bind nodes to tensor compute expressions.
  for (auto const& n : graph_->nodes()) {
    if (n->kind() == prim::ListConstruct) {
      continue;
    } else if (n->kind() == prim::Constant) {
      bindConstant(n->output());
      continue;
    } else {
      for (auto const& output : n->outputs()) {
        if (output->hasUses()) {
          Tensor t = computeValue(output);
          bufs_.emplace(output, t.buf());
          block->append_stmt(t.stmt());
        }
      }
    }
    if (hasRandom_ && hasBroadcast_) {
      throw std::runtime_error(
          "Cannot support broadcast and random within one kernel");
    }
  }

  // Move output operands from `bufs_` to `bufOutputs_`
  for (auto& output : graph_->outputs()) {
    if (!bufs_.count(output)) {
      throw malformed_input("cannot find output Tensor");
    }
    const auto& tt = output->type()->expect<TensorType>();
    if (has_symbolic_shapes_) {
      // We only support contiguous tensors with symbolic shapes at this time.
      auto sizes = sizesFromSymbolicShape(tt->symbolic_sizes());
      tensorOutputSymbolicSizes_.push_back(sizes);
    } else {
      // The "strided" tensor will be incorrect if used in NNC,
      // since NNC views it as contiguous. Only convert it to the right
      // strides at the end of the kernel (if already contiguous it's a no-op)
      Tensor properly_strided_output = convertOutputToCorrectStrides(output);
      if (properly_strided_output.stmt()) {
        block->append_stmt(properly_strided_output.stmt());
      }
      // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
      bufs_[output] = properly_strided_output.buf();
      auto sizes = *tt->sizes().concrete_sizes();
      tensorOutputSizes_.push_back(sizes);
      auto strides = tt->strides().concrete_sizes();

      // If the tensor is not dense or overlaps, we have
      // no way of matching the profiled striding
      if (strides && denseAndNonOverlapping(sizes, *strides)) {
        tensorOutputStrides_.push_back(*strides);
      } else {
        tensorOutputStrides_.push_back(TensorType::contiguousStridesOf(sizes));
      }
    }

    bufOutputs_.insert(bufs_.at(output));
    bufferArgs_.emplace_back(BufHandle(bufs_.at(output)));
    tensorOutputTensorOptions_.emplace_back(
        c10::TensorOptions(tensorType(bufs_.at(output))).device(device_));
    bufs_.erase(output);
  }

  BackendType backendType = inferBackendTypeFromDevice(device_);
  stmt_ = transformLoops(backendType, block);

  for (auto c : constants_) {
    bufferArgs_.emplace_back(BufHandle(c.buf));
  }

  if (has_symbolic_shapes_) {
    tensorOutputSizes_.resize(bufOutputs_.size());
    tensorOutputStrides_.resize(bufOutputs_.size());
  }

  // Generate code.
  codegen_ = CreateCodeGen(
      getCodeGenName(backendType),
      stmt_,
      bufferArgs_,
      device_,
      kernel_func_name_);
}

void TensorExprKernel::recompile() {
  codegen_ = CreateCodeGen(
      "llvm_codegen", stmt_, bufferArgs_, device_, kernel_func_name_);
}

TensorExprKernel::TensorExprKernel(
    const std::shared_ptr<Graph>& subgraph,
    const std::string& kernel_func_name,
    std::unordered_map<c10::Symbol, NNCLoweringFunction> custom_lowerings,
    std::vector<int64_t> symbolic_shape_inputs,
    bool pre_alloc /*= false*/)
    : graph_(subgraph),
      code_(subgraph, ""),
      symbolic_shape_inputs_(std::move(symbolic_shape_inputs)),
      custom_lowerings_(std::move(custom_lowerings)),
      pre_alloc_(pre_alloc),
      kernel_func_name_(kernel_func_name) {
  allow_fallback_ = fallbackAllowed();
  if (!allow_fallback_) {
    compile();
    return;
  }

  use_fallback_ = fallbackEnforced();
  if (use_fallback_) {
    return;
  }

  try {
    compile();
  } catch (...) {
    use_fallback_ = true;
  }
}

void TensorExprKernel::run(Stack& stack) {
  if (!use_fallback_ && !allow_fallback_) {
    runKernel(stack);
  } else if (!use_fallback_ && allow_fallback_) {
    try {
      runKernel(stack);
    } catch (...) {
      fallback(stack);
    }
  } else {
    fallback(stack);
  }
}

std::vector<CodeGen::CallArg> TensorExprKernel::prepareRunArgs(
    const at::ArrayRef<IValue>& inputs,
    std::vector<at::Tensor>& outputs) {
  // TODO: preallocate `runArgs` during compilation and fill in values where
  // possible (e.g. for constant tensors)
  std::vector<CodeGen::CallArg> runArgs;
  runArgs.reserve(inputs.size() + bufOutputs_.size());

  for (auto& input : inputs) {
    if (input.isInt()) {
      runArgs.emplace_back(input.toInt());
    } else if (input.isDouble()) {
      runArgs.emplace_back(input.toDouble());
    } else if (input.isTensor()) {
      runArgs.emplace_back(input.toTensor().data_ptr());
    }
  }

  if (has_symbolic_shapes_) {
    // If there are symbolic shapes, then the output tensor size wouldn't have
    // been computed at compile time. That has to be done here by using the
    // symbolic shape input params passed in to this call.
    TORCH_INTERNAL_ASSERT(
        tensorOutputSymbolicSizes_.size() == bufOutputs_.size());
    TORCH_INTERNAL_ASSERT(tensorOutputSizes_.size() == bufOutputs_.size());
    TORCH_INTERNAL_ASSERT(tensorOutputStrides_.size() == bufOutputs_.size());
    for (size_t i = 0, e = bufOutputs_.size(); i < e; ++i) {
      tensorOutputSizes_[i].clear();
      for (auto t : tensorOutputSymbolicSizes_[i]) {
        if (t.AsNode<LongImm>()) {
          tensorOutputSizes_[i].emplace_back(immediateAs<int64_t>(t.node()));
        } else {
          auto input_pos = shapeSymbolInputPos_.at(t.node());
          TORCH_INTERNAL_ASSERT(input_pos < inputs.size());
          TORCH_INTERNAL_ASSERT(inputs[input_pos].isInt());
          tensorOutputSizes_[i].emplace_back(inputs[input_pos].toInt());
        }
      }
      tensorOutputStrides_[i] =
          TensorType::contiguousStridesOf(tensorOutputSizes_[i]);
    }
  }
  for (size_t i = 0, e = bufOutputs_.size(); i < e; ++i) {
    auto const& opts = tensorOutputTensorOptions_[i];
    outputs.emplace_back(codegen_->empty_strided(
        tensorOutputSizes_[i],
        tensorOutputStrides_[i],
        opts.dtype,
        opts.layout,
        opts.device,
        opts.pinned_memory));
    runArgs.emplace_back(outputs.back().data_ptr());
  }

  for (auto c : constants_) {
    runArgs.emplace_back(c.ptr);
  }

  return runArgs;
}

StmtPtr TensorExprKernel::getCodeGenStmt() {
  return codegen_->stmt();
}

void TensorExprKernel::runKernel(Stack& stack) {
  // Set up arguments (inputs, then outputs) for kernel call.
  auto inputs = last(stack, nInputs_);
  std::vector<at::Tensor> outputs;

  std::vector<CodeGen::CallArg> runArgs = prepareRunArgs(inputs, outputs);

  // Call the kernel.
  codegen_->call(runArgs);

  // Update the stack.
  drop(stack, nInputs_);
  for (auto& o : outputs) {
    push_one(stack, std::move(o));
  }
}

void TensorExprKernel::runFast(
    const std::vector<void*>& inputs,
    const std::vector<void*>& outputs) {
  std::vector<void*> args(inputs);
  args.reserve(inputs.size() + outputs.size() + constants_.size());
  args.insert(args.end(), outputs.begin(), outputs.end());

  // TODO: we can consider preallocating and pre-filling the args vector.
  for (auto c : constants_) {
    args.push_back(c.ptr);
  }

  // Call the kernel.
  codegen_->call_raw(args);
}

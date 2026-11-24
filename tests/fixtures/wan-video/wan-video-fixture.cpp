#include "wan-video-fixture.hpp"

#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-attention-context.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-feed-forward.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-patch-embedding.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-vae.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/wan/wan-video-block.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestCatalog.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-step-template-json.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <cstring>
#include <fstream>

namespace ndnsf::di::wan::fixture {

onnx::TensorProto
floatTensor(std::initializer_list<int> dims, std::initializer_list<float> data)
{
  onnx::TensorProto result;
  result.set_data_type(onnx::TensorProto::FLOAT);
  for (const auto size : dims) result.add_dims(size);
  for (const auto value : data) result.add_float_data(value);
  return result;
}

Umt5Source
makeUmt5Source()
{
  auto table = std::make_shared<EmbeddingTable>();
  table->bytes.resize(sizeof(UMT5_WORDS));
  std::memcpy(table->bytes.data(), UMT5_WORDS.data(), sizeof(UMT5_WORDS));
  auto embedding = lowerUmt5Embedding(3, 3, 2, table);
  const auto zero = floatTensor({2, 2}, {0, 0, 0, 0});
  const auto identity = floatTensor({2, 2}, {1, 0, 0, 1});
  const auto position = floatTensor({4, 2}, {0, 0, 0, 0, 0, 0, 0, 0});
  const auto scaled = floatTensor({2, 2}, {0.25F, 0, 0, 0.5F});
  const auto layer = [&] (float first, float second) {
    return Umt5EncoderLayer{
      lowerUmt5Rms(3, floatTensor({2}, {first, second}), UMT5_EPSILON),
      lowerUmt5Attention(3, {zero, zero, identity, identity, position}, 36),
      lowerUmt5Rms(3, floatTensor({2}, {second, first}), UMT5_EPSILON),
      lowerUmt5FeedForward(3, identity, identity, scaled)};
  };
  auto first = layer(1, 2);
  auto second = layer(0.5F, 0.75F);
  auto final = lowerUmt5Rms(3, floatTensor({2}, {1.5F, 2.25F}), UMT5_EPSILON);
  return {std::move(table), std::move(embedding), {std::move(first), std::move(second)},
          std::move(final)};
}

onnx::ModelProto
makeLinearSource(int tokens, int inWidth, int outWidth,
                 const std::vector<std::pair<int, int>>& nonzero, bool normalized)
{
  onnx::ModelProto model;
  model.set_ir_version(9);
  model.add_opset_import()->set_version(18);
  auto* graph = model.mutable_graph();
  graph->set_name("block-projection");
  const auto port = [&] (onnx::ValueInfoProto& info, const char* name, int width) {
    info.set_name(name);
    auto* tensor = info.mutable_type()->mutable_tensor_type();
    tensor->set_elem_type(onnx::TensorProto::FLOAT);
    for (int size : {1, tokens, width}) tensor->mutable_shape()->add_dim()->set_dim_value(size);
  };
  port(*graph->add_input(), "input", inWidth);
  port(*graph->add_output(), "output", outWidth);
  const auto parameter = [&] (const char* name, std::initializer_list<int> dims) {
    auto* tensor = graph->add_initializer();
    tensor->set_name(name);
    tensor->set_data_type(onnx::TensorProto::FLOAT);
    for (const auto dim : dims) tensor->add_dims(dim);
    return tensor;
  };
  auto* weights = parameter("weight", {inWidth, outWidth});
  weights->mutable_float_data()->Resize(inWidth * outWidth, 0);
  for (const auto& [row, col] : nonzero) weights->set_float_data(row * outWidth + col, 1);
  parameter("bias", {outWidth})->mutable_float_data()->Resize(outWidth, 0);
  const auto emit = [&] (const char* op, const char* out,
                         std::initializer_list<const char*> inputs) {
    auto* node = graph->add_node();
    node->set_op_type(op);
    for (const auto* name : inputs) node->add_input(name);
    node->add_output(out);
  };
  emit("MatMul", "linear", {"input", "weight"});
  emit("Add", normalized ? "biased" : "output", {"linear", "bias"});
  if (normalized) {
    auto* axes = graph->add_initializer();
    axes->set_name("axes");
    axes->set_data_type(onnx::TensorProto::INT64);
    axes->add_dims(1);
    axes->add_int64_data(-1);
    parameter("epsilon", {})->add_float_data(0.25F);
    parameter("gain", {outWidth})->mutable_float_data()->Resize(outWidth, 1);
    emit("Mul", "squares", {"biased", "biased"});
    emit("ReduceMean", "mean", {"squares", "axes"});
    emit("Add", "stabilized", {"mean", "epsilon"});
    emit("Sqrt", "rms", {"stabilized"});
    emit("Div", "normalized", {"biased", "rms"});
    emit("Mul", "output", {"normalized", "gain"});
  }
  return model;
}

std::map<std::string, onnx::TensorProto>
makeVaeWeights()
{
  std::map<std::string, onnx::TensorProto> weights;
  const auto tensor = [&] (const std::string& name, std::initializer_list<int> shape, float fill)
    -> onnx::TensorProto& {
    auto& value = weights[name];
    value.set_name(name);
    value.set_data_type(onnx::TensorProto::FLOAT);
    int count = 1;
    for (const auto extent : shape) {
      value.add_dims(extent);
      count *= extent;
    }
    for (int i = 0; i < count; ++i) value.add_float_data(fill);
    return value;
  };
  const auto conv = [&] (const std::string& name, int out, int in, int t, int s,
                         float bias = 0) -> onnx::TensorProto& {
    tensor(name + ".bias", {out}, bias);
    return t ? tensor(name + ".weight", {out, in, t, s, s}, 0) :
               tensor(name + ".weight", {out, in, s, s}, 0);
  };
  auto& latentProjection = conv("conv2", 2, 2, 1, 1);
  latentProjection.set_float_data(0, 1);
  auto& inputProjection = conv("decoder.conv1", 8, 2, 3, 3);
  for (int c = 0; c < 8; ++c) inputProjection.set_float_data(c * 54 + 22, 1);
  const auto residual = [&] (const std::string& name, int in, int out) {
    tensor(name + ".residual.0.gamma", {in, 1, 1, 1}, 1);
    tensor(name + ".residual.3.gamma", {out, 1, 1, 1}, 1);
    conv(name + ".residual.2", out, in, 3, 3);
    conv(name + ".residual.6", out, out, 3, 3, 0.25F);
    if (out != in) {
      auto& shortcut = conv(name + ".shortcut", out, in, 1, 1);
      for (int c = 0; c < out; ++c) shortcut.set_float_data(c * in + c % in, 1);
    }
  };
  residual("decoder.middle.0", 8, 8);
  tensor("decoder.middle.1.norm.gamma", {8, 1, 1}, 1);
  conv("decoder.middle.1.to_qkv", 24, 8, 0, 1);
  conv("decoder.middle.1.proj", 8, 8, 0, 1);
  residual("decoder.middle.2", 8, 8);
  const std::map<int, int> outWidths{
    {0, 8}, {1, 8}, {2, 8}, {4, 8}, {5, 8}, {6, 8},
    {8, 4}, {9, 4}, {10, 4}, {12, 2}, {13, 2}, {14, 2}};
  int channels = 8;
  for (int i = 0; i < 15; ++i) {
    const auto name = "decoder.upsamples." + std::to_string(i);
    if (outWidths.count(i)) {
      const auto width = outWidths.at(i);
      residual(name, channels, width);
      channels = width;
      continue;
    }
    auto& space = conv(name + ".resample.1", channels / 2, channels, 0, 3);
    for (int c = 0; c < channels / 2; ++c)
      space.set_float_data((c * channels + c) * 9 + 4, 1);
    if (i != 11) {
      auto& time = conv(name + ".time_conv", 2 * channels, channels, 3, 1);
      auto& offsets = weights.at(name + ".time_conv.bias");
      for (int c = 0; c < channels; ++c) {
        time.set_float_data((c * channels + c) * 3 + 2, 1);
        time.set_float_data(((c + channels) * channels + c) * 3 + 2, 1);
        time.set_float_data(((c + channels) * channels + c) * 3 + 1, 0.5F);
        offsets.set_float_data(c + channels, 4);
      }
    }
    channels /= 2;
  }
  // Distinct head channels make the result sensitive to the preceding signal.
  weights.at("decoder.upsamples.14.residual.6.bias").set_float_data(1, 2);
  tensor("decoder.head.0.gamma", {2, 1, 1, 1}, 1);
  auto& rgb = conv("decoder.head.2", 3, 2, 3, 3);
  rgb.set_float_data(22, 0.5F);
  rgb.set_float_data(3 * 27 + 22, 0.75F);
  rgb.set_float_data(4 * 27 + 22, -0.5F);
  tensor("encoder.conv1.bias", {1}, 0);
  return weights;
}

NativeJson
makeCatalogTemplate(const NativeModelDescriptor& descriptor,
                    const NativeRoleRecipeProfile& profile,
                    const std::string& sourceName, const std::string& manifestDigest,
                    const std::string& artifactRoot, const std::string& serviceName,
                    const std::map<std::string, NativeRoleResourceRequirement>& resources)
{
  auto resourceRows = NativeJson::object();
  for (const auto& [role, requirement] : resources)
    resourceRows[role] = nativeParseJson(requirement.canonicalJson());
  return {
    {"schema", "ndnsf-di-native-request-catalog-v1"},
    {"model", nativeParseJson(descriptor.canonicalJson())},
    {"source", {{"data_name", sourceName}, {"model_manifest_digest", manifestDigest}}},
    {"recipe", {{"artifact_profile_digest", profile.artifactProfileDigest},
                {"assembler_descriptor_digest", profile.assemblerDescriptorDigest},
                {"backend_abi", profile.backendAbi}, {"precision", profile.precision},
                {"quantization", profile.quantization}, {"layout", profile.layout},
                {"padding", profile.padding}, {"protection_epoch", profile.protectionEpoch},
                {"max_source_bytes", profile.maxSourceBytes},
                {"max_assembled_bytes", profile.maxAssembledBytes},
                {"max_nodes", profile.maxNodes}}},
    {"publication", {{"artifact_root", artifactRoot}}},
    {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
    {"splitter", {{"kind", "ONNX_SOURCE"}, {"service_name", serviceName},
                  {"resources_by_role", std::move(resourceRows)}}}};
}

namespace {

constexpr int TEXT_WIDTH = 2;
constexpr int TEXT_SLOTS = 3;
constexpr int HIDDEN = 24;
constexpr int VIDEO_TOKENS = 12;
constexpr int HEADS = 4;
constexpr int HEAD_WIDTH = HIDDEN / HEADS;
constexpr int CHANNELS = 2;
constexpr int LATENT_FRAMES = 3;
constexpr int LATENT_HEIGHT = 4;
constexpr int LATENT_WIDTH = 4;
// The adapter bound covers both task input and the final FP32 TensorBundle.
constexpr std::size_t VIDEO_TASK_PAYLOAD_LIMIT = 128U << 10;
constexpr std::uint64_t BYTE_LIMIT = 32U << 20;
constexpr std::uint64_t ASSEMBLY_LIMIT = 64U << 20;
constexpr std::size_t NODE_LIMIT = 16U << 10;
constexpr std::uint64_t ATTENTION_BUDGET = 64U << 10;

onnx::TensorProto
filledTensor(std::initializer_list<int> shape, float fill = 0)
{
  auto tensor = floatTensor(shape, {});
  int count = 1;
  for (const int extent : shape) count *= extent;
  tensor.mutable_float_data()->Resize(count, fill);
  return tensor;
}

std::vector<std::pair<int, int>>
channelMap(int inWidth, int outWidth, int shift = 0)
{
  std::vector<std::pair<int, int>> result;
  for (int column = 0; column < outWidth; ++column)
    result.emplace_back((column + shift) % inWidth, column);
  return result;
}

void
setIntList(onnx::NodeProto& node, const char* name, std::initializer_list<int> values)
{
  auto* attr = node.add_attribute();
  attr->set_name(name);
  attr->set_type(onnx::AttributeProto::INTS);
  for (const int value : values) attr->add_ints(value);
}

onnx::ModelProto
makePatchSource()
{
  onnx::ModelProto source;
  source.set_ir_version(9);
  source.add_opset_import()->set_version(17);
  auto& graph = *source.mutable_graph();
  graph.set_name("patch");
  const auto declare = [] (onnx::ValueInfoProto* info, const char* name,
                            std::initializer_list<int> dims) {
    info->set_name(name);
    auto* type = info->mutable_type()->mutable_tensor_type();
    type->set_elem_type(onnx::TensorProto::FLOAT);
    for (const int dim : dims) type->mutable_shape()->add_dim()->set_dim_value(dim);
  };
  declare(graph.add_input(), "latent", {1, CHANNELS, LATENT_FRAMES, LATENT_HEIGHT, LATENT_WIDTH});
  declare(graph.add_output(), "projection",
          {1, HIDDEN, LATENT_FRAMES, LATENT_HEIGHT / 2, LATENT_WIDTH / 2});
  auto* weight = graph.add_initializer();
  *weight = filledTensor({HIDDEN, CHANNELS, 1, 2, 2});
  weight->set_name("weight");
  for (int channel = 0; channel < HIDDEN; ++channel) {
    weight->set_float_data(8 * channel + channel % 4, 0.5F);
    weight->set_float_data(8 * channel + 4 + (channel + 1) % 4, -0.25F);
  }
  auto* bias = graph.add_initializer();
  *bias = filledTensor({HIDDEN}, 0.125F);
  bias->set_name("bias");
  auto* conv = graph.add_node();
  conv->set_name("patch_embedding");
  conv->set_op_type("Conv");
  for (const auto* input : {"latent", "weight", "bias"}) conv->add_input(input);
  conv->add_output("projection");
  setIntList(*conv, "strides", {1, 2, 2});
  return source;
}

onnx::ModelProto
makeFfnSource(int tokens, int inWidth, int middleWidth, bool text)
{
  auto source = makeLinearSource(tokens, inWidth, middleWidth, channelMap(inWidth, middleWidth),
                                 false);
  source.mutable_opset_import(0)->set_version(20);
  auto& graph = *source.mutable_graph();
  graph.mutable_node(1)->set_output(0, "biased");
  auto* gelu = graph.add_node();
  gelu->set_op_type("Gelu");
  gelu->add_input("biased");
  gelu->add_output("activated");
  auto* approximate = gelu->add_attribute();
  approximate->set_name("approximate");
  approximate->set_type(onnx::AttributeProto::STRING);
  approximate->set_s("tanh");
  const int outWidth = text ? middleWidth : inWidth;
  const auto down = makeLinearSource(tokens, middleWidth, outWidth,
                                     channelMap(middleWidth, outWidth, 2), false);
  for (auto tensor : down.graph().initializer()) {
    tensor.set_name("down-" + tensor.name());
    for (auto& value : *tensor.mutable_float_data()) value *= 0.25F;
    *graph.add_initializer() = std::move(tensor);
  }
  auto* linear = graph.add_node();
  *linear = down.graph().node(0);
  linear->set_input(0, "activated");
  linear->set_input(1, "down-weight");
  linear->set_output(0, "contracted");
  *graph.add_node() = down.graph().node(1);
  graph.mutable_node(4)->set_input(0, "contracted");
  graph.mutable_node(4)->set_input(1, "down-bias");
  auto* shape = graph.mutable_output(0)->mutable_type()->mutable_tensor_type()->mutable_shape();
  shape->mutable_dim(2)->set_dim_value(outWidth);
  if (text) {
    shape->mutable_dim(1)->set_dim_param("valid_tokens");
    graph.mutable_input(0)->mutable_type()->mutable_tensor_type()->mutable_shape()
      ->mutable_dim(1)->set_dim_param("valid_tokens");
  }
  return source;
}

NativeStageLayout
stage(const std::string& component, const std::string& label, bool parallel)
{
  const auto name = component + "/" + label;
  const auto rolePrefix = "/" + name;
  return {name, nativePlanningDigest("wan-fixture/region/" + name),
          parallel ? std::vector<std::string>{rolePrefix + "/rank-0", rolePrefix + "/rank-1"}
                   : std::vector<std::string>{rolePrefix + "/serial"}};
}

VideoDenoiserLayout
makeDenoiser(std::map<std::string, onnx::TensorProto>* checkpoint = nullptr)
{
  const VideoHeadGrid grid{LATENT_FRAMES, LATENT_HEIGHT / 2, LATENT_WIDTH / 2, VIDEO_TOKENS};
  const VideoRotaryTile tile{grid.frames, grid.height, grid.width, HEADS, HEAD_WIDTH, 0,
                             grid.paddedTokens};
  const auto q = makeLinearSource(VIDEO_TOKENS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN), true);
  const auto k = makeLinearSource(VIDEO_TOKENS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN, 1), true);
  const auto v = makeLinearSource(VIDEO_TOKENS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN, 2), false);
  const auto o = makeLinearSource(VIDEO_TOKENS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN, 3), false);
  const auto textK = makeLinearSource(TEXT_SLOTS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN), true);
  const auto textV = makeLinearSource(TEXT_SLOTS, HIDDEN, HIDDEN, channelMap(HIDDEN, HIDDEN, 5),
                                      false);
  const VideoBlockAttention self{
    lowerVideoAttentionContext(k, v, tile, ATTENTION_BUDGET),
    lowerVideoAttentionQuery(q, o, tile, VIDEO_TOKENS, 3, ATTENTION_BUDGET, ATTENTION_BUDGET)};
  const VideoBlockAttention cross{
    lowerTextAttentionContext(textK, textV, HEADS, HEAD_WIDTH),
    lowerTextAttentionQuery(q, o, HEADS, HEAD_WIDTH, TEXT_SLOTS, 1, ATTENTION_BUDGET)};
  const auto ffnSource = makeFfnSource(VIDEO_TOKENS, HIDDEN, HIDDEN + 8, false);
  const auto textSource = makeFfnSource(TEXT_SLOTS, TEXT_WIDTH, HIDDEN, true);
  const auto patchSource = makePatchSource();
  const auto ffn = lowerFeedForward(ffnSource);
  const auto text = lowerTextEmbedding(textSource, TEXT_SLOTS);
  const auto patch = lowerPatchEmbedding(patchSource);
  auto timeUp = filledTensor({4, HIDDEN}), timeDown = filledTensor({HIDDEN, HIDDEN});
  auto timeMod = filledTensor({HIDDEN, 6 * HIDDEN});
  for (int c = 0; c < HIDDEN; ++c) {
    timeUp.set_float_data((c % 4) * HIDDEN + c, 0.125F);
    timeDown.set_float_data(c * HIDDEN + c, 0.5F);
    for (int group = 0; group < 6; ++group)
      timeMod.set_float_data(c * 6 * HIDDEN + group * HIDDEN + c, 0.0625F);
  }
  const auto timeBias = filledTensor({HIDDEN}), modBias = filledTensor({6 * HIDDEN});
  const auto time = lowerTimeEmbedding({{{timeUp, timeBias}, {timeDown, timeBias},
                                          {timeMod, modBias}}});
  auto headWeight = filledTensor({HIDDEN, 4 * CHANNELS});
  for (int c = 0; c < HIDDEN; ++c) headWeight.set_float_data(c * 4 * CHANNELS + c % 8, 0.125F);
  const auto headModulation = filledTensor({1, 2, HIDDEN});
  const auto headBias = filledTensor({4 * CHANNELS}, 0.125F);
  const auto head = lowerVideoHead(grid, headModulation, headWeight, headBias, 1e-6F);
  const auto save = [&](const std::string& name, onnx::TensorProto tensor, bool transpose = false) {
    if (!checkpoint) return;
    tensor.set_name(name);
    if (transpose) {
      const auto in = tensor.dims(0), out = tensor.dims(1);
      auto physical = tensor;
      physical.mutable_dims()->SwapElements(0, 1);
      for (int row = 0; row < out; ++row) {
        for (int col = 0; col < in; ++col)
          physical.set_float_data(row * in + col, tensor.float_data(col * out + row));
      }
      tensor = std::move(physical);
    }
    checkpoint->emplace(name, std::move(tensor));
  };
  const auto saveLinear = [&](const std::string& name, const onnx::ModelProto& source,
                              const std::string& local = "") {
    for (const auto& tensor : source.graph().initializer()) {
      if (tensor.name() == local + "weight") save(name + ".weight", tensor, true);
      if (tensor.name() == local + "bias") save(name + ".bias", tensor);
    }
  };
  if (checkpoint) {
    save("patch_embedding.weight", patchSource.graph().initializer(0));
    save("patch_embedding.bias", patchSource.graph().initializer(1));
    saveLinear("text_embedding.0", textSource);
    saveLinear("text_embedding.2", textSource, "down-");
    save("time_embedding.0.weight", timeUp, true);
    save("time_embedding.0.bias", timeBias);
    save("time_embedding.2.weight", timeDown, true);
    save("time_embedding.2.bias", timeBias);
    save("time_projection.1.weight", timeMod, true);
    save("time_projection.1.bias", modBias);
    save("head.modulation", headModulation);
    save("head.head.weight", headWeight, true);
    save("head.head.bias", headBias);
  }
  const auto tp = stage("dit", "split", true);
  std::vector<ConnectedVideoBlock> blocks;
  const auto scale = filledTensor({HIDDEN}, 1), bias = filledTensor({HIDDEN});
  for (const float gate : {0.125F, 0.25F, 0.375F}) {
    auto modulation = filledTensor({1, 6, HIDDEN});
    for (int c = 0; c < HIDDEN; ++c) {
      modulation.set_float_data(HIDDEN + c, 0.0625F * (c % 4));
      modulation.set_float_data(2 * HIDDEN + c, gate);
      modulation.set_float_data(5 * HIDDEN + c, 0.5F);
    }
    if (checkpoint) {
      const auto prefix = "blocks." + std::to_string(blocks.size()) + ".";
      save(prefix + "modulation", modulation);
      save(prefix + "norm3.weight", scale);
      save(prefix + "norm3.bias", bias);
      saveLinear(prefix + "ffn.0", ffnSource);
      saveLinear(prefix + "ffn.2", ffnSource, "down-");
      const auto saveAttention = [&](const std::string& group, const onnx::ModelProto& key,
                                      const onnx::ModelProto& value) {
        const auto path = prefix + group + ".";
        saveLinear(path + "q", q);
        saveLinear(path + "k", key);
        saveLinear(path + "v", value);
        saveLinear(path + "o", o);
        save(path + "norm_q.weight", scale);
        save(path + "norm_k.weight", scale);
      };
      saveAttention("self_attn", k, v);
      saveAttention("cross_attn", textK, textV);
    }
    blocks.push_back(connectVideoBlock(
      lowerVideoBlock(VIDEO_TOKENS, modulation, scale, bias, 1e-6F), self, cross, ffn, tp));
  }
  return connectVideoDenoiser(patch, text, time, blocks, head, grid, tp);
}

NativeCanonicalSource
canonicalSource(const onnx::ModelProto& model)
{
  const auto wire = model.SerializeAsString();
  NativeCanonicalSource source;
  source.modelBytes.assign(wire.begin(), wire.end());
  return source;
}

NativeJson
writeComponent(const std::filesystem::path& root, const std::string& component,
               const NativeCanonicalSource& source, const OnnxParallelPlan& layout,
               const ndn::Name& requester, const ndn::Name& service, const std::string& epoch,
               const NativeAssemblyControl& control, bool tileComponent = false)
{
  const auto modelName = "tiny-wan-" + component;
  const auto graphDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  const auto initializerDigest = nativeCanonicalInitializerDigest(source, control);
  auto content = graphDigest + initializerDigest;
  constexpr std::uint64_t TILED_BYTES = 900;
  if (tileComponent)
    content += nativePlanningDigest(std::to_string(TILED_BYTES) +
      nativeCanonicalJson(nativeV2StepTemplateJson(layout.plan)));
  const auto contentDigest = nativePlanningDigest(content);
  const auto packageDigest = nativePlanningDigest("wan-fixture/model/" + contentDigest);
  const auto descriptor = ndnsf::di::fixture::completeModel({
    modelName, contentDigest, nativePlanningDigest(modelName), graphDigest,
    "onnx", "fp32", "wan", "1"});
  const auto assembledBytes = component == "vae" ? ASSEMBLY_LIMIT : BYTE_LIMIT;
  const NativeRoleRecipeProfile profile{
    nativePlanningDigest("wan-fixture/profile/" + component),
    nativePlanningDigest("wan-fixture/assembler"), "onnxruntime-cpu-v1", "fp32", "none",
    "native", "right", epoch, BYTE_LIMIT, assembledBytes, NODE_LIMIT};
  std::map<std::string, NativeRoleResourceRequirement> resources;
  for (const auto& role : layout.plan.roles)
    resources[role] = {descriptor.adapter.backends, 4U << 20, assembledBytes, 0,
                       8U << 20, 16U << 20, 1.0};
  auto catalog = makeCatalogTemplate(descriptor, profile,
    ndn::Name(requester).append("model-source").append(component)
      .append(contentDigest.substr(7)).toUri(),
    packageDigest,
    ndn::Name(requester).append("model-artifacts").append(component)
      .append(contentDigest.substr(7)).toUri(),
    service.toUri(), resources);
  catalog["max_payload_bytes"] = VIDEO_TASK_PAYLOAD_LIMIT;
  auto package = compileWanComponent(source, layout, tileComponent ? TILED_BYTES : BYTE_LIMIT,
                                     catalog, control);
  const auto loaded = NativeRequestCatalog::load(package.configurationJson, package.source, control);
  const auto& candidate = loaded.planningCache->candidates.at(0).candidate;
  if (candidate.executionPlan.stageLayout.size() != layout.plan.stageLayout.size() ||
      candidate.executionPlan.roles != layout.plan.roles)
    throw std::runtime_error("Wan fixture catalog changed its selected layout");
  auto stages = NativeJson::array();
  for (const auto& stage : candidate.executionPlan.stageLayout)
    stages.push_back(stage.unitRoleIds);
  const auto path = writeWanComponent(package, root / component, control);
  const auto& model = loaded.model;
  return {
    {"catalog", path.lexically_relative(root.parent_path()).string()},
    {"candidate_digest", candidate.candidateDigest}, {"stages", std::move(stages)},
    {"graph_digest", model.canonicalSourceDigest},
    {"package_digest", packageDigest}, {"initializer_digest", initializerDigest}};
}

} // namespace

std::map<std::string, onnx::TensorProto>
makeDitCheckpointWeights()
{
  std::map<std::string, onnx::TensorProto> weights;
  (void)makeDenoiser(&weights);
  return weights;
}

NativeJson
writeWanVideoFixture(const std::filesystem::path& destination, const ndn::Name& requester,
                     const ndn::Name& service, const std::string& protectionEpoch, bool tileDit)
{
  if (destination.empty() || requester.empty() || service.empty() || protectionEpoch.empty())
    throw std::invalid_argument("Wan fixture requires an output and explicit deployment identities");
  const auto root = std::filesystem::absolute(destination).lexically_normal();
  if (!std::filesystem::create_directory(root))
    throw std::invalid_argument("Wan fixture requires a fresh output directory");
  const NativeAssemblyControl control{
    std::chrono::steady_clock::now() + std::chrono::minutes(2), [] {}, BYTE_LIMIT, ASSEMBLY_LIMIT};
  const auto umt5 = makeUmt5Source();
  const auto encoder = connectUmt5Encoder(umt5.embedding, umt5.layers, umt5.finalNorm);
  const auto textTp = makeUmt5TensorParallelPlan(encoder, stage("umt5", "split", true));
  const auto textPp = makeUmt5PipelinePlan(encoder, 1, {stage("umt5", "first", false),
                                                      stage("umt5", "later", false)});
  const auto dit = makeDenoiser();
  const auto ditSource = canonicalSource(dit.model);
  const auto ditPp = makeVideoDenoiserStagedPlan(dit, 1, {stage("dit", "first", false),
                                                       stage("dit", "later", false)});
  const auto ppTp = makeVideoDenoiserStagedPlan(dit, 1, {stage("dit", "first", false),
                                                      stage("dit", "later", true)});
  const auto tpPp = makeVideoDenoiserStagedPlan(dit, 2, {stage("dit", "first", true),
                                                      stage("dit", "later", false)});
  auto weights = makeVaeWeights();
  weights.at("conv2.weight").set_float_data(3, 0.5F);
  auto& projection = weights.at("decoder.conv1.weight");
  for (int c = 0; c < 8; ++c) projection.set_float_data(c * 54 + 49, c % 2 ? 0.25F : -0.5F);
  const VaeConvTile tile{1, LATENT_HEIGHT, LATENT_WIDTH};
  const auto initial = lowerWan21VaeDecoder(tile, weights, {0, 0}, {1, 1}, true, 4,
                                            ATTENTION_BUDGET);
  const auto later = lowerWan21VaeDecoder(tile, weights, {0, 0}, {1, 1}, false, 4,
                                          ATTENTION_BUDGET);
  const auto clip = connectVaeClip(initial, later, LATENT_FRAMES, NODE_LIMIT);
  const auto clipSource = canonicalSource(clip.model);
  const auto vaeTp = makeVaeTensorParallelPlan(clip, stage("vae", "split", true));
  const auto vaePp = makeVaePipelinePlan(clip, {stage("vae", "first", false),
                                              stage("vae", "later", false)});

  NativeJson modes = NativeJson::object();
  for (const auto& [name, plan] :
       std::array<std::pair<const char*, const OnnxParallelPlan*>, 4>{{
         {"tp", &dit.tensorParallel}, {"pp", &ditPp}, {"pp-tp", &ppTp}, {"tp-pp", &tpPp}}}) {
    const auto mode = root / name;
    std::filesystem::create_directory(mode);
    const auto tp = plan == &dit.tensorParallel;
    modes[name] = {
      {"umt5", writeComponent(mode, "umt5", encoder.source, tp ? textTp : textPp,
                             requester, service, protectionEpoch, control)},
      {"dit", writeComponent(mode, "dit", ditSource, *plan, requester, service,
                            protectionEpoch, control, tileDit)},
      {"vae", writeComponent(mode, "vae", clipSource, tp ? vaeTp : vaePp,
                            requester, service, protectionEpoch, control)}};
  }
  NativeJson manifest{
    {"schema", "ndnsf-test-wan-video-fixture-v1"},
    {"requester", requester.toUri()}, {"service", service.toUri()},
    {"protection_epoch", protectionEpoch},
    {"latent_shape", {CHANNELS, LATENT_FRAMES, LATENT_HEIGHT, LATENT_WIDTH}},
    {"frames_height_width", {9, 32, 32}}, {"padded_video_tokens", VIDEO_TOKENS},
    {"text_width", TEXT_WIDTH}, {"steps", 3}, {"shift", 3.0}, {"guidance", 2.0}, {"seed", 271},
    {"positive_tokens", {{"ids", {2, 0, 1}}, {"valid_tokens", 3}}},
    {"negative_tokens", {{"ids", {1, 0, 0}}, {"valid_tokens", 1}}},
    {"modes", std::move(modes)}};
  std::ofstream output(root / "fixture.json", std::ios::binary);
  output << nativeCanonicalJson(manifest) << '\n';
  output.close();
  if (!output) throw std::runtime_error("could not write Wan fixture manifest");
  return manifest;
}

} // namespace ndnsf::di::wan::fixture

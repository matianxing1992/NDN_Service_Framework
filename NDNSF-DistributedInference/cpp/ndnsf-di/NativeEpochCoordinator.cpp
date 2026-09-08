#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

namespace ndnsf::di {
namespace {

constexpr const char* FEEDBACK_TENSOR = "__ndnsf_token_feedback_terminal";
constexpr const char* EPOCH_TERMINAL_TENSOR = "__ndnsf_epoch_terminal";

class DecodeStateReleaseGuard
{
public:
  DecodeStateReleaseGuard(NativeProviderRuntime& runtime,
                          std::string sessionId,
                          std::string role)
    : m_runtime(runtime)
    , m_sessionId(std::move(sessionId))
    , m_role(std::move(role))
  {
  }

  ~DecodeStateReleaseGuard()
  {
    try {
      m_runtime.releaseDecodeState(m_sessionId, m_role);
    }
    catch (...) {
      // Cleanup evidence must never replace the coordinator's terminal result.
    }
  }

private:
  NativeProviderRuntime& m_runtime;
  std::string m_sessionId;
  std::string m_role;
};

void
throwIfStopped(const std::function<std::optional<NativeEpochStopReason>()>& stopCheck,
               const std::function<void()>& executionGuard)
{
  if (executionGuard) executionGuard();
  if (!stopCheck) {
    return;
  }
  const auto reason = stopCheck();
  if (!reason) {
    return;
  }
  switch (*reason) {
    case NativeEpochStopReason::Cancelled:
      throw std::runtime_error("ATTEMPT_CANCELLED");
    case NativeEpochStopReason::Deadline:
      throw std::runtime_error("REQUEST_DEADLINE");
  }
  throw std::runtime_error("native epoch coordinator stop reason is invalid");
}

void
throwIfStopped(const NativeEpochCoordinatorConfig& config)
{
  throwIfStopped(config.stopCheck, config.executionGuard);
}

void
traceEpoch(const std::string& phase, const std::string& role, std::size_t epoch)
{
  const auto* enabled = std::getenv("NDNSF_DI_RUNTIME_TIMING");
  if (enabled != nullptr && *enabled != '\0' && *enabled != '0') {
    std::ostringstream record;
    record << "NDNSF_DI_EPOCH_COORDINATOR phase=" << phase
           << " role=" << role << " epoch=" << epoch;
    logRuntimeTrace(record.str());
  }
}

std::optional<std::string>
feedbackScopeFor(const RoleSpec& role)
{
  for (const auto& edge : role.outputs) {
    if (edge.operationKind == "TOKEN_FEEDBACK") {
      return edge.scope;
    }
  }
  return std::nullopt;
}

GenerationEpochLineageV1
lineageForEdge(GenerationEpochLineageV1 lineage, const DependencyEdge& edge);

bool
hasTerminalMarker(const TensorBundle& bundle,
                  const std::string& markerName,
                  bool required)
{
  if (!isEncodedTensorBundle(bundle.payload)) {
    if (!required) {
      return false;
    }
    throw std::invalid_argument("terminal control is not an encoded tensor bundle");
  }
  const auto tensors = decodeTensorBundle(bundle.payload);
  try {
    const auto& marker = findTensor(tensors, markerName);
    if (marker.elementType != TensorElementType::Bool || marker.payload.size() != 1) {
      throw std::invalid_argument("terminal control marker has invalid type");
    }
    return marker.payload.front() != 0;
  }
  catch (const std::out_of_range&) {
    if (!required) {
      return false;
    }
    throw std::invalid_argument("terminal control is missing its marker");
  }
}

bool
isTerminalFeedback(const TensorBundle& bundle)
{
  return hasTerminalMarker(bundle, FEEDBACK_TENSOR, true);
}

bool
isTerminalActivation(const TensorBundle& bundle)
{
  return hasTerminalMarker(bundle, EPOCH_TERMINAL_TENSOR, false);
}

TensorBundle
makeTokenFeedback(std::int64_t token, bool terminal)
{
  std::vector<std::uint8_t> tokenBytes(sizeof(token));
  std::memcpy(tokenBytes.data(), &token, sizeof(token));
  std::vector<std::uint8_t> terminalByte{static_cast<std::uint8_t>(terminal ? 1 : 0)};
  return makeEncodedTensorBundle(
    "token-feedback",
    {
      NamedTensor{"input_ids", TensorElementType::Int64, {1, 1},
                  std::move(tokenBytes)},
      NamedTensor{FEEDBACK_TENSOR, TensorElementType::Bool, {1},
                  std::move(terminalByte)},
    });
}

TensorBundle
makeTerminalActivation()
{
  return makeEncodedTensorBundle(
    "epoch-terminal",
    {NamedTensor{EPOCH_TERMINAL_TENSOR, TensorElementType::Bool, {1}, {1}}});
}

void
publishTerminalActivation(const NativeEpochCoordinatorConfig& config,
                          const RoleSpec& role,
                          const std::optional<GenerationEpochLineageV1>& lineage =
                            std::nullopt)
{
  for (const auto& edge : role.outputs) {
    if (edge.operationKind == "ACTIVATION") {
      auto marker = makeTerminalActivation();
      if (lineage) {
        marker = attachGenerationEpochLineage(
          marker, lineageForEdge(*lineage, edge));
      }
      config.io->publishOutput(config.sessionId, edge, marker);
    }
  }
}

std::map<std::string, TensorBundle>
fetchInputs(const NativeEpochCoordinatorConfig& config,
            const RoleSpec& role,
            std::map<std::string, TensorBundle> inputs)
{
  for (const auto& edge : role.inputs) {
    if (inputs.find(edge.scope) != inputs.end()) {
      continue;
    }
    auto future = config.io->prefetchInput(config.sessionId, edge);
    auto bundle = future.get();
    validateTensorBundleForEdge(edge, bundle);
    inputs.emplace(edge.scope, std::move(bundle));
  }
  return inputs;
}

std::string
digestText(const std::string& value)
{
  return sha256TensorBytes(
    std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::vector<std::int64_t>
tokenIdsFromInputs(const NativeEpochCoordinatorConfig& config,
                   const std::map<std::string, TensorBundle>& inputs)
{
  std::optional<std::vector<std::int64_t>> result;
  for (const auto& item : inputs) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      continue;
    }
    const auto tensors = decodeTensorBundle(item.second.payload);
    const auto found = std::find_if(tensors.begin(), tensors.end(), [&] (const auto& tensor) {
      return tensor.name == config.tokenInputName;
    });
    if (found == tensors.end()) {
      continue;
    }
    if (result || found->elementType != TensorElementType::Int64 ||
        found->shape.empty() || found->payload.empty() ||
        found->payload.size() % sizeof(std::int64_t) != 0) {
      throw std::invalid_argument(
        "native epoch coordinator requires one canonical Int64 token input");
    }
    std::vector<std::int64_t> tokens(found->payload.size() / sizeof(std::int64_t));
    std::memcpy(tokens.data(), found->payload.data(), found->payload.size());
    if (std::any_of(tokens.begin(), tokens.end(), [] (std::int64_t token) {
          return token < 0;
        })) {
      throw std::invalid_argument("native epoch coordinator token ID is negative");
    }
    result = std::move(tokens);
  }
  if (!result) {
    throw std::invalid_argument(
      "native epoch coordinator is missing its canonical token input");
  }
  return *result;
}

std::size_t
actualNewInputExtent(
  const std::map<std::string, TensorBundle>& inputs,
  const std::vector<std::string>& stateInputNames,
  const std::string& preferredInputName)
{
  std::size_t nonStateExtent = 0;
  std::size_t preferredExtent = 0;
  bool foundPreferred = false;
  for (const auto& item : inputs) {
    if (!isEncodedTensorBundle(item.second.payload)) {
      throw std::invalid_argument(
        "native epoch cache observation requires encoded tensor inputs");
    }
    for (const auto& tensor : decodeTensorBundle(item.second.payload)) {
      if (std::find(stateInputNames.begin(), stateInputNames.end(), tensor.name) !=
          stateInputNames.end()) {
        continue;
      }
      std::size_t elements = 1;
      if (tensor.shape.empty()) {
        elements = tensor.payload.empty() ? 0 : 1;
      }
      else {
        for (const auto dimension : tensor.shape) {
          if (dimension <= 0 ||
              elements > std::numeric_limits<std::size_t>::max() /
                static_cast<std::size_t>(dimension)) {
            throw std::invalid_argument(
              "native epoch cache observation tensor extent is invalid");
          }
          elements *= static_cast<std::size_t>(dimension);
        }
      }
      if (nonStateExtent > std::numeric_limits<std::size_t>::max() - elements) {
        throw std::overflow_error(
          "native epoch cache observation extent overflow");
      }
      nonStateExtent += elements;
      if (tensor.name == preferredInputName) {
        if (preferredExtent >
            std::numeric_limits<std::size_t>::max() - elements) {
          throw std::overflow_error(
            "native epoch cache observation preferred extent overflow");
        }
        preferredExtent += elements;
        foundPreferred = true;
      }
    }
  }
  const auto extent = foundPreferred ? preferredExtent : nonStateExtent;
  if (extent == 0) {
    throw std::invalid_argument(
      "native epoch cache observation has no non-state input extent");
  }
  return extent;
}

std::string
initialPrefixDigest(const DecodeStateIdentityV1& identity,
                    const std::vector<std::int64_t>& tokens)
{
  std::ostringstream value;
  value << "NDNSF-DI-PREFIX-V1/PROMPT\n" << identity.tokenizerDigest
        << "\n" << tokens.size();
  for (const auto token : tokens) {
    value << "\n" << token;
  }
  return digestText(value.str());
}

std::string
appendedPrefixDigest(const GenerationEpochLineageV1& predecessor,
                     std::int64_t token)
{
  std::ostringstream value;
  value << "NDNSF-DI-PREFIX-V1/APPEND\n"
        << predecessor.logicalPrefixDigest << "\n"
        << predecessor.logicalPrefixTokenCount + 1 << "\n" << token;
  return digestText(value.str());
}

std::string
positionDigest(const std::string& positionPolicyDigest,
               const std::string& prefixDigest,
               std::uint32_t prefixTokenCount)
{
  std::ostringstream value;
  value << "NDNSF-DI-POSITION-V1\n" << positionPolicyDigest << "\n"
        << prefixDigest << "\n" << prefixTokenCount;
  return digestText(value.str());
}

GenerationEpochLineageV1
initialGenerationLineage(const NativeEpochCoordinatorConfig& config,
                         const std::map<std::string, TensorBundle>& inputs)
{
  if (!config.stateIdentityTemplate) {
    throw std::invalid_argument(
      "native epoch coordinator is missing its trusted generation identity template");
  }
  const auto tokens = tokenIdsFromInputs(config, inputs);
  if (tokens.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::overflow_error("generation prefix token count overflow");
  }
  GenerationEpochLineageV1 lineage;
  lineage.requestId = config.requestId.empty() ? config.sessionId : config.requestId;
  lineage.attemptEpoch = config.attemptEpoch;
  lineage.planDigest = config.lineagePlanDigest;
  lineage.generationId = config.stateIdentityTemplate->generationId;
  lineage.streamEpoch = config.streamEpoch;
  lineage.inferenceEpoch = 0;
  lineage.transitionKind = GenerationEpochLineageV1::PREFILL;
  if (config.conversationStateBinding) {
    // APPEND_DELTA inputs contain only the new turn suffix.  The restored
    // Provider state already represents the parent prefix, so the lineage
    // must extend that authenticated prefix instead of hashing the suffix as
    // a new prompt.  This keeps request-local epoch zero distinct from the
    // conversation context epoch while still proving the exact logical
    // parent+suffix prefix.
    const auto& parent = config.conversationStateBinding->identity;
    if (tokens.size() > std::numeric_limits<std::uint32_t>::max() ||
        parent.prefixTokenCount > std::numeric_limits<std::uint32_t>::max() -
          static_cast<std::uint32_t>(tokens.size())) {
      throw std::overflow_error("conversation prefix token count overflow");
    }
    lineage.logicalPrefixTokenCount = parent.prefixTokenCount;
    lineage.logicalPrefixDigest = parent.prefixDigest;
    for (const auto token : tokens) {
      lineage.logicalPrefixDigest = appendedPrefixDigest(lineage, token);
      ++lineage.logicalPrefixTokenCount;
    }
  }
  else {
    lineage.logicalPrefixTokenCount = static_cast<std::uint32_t>(tokens.size());
    lineage.logicalPrefixDigest = initialPrefixDigest(
      *config.stateIdentityTemplate, tokens);
  }
  lineage.positionDigest = positionDigest(
    config.positionPolicyDigest, lineage.logicalPrefixDigest,
    lineage.logicalPrefixTokenCount);
  return lineage;
}

GenerationEpochLineageV1
nextGenerationLineage(const NativeEpochCoordinatorConfig& config,
                      const GenerationEpochLineageV1& predecessor,
                      std::int64_t token)
{
  if (!config.stateIdentityTemplate || token < 0 ||
      predecessor.logicalPrefixTokenCount ==
        std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("generation lineage cannot advance");
  }
  auto lineage = predecessor;
  ++lineage.inferenceEpoch;
  lineage.transitionKind = GenerationEpochLineageV1::DECODE;
  ++lineage.logicalPrefixTokenCount;
  lineage.logicalPrefixDigest = appendedPrefixDigest(predecessor, token);
  lineage.positionDigest = positionDigest(
    config.positionPolicyDigest, lineage.logicalPrefixDigest,
    lineage.logicalPrefixTokenCount);
  lineage.producerRole.clear();
  lineage.consumerRole.clear();
  lineage.operationIndex = 0;
  return lineage;
}

GenerationEpochLineageV1
lineageForEdge(GenerationEpochLineageV1 lineage, const DependencyEdge& edge)
{
  lineage.producerRole = edge.producerRole;
  lineage.consumerRole = edge.consumerRole;
  lineage.operationIndex = edge.collectiveOperationIndex;
  lineage.validate();
  return lineage;
}

std::optional<GenerationEpochLineageV1>
extractAndVerifyInputLineage(const RoleSpec& role,
                             std::map<std::string, TensorBundle>& inputs)
{
  std::optional<GenerationEpochLineageV1> accepted;
  for (const auto& edge : role.inputs) {
    if (edge.operationKind != "ACTIVATION" &&
        edge.operationKind != "TOKEN_FEEDBACK") {
      continue;
    }
    const auto found = inputs.find(edge.scope);
    if (found == inputs.end()) {
      throw std::runtime_error("generation dependency input was not fetched");
    }
    const auto lineage = extractGenerationEpochLineage(found->second);
    if (!lineage || lineage->requestId != role.requestId ||
        lineage->attemptEpoch != role.attemptEpoch ||
        lineage->inferenceEpoch != role.inferenceEpoch ||
        lineage->producerRole != edge.producerRole ||
        lineage->consumerRole != edge.consumerRole ||
        lineage->operationIndex != edge.collectiveOperationIndex) {
      throw std::runtime_error("GENERATION_EPOCH_LINEAGE_MISMATCH");
    }
    if (accepted && !accepted->sameGenerationState(*lineage)) {
      throw std::runtime_error("GENERATION_EPOCH_LINEAGE_CONFLICT");
    }
    accepted = *lineage;
    accepted->producerRole.clear();
    accepted->consumerRole.clear();
    accepted->operationIndex = 0;
    found->second = stripGenerationEpochLineage(found->second);
  }
  return accepted;
}

DecodeStateIdentityV1
decodeStateIdentityForEpoch(
  const NativeEpochCoordinatorConfig& config,
  const GenerationEpochLineageV1& lineage,
  const std::optional<DecodeStateIdentityV1>& predecessor,
  std::size_t epoch)
{
  if (!config.stateIdentityTemplate) {
    throw std::invalid_argument(
      "stateful native epoch coordinator is missing exact identity template");
  }
  auto identity = *config.stateIdentityTemplate;
  identity.roleName = config.role;
  identity.requestId = config.requestId.empty()
    ? config.sessionId : config.requestId;
  identity.attemptEpoch = config.attemptEpoch;
  if (lineage.inferenceEpoch != epoch ||
      (predecessor &&
       (predecessor->stateInferenceEpoch + 1 != epoch ||
        predecessor->prefixTokenCount + 1 != lineage.logicalPrefixTokenCount)) ||
      (!predecessor && epoch != 0)) {
    throw std::runtime_error("GENERATION_DECODE_STATE_LINEAGE_MISMATCH");
  }
  identity.prefixDigest = lineage.logicalPrefixDigest;
  identity.prefixTokenCount = lineage.logicalPrefixTokenCount;
  identity.positionDigest = lineage.positionDigest;
  identity.cacheEpoch = config.stateIdentityTemplate->cacheEpoch;
  identity.stateInferenceEpoch = epoch;
  identity.predecessorInferenceEpoch = epoch == 0
    ? std::optional<std::uint64_t>{}
    : std::optional<std::uint64_t>{epoch - 1};
  identity.validate();
  return identity;
}

std::vector<float>
lastLogits(const std::map<std::string, TensorBundle>& outputs)
{
  const auto encoded = std::find_if(
    outputs.begin(), outputs.end(), [] (const auto& item) {
      return isEncodedTensorBundle(item.second.payload);
    });
  if (encoded == outputs.end()) {
    throw std::runtime_error("native epoch coordinator received no encoded logits");
  }
  const auto tensors = decodeTensorBundle(encoded->second.payload);
  const auto& logits = findTensor(tensors, "logits");
  if (logits.elementType != TensorElementType::Float32 || logits.shape.empty() ||
      logits.payload.size() % sizeof(float) != 0) {
    throw std::invalid_argument("native epoch coordinator logits are invalid");
  }
  const auto vocabulary = static_cast<std::size_t>(logits.shape.back());
  if (vocabulary == 0 || logits.payload.size() % (vocabulary * sizeof(float)) != 0) {
    throw std::invalid_argument("native epoch coordinator logits vocabulary is invalid");
  }
  const auto sequence = logits.payload.size() / (vocabulary * sizeof(float));
  const auto* values = reinterpret_cast<const float*>(logits.payload.data());
  const auto* begin = values + (sequence - 1) * vocabulary;
  std::vector<float> result(begin, begin + vocabulary);
  if (std::any_of(result.begin(), result.end(), [] (const float value) {
        return !std::isfinite(value);
      })) {
    throw std::invalid_argument("native epoch coordinator logits are non-finite");
  }
  return result;
}

std::uint64_t
splitmix64(std::uint64_t value)
{
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

double
deterministicUnit(std::uint64_t seed, std::uint64_t step)
{
  const auto bits = splitmix64(seed + step);
  return static_cast<double>(bits >> 11) /
         static_cast<double>(1ULL << 53);
}

std::int64_t
sampleToken(const std::map<std::string, TensorBundle>& outputs,
            const NativeEpochCoordinatorConfig& config,
            const std::vector<std::int64_t>& generated,
            std::size_t step)
{
  const auto inputLogits = lastLogits(outputs);
  if (inputLogits.empty() || config.samplingTopK == 0 ||
      config.samplingTopK > inputLogits.size() ||
      !std::isfinite(config.samplingTopP) || config.samplingTopP <= 0.0 || config.samplingTopP > 1.0 ||
      !std::isfinite(config.samplingRepetitionPenalty) || config.samplingRepetitionPenalty < 0.1 ||
      config.samplingRepetitionPenalty > 2.0 || !std::isfinite(config.samplingTemperature) ||
      (config.samplingMode == "Greedy" ? config.samplingTemperature != 0.0 :
        config.samplingMode != "SeededTopKTopP" || config.samplingTemperature <= 0.0 ||
        config.samplingTemperature > 5.0)) {
    throw std::invalid_argument("invalid native sampling contract");
  }
  // Preserve the exact transported float32 values, then apply the reference
  // sampler's double-precision arithmetic without rounding penalties to float.
  std::vector<double> logits(inputLogits.begin(), inputLogits.end());
  std::set<std::int64_t> penalized;
  for (const auto token : generated) {
    if (token < 0 || static_cast<std::size_t>(token) >= logits.size()) {
      continue;
    }
    if (!penalized.insert(token).second) {
      continue;
    }
    if (config.samplingRepetitionPenalty != 1.0) {
      auto& value = logits[static_cast<std::size_t>(token)];
      value = value >= 0.0
        ? value / config.samplingRepetitionPenalty
        : value * config.samplingRepetitionPenalty;
    }
  }
  if (config.samplingMode == "Greedy") {
    return static_cast<std::int64_t>(std::distance(
      logits.begin(), std::max_element(logits.begin(), logits.end())));
  }
  const auto count = config.samplingTopK;
  std::vector<std::size_t> candidates(logits.size());
  std::iota(candidates.begin(), candidates.end(), 0);
  std::stable_sort(candidates.begin(), candidates.end(), [&logits] (auto left, auto right) {
    return logits[left] > logits[right];
  });
  candidates.resize(count);
  double maximum = -std::numeric_limits<double>::infinity();
  for (const auto index : candidates) {
    maximum = std::max(maximum,
      static_cast<double>(logits[index]) / config.samplingTemperature);
  }
  std::vector<double> weights;
  weights.reserve(candidates.size());
  double total = 0.0;
  for (const auto index : candidates) {
    const auto weight = std::exp(
      static_cast<double>(logits[index]) / config.samplingTemperature - maximum);
    if (!std::isfinite(weight)) {
      throw std::invalid_argument("native sampling distribution is non-finite");
    }
    weights.push_back(weight);
    total += weight;
  }
  if (!std::isfinite(total) || total <= 0.0) {
    throw std::invalid_argument("native sampling distribution is empty");
  }
  std::size_t retained = candidates.size();
  double cumulative = 0.0;
  for (std::size_t index = 0; index < weights.size(); ++index) {
    cumulative += weights[index] / total;
    if (cumulative >= config.samplingTopP) {
      retained = index + 1;
      break;
    }
  }
  double retainedTotal = 0.0;
  for (std::size_t index = 0; index < retained; ++index) {
    retainedTotal += weights[index];
  }
  if (!std::isfinite(retainedTotal) || retainedTotal <= 0.0) {
    throw std::invalid_argument("native sampling retained distribution is empty");
  }
  const auto draw = deterministicUnit(config.samplingSeed, step) * retainedTotal;
  double prefix = 0.0;
  for (std::size_t index = 0; index < retained; ++index) {
    prefix += weights[index];
    if (draw < prefix || index + 1 == retained) {
      return static_cast<std::int64_t>(candidates[index]);
    }
  }
  throw std::logic_error("native sampler failed to select a token");
}

std::string
jsonEscape(const std::string& value)
{
  std::ostringstream output;
  output << '"';
  for (const auto ch : value) {
    switch (ch) {
      case '"': output << "\\\""; break;
      case '\\': output << "\\\\"; break;
      case '\n': output << "\\n"; break;
      case '\r': output << "\\r"; break;
      case '\t': output << "\\t"; break;
      default: output << ch; break;
    }
  }
  output << '"';
  return output.str();
}

bool
hasStopSuffix(const std::string& text,
              const std::vector<std::string>& stopStrings)
{
  return std::any_of(stopStrings.begin(), stopStrings.end(), [&text] (const auto& stop) {
    return !stop.empty() && text.size() >= stop.size() &&
           text.compare(text.size() - stop.size(), stop.size(), stop) == 0;
  });
}

std::string
makeTokenEvent(std::int64_t token,
               std::size_t epoch,
               const std::vector<std::int64_t>& generated,
               const std::string& finishHint,
               const std::string& samplingDigest,
               const std::string& textDelta)
{
  std::ostringstream prefix;
  for (std::size_t i = 0; i < generated.size(); ++i) {
    if (i != 0) prefix << ',';
    prefix << generated[i];
  }
  ndn::util::Sha256 digest;
  digest << prefix.str();
  std::ostringstream event;
  event << "{\"schema\":\"GenerationTokenEventV1\","
        << "\"tokenId\":" << token
        << ",\"tokenEpoch\":" << epoch
        << ",\"cumulativeTokenCount\":" << epoch
        << ",\"textDelta\":" << jsonEscape(textDelta) << ","
        << "\"finishHint\":" << jsonEscape(finishHint) << ","
        << "\"samplingDigest\":" << jsonEscape(samplingDigest) << ","
        << "\"acceptedPrefixDigest\":\"sha256:" << digest.toString()
        << "\"}";
  return event.str();
}

std::vector<std::uint8_t>
makeFinalPayload(const std::vector<std::int64_t>& generated,
                 const std::string& finishHint,
                 const std::string& textValue)
{
  const auto finishReason = finishHint == "EOS" ? "eos" :
    finishHint == "STOP_SEQUENCE" ? "stop_sequence" :
    finishHint == "MAX_TOKENS" ? "max_tokens" : "failed";
  std::ostringstream text;
  text << "{\"schema\":\"NDNSF-DI-FINAL-V1\",\"finishHint\":"
       << jsonEscape(finishHint) << ",\"finishReason\":"
       << jsonEscape(finishReason) << ",\"text\":" << jsonEscape(textValue)
       << ",\"tokenIds\":[";
  for (std::size_t i = 0; i < generated.size(); ++i) {
    if (i != 0) text << ',';
    text << generated[i];
  }
  text << "]}";
  const auto value = text.str();
  return {value.begin(), value.end()};
}

} // namespace

bool
nativeRoleHasOnlyInternalFeedbackOutputs(const RoleSpec& role)
{
  return !role.outputs.empty() && std::all_of(
    role.outputs.begin(), role.outputs.end(), [] (const auto& edge) {
      return edge.operationKind == "TOKEN_FEEDBACK";
    });
}

NativeEpochCoordinatorResult
runNativeEpochCoordinator(NativeEpochCoordinatorConfig config)
{
  if (!config.io || config.sessionId.empty() || config.localProvider.empty() ||
      config.role.empty() || config.maxEpochs == 0 || config.streamEpoch == 0 ||
      config.lineagePlanDigest.empty() || config.positionPolicyDigest.empty() ||
      !config.stateIdentityTemplate) {
    throw std::invalid_argument("native epoch coordinator configuration is incomplete");
  }
  if ((!config.stopStrings.empty() || config.requireTextOutput) &&
      !config.textDecoder) {
    throw std::invalid_argument("NATIVE_TEXT_DECODER_REQUIRED");
  }
  if (config.textDecoder && !config.stableTextDecoder) {
    throw std::invalid_argument("NATIVE_STABLE_TEXT_DECODER_REQUIRED");
  }
  if (config.attemptEpoch < 1 || config.attemptEpoch > 2 ||
      (config.attemptEpoch == 1 && !config.committedPrefixTokenIds.empty()) ||
      config.committedPrefixTokenIds.size() >= config.maxEpochs ||
      std::any_of(config.committedPrefixTokenIds.begin(),
                  config.committedPrefixTokenIds.end(),
                  [&] (std::int64_t token) {
                    return token < 0 || config.eosTokenIds.count(token) != 0;
                  })) {
    throw std::invalid_argument(
      "native epoch coordinator recovery prefix is invalid");
  }
  if (config.conversationStateBinding) {
    config.conversationStateBinding->validate();
    if (config.conversationStateLookupNowMs == 0 ||
        config.stateInputNames.empty() ||
        config.conversationStateBinding->identity.roleName != config.role ||
        !config.stateIdentityTemplate ||
        config.conversationStateBinding->identity.requestId ==
          config.stateIdentityTemplate->requestId ||
        config.conversationStateBinding->identity.generationId ==
          config.stateIdentityTemplate->generationId) {
      throw std::invalid_argument(
        "native conversation continuation requires a fresh stateful request");
    }
  }
  if (config.maxCheckpointFinalizeTokens == 0 ||
      config.maxCheckpointFinalizeTokens > 32) {
    throw std::invalid_argument(
      "native checkpoint finalization bound is out of range");
  }
  const auto feedback = feedbackScopeFor(
    roleSpecFor(config.plan, config.role, config.sessionId,
                config.assignment, config.localProvider, 0));
  const bool terminalRole = feedback.has_value();
  if (!terminalRole && config.eventSink) {
    throw std::invalid_argument("only the terminal role may publish token events");
  }

  NativeEpochCoordinatorResult result;
  DecodeStateReleaseGuard stateRelease(config.runtime,
                                       config.sessionId,
                                       config.role);
  std::vector<std::int64_t> generated;
  generated.reserve(config.maxEpochs);
  std::string generatedText;
  std::optional<DecodeStateIdentityV1> committedStateIdentity;
  std::string finishHint = "MAX_TOKENS";
  // A non-terminal upstream role needs one bounded drain epoch after the
  // terminal role emits its final feedback.  That epoch only observes the
  // terminal marker and returns before invoking ONNX Runtime; it is not an
  // additional token/model step.  The terminal role itself returns at
  // maxGeneratedTokens, so this does not extend generation.
  for (std::size_t epoch = 0; epoch <= config.maxEpochs; ++epoch) {
    throwIfStopped(config);
    traceEpoch("epoch_start", config.role, epoch);
    auto role = roleSpecFor(config.plan, config.role, config.sessionId,
                            config.assignment, config.localProvider, epoch);
    role.requestId = config.requestId.empty() ? config.sessionId : config.requestId;
    role.attemptEpoch = config.attemptEpoch;
    for (auto& edge : role.inputs) {
      edge.requestId = role.requestId;
      edge.attemptEpoch = role.attemptEpoch;
    }
    for (auto& edge : role.outputs) {
      edge.requestId = role.requestId;
      edge.attemptEpoch = role.attemptEpoch;
    }

    // The first prefill has no predecessor token.  The signed plan still
    // declares the feedback edge for decode epochs, but epoch zero removes it
    // from this role projection before the worker validates its inputs.
    RoleSpec executable = role;
    executable.inferenceEpoch = epoch;
    executable.stateInputNames = config.stateInputNames;
    executable.stateOutputNames = config.stateOutputNames;
    executable.deferStateCommit = !config.stateInputNames.empty();
    executable.streamingStateExecution = !config.stateInputNames.empty();
    if (epoch == 0 && config.conversationStateBinding) {
      executable.conversationStateBinding = config.conversationStateBinding;
      executable.conversationStateLookupNowMs =
        config.conversationStateLookupNowMs;
    }
    if (epoch == 0) {
      executable.inputs.erase(
        std::remove_if(executable.inputs.begin(), executable.inputs.end(),
                       [] (const auto& edge) {
                         return edge.operationKind == "TOKEN_FEEDBACK";
                       }),
        executable.inputs.end());
    }
    std::map<std::string, TensorBundle> inputs;
    if (epoch == 0) {
      inputs.insert(config.initialInputs.begin(), config.initialInputs.end());
    }
    inputs = fetchInputs(config, executable, std::move(inputs));
    throwIfStopped(config);
    // Terminal control markers normally describe only the bounded in-band
    // drain. Conversation-enabled turns carry the final token lineage as well;
    // those roles perform one state-only finalization pass before forwarding
    // the marker. A marker without lineage remains the ordinary no-ORT drain.
    bool checkpointFinalization = false;
    std::optional<GenerationEpochLineageV1> terminalLineage;
    for (const auto& edge : executable.inputs) {
      if (edge.operationKind != "ACTIVATION") {
        continue;
      }
      const auto found = inputs.find(edge.scope);
      if (found != inputs.end() && isTerminalActivation(found->second)) {
        terminalLineage = extractGenerationEpochLineage(found->second);
        checkpointFinalization = terminalLineage.has_value() &&
          terminalLineage->transitionKind ==
            GenerationEpochLineageV1::CHECKPOINT_FINALIZE;
        if (!config.checkpointFinalize || !checkpointFinalization) {
          publishTerminalActivation(config, role);
          result.stoppedByUpstream = true;
          return result;
        }
      }
    }
    if (!executable.inputs.empty()) {
      for (const auto& edge : executable.inputs) {
        if (edge.operationKind == "TOKEN_FEEDBACK") {
          const auto found = inputs.find(edge.scope);
          if (found == inputs.end()) {
            throw std::runtime_error("TOKEN_FEEDBACK input was not fetched");
          }
          if (isTerminalFeedback(found->second)) {
            terminalLineage = extractGenerationEpochLineage(found->second);
            checkpointFinalization = terminalLineage.has_value() &&
              terminalLineage->transitionKind ==
                GenerationEpochLineageV1::CHECKPOINT_FINALIZE;
            if (!config.checkpointFinalize || !checkpointFinalization) {
              publishTerminalActivation(config, role);
              result.stoppedByUpstream = true;
              return result;
            }
          }
        }
      }
    }

    auto epochLineage = extractAndVerifyInputLineage(executable, inputs);
    if (!epochLineage) {
      if (epoch != 0) {
        throw std::runtime_error("GENERATION_EPOCH_LINEAGE_MISSING");
      }
      epochLineage = initialGenerationLineage(config, inputs);
    }
    if (!config.stateIdentityTemplate ||
        epochLineage->requestId != executable.requestId ||
        epochLineage->attemptEpoch != executable.attemptEpoch ||
        epochLineage->streamEpoch != config.streamEpoch ||
        epochLineage->planDigest != config.lineagePlanDigest ||
        epochLineage->generationId != config.stateIdentityTemplate->generationId ||
        epochLineage->inferenceEpoch != epoch) {
      throw std::runtime_error("GENERATION_EPOCH_LINEAGE_AUTHORITY_MISMATCH");
    }
    executable.generationLineage = *epochLineage;
    NativeEpochCoordinatorResult::CacheObservation cacheObservation;
    cacheObservation.inferenceEpoch = epoch;
    cacheObservation.actualNewInputExtent = actualNewInputExtent(
      inputs, config.stateInputNames, config.tokenInputName);
    cacheObservation.representedPrefixTokenCount =
      epochLineage->logicalPrefixTokenCount;
    // A conversation continuation starts request-local epoch zero from an
    // already promoted parent state.  Count that exact parent prefix as
    // avoided work; it is distinct from `decodeStateHit`, which describes a
    // predecessor produced by an earlier epoch of this Request.
    cacheObservation.prefixWorkAvoided = committedStateIdentity
      ? committedStateIdentity->prefixTokenCount
      : (epoch == 0 && config.conversationStateBinding
           ? config.conversationStateBinding->identity.prefixTokenCount
           : 0);
    cacheObservation.decodeStateHit = committedStateIdentity.has_value();
    cacheObservation.conversationStateHit =
      epoch == 0 && config.conversationStateBinding.has_value();
    if (!config.stateInputNames.empty()) {
      executable.predecessorDecodeStateIdentity = committedStateIdentity;
      executable.candidateDecodeStateIdentity = decodeStateIdentityForEpoch(
        config, *epochLineage, committedStateIdentity, epoch);
      if (epoch > 0 && !committedStateIdentity) {
        throw std::runtime_error("PROVIDER_DECODE_STATE_MISSING");
      }
    }
    traceEpoch("inputs_ready", config.role, epoch);

    // The terminal role's feedback is an internal Data edge.  The worker
    // publishes ordinary activation outputs, while the coordinator creates
    // the token feedback bundle from logits after run() returns.
    executable.outputs.erase(
      std::remove_if(executable.outputs.begin(), executable.outputs.end(),
                     [] (const auto& edge) {
                       return edge.operationKind == "TOKEN_FEEDBACK";
                     }),
      executable.outputs.end());
    if (checkpointFinalization) {
      if (!terminalLineage ||
          terminalLineage->transitionKind !=
            GenerationEpochLineageV1::CHECKPOINT_FINALIZE ||
          terminalLineage->inferenceEpoch != epoch ||
          epoch > config.maxCheckpointFinalizeTokens + config.maxEpochs) {
        throw std::runtime_error(
          "CHECKPOINT_FINALIZE lineage exceeds the sealed bound");
      }
      // A state-only finalization must not publish a regular activation. The
      // marker is forwarded after the candidate state commits below.
      executable.outputs.clear();
    }
    throwIfStopped(config);
    auto roleFuture = config.runtime.executeRoleAsync(
      config.sessionId, executable, config.io, std::move(inputs), {},
      [stopCheck = config.stopCheck, executionGuard = config.executionGuard] {
        throwIfStopped(stopCheck, executionGuard);
      });
    traceEpoch("role_submitted", config.role, epoch);
    auto roleResult = roleFuture.get();
    traceEpoch("role_done", config.role, epoch);
    try {
      throwIfStopped(config);
      if (config.resultObserver) {
        config.resultObserver(executable, roleResult);
      }
      throwIfStopped(config);
      if (roleResult.runtimeMetrics) {
        result.runtimeMetrics.push_back(
          NativeEpochCoordinatorResult::RuntimeMetricsObservation{
            executable.role, epoch, *roleResult.runtimeMetrics});
      }
      result.cacheObservations.push_back(cacheObservation);
      ++result.epochsExecuted;

      if (!terminalRole) {
        throwIfStopped(config);
        if (executable.deferStateCommit &&
            !config.runtime.commitDecodeStateTransition(
              config.sessionId, executable)) {
          throw std::runtime_error("PROVIDER_DECODE_STATE_COMMIT_FAILED");
        }
        if (executable.candidateDecodeStateIdentity) {
          committedStateIdentity = executable.candidateDecodeStateIdentity;
          result.finalizedRole = executable;
        }
        if (checkpointFinalization) {
          publishTerminalActivation(config, role, epochLineage);
          result.stoppedByUpstream = true;
          return result;
        }
        continue;
      }
      const auto token = sampleToken(roleResult.outputsByScope, config,
                                     generated, epoch);
      const auto candidateTokenIds = [&] {
        auto value = generated;
        value.push_back(token);
        return value;
      }();
      std::string candidateText;
      std::string stableCandidateText;
      std::string textDelta;
      if (config.textDecoder) {
        candidateText = config.textDecoder(candidateTokenIds);
      }
      else if (config.requireTextOutput) {
        throw std::runtime_error("NATIVE_TEXT_DECODER_REQUIRED");
      }
      const bool replayingCommittedPrefix =
        epoch < config.committedPrefixTokenIds.size();
      if (replayingCommittedPrefix &&
          token != config.committedPrefixTokenIds[epoch]) {
        throw std::runtime_error(
          "native epoch coordinator recomputed prefix mismatch");
      }
      const bool eos = config.eosTokenIds.count(token) != 0;
      const bool atMax = epoch + 1 >= config.maxEpochs;
      const bool stopSequence = !eos && hasStopSuffix(
        candidateText, config.stopStrings);
      finishHint = eos ? "EOS" : stopSequence ? "STOP_SEQUENCE" :
                   atMax ? "MAX_TOKENS" : "NONE";
      if (config.textDecoder) {
        // Flush withheld bytes into the terminal event before any acceptance
        // or state commit. Stop detection above requires the complete decode.
        stableCandidateText = config.stableTextDecoder(
          candidateTokenIds, eos || stopSequence || atMax);
        if ((eos || stopSequence || atMax) && stableCandidateText != candidateText) {
          throw std::runtime_error("NATIVE_FINAL_TEXT_DECODE_MISMATCH");
        }
        if (stableCandidateText.size() < generatedText.size() ||
            stableCandidateText.compare(0, generatedText.size(), generatedText) != 0) {
          throw std::runtime_error("native tokenizer rewrote committed text prefix");
        }
        textDelta = stableCandidateText.substr(generatedText.size());
      }
      if (replayingCommittedPrefix) {
        ++result.prefixTokensRecomputed;
      }
      else {
        const auto event = makeTokenEvent(token, epoch + 1, candidateTokenIds,
                                          finishHint, config.samplingDigest,
                                          textDelta);
        throwIfStopped(config);
        if (config.eventSink &&
            !config.eventSink(std::vector<std::uint8_t>(event.begin(), event.end()))) {
          throw std::runtime_error("native epoch token event admission was rejected");
        }
        if (config.eventSink) {
          ++result.eventsPublished;
        }
        throwIfStopped(config);
      }
      generated.push_back(token);
      if (config.textDecoder) {
        generatedText = std::move(stableCandidateText);
      }

      // Feedback is produced for the next decode epoch.  Use the next
      // sequence projection so its DATA_V1 operation index and exact name are
      // identical to the input edge that the upstream role will fetch on its
      // next epoch.  Publishing the current projection would create a
      // one-epoch operation/name mismatch under a streaming stride.
      const auto nextRole = roleSpecFor(
        config.plan, config.role, config.sessionId, config.assignment,
        config.localProvider, epoch + 1);
      const auto feedbackEdge = std::find_if(
        nextRole.outputs.begin(), nextRole.outputs.end(), [] (const auto& edge) {
          return edge.operationKind == "TOKEN_FEEDBACK";
        });
      if (feedbackEdge == nextRole.outputs.end()) {
        throw std::runtime_error("terminal role is missing TOKEN_FEEDBACK output");
      }
      const bool terminal = eos || stopSequence || atMax;
      auto feedbackBundle = makeTokenFeedback(token, terminal);
      if (!terminal) {
        feedbackBundle = attachGenerationEpochLineage(
          feedbackBundle,
          lineageForEdge(
            nextGenerationLineage(config, *epochLineage, token),
            *feedbackEdge));
      }
      else if (config.checkpointFinalize) {
        // Preserve the final-token lineage for the bounded state-only pass
        // through non-terminal roles. Ordinary streams retain the historical
        // marker-only drain semantics.
        auto finalizationLineage = nextGenerationLineage(
          config, *epochLineage, token);
        finalizationLineage.transitionKind =
          GenerationEpochLineageV1::CHECKPOINT_FINALIZE;
        feedbackBundle = attachGenerationEpochLineage(
          feedbackBundle,
          lineageForEdge(
            std::move(finalizationLineage),
            *feedbackEdge));
      }
      throwIfStopped(config);
      config.io->publishOutput(config.sessionId, *feedbackEdge, feedbackBundle);
      throwIfStopped(config);
      if (executable.deferStateCommit &&
          !config.runtime.commitDecodeStateTransition(
            config.sessionId, executable)) {
        throw std::runtime_error("PROVIDER_DECODE_STATE_COMMIT_FAILED");
      }
      if (executable.candidateDecodeStateIdentity) {
        committedStateIdentity = executable.candidateDecodeStateIdentity;
        result.finalizedRole = executable;
      }
      if (eos || stopSequence || atMax) {
        result.finalPayload = makeFinalPayload(generated, finishHint,
                                               config.textDecoder ? candidateText : generatedText);
        return result;
      }
    }
    catch (...) {
      if (executable.deferStateCommit) {
        config.runtime.rollbackDecodeStateTransition(
          config.sessionId, executable);
      }
      throw;
    }
  }
  throw std::runtime_error("native epoch coordinator exhausted without terminal response");
}

} // namespace ndnsf::di

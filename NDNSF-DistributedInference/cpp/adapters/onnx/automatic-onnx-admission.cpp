#include "automatic-onnx-admission.hpp"
#include "automatic-onnx-io.hpp"
#include "prepared-onnx-metadata.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativeSelectionJsonValues.hpp"

#include <algorithm>
#include <chrono>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace ndnsf::di {
namespace {

void
active(const NativeAssemblyControl& control)
{
  if (!control.requireActive)
    throw std::invalid_argument("AUTOMATIC_SOURCE_OWNER");
  control.requireActive();
  if (std::chrono::steady_clock::now() >= control.deadline)
    throw std::runtime_error("AUTOMATIC_SOURCE_DEADLINE");
}

[[noreturn]] void
reject(const char* reason)
{
  throw std::invalid_argument(reason);
}

void
exactKeys(const NativeJson& value, std::initializer_list<const char*> keys)
{
  if (!value.is_object() || value.size() != keys.size())
    reject("AUTOMATIC_SOURCE_BINDING_FORMAT");
  for (const auto* key : keys)
    if (!value.contains(key)) reject("AUTOMATIC_SOURCE_BINDING_FORMAT");
}

std::string
rawName(const std::string& scoped)
{
  if (scoped.size() <= 6 || scoped.compare(0, 6, "graph/") != 0)
    reject("AUTOMATIC_SOURCE_SCOPE");
  return scoped.substr(6);
}

void
validatePort(const NativeAssemblyTensorContractV3& port,
             const AutomaticOnnxContract& contract,
             const ExecutionShapeEnvelope& envelope,
             const NativeAssemblyControl& control)
{
  active(control);
  const auto name = "graph/" + port.name;
  const auto tensor = std::find_if(contract.tensors.begin(), contract.tensors.end(),
    [&] (const auto& item) { return item.name == name; });
  const auto bounds = envelope.tensors.find(name);
  if (tensor == contract.tensors.end() || !tensor->wireElementType ||
      tensorElementTypeName(*tensor->wireElementType) != port.dtype ||
      bounds == envelope.tensors.end() || !bounds->second.maxBytes ||
      !bounds->second.maxEncodedBytes || !bounds->second.maximumDimensions ||
      !std::all_of(bounds->second.maximumDimensions->begin(),
                   bounds->second.maximumDimensions->end(), [] (const auto& dimension) {
                     return dimension && *dimension <=
                       std::uint64_t(std::numeric_limits<std::int64_t>::max());
                   })) {
    reject("AUTOMATIC_SOURCE_STAGE_BOUNDARY");
  }
  if ((!port.shape.empty() || tensor->dimensions) &&
      port.shape.size() != bounds->second.maximumDimensions->size())
    reject("AUTOMATIC_SOURCE_STAGE_RANK");
  if (!tensor->dimensions) return;
  if (port.shape.size() != tensor->dimensions->size())
    reject("AUTOMATIC_SOURCE_STAGE_RANK");
  for (std::size_t axis = 0; axis < port.shape.size(); ++axis) {
    active(control);
    const auto& source = tensor->dimensions->at(axis);
    const auto* fixed = std::get_if<std::int64_t>(&port.shape[axis]);
    if (source.kind == DimensionConstraint::Kind::FIXED) {
      if (!fixed || *fixed != source.fixed) reject("AUTOMATIC_SOURCE_STAGE_DIMENSION");
    }
    else if (fixed) {
      reject("AUTOMATIC_SOURCE_STAGE_SAMPLE_DIMENSION");
    }
  }
}

} // namespace

AutomaticOnnxRequestBinding::AutomaticOnnxRequestBinding(
  std::shared_ptr<const AutomaticOnnxContract> source, ExecutionShapeEnvelope envelope,
  const NativeSelectionProjectionV3& projection, const NativeRequestControl& control)
  : m_source(std::move(source))
  , m_envelope(std::move(envelope))
  , m_requestId(projection.requestId)
  , m_attempt(projection.attempt)
  , m_role(projection.executionRole.roleId)
  , m_assemblyRole(projection.assemblyContract().selectedRole)
  , m_backend(projection.assemblyContract().backend)
  , m_recipe(projection.assemblyContract().recipeDigest)
  , m_deadline(control.deadline)
  , m_maxSourceBytes(projection.assemblyContract().maxSourceBytes)
  , m_maxMetadataBytes(std::min(projection.assemblyContract().maxAssembledBytes,
                                NativeCanonicalMaterialPublicationMaxBytes))
{
  const auto& role = projection.assemblyContract();
  for (const auto& input : role.expectedInputs) m_inputs.push_back("graph/" + input.name);
  for (const auto& output : role.expectedOutputs) m_outputs.push_back("graph/" + output.name);
  if (m_envelope.maxNewTokens != 0) {
    const auto& generation = *m_source->generation;
    for (const auto* name : {&generation.tokenInput, &generation.maskInput,
                             &generation.positionInput}) {
      if (!name->empty() && std::find(m_inputs.begin(), m_inputs.end(), *name) != m_inputs.end())
        m_controlInputs.push_back(rawName(*name));
    }
  }
}

std::shared_ptr<const AutomaticOnnxRequestBinding>
AutomaticOnnxRequestBinding::admit(const AutomaticOnnxSourceBinding& source,
                                  const NativeSelectionProjectionV3& projection,
                                  const NativeRequestControl& control)
{
  control.requireActive();
  if (control.requestId != projection.requestId || control.attempt != projection.attempt)
    reject("AUTOMATIC_REQUEST_CONTROL_BINDING");
  const auto& role = projection.assemblyContract();
  const NativeAssemblyControl admissionControl{control.deadline,
    [&control] { control.requireActive(); }, role.maxSourceBytes,
    std::min(role.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes)};
  auto envelope = admitAutomaticOnnxProjection(source, projection, admissionControl);
  auto result = std::shared_ptr<const AutomaticOnnxRequestBinding>(
    new AutomaticOnnxRequestBinding(source.m_contract, std::move(envelope), projection, control));
  control.requireActive();
  return result;
}

NativeAssemblyControl
AutomaticOnnxRequestBinding::validationControl(const RoleExecutionContext& ctx) const
{
  if (ctx.requestId != m_requestId || ctx.attemptEpoch != m_attempt || ctx.role != m_role)
    reject("AUTOMATIC_REQUEST_EXECUTION_BINDING");
  return {m_deadline, [&ctx] { if (ctx.executionGuard) ctx.executionGuard(); },
          m_maxSourceBytes, m_maxMetadataBytes};
}

void
AutomaticOnnxRequestBinding::requireInvocation(const RoleExecutionContext& ctx,
                                              const NativeModelRunnerSpec& spec,
                                              const AutomaticOnnxContract& loadedSource) const
{
  const auto control = validationControl(ctx);
  active(control);
  const auto recipe = spec.metadata.find("recipeDigest");
  if (&loadedSource != m_source.get() || ctx.requestId != m_requestId ||
      ctx.attemptEpoch != m_attempt || ctx.role != m_role || spec.role != m_assemblyRole ||
      spec.backend != m_backend || recipe == spec.metadata.end() || recipe->second != m_recipe)
    reject("AUTOMATIC_REQUEST_EXECUTION_BINDING");
}

AutomaticOnnxSourceBinding::AutomaticOnnxSourceBinding(
  std::string modelDigest, std::uint64_t initializerBytes, std::string initializerObjectDigest,
  std::vector<AutomaticOnnxSourceFile> files,
  std::shared_ptr<const AutomaticOnnxContract> contract)
  : m_modelDigest(std::move(modelDigest))
  , m_initializerBytes(initializerBytes)
  , m_initializerObjectDigest(std::move(initializerObjectDigest))
  , m_files(std::move(files))
  , m_contract(std::move(contract))
{
}

AutomaticOnnxSourceBinding
AutomaticOnnxSourceBinding::fromSourceMetadata(
  const NativeJson& automatic, const std::string& modelDigest, std::uint64_t initializerBytes,
  const std::string& initializerObjectDigest, const NativeAssemblyControl& control)
{
  auto contract = decodeNativeAutomaticOnnxSourceMetadata(
    automatic, modelDigest, initializerBytes, initializerObjectDigest, control);
  std::vector<AutomaticOnnxSourceFile> files;
  files.reserve(automatic.at("externalFiles").size());
  for (const auto& file : automatic.at("externalFiles")) {
    active(control);
    // The shared decoder already checked these exact fields and the inventory.
    files.push_back({file.at("location").get<std::string>(), file.at("bytes").get<std::uint64_t>(),
                     file.at("digest").get<std::string>()});
  }
  return AutomaticOnnxSourceBinding(modelDigest, initializerBytes, initializerObjectDigest,
                                    std::move(files), std::move(contract));
}

AutomaticOnnxSourceBinding
AutomaticOnnxSourceBinding::fromCanonicalJson(const std::string& encoded,
                                             const NativeAssemblyControl& control)
{
  active(control);
  if (encoded.empty() || encoded.size() >
      std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes))
    reject("AUTOMATIC_SOURCE_BINDING_LIMIT");
  try {
    const auto value = nativeParseJson(encoded, 64);
    exactKeys(value, {"schema", "modelDigest", "initializerBytes", "initializerObjectDigest",
                      "automaticSource"});
    if (value.at("schema") != NativeJson("ndnsf-di-automatic-source-binding-v1") ||
        !value.at("modelDigest").is_string() || !value.at("initializerBytes").is_number_unsigned() ||
        !value.at("initializerObjectDigest").is_string() || nativeCanonicalJson(value) != encoded)
      reject("AUTOMATIC_SOURCE_BINDING_FORMAT");
    return fromSourceMetadata(value.at("automaticSource"), value.at("modelDigest").get<std::string>(),
      value.at("initializerBytes").get<std::uint64_t>(),
      value.at("initializerObjectDigest").get<std::string>(), control);
  }
  catch (const NativeJson::exception&) {
    reject("AUTOMATIC_SOURCE_BINDING_FORMAT");
  }
}

std::string
AutomaticOnnxSourceBinding::canonicalJson(const NativeAssemblyControl& control) const
{
  active(control);
  auto files = NativeJson::array();
  for (const auto& file : m_files) {
    active(control);
    files.push_back({{"location", file.location}, {"bytes", file.bytes}, {"digest", file.digest}});
  }
  auto encoded = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-automatic-source-binding-v1"}, {"modelDigest", m_modelDigest},
    {"initializerBytes", m_initializerBytes}, {"initializerObjectDigest", m_initializerObjectDigest},
    {"automaticSource", {{"contract", nativeParseJson(m_contract->canonicalJson(control), 64)},
                          {"externalFiles", std::move(files)}}}});
  if (encoded.size() > std::min(control.maxAssembledBytes, NativeCanonicalMaterialPublicationMaxBytes))
    reject("AUTOMATIC_SOURCE_BINDING_LIMIT");
  active(control);
  return encoded;
}

ExecutionShapeEnvelope
admitAutomaticOnnxProjection(const AutomaticOnnxSourceBinding& binding,
                             const NativeSelectionProjectionV3& projection,
                             const NativeAssemblyControl& control)
{
  active(control);
  validateNativeAutomaticSelectionBinding(projection);
  if (!projection.automaticShapeSeed) reject("AUTOMATIC_SOURCE_SEED_REQUIRED");
  (void)automaticOnnxIoContractForProjection(projection);
  const auto& contract = binding.sourceContract();
  auto envelope = rebindAutomaticOnnxShapeSeed(contract, *projection.automaticShapeSeed, control);
  for (const auto* ports : {&projection.assemblyContract().expectedInputs,
                            &projection.assemblyContract().expectedOutputs}) {
    for (const auto& port : *ports) validatePort(port, contract, envelope, control);
  }
  if (projection.generationContract.enabled) {
    if (!contract.generation) reject("AUTOMATIC_SOURCE_GENERATION_REQUIRED");
    const auto& source = *contract.generation;
    const auto& generation = projection.generationContract;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    for (const auto& state : source.states) {
      active(control);
      inputs.push_back(rawName(state.input));
      outputs.push_back(rawName(state.output));
    }
    const auto sameOptional = [&] (const auto& actual, const auto& expected) {
      return actual.empty() || (!expected.empty() && actual == rawName(expected));
    };
    const auto tokens = envelope.inputShapes.find(source.tokenInput);
    if (generation.tokenInputName != rawName(source.tokenInput) ||
        generation.stateInputNames != inputs || generation.stateOutputNames != outputs ||
        !sameOptional(generation.attentionMaskInputName, source.maskInput) ||
        !sameOptional(generation.positionIdsInputName, source.positionInput) ||
        !generation.cachePositionInputName.empty() || tokens == envelope.inputShapes.end() ||
        tokens->second.size() != 2 || tokens->second.at(source.tokenBatchAxis) != 1 ||
        std::any_of(generation.eosTokenIds.begin(), generation.eosTokenIds.end(),
          [&] (std::int64_t token) {
            return token < 0 || std::uint64_t(token) >= source.vocabularySize;
          })) {
      reject("AUTOMATIC_SOURCE_GENERATION_INTERFACE");
    }
  }
  active(control);
  return envelope;
}

void
validateAutomaticOnnxRunnerSource(const NativeModelRunnerSpec& spec,
                                  const NativeSelectionProjectionV3& projection,
                                  const NativeAssemblyControl& control)
{
  active(control);
  if (!projection.automaticShapeSeed) {
    if (spec.metadata.count("automaticSourceBinding") != 0)
      reject("AUTOMATIC_SOURCE_LEGACY_PROJECTION");
    return;
  }
  const auto recipe = spec.metadata.find("recipeDigest");
  const auto source = spec.metadata.find("automaticSourceBinding");
  if (spec.role != projection.assemblyContract().selectedRole ||
      spec.backend != projection.assemblyContract().backend || recipe == spec.metadata.end() ||
      recipe->second != projection.assemblyContract().recipeDigest || source == spec.metadata.end())
    reject("AUTOMATIC_SOURCE_RUNNER_BINDING");
  const auto binding = AutomaticOnnxSourceBinding::fromCanonicalJson(source->second, control);
  (void)admitAutomaticOnnxProjection(binding, projection, control);
}

} // namespace ndnsf::di

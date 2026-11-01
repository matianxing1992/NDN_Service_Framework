#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include <boost/test/unit_test.hpp>
#include <openssl/evp.h>

using namespace ndnsf::di;

namespace {
const std::string digest = "sha256:" + std::string(64, 'a');

DecodeStateIdentityV1 identity()
{
  DecodeStateIdentityV1 result;
  result.modelDigest = digest;
  result.graphSemanticDigest = digest;
  result.artifactDigest = digest;
  result.adapterDigest = digest;
  result.tokenizerDigest = digest;
  result.runnerDigest = digest;
  result.roleName = "decoder";
  result.roleSplitDigest = digest;
  result.layerEnd = 1;
  result.prefixDigest = digest;
  result.prefixTokenCount = 2;
  result.positionDigest = digest;
  result.precision = "float32";
  result.layoutDigest = digest;
  result.stateSchemaDigest = digest;
  result.stateComponentDigests = {digest};
  result.runtimeAbiDigest = digest;
  result.securityDomainDigest = digest;
  result.providerIdentity = "/provider/a";
  result.providerBootId = "boot-a";
  result.requestId = "request-a";
  result.generationId = "generation-a";
  return result;
}

std::string hash(const std::vector<std::uint8_t>& wire)
{
  unsigned char bytes[EVP_MAX_MD_SIZE];
  unsigned length = 0;
  BOOST_REQUIRE_EQUAL(EVP_Digest(wire.data(), wire.size(), bytes, &length, EVP_sha256(), nullptr), 1);
  BOOST_REQUIRE_EQUAL(length, 32U);
  std::string result = "sha256:";
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < length; ++i) {
    result.push_back(hex[bytes[i] >> 4]); result.push_back(hex[bytes[i] & 15]);
  }
  return result;
}

NativePortableKvSnapshotV1 snapshot()
{
  NativePortableKvComponentV1 component;
  component.inputName = "past";
  component.elementType = TensorElementType::Float32;
  component.shape = {2};
  component.payloadBytes = 8;
  component.tensorWire = encodeTensorBundle({NamedTensor{
    "past", TensorElementType::Float32, {2}, {0, 0, 128, 63, 0, 0, 0, 64}}});
  component.contentDigest = hash(component.tensorWire);
  NativePortableKvSnapshotV1 result;
  result.originIdentity = identity();
  result.stateIoDigest = digest;
  result.components = {component};
  result.logicalBytes = 8;
  result.encodedBytes = component.tensorWire.size();
  return result;
}

NativePortableKvLimitsV1 limits()
{
  return {64, 1024, {}};
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec204PortableKvTypes)

BOOST_AUTO_TEST_CASE(ValidCodecStatePreservesExactBytes)
{
  const auto value = snapshot();
  BOOST_CHECK_NO_THROW(validateNativePortableKvSnapshot(value, limits()));
  BOOST_CHECK_NO_THROW(validateNativePortableKvSnapshot(value, {8, value.encodedBytes, {}}));
  const auto decoded = decodeTensorBundle(value.components[0].tensorWire);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_CHECK_EQUAL(decoded[0].payload.size(), 8U);
  BOOST_CHECK_EQUAL(decoded[0].payload[2], 128U);
  BOOST_CHECK_EQUAL(decoded[0].payload[7], 64U);
}

BOOST_AUTO_TEST_CASE(DynamicSuccessorInputNameIsPreservedExactly)
{
  const std::string inputName = "past_key_values.17.key";
  auto value = snapshot();
  auto& component = value.components[0];
  component.inputName = inputName;
  component.shape = {1};
  component.payloadBytes = 4;
  component.tensorWire = encodeTensorBundle({NamedTensor{
    inputName, TensorElementType::Float32, {1}, {0, 0, 128, 63}}});
  component.contentDigest = hash(component.tensorWire);
  value.logicalBytes = component.payloadBytes;
  value.encodedBytes = component.tensorWire.size();

  BOOST_CHECK_NO_THROW(validateNativePortableKvSnapshot(value, limits()));
  const auto decoded = decodeTensorBundle(component.tensorWire);
  BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
  BOOST_CHECK_EQUAL(decoded[0].name, inputName);

  auto mismatched = value;
  mismatched.components[0].tensorWire = encodeTensorBundle({NamedTensor{
    "past_key_values.17.value", TensorElementType::Float32, {1}, {0, 0, 128, 63}}});
  mismatched.components[0].contentDigest = hash(mismatched.components[0].tensorWire);
  mismatched.encodedBytes = mismatched.components[0].tensorWire.size();
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(mismatched, limits()),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AllSupportedElementTypesPreserveDeclaredWidthAndPayload)
{
  const std::vector<NamedTensor> expected{
    {"state-f32", TensorElementType::Float32, {1}, {0, 0, 128, 63}},
    {"state-f16", TensorElementType::Float16, {1}, {0, 60}},
    {"state-i64", TensorElementType::Int64, {1}, {1, 0, 0, 0, 0, 0, 0, 0}},
    {"state-bool", TensorElementType::Bool, {1}, {1}},
    {"state-u8", TensorElementType::UInt8, {1}, {255}},
  };
  NativePortableKvSnapshotV1 value;
  value.originIdentity = identity();
  value.stateIoDigest = digest;
  for (const auto& tensor : expected) {
    NativePortableKvComponentV1 component;
    component.inputName = tensor.name;
    component.elementType = tensor.elementType;
    component.shape = tensor.shape;
    component.payloadBytes = tensor.payload.size();
    component.tensorWire = encodeTensorBundle({tensor});
    component.contentDigest = hash(component.tensorWire);
    value.logicalBytes += component.payloadBytes;
    value.encodedBytes += component.tensorWire.size();
    value.components.push_back(std::move(component));
  }

  BOOST_CHECK_EQUAL(value.logicalBytes, 16U);
  BOOST_CHECK_NO_THROW(validateNativePortableKvSnapshot(value, limits()));
  BOOST_REQUIRE_EQUAL(value.components.size(), expected.size());
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const auto decoded = decodeTensorBundle(value.components[i].tensorWire);
    BOOST_REQUIRE_EQUAL(decoded.size(), 1U);
    BOOST_CHECK_EQUAL(decoded[0].name, expected[i].name);
    BOOST_CHECK(decoded[0].elementType == expected[i].elementType);
    BOOST_CHECK(decoded[0].shape == expected[i].shape);
    BOOST_CHECK_EQUAL(decoded[0].payload.size(), expected[i].payload.size());
    BOOST_CHECK(decoded[0].payload == expected[i].payload);
  }

  auto unsupported = snapshot();
  auto& component = unsupported.components[0];
  component.elementType = static_cast<TensorElementType>(6);
  const auto typeOffset = 8U + 2U * sizeof(std::uint32_t) + component.inputName.size();
  BOOST_REQUIRE_LE(typeOffset + sizeof(std::uint32_t), component.tensorWire.size());
  for (std::size_t i = 0; i < sizeof(std::uint32_t); ++i)
    component.tensorWire[typeOffset + i] = i == 0 ? 6 : 0;
  component.contentDigest = hash(component.tensorWire);
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(unsupported, limits()),
                    std::invalid_argument);

  BOOST_CHECK_THROW(encodeTensorBundle({NamedTensor{
    "unsupported", static_cast<TensorElementType>(6), {1}, {1}}}),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ActualFramingRejectsMisdeclaredPayloadBeforeDecode)
{
  auto value = snapshot();
  value.components[0].payloadBytes = 4;
  value.logicalBytes = 4;
  auto budget = limits(); budget.maxPayloadBytes = 4;
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, budget), std::invalid_argument);
  value = snapshot(); value.components[0].shape = {1};
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, limits()), std::invalid_argument);
  value = snapshot(); value.components[0].tensorWire.pop_back();
  value.encodedBytes = value.components[0].tensorWire.size();
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, limits()), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(RejectsCorruptionDuplicatesAndBudgetOverflow)
{
  auto value = snapshot(); value.components[0].tensorWire.back() ^= 1;
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, limits()), std::invalid_argument);
  value = snapshot(); value.components.push_back(value.components[0]);
  value.logicalBytes *= 2; value.encodedBytes *= 2;
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, limits()), std::invalid_argument);
  value = snapshot(); auto budget = limits(); budget.maxPayloadBytes = 8;
  budget.maxEncodedBytes = value.encodedBytes - 1;
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, budget), std::invalid_argument);
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, {}), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(EmptyTotalStateAndCancellationAreExplicitFailures)
{
  auto value = snapshot();
  auto& component = value.components[0];
  component.shape = {0}; component.payloadBytes = 0;
  component.tensorWire = encodeTensorBundle({NamedTensor{"past", TensorElementType::Float32, {0}, {}}});
  component.contentDigest = hash(component.tensorWire);
  value.logicalBytes = 0; value.encodedBytes = component.tensorWire.size();
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(value, limits()), std::invalid_argument);
  auto budget = limits(); budget.cancelled = [] { return true; };
  BOOST_CHECK_THROW(validateNativePortableKvSnapshot(snapshot(), budget), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DestinationIncarnationCanChangeButSemanticsCannot)
{
  const auto origin = identity(); auto destination = origin;
  destination.providerIdentity = "/provider/b";
  destination.providerBootId = "boot-b"; destination.cacheEpoch = 7;
  destination.requestId = "request-b"; destination.attemptEpoch = 2;
  destination.generationId = "generation-b";
  BOOST_CHECK(nativePortableKvIdentityCompatible(origin, destination));
  const auto rejects = [&](const char* field, auto mutate) {
    BOOST_TEST_CONTEXT(field) {
      auto changed = destination;
      mutate(changed);
      BOOST_CHECK(!nativePortableKvIdentityCompatible(origin, changed));
    }
  };
  const auto other = "sha256:" + std::string(64, 'b');
  rejects("model", [&](auto& value) { value.modelDigest = other; });
  rejects("tokenizer", [&](auto& value) { value.tokenizerDigest = other; });
  rejects("precision", [](auto& value) { value.precision = "float16"; });
  rejects("role partition", [&](auto& value) { value.roleSplitDigest = other; });
  rejects("layer range", [](auto& value) { ++value.layerEnd; });
  rejects("prefix", [](auto& value) { ++value.prefixTokenCount; });
  rejects("saved epoch", [](auto& value) {
    ++value.stateInferenceEpoch;
    value.predecessorInferenceEpoch = value.stateInferenceEpoch - 1;
  });
  rejects("security domain", [&](auto& value) { value.securityDomainDigest = other; });
}

BOOST_AUTO_TEST_CASE(OrdinaryRunnerDoesNotManufacturePortableSupport)
{
  auto runner = makeNativeModelRunner([](const RoleExecutionContext&) {
    return std::map<std::string, TensorBundle>{};
  });
  BOOST_CHECK(!runner->supportsPortableConversationState());
  BOOST_CHECK_THROW(runner->exportConversationState({}, identity(), limits()), std::runtime_error);
  BOOST_CHECK_THROW(runner->importConversationState(snapshot(), identity(), "new", limits()), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

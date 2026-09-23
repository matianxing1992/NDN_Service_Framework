#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <stdexcept>
#include <string>

namespace {
using namespace ndnsf::di;

struct Fixture
{
  NativeModelRef model;
  NativeRequestContract contract;
  NativeApplicationInput input;
  NativeModelArtifactReference reference;

  Fixture()
  {
    static_cast<NativeModelDescriptor&>(model) = fixture::completeModel({
      "spec188-model", nativePlanningDigest("model-content"),
      nativePlanningDigest("model-semantics"), nativePlanningDigest("planning-graph"),
      "onnx", "float32", "fixture", "1"});
    reference = {"/fixture/model-artifacts", "/fixture/model/source",
      nativePlanningDigest("model-manifest"), nativePlanningDigest("model-source"),
      123456, nativePlanningDigest("canonical-source-graph"), "protected-v1", "/SERVICE/fixture",
      nativePlanningDigest("recipe"), 1};
    model.artifactReference = reference;
    contract = {"/fixture", "task", model.adapterId,
      model.adapter.descriptorDigest(), nativePlanningDigest("composition"),
      nativePlanningDigest("task")};
    input.taskName = "task";
    input.inputSchemaDigest = model.adapter.inputSchemaDigest;
    input.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
    input.payload = {0x01, 0x02, 0x03};
  }
};

NativeJson encode(const Fixture& fixture, const std::string& requestId)
{
  const auto result = encodeNativeRequestEnvelope(fixture.model, fixture.input,
    fixture.contract, requestId, 1, 10000);
  return nativeParseJson(std::string(result.wire.begin(), result.wire.end()));
}
}

BOOST_AUTO_TEST_SUITE(Spec188PreparedRequestProjection)

BOOST_AUTO_TEST_CASE(ReferenceIsTypedAndPayloadFreeAcrossRepeatedRequests)
{
  Fixture fixture;
  fixture.reference.validate();
  const auto first = encode(fixture, "/request/spec188/1");
  const auto second = encode(fixture, "/request/spec188/2");

  BOOST_REQUIRE(first.contains("model_reference"));
  BOOST_REQUIRE(second.contains("model_reference"));
  BOOST_CHECK_EQUAL(first.at("model_reference"), second.at("model_reference"));
  BOOST_CHECK_EQUAL(first.at("model_reference").at("object_digest"), fixture.reference.objectDigest);
  BOOST_CHECK_EQUAL(first.at("model_reference").at("manifest_digest"), fixture.reference.manifestDigest);
  BOOST_CHECK_EQUAL(first.at("model_reference").at("object_bytes"), fixture.reference.objectBytes);
  BOOST_CHECK(!first.contains("model_payload"));
  BOOST_CHECK(!first.at("model").contains("payload"));
  BOOST_CHECK(!first.at("model_reference").contains("payload"));
  BOOST_CHECK(first.at("input_payload_b64").is_string());
  BOOST_CHECK(first.at("request_id") != second.at("request_id"));
  BOOST_CHECK(first.at("invocation_id") != second.at("invocation_id"));
}

BOOST_AUTO_TEST_CASE(ApplicationInputReferenceRemainsSeparate)
{
  Fixture fixture;
  fixture.input.payload.clear();
  fixture.input.transportMode = NativeInputTransportMode::RepositoryReference;
  fixture.input.repositoryReference = nativeCanonicalJson({
    {"dataName", "/fixture/input/encrypted"}, {"encrypted", true},
    {"plaintextSize", 3}, {"authorizationScope", "/SERVICE/fixture"},
    {"protectionEpoch", "protected-v1"},
    {"manifestDigest", nativePlanningDigest("input-manifest")},
    {"ciphertextDigest", nativePlanningDigest("input-ciphertext")} });
  const auto wire = encode(fixture, "/request/spec188/input-ref");

  BOOST_REQUIRE(wire.contains("model_reference"));
  BOOST_REQUIRE(wire.contains("input_reference"));
  BOOST_CHECK_EQUAL(wire.at("model_reference").at("object_name"), fixture.reference.objectName);
  BOOST_CHECK_EQUAL(wire.at("input_reference").at("dataName"), "/fixture/input/encrypted");
  BOOST_CHECK(wire.at("model_reference") != wire.at("input_reference"));
  BOOST_CHECK(!wire.at("model_reference").contains("dataName"));
}

BOOST_AUTO_TEST_CASE(TamperedModelReferenceFailsBeforeEncoding)
{
  Fixture fixture;
  fixture.model.artifactReference->objectName = "https://untrusted.example/model";
  BOOST_CHECK_THROW(encode(fixture, "/request/spec188/tampered"), std::invalid_argument);

  fixture.model.artifactReference = fixture.reference;
  fixture.model.artifactReference->graphDigest = "malformed-graph-digest";
  BOOST_CHECK_THROW(encode(fixture, "/request/spec188/malformed-graph"), std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

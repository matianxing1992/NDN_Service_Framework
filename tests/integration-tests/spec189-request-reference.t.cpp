#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestEnvelope.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace ndnsf::di;

struct Fixture
{
  NativeModelRef model;
  NativeRequestContract contract;
  NativeApplicationInput input;

  Fixture()
  {
    static_cast<NativeModelDescriptor&>(model) = fixture::completeModel({
      "spec189-qwen-reference", nativePlanningDigest("qwen-content"),
      nativePlanningDigest("qwen-semantics"), nativePlanningDigest("qwen-graph"),
      "onnx", "float32", "fixture", "1"});
    model.artifactReference = NativeModelArtifactReference{
      "/spec189/repo", "/spec189/repo/qwen/root",
      nativePlanningDigest("manifest"), nativePlanningDigest("root-object"),
      123456, nativePlanningDigest("qwen-graph"), "epoch-1", "/SERVICE/spec189",
      nativePlanningDigest("recipe"), 1};
    contract = {"/SERVICE/spec189", "task", model.adapterId,
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

BOOST_AUTO_TEST_SUITE(Spec189RequestReference)

BOOST_AUTO_TEST_CASE(RepeatedRequestCarriesOnlyStableModelReference)
{
  Fixture fixture;
  const auto first = encode(fixture, "/spec189/request/1");
  const auto second = encode(fixture, "/spec189/request/2");

  BOOST_REQUIRE(first.contains("model_reference"));
  BOOST_REQUIRE(second.contains("model_reference"));
  const auto& reference = first.at("model_reference");
  BOOST_CHECK_EQUAL(reference.at("repo_namespace"), "/spec189/repo");
  BOOST_CHECK_EQUAL(reference.at("object_name"), "/spec189/repo/qwen/root");
  BOOST_CHECK_EQUAL(reference.at("manifest_digest"), nativePlanningDigest("manifest"));
  BOOST_CHECK_EQUAL(reference.at("object_digest"), nativePlanningDigest("root-object"));
  BOOST_CHECK_EQUAL(reference.at("object_bytes"), 123456U);
  BOOST_CHECK_EQUAL(reference.at("graph_digest"), nativePlanningDigest("qwen-graph"));
  BOOST_CHECK_EQUAL(reference.at("protection_epoch"), "epoch-1");
  BOOST_CHECK_EQUAL(reference.at("authorization_scope"), "/SERVICE/spec189");
  BOOST_CHECK_EQUAL(reference.at("recipe_digest"), nativePlanningDigest("recipe"));
  BOOST_CHECK_EQUAL(first.at("model_reference"), second.at("model_reference"));
  BOOST_CHECK(!first.contains("model_payload"));
  BOOST_CHECK(!second.contains("model_payload"));
  BOOST_CHECK(!first.at("model").contains("payload"));
  BOOST_CHECK(!first.at("model_reference").contains("payload"));
  BOOST_CHECK(first.at("request_id") != second.at("request_id"));
  BOOST_CHECK(first.at("invocation_id") != second.at("invocation_id"));
}

BOOST_AUTO_TEST_CASE(ArbitraryModelUrlAndInvalidReferenceAreRejected)
{
  Fixture fixture;
  fixture.model.artifactReference->objectName = "https://untrusted.example/qwen.onnx";
  BOOST_CHECK_THROW(encode(fixture, "/spec189/request/url"), std::invalid_argument);

  fixture.model.artifactReference->objectName = "/spec189/repo/qwen/root";
  fixture.model.artifactReference->objectBytes = 0;
  BOOST_CHECK_THROW(encode(fixture, "/spec189/request/size"), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(OversizedInlinePayloadIsRejectedBeforeWireEncoding)
{
  Fixture fixture;
  // This wire-only selector's 16 MiB limit is intentionally exercised
  // without constructing a model payload: application input is the only
  // inline data allowed on a request wire.  PreparedModel/Repo reuse remains
  // a separate production-path gate and is not claimed by this selector.
  fixture.input.payload.assign(16 * 1024 * 1024 + 1, 0x7f);
  BOOST_CHECK_EXCEPTION(encode(fixture, "/spec189/request/oversized"),
                        std::invalid_argument,
                        [] (const std::invalid_argument& error) {
                          return std::string(error.what()).find("exceeds") != std::string::npos;
                        });
}

BOOST_AUTO_TEST_SUITE_END()

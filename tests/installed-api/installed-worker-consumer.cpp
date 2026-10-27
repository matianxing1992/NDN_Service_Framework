// Reuse frozen vectors and native test helpers; no alternate assembly implementation.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ndnsf_installed_worker
#include "tests/unit-tests/di-native-onnx-recipe.t.cpp"

namespace ndnsf::di {

BOOST_AUTO_TEST_SUITE(Spec217InstalledWorker)

BOOST_AUTO_TEST_CASE(DefaultLocationAssemblesColdCertifiedModel)
{
  const char* expected = std::getenv("NDNSF_SPEC217_EXPECTED_WORKER");
  BOOST_REQUIRE(expected != nullptr);
  BOOST_REQUIRE(std::getenv("NDNSF_DI_WORKER_BINARY") == nullptr);
  const auto location = resolveNativeOnnxWorkerLocation();
  BOOST_REQUIRE_EQUAL(location.path, expected);
  BOOST_REQUIRE(!location.sha256.empty());

  const auto row = workerAcceptRow();
  const auto source = sourceFromRow(row);
  const auto rootPayload = activationRootPayload(row, source.modelBytes);
  auto projection = activationProjection(row, rootPayload);
  projection.assembly.recipeDigest = nativePlanningDigest(
    canonicalNativeOnnxRecipeJson(projection.assembly));
  projection.selectedRole = projection.assembly;
  projection.executionRole.roleId = projection.assembly.selectedRole;
  projection.planDigest = nativePlanningDigest("spec217-installed-worker-fixture-plan");
  projection.dataflow.role = projection.executionRole.roleId;
  projection.dataflow.requestId = projection.requestId;
  projection.dataflow.attempt = projection.attempt;
  projection.dataflow.planDigest = projection.planDigest;
  projection.dataflow.terminalResponseOwner = true;
  const auto cache = activationCacheDir("installed-default-worker");
  BOOST_REQUIRE(!std::filesystem::exists(cache));
  const auto signatures = activationSignLog();
  const auto fetchers = activationFetchers(kActivationRootName, rootPayload,
                                          kActivationSourceName, source.modelBytes);
  const auto options = activationOptions(cache.string(), location, signatures);
  const auto assembled = prepareNativeCanonicalOnnxRole(fetchers, projection, options);
  BOOST_REQUIRE_EQUAL(signatures->size(), 1U);
  BOOST_CHECK_EQUAL(sha256HexOf(activationReadFile(assembled.path)),
                    "sha256:59269bc795b453dcc15fcb34dd6c9b1625070bc094f32361fcd236c3b6600383");
  BOOST_CHECK_EQUAL(assembled.metadata.at("assembledFrom"), "canonical-root-post-selection");
  BOOST_CHECK_EQUAL(assembled.metadata.at("assemblySignature"),
                    "spec182-activation-signature-v1");
  BOOST_CHECK_EQUAL(assembled.metadata.at("outputScope"), "final-response");
  BOOST_CHECK_EQUAL(assembled.metadata.at("final"), "true");
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di

#include "tests/boost-test.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"

namespace ndnsf::di::test {

BOOST_AUTO_TEST_CASE(NativePreparationContextBindsBothModelAndPostprocessAdapters)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  projection.assembly.modelManifestDigest = "sha256:" + std::string(64, 'b');
  projection.assembly.artifactDigest = "sha256:" + std::string(64, 'c');
  const NativeRunnerPreparationContext context{
    "/provider/selected", "boot-1", 1234, "/private/cache"};
  std::string previousProfile;
  for (const auto* kind : {"onnx-model", "native-yolo-postprocess"}) {
    NativeModelRunnerSpec spec;
    spec.kind = kind;
    spec.role = "selected-role";
    spec.path = "adapter-owned-path";
    spec.metadata["stateMode"] = "adapter-owned-state-mode";
    spec.metadata["evidence.providerName"] = "/adapter-supplied-provider";
    spec.metadata["evidence.planDigest"] = "adapter-supplied-plan";
    bindNativeRunnerPreparationContext(spec, projection, context);
    BOOST_CHECK_EQUAL(spec.kind, kind);
    BOOST_CHECK_EQUAL(spec.role, "selected-role");
    BOOST_CHECK_EQUAL(spec.path, "adapter-owned-path");
    BOOST_CHECK_EQUAL(spec.metadata.at("stateMode"), "adapter-owned-state-mode");
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.providerName"), context.providerName);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.providerBootId"), context.providerBootId);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.createdAtMs"), "1234");
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.planDigest"), projection.planDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.modelDigest"), projection.assembly.modelManifestDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("evidence.artifactDigest"), projection.assembly.artifactDigest);
    BOOST_CHECK_EQUAL(spec.metadata.at("profileAfterRequest"), "true");
    const auto profile = spec.metadata.at("providerProfilePrefix");
    BOOST_CHECK_EQUAL(profile.find("/private/cache/ort-profile-"), 0);
    BOOST_CHECK_NE(profile, previousProfile);
    previousProfile = profile;
  }
}

BOOST_AUTO_TEST_CASE(NativePreparationContextPreservesPlanFallbackForPathlessRoles)
{
  NativeSelectionProjectionV3 projection;
  projection.planDigest = "sha256:" + std::string(64, 'a');
  NativeModelRunnerSpec spec;
  bindNativeRunnerPreparationContext(spec, projection,
    {"/provider/merge", "boot-1", 1234, "/private/cache"});
  BOOST_CHECK_EQUAL(spec.metadata.at("evidence.modelDigest"), projection.planDigest);
  BOOST_CHECK(spec.path.empty());
}

} // namespace ndnsf::di::test

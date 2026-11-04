#ifndef NDNSF_TESTS_UAV_PERSON_PROFILE_GOLDEN_HPP
#define NDNSF_TESTS_UAV_PERSON_PROFILE_GOLDEN_HPP

#include <string_view>

namespace ndnsf::test {

// Literal wire bytes/digest shared by native and adapter contract tests.
inline constexpr std::string_view PERSON_PROFILE_GOLDEN = R"profile({"architecture":"osnet_x1_0","cameraRelations":[{"cameraA":"UAV1","cameraB":"UAV2","relation":"overlap"},{"cameraA":"UAV1","cameraB":"UAV3","relation":"non-overlap"},{"cameraA":"UAV2","cameraB":"UAV3","relation":"non-overlap"}],"cameras":["UAV1","UAV2","UAV3"],"detectorDigest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","featureDimension":512,"limits":{"ambiguityMarginPpm":50000,"galleryTtlUs":30000000,"maxBackendTracksPerCamera":512,"maxChildLogBytes":16777216,"maxClockSkewUs":100000,"maxCosineDistancePpm":300000,"maxFrameBytes":4194304,"maxFrameHeight":1080,"maxFrameWidth":1920,"maxGlobalIdentities":256,"maxInFlightWindows":1,"maxLocalKeys":1024,"maxPersonsPerFrame":64,"maxQueuedWindows":2,"maxResultBytes":4194304,"maxRetainedTransactionWindows":120,"maxSnapshotBytes":8388608,"maxTransitionUs":30000000,"minTransitionUs":0,"reidBatchSize":32,"runDeadlineMs":900000,"sampleHz":2,"samplesPerIdentity":8,"workerTimeoutMs":30000},"preprocessId":"osnet-rgb-256x128-imagenet-l2-v1","reidDigest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","requestedDevice":"cuda:0","schema":"uav-person-tracking-profile-v1","trackingMode":"indoor-person-reid"})profile";
inline constexpr std::string_view PERSON_PROFILE_DIGEST = "sha256:35d3f6ef27e3b62296d2ab9f290c0db8b3b7d3c022d418f183b3f55b2bd65048";

} // namespace ndnsf::test

#endif // NDNSF_TESTS_UAV_PERSON_PROFILE_GOLDEN_HPP

#ifndef NDNSF_TESTS_UAV_PERSON_DETECTOR_PROFILE_GOLDEN_HPP
#define NDNSF_TESTS_UAV_PERSON_DETECTOR_PROFILE_GOLDEN_HPP

#include <string_view>

namespace ndnsf::test {

// Independent literal bytes/digest for the explicit detector preprocessing contract.
inline constexpr std::string_view PERSON_DETECTOR_PROFILE_GOLDEN = R"detector({"architecture":"osnet_x1_0","cameraRelations":[{"cameraA":"UAV1","cameraB":"UAV2","relation":"overlap"},{"cameraA":"UAV1","cameraB":"UAV3","relation":"non-overlap"},{"cameraA":"UAV2","cameraB":"UAV3","relation":"non-overlap"}],"cameras":["UAV1","UAV2","UAV3"],"detectorDigest":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","detectorPreprocessId":"yolo-letterbox1280-raw-box-v1","featureDimension":512,"limits":{"ambiguityMarginPpm":50000,"galleryTtlUs":30000000,"maxBackendTracksPerCamera":512,"maxChildLogBytes":16777216,"maxClockSkewUs":100000,"maxCosineDistancePpm":300000,"maxFrameBytes":4194304,"maxFrameHeight":1080,"maxFrameWidth":1920,"maxGlobalIdentities":256,"maxInFlightWindows":1,"maxLocalKeys":1024,"maxPersonsPerFrame":64,"maxQueuedWindows":2,"maxResultBytes":4194304,"maxRetainedTransactionWindows":120,"maxSnapshotBytes":8388608,"maxTransitionUs":30000000,"minTransitionUs":0,"reidBatchSize":32,"runDeadlineMs":900000,"sampleHz":2,"samplesPerIdentity":8,"workerTimeoutMs":30000},"preprocessId":"osnet-rgb-256x128-imagenet-l2-v1","reidDigest":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","requestedDevice":"cuda:0","schema":"uav-person-tracking-profile-v2","trackingMode":"indoor-person-reid"})detector";
inline constexpr std::string_view PERSON_DETECTOR_PROFILE_DIGEST = "sha256:15539f4af8eae63b99cff2432ebdd677eb3797039d91f9ab500a9c3866621586";

} // namespace ndnsf::test

#endif // NDNSF_TESTS_UAV_PERSON_DETECTOR_PROFILE_GOLDEN_HPP

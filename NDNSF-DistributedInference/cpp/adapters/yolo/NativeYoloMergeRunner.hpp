#ifndef NDNSF_DI_ADAPTERS_YOLO_NATIVE_MERGE_RUNNER_HPP
#define NDNSF_DI_ADAPTERS_YOLO_NATIVE_MERGE_RUNNER_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"

namespace ndnsf::di {

/** Build the request-scoped native consumer spec for a declared YOLO Merge. */
NativeModelRunnerSpec
nativeYoloMergeRunnerSpecFromProjection(const NativeSelectionProjectionV3& projection);

/** Construct the CPU-only YOLO postprocessing runner. */
std::shared_ptr<NativeModelRunner>
makeNativeYoloMergeRunner(const NativeModelRunnerSpec& spec);

} // namespace ndnsf::di

#endif

#ifndef NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_STEP_TEMPLATE_JSON_HPP
#define NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_STEP_TEMPLATE_JSON_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

namespace ndnsf::di {

struct NativeExecutionPlan;

/** Validated v2 step/layout commitment, reusing the execution wire serializer. */
NativeJson
nativeV2StepTemplateJson(const NativeExecutionPlan& plan);

} // namespace ndnsf::di

#endif // NDNSF_DISTRIBUTED_INFERENCE_DETAIL_NATIVE_STEP_TEMPLATE_JSON_HPP

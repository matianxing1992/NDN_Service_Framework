#ifndef NDNSF_DI_WAN_ACTIVATION_TILES_HPP
#define NDNSF_DI_WAN_ACTIVATION_TILES_HPP

#include "wan-summed-projection.hpp"

namespace ndnsf::di::wan {

struct TiledActivationLayout
{
  onnx::ModelProto model;
  OnnxParallelPlan execution;
};

/** Lower oversized fixed-shape cross-role ports to ordinary ONNX and exact edges.
 * Each tile is named by its axis and half-open range. A SUM remains native and
 * reduces matching tiles. Split and Concat stay inside CALL graphs.
 */
TiledActivationLayout
tileWanExactActivations(onnx::ModelProto model, OnnxParallelPlan execution,
                        std::uint64_t maxBundleBytes);

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_ACTIVATION_TILES_HPP

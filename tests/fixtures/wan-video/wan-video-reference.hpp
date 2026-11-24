#ifndef NDNSF_TESTS_WAN_VIDEO_REFERENCE_HPP
#define NDNSF_TESTS_WAN_VIDEO_REFERENCE_HPP

#include <ndn-cxx/util/span.hpp>

#include <array>
#include <cstdint>
#include <vector>

namespace ndnsf::di::wan::fixture {

/** Independent arithmetic for the deterministic, sparse Wan video fixture. */
std::vector<double>
referenceUmt5(ndn::span<const std::int64_t> ids, std::size_t valid);

std::vector<double>
referenceVaeRgb(const double* latent, int chunks, int height, int width,
                const std::array<double, 2>& secondChannelFactors);

struct WanReferenceStep
{
  std::int64_t timestep = 0;
  std::array<std::vector<double>, 2> predictions;
  std::vector<double> latent;
};

struct WanVideoReference
{
  std::vector<float> initialLatent;
  std::array<std::vector<double>, 2> textContexts;
  std::array<WanReferenceStep, 3> steps;
  std::vector<double> rgb;
};

/** Reproduce the tiny fixture's fixed seed, prompts, shift and guidance in FP64. */
WanVideoReference
referenceWanVideo(double projectionEpsilon = 0.25);

} // namespace ndnsf::di::wan::fixture

#endif // NDNSF_TESTS_WAN_VIDEO_REFERENCE_HPP

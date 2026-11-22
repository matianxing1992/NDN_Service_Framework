#ifndef NDNSF_DI_WAN_UNIPC_HPP
#define NDNSF_DI_WAN_UNIPC_HPP

#include <ndn-cxx/util/span.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

namespace ndnsf::di::wan {

/** One Wan2.1 T2V flow-prediction trajectory using the BH2 UniPC solver. */
class WanUniPc
{
public:
  struct StepInfo
  {
    unsigned predictorOrder;
    unsigned correctorOrder;
  };

  WanUniPc(std::size_t steps, double shift, std::vector<float> initialLatent);

  std::size_t stepIndex() const noexcept { return m_step; }
  bool finished() const noexcept { return m_step == m_timesteps.size(); }
  std::int64_t timestep() const;
  const std::vector<float>& latent() const noexcept { return m_latent; }

  /** Commit exactly one step using two already admitted FP32 predictions.
   * The generation owner validates tensor shape and child identity before
   * calling. An error or cancellation from requireActive leaves state intact.
   */
  StepInfo
  advance(std::size_t index, ndn::span<const float> positive, ndn::span<const float> negative,
          float guidance, const std::function<void()>& requireActive);

private:
  std::vector<float> m_sigmas;
  std::vector<std::int64_t> m_timesteps;
  std::size_t m_step = 0;
  unsigned m_previousOrder = 0;
  std::vector<float> m_latent;
  std::vector<float> m_lastSample;
  std::vector<float> m_latestX0;
  std::vector<float> m_olderX0;
};

} // namespace ndnsf::di::wan

#endif // NDNSF_DI_WAN_UNIPC_HPP

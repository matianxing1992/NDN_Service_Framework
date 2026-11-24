#include "wan-unipc.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ndnsf::di::wan {
namespace {

constexpr std::size_t TRAINING_STEPS = 1000;
constexpr std::size_t CHECK_INTERVAL = 64 * 1024;

double
logSnr(double sigma)
{
  return std::log1p(-sigma) - std::log(sigma);
}

struct Bh2Interval
{
  Bh2Interval(double first, double next)
    : lambda(logSnr(first))
    , h(next == 0 ? 0 : logSnr(next) - lambda)
    , sampleWeight(next / first)
    , modelWeight(next == 0 ? 1 : -(1 - next) * std::expm1(-h))
  {
  }

  double
  historyRatio(double sigma) const
  {
    return (logSnr(sigma) - lambda) / h;
  }

  double lambda;
  double h;
  double sampleWeight;
  double modelWeight;
};

} // namespace

WanUniPc::WanUniPc(std::size_t steps, double shift, std::vector<float> initialLatent)
  : m_latent(std::move(initialLatent))
{
  if (steps == 0 || steps > TRAINING_STEPS || !std::isfinite(shift) || shift <= 0 ||
      m_latent.empty() || std::any_of(m_latent.begin(), m_latent.end(),
                                    [] (float value) { return !std::isfinite(value); }))
    throw std::invalid_argument("invalid Wan UniPC schedule or initial latent");

  // Wan builds its training maximum in FP32, then shifts the inference grid
  // in FP64 before storing sigmas in FP32 and truncating model timesteps.
  constexpr double maxSigma = static_cast<float>(1 - 1.0 / TRAINING_STEPS);
  const double spacing = -maxSigma / static_cast<double>(steps);
  m_sigmas.reserve(steps + 1);
  m_timesteps.reserve(steps);
  for (std::size_t i = 0; i < steps; ++i) {
    const double factor = maxSigma + static_cast<double>(i) * spacing;
    const double shifted = (shift * factor) / (1 + (shift - 1) * factor);
    const auto sigma = static_cast<float>(shifted);
    if (!(sigma > 0 && sigma < 1) || (!m_sigmas.empty() && sigma >= m_sigmas.back()))
      throw std::invalid_argument("Wan UniPC requires strictly decreasing FP32 sigmas");
    m_sigmas.push_back(sigma);
    m_timesteps.push_back(static_cast<std::int64_t>(shifted * TRAINING_STEPS));
  }
  m_sigmas.push_back(0);
}

std::int64_t
WanUniPc::timestep() const
{
  if (finished()) throw std::logic_error("Wan UniPC has no remaining timestep");
  return m_timesteps[m_step];
}

WanUniPc::StepInfo
WanUniPc::advance(std::size_t index, ndn::span<const float> positive, ndn::span<const float> negative,
                 float guidance, const std::function<void()>& requireActive)
{
  requireActive();
  const auto size = m_latent.size();
  if (finished() || index != m_step || positive.size() != size || negative.size() != size ||
      !std::isfinite(guidance))
    throw std::invalid_argument("Wan UniPC prediction does not match the current step");

  const auto sigma = m_sigmas[m_step];
  const auto nextSigma = m_sigmas[m_step + 1];
  const bool correct = m_step != 0;
  const bool secondOrder = correct && nextSigma != 0;
  const StepInfo info{secondOrder ? 2U : 1U, m_previousOrder};
  const Bh2Interval predictor(sigma, nextSigma);
  const double previousSigma = correct ? m_sigmas[m_step - 1] : sigma;
  const Bh2Interval corrector(previousSigma, sigma);
  const double predictionHistory = secondOrder ? 0.5 / predictor.historyRatio(previousSigma) : 0;
  double correctionHistory = 0;
  double correctionCurrent = 0.5;
  if (info.correctorOrder == 2) {
    const double ratio = corrector.historyRatio(m_sigmas[m_step - 2]);
    const double bh = std::expm1(-corrector.h);
    const double phi = bh / -corrector.h - 1;
    const double b0 = phi / bh;
    const double b1 = 2 * (phi / -corrector.h - 0.5) / bh;
    const double historyWeight = (b1 - b0) / (ratio - 1);
    correctionHistory = historyWeight / ratio;
    correctionCurrent = b0 - historyWeight;
  }

  std::vector<float> converted(nextSigma != 0 ? size : 0), predicted(size);
  std::vector<float> corrected(correct && nextSigma != 0 ? size : 0);
  for (std::size_t i = 0; i < size; ++i) {
    if (i % CHECK_INTERVAL == 0) requireActive();
    const float flow = negative[i] + guidance * (positive[i] - negative[i]);
    const float x0 = m_latent[i] - sigma * flow;
    float sample = m_latent[i];
    if (correct) {
      const auto lastX0 = m_latestX0[i];
      double residual = correctionCurrent * (x0 - lastX0);
      if (info.correctorOrder == 2)
        residual += correctionHistory * (m_olderX0[i] - lastX0);
      sample = static_cast<float>(corrector.sampleWeight * m_lastSample[i] +
                                  corrector.modelWeight * (lastX0 + residual));
      if (!corrected.empty()) corrected[i] = sample;
    }
    const double residual = secondOrder ? predictionHistory * (m_latestX0[i] - x0) : 0;
    const auto next = static_cast<float>(predictor.sampleWeight * sample +
                                        predictor.modelWeight * (x0 + residual));
    if (!std::isfinite(x0) || !std::isfinite(sample) || !std::isfinite(next))
      throw std::invalid_argument("Wan UniPC requires finite predictions and latents");
    if (!converted.empty()) converted[i] = x0;
    predicted[i] = next;
  }
  requireActive();
  ++m_step;
  m_previousOrder = info.predictorOrder;
  if (finished()) {
    std::vector<float>().swap(m_lastSample);
    std::vector<float>().swap(m_latestX0);
    std::vector<float>().swap(m_olderX0);
  }
  else {
    m_lastSample = correct ? std::move(corrected) : std::move(m_latent);
    m_olderX0 = std::move(m_latestX0);
    m_latestX0 = std::move(converted);
  }
  m_latent = std::move(predicted);
  return info;
}

} // namespace ndnsf::di::wan

#include "wan-video-reference.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <random>
#include <stdexcept>

namespace ndnsf::di::wan::fixture {
namespace {

constexpr int HIDDEN = 24;
constexpr int HEAD_WIDTH = 6;
constexpr int TEXT_SLOTS = 3;
constexpr int VIDEO_TOKENS = 12;
constexpr int LATENT_SITES = 48;
using HiddenRow = std::array<double, HIDDEN>;
using Video = std::array<HiddenRow, VIDEO_TOKENS>;
using Text = std::array<HiddenRow, TEXT_SLOTS>;

double
gelu(double x)
{
  const double factor = std::sqrt(2 / std::acos(-1.0));
  return 0.5 * x * (1 + std::tanh(factor * (x + 0.044715 * x * x * x)));
}

double
silu(double x)
{
  return x / (1 + std::exp(-x));
}

HiddenRow
normalize(HiddenRow row, bool center, double projectionEpsilon = 0.25)
{
  if (center) {
    const double mean = std::accumulate(row.begin(), row.end(), 0.0) / HIDDEN;
    for (auto& x : row) x -= mean;
  }
  const double square = std::inner_product(row.begin(), row.end(), row.begin(), 0.0);
  const double rms = std::sqrt(square / HIDDEN + (center ? 1e-6 : projectionEpsilon));
  for (auto& x : row) x /= rms;
  return row;
}

HiddenRow
project(const HiddenRow& input, int offset)
{
  HiddenRow result;
  for (int c = 0; c < HIDDEN; ++c) result[c] = input[(c + offset) % HIDDEN];
  return result;
}

HiddenRow
rotate(HiddenRow input, int token)
{
  const std::array<int, 3> position{token / 4, (token / 2) % 2, token % 2};
  for (int head = 0; head < HIDDEN; head += HEAD_WIDTH) {
    for (int axis = 0; axis < 3; ++axis) {
      const auto c = head + 2 * axis;
      const double real = input[c], imag = input[c + 1];
      const double cosine = std::cos(position[axis]), sine = std::sin(position[axis]);
      input[c] = real * cosine - imag * sine;
      input[c + 1] = real * sine + imag * cosine;
    }
  }
  return input;
}

template<std::size_t Keys>
HiddenRow
attention(const HiddenRow& query, const std::array<HiddenRow, Keys>& keys,
          const std::array<HiddenRow, Keys>& values)
{
  HiddenRow result{};
  for (int head = 0; head < HIDDEN; head += HEAD_WIDTH) {
    std::array<double, Keys> scores;
    for (std::size_t k = 0; k < Keys; ++k) {
      scores[k] = std::inner_product(query.begin() + head, query.begin() + head + HEAD_WIDTH,
                                     keys[k].begin() + head, 0.0) / std::sqrt(HEAD_WIDTH);
    }
    const auto maximum = *std::max_element(scores.begin(), scores.end());
    for (auto& score : scores) score = std::exp(score - maximum);
    const auto total = std::accumulate(scores.begin(), scores.end(), 0.0);
    for (int c = head; c < head + HEAD_WIDTH; ++c) {
      for (std::size_t k = 0; k < Keys; ++k) result[c] += scores[k] * values[k][c] / total;
    }
  }
  return project(result, 3);
}

int
site(int token, int patch)
{
  const int f = token / 4;
  const int y = 2 * ((token / 2) % 2) + patch / 2;
  const int x = 2 * (token % 2) + patch % 2;
  return 16 * f + 4 * y + x;
}

std::vector<double>
denoise(const std::vector<double>& latent, const std::vector<double>& context, double timestep,
        double projectionEpsilon)
{
  Video hidden;
  for (int token = 0; token < VIDEO_TOKENS; ++token) {
    for (int c = 0; c < HIDDEN; ++c) {
      hidden[token][c] = 0.125 + 0.5 * latent[site(token, c % 4)] -
                        0.25 * latent[LATENT_SITES + site(token, (c + 1) % 4)];
    }
  }
  // Text padding is applied before the projection. All three projected rows,
  // even those made from padding, contribute to the cross-attention denominator.
  Text textKeys{}, textValues{};
  for (int token = 0; token < TEXT_SLOTS; ++token) {
    HiddenRow projected{};
    if (static_cast<std::size_t>(2 * token) < context.size()) {
      for (int c = 0; c < HIDDEN; ++c) projected[c] = 0.25 * gelu(context[2 * token + c % 2]);
    }
    textKeys[token] = normalize(projected, false, projectionEpsilon);
    textValues[token] = project(projected, 5);
  }
  const std::array<double, 4> phases{std::cos(timestep), std::cos(timestep / 100),
                                     std::sin(timestep), std::sin(timestep / 100)};
  HiddenRow time, modulation;
  for (int c = 0; c < HIDDEN; ++c) {
    time[c] = 0.5 * silu(0.125 * phases[c % 4]);
    modulation[c] = 0.0625 * silu(time[c]);
  }

  for (double gate : {0.125, 0.25, 0.375}) {
    Video queries, keys, values;
    for (int token = 0; token < VIDEO_TOKENS; ++token) {
      auto input = normalize(hidden[token], true);
      for (int c = 0; c < HIDDEN; ++c)
        input[c] = input[c] * (1 + modulation[c] + 0.0625 * (c % 4)) + modulation[c];
      queries[token] = rotate(normalize(input, false, projectionEpsilon), token);
      keys[token] = rotate(normalize(project(input, 1), false, projectionEpsilon), token);
      values[token] = project(input, 2);
    }
    for (int token = 0; token < VIDEO_TOKENS; ++token) {
      auto& row = hidden[token];
      const auto self = attention(queries[token], keys, values);
      for (int c = 0; c < HIDDEN; ++c) row[c] += (gate + modulation[c]) * self[c];
      const auto cross = attention(normalize(normalize(row, true), false, projectionEpsilon), textKeys, textValues);
      for (int c = 0; c < HIDDEN; ++c) row[c] += cross[c];
      auto ffnInput = normalize(row, true);
      for (int c = 0; c < HIDDEN; ++c)
        ffnInput[c] = ffnInput[c] * (1 + modulation[c]) + modulation[c];
      // The rank-independent expansion has 32 cells. In this fixture the contraction
      // samples cells 2..25, whose original inputs are the corresponding columns mod 24.
      for (int c = 0; c < HIDDEN; ++c)
        row[c] += (0.5 + modulation[c]) * 0.25 * gelu(ffnInput[(c + 2) % HIDDEN]);
    }
  }

  std::vector<double> prediction(2 * LATENT_SITES);
  for (int token = 0; token < VIDEO_TOKENS; ++token) {
    const auto normalized = normalize(hidden[token], true);
    std::array<double, 8> patch;
    patch.fill(0.125);
    for (int c = 0; c < HIDDEN; ++c)
      patch[c % 8] += 0.125 * (normalized[c] * (1 + time[c]) + time[c]);
    for (int p = 0; p < 8; ++p)
      prediction[(p % 2) * LATENT_SITES + site(token, p / 2)] = patch[p];
  }
  return prediction;
}

} // namespace

std::vector<double>
referenceUmt5(ndn::span<const std::int64_t> ids, std::size_t valid)
{
  using Token = std::array<double, 2>;
  const std::array<Token, 3> embeddings{{{1, -1}, {2, 3}, {100, -50}}};
  if (valid == 0 || valid > ids.size()) throw std::invalid_argument("invalid fixture mask");
  const auto rms = [] (const Token& row, double a, double b) {
    const double scale = std::sqrt((row[0] * row[0] + row[1] * row[1]) / 2 + 0.25);
    return Token{row[0] * a / scale, row[1] * b / scale};
  };
  std::vector<Token> hidden;
  for (auto id : ids.first(valid)) hidden.push_back(embeddings.at(id));
  for (const auto& gain : {Token{1, 2}, Token{0.5, 0.75}}) {
    Token mean{};
    for (const auto& row : hidden) {
      const auto value = rms(row, gain[0], gain[1]);
      for (int c = 0; c < 2; ++c) mean[c] += value[c] / valid;
    }
    for (auto& row : hidden) {
      for (int c = 0; c < 2; ++c) row[c] += mean[c];
      const auto gate = rms(row, gain[1], gain[0]);
      row[0] += gate[0] * gelu(gate[0]) * 0.25;
      row[1] += gate[1] * gelu(gate[1]) * 0.5;
    }
  }
  std::vector<double> output;
  for (const auto& row : hidden) {
    const auto value = rms(row, 1.5, 2.25);
    output.insert(output.end(), value.begin(), value.end());
  }
  return output;
}

WanVideoReference
referenceWanVideo(double projectionEpsilon)
{
  WanVideoReference result;
  const std::array<std::int64_t, 3> positive{2, 0, 1}, negative{1, 0, 0};
  result.textContexts = {referenceUmt5(positive, 3), referenceUmt5(negative, 1)};

  std::mt19937_64 generator(271);
  const auto uniform = [&] { return static_cast<double>((generator() >> 11) + 1) / 0x1p53; };
  for (int cell = 0; cell < 2 * LATENT_SITES; cell += 2) {
    const double radius = std::sqrt(-2 * std::log(uniform()));
    const double angle = 2 * std::acos(-1.0) * uniform();
    result.initialLatent.push_back(static_cast<float>(radius * std::cos(angle)));
    result.initialLatent.push_back(static_cast<float>(radius * std::sin(angle)));
  }

  std::vector<double> sample(result.initialLatent.begin(), result.initialLatent.end());
  std::array<double, 3> sigmas, lambda;
  for (std::size_t step = 0; step < result.steps.size(); ++step) {
    const double fraction = static_cast<double>(3 - step) / 3;
    const double unshifted = fraction * static_cast<double>(0.999F);
    const double sigma = 3 * unshifted / (1 + 2 * unshifted);
    result.steps[step].timestep = static_cast<std::int64_t>(1000 * sigma);
    sigmas[step] = static_cast<float>(sigma);
    lambda[step] = std::log((1 - sigmas[step]) / sigmas[step]);
  }
  const auto propagate = [&] (std::size_t from, double base, double x0) {
    const auto next = from + 1;
    const double alphaWeight = (1 - sigmas[next]) * (1 - std::exp(lambda[from] - lambda[next]));
    return sigmas[next] / sigmas[from] * base + alphaWeight * x0;
  };
  std::vector<double> firstX0(sample.size());
  for (std::size_t index = 0; index < result.steps.size(); ++index) {
    auto& step = result.steps[index];
    for (std::size_t branch = 0; branch < result.textContexts.size(); ++branch)
      step.predictions[branch] = denoise(sample, result.textContexts[branch], step.timestep, projectionEpsilon);
    for (std::size_t cell = 0; cell < sample.size(); ++cell) {
      const auto flow = 2 * step.predictions[0][cell] - step.predictions[1][cell];
      const double x0 = sample[cell] - sigmas[index] * flow;
      if (index == 0) {
        firstX0[cell] = x0;
        sample[cell] = propagate(0, sample[cell], x0);
      }
      else if (index == 1) {
        const double corrected = propagate(0, result.initialLatent[cell], (firstX0[cell] + x0) / 2);
        const double spacingRatio = (lambda[1] - lambda[0]) / (lambda[2] - lambda[1]);
        sample[cell] = propagate(1, corrected, x0 + (x0 - firstX0[cell]) / (2 * spacingRatio));
      }
      else {
        // At sigma zero the corrected sample's coefficient vanishes, leaving x0.
        sample[cell] = x0;
      }
    }
    step.latent = sample;
  }
  result.rgb = referenceVaeRgb(sample.data(), 3, 4, 4, {-0.25, 0.125});
  return result;
}

// Dense independent calculation for the deliberately sparse test checkpoint.
// Spatial convolutions project at their center and spatial upsampling repeats
// each latent site. Both temporal upsamplers keep their own causal history.
std::vector<double>
referenceVaeRgb(const double* latent, int chunks, int height, int width,
                const std::array<double, 2>& secondChannelFactors)
{
  constexpr int SPATIAL_SCALE = 8;
  const int sites = height * width;
  const int outputWidth = width * SPATIAL_SCALE;
  const int framePixels = height * SPATIAL_SCALE * outputWidth;
  const int outputFrames = 1 + 4 * (chunks - 1);
  std::vector<double> result(3 * outputFrames * framePixels);
  using Channels = std::array<double, 2>;
  for (int site = 0; site < sites; ++site) {
    std::array<Channels, 2> previous{};
    int outputFrame = 0;
    for (int chunk = 0; chunk < chunks; ++chunk) {
      const double first = latent[chunk * sites + site];
      const double second = latent[(chunks + chunk) * sites + site];
      // Middle residuals and the first three upsampling residuals add 1.25.
      std::vector<Channels> signal{{first + secondChannelFactors[0] * second + 1.25,
                                    first + secondChannelFactors[1] * second + 1.25}};
      for (auto& history : previous) {
        if (chunk > 0) {
          std::vector<Channels> expanded;
          for (const auto& value : signal) {
            expanded.push_back(value);
            expanded.push_back({value[0] + 4 + 0.5 * history[0],
                                value[1] + 4 + 0.5 * history[1]});
            history = value;
          }
          signal = std::move(expanded);
        }
        for (auto& value : signal) {
          value[0] += 0.75;
          value[1] += 0.75;
        }
      }
      for (const auto& value : signal) {
        const double x = value[0] + 0.75;
        const double y = value[1] + 2.5;
        const double rms = std::sqrt((x * x + y * y) / 2);
        const std::array<double, 3> rgb{0.5 * silu(x / rms), 0.75 * silu(y / rms),
                                        -0.5 * silu(x / rms)};
        const int top = site / width * SPATIAL_SCALE;
        const int left = site % width * SPATIAL_SCALE;
        for (int channel = 0; channel < 3; ++channel) {
          for (int row = top; row < top + SPATIAL_SCALE; ++row) {
            const auto start = (channel * outputFrames + outputFrame) * framePixels +
                               row * outputWidth + left;
            std::fill_n(result.begin() + start, SPATIAL_SCALE, rgb[channel]);
          }
        }
        ++outputFrame;
      }
    }
  }
  return result;
}

} // namespace ndnsf::di::wan::fixture

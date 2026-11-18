#include "uav-byte-tracker.hpp"

#include "uav-byte-track-state.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace ndnsf::examples::uav::byte_track {
namespace {

constexpr double POSITION_WEIGHT = 1.0 / 20.0;
constexpr double VELOCITY_WEIGHT = 1.0 / 160.0;

Matrix8
identity()
{
  Matrix8 result{};
  for (size_t i = 0; i < 8; ++i) result[i][i] = 1;
  return result;
}

Matrix8
multiply(const Matrix8& lhs, const Matrix8& rhs)
{
  Matrix8 result{};
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 8; ++column) {
      double value = 0;
      for (size_t inner = 0; inner < 8; ++inner) {
        value += lhs[row][inner] * rhs[inner][column];
      }
      result[row][column] = value;
    }
  }
  return result;
}

Matrix8
transpose(const Matrix8& value)
{
  Matrix8 result{};
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 8; ++column) {
      result[row][column] = value[column][row];
    }
  }
  return result;
}

Vector8
multiply(const Vector8& vector, const Matrix8& matrix)
{
  Vector8 result{};
  for (size_t column = 0; column < 8; ++column) {
    double value = 0;
    for (size_t row = 0; row < 8; ++row) value += vector[row] * matrix[row][column];
    result[column] = value;
  }
  return result;
}

Matrix8
motionMatrix()
{
  Matrix8 motion = identity();
  for (size_t index = 0; index < 4; ++index) motion[index][index + 4] = 1;
  return motion;
}

/** Solve a 4x4 system by Gauss-Jordan elimination with partial pivoting. */
bool
invert4(std::array<std::array<double, 4>, 4> input,
        std::array<std::array<double, 4>, 4>& inverse)
{
  inverse = {};
  for (size_t i = 0; i < 4; ++i) inverse[i][i] = 1;
  for (size_t column = 0; column < 4; ++column) {
    size_t pivot = column;
    for (size_t row = column + 1; row < 4; ++row) {
      if (std::fabs(input[row][column]) > std::fabs(input[pivot][column])) pivot = row;
    }
    if (std::fabs(input[pivot][column]) < 1e-12) return false;
    if (pivot != column) {
      std::swap(input[pivot], input[column]);
      std::swap(inverse[pivot], inverse[column]);
    }
    const double divisor = input[column][column];
    for (size_t k = 0; k < 4; ++k) {
      input[column][k] /= divisor;
      inverse[column][k] /= divisor;
    }
    for (size_t row = 0; row < 4; ++row) {
      if (row == column) continue;
      const double factor = input[row][column];
      if (factor == 0) continue;
      for (size_t k = 0; k < 4; ++k) {
        input[row][k] -= factor * input[column][k];
        inverse[row][k] -= factor * inverse[column][k];
      }
    }
  }
  return true;
}

} // namespace

Box
Track::tlwh() const
{
  if (!initialized) return initialTlwh;
  Box result{};
  result[2] = mean[2] * mean[3];
  result[3] = mean[3];
  result[0] = mean[0] - result[2] / 2;
  result[1] = mean[1] - result[3] / 2;
  return result;
}

void
KalmanFilterXYAH::initiate(const Box& measurement, Vector8& mean, Matrix8& covariance) const
{
  if (!(measurement[3] > 0) || !(measurement[2] > 0)) {
    throw std::invalid_argument("initial ByteTrack measurement must have positive size");
  }
  for (size_t index = 0; index < 4; ++index) mean[index] = measurement[index];
  for (size_t index = 4; index < 8; ++index) mean[index] = 0;
  const double std[8] = {
    2 * POSITION_WEIGHT * measurement[3],
    2 * POSITION_WEIGHT * measurement[3],
    1e-2,
    2 * POSITION_WEIGHT * measurement[3],
    10 * VELOCITY_WEIGHT * measurement[3],
    10 * VELOCITY_WEIGHT * measurement[3],
    1e-5,
    10 * VELOCITY_WEIGHT * measurement[3],
  };
  covariance = {};
  for (size_t index = 0; index < 8; ++index) covariance[index][index] = std[index] * std[index];
}

void
KalmanFilterXYAH::predict(Vector8& mean, Matrix8& covariance) const
{
  const double height = mean[3];
  const double std[8] = {
    POSITION_WEIGHT * height,
    POSITION_WEIGHT * height,
    1e-2,
    POSITION_WEIGHT * height,
    VELOCITY_WEIGHT * height,
    VELOCITY_WEIGHT * height,
    1e-5,
    VELOCITY_WEIGHT * height,
  };
  Matrix8 motionCovariance{};
  for (size_t index = 0; index < 8; ++index) {
    motionCovariance[index][index] = std[index] * std[index];
  }
  const Matrix8 motion = motionMatrix();
  mean = multiply(mean, transpose(motion));
  covariance = multiply(multiply(motion, covariance), transpose(motion));
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 8; ++column) {
      covariance[row][column] += motionCovariance[row][column];
    }
  }
}

void
KalmanFilterXYAH::project(const Vector8& mean, const Matrix8& covariance,
                          std::array<double, 4>& projectedMean,
                          std::array<std::array<double, 4>, 4>& projectedCovariance) const
{
  const double height = mean[3];
  const double std[4] = {
    POSITION_WEIGHT * height,
    POSITION_WEIGHT * height,
    1e-1,
    POSITION_WEIGHT * height,
  };
  for (size_t index = 0; index < 4; ++index) projectedMean[index] = mean[index];
  for (size_t row = 0; row < 4; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      projectedCovariance[row][column] = covariance[row][column];
    }
    projectedCovariance[row][row] += std[row] * std[row];
  }
}

bool
KalmanFilterXYAH::update(Vector8& mean, Matrix8& covariance, const Box& measurement) const
{
  std::array<double, 4> projectedMean{};
  std::array<std::array<double, 4>, 4> projectedCovariance{};
  project(mean, covariance, projectedMean, projectedCovariance);
  std::array<std::array<double, 4>, 4> inverse{};
  if (!invert4(projectedCovariance, inverse)) return false;

  // Gain = covariance[:, :4] * inverse(projected covariance)
  std::array<std::array<double, 4>, 8> gain{};
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      double value = 0;
      for (size_t inner = 0; inner < 4; ++inner) {
        value += covariance[row][inner] * inverse[inner][column];
      }
      gain[row][column] = value;
    }
  }
  std::array<double, 4> innovation{};
  for (size_t index = 0; index < 4; ++index) innovation[index] = measurement[index] - projectedMean[index];
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      mean[row] += gain[row][column] * innovation[column];
    }
  }
  // covariance -= gain * projectedCovariance * gain^T
  std::array<std::array<double, 4>, 8> temp{};
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 4; ++column) {
      double value = 0;
      for (size_t inner = 0; inner < 4; ++inner) {
        value += gain[row][inner] * projectedCovariance[inner][column];
      }
      temp[row][column] = value;
    }
  }
  for (size_t row = 0; row < 8; ++row) {
    for (size_t column = 0; column < 8; ++column) {
      double value = 0;
      for (size_t inner = 0; inner < 4; ++inner) {
        value += temp[row][inner] * gain[column][inner];
      }
      covariance[row][column] -= value;
    }
  }
  return true;
}




namespace {

Box
xyahFromTlwh(const Box& tlwh)
{
  Box result = tlwh;
  result[0] += tlwh[2] / 2;
  result[1] += tlwh[3] / 2;
  result[2] /= tlwh[3];
  return result;
}

double
intersectionOverUnion(const Box& lhs, const Box& rhs)
{
  const double x1 = std::max(lhs[0], rhs[0]);
  const double y1 = std::max(lhs[1], rhs[1]);
  const double x2 = std::min(lhs[0] + lhs[2], rhs[0] + rhs[2]);
  const double y2 = std::min(lhs[1] + lhs[3], rhs[1] + rhs[3]);
  const double intersection = std::max(0.0, x2 - x1) * std::max(0.0, y2 - y1);
  const double area = lhs[2] * lhs[3] + rhs[2] * rhs[3] - intersection;
  return area <= 0 ? 0 : intersection / area;
}

std::vector<std::vector<double>>
iouDistance(const std::vector<Track>& tracks, const std::vector<Track>& detections)
{
  std::vector<std::vector<double>> costs(tracks.size(),
    std::vector<double>(detections.size(), 1.0));
  for (size_t row = 0; row < tracks.size(); ++row) {
    for (size_t column = 0; column < detections.size(); ++column) {
      costs[row][column] = 1.0 - intersectionOverUnion(tracks[row].tlwh(),
                                                       detections[column].initialTlwh);
    }
  }
  return costs;
}

/** Hungarian assignment for rectangular cost matrices; returns row->column. */
std::vector<int>
assign(const std::vector<std::vector<double>>& costs)
{
  size_t rows = costs.size();
  size_t columns = rows ? costs[0].size() : 0;
  std::vector<int> result(rows, -1);
  if (rows == 0 || columns == 0) return result;
  bool transposed = rows > columns;
  size_t n = transposed ? columns : rows;
  size_t m = transposed ? rows : columns;
  const double infinity = 1e100;
  std::vector<double> u(n + 1), v(m + 1);
  std::vector<int> p(m + 1), way(m + 1);
  for (size_t i = 1; i <= n; ++i) {
    p[0] = static_cast<int>(i);
    size_t j0 = 0;
    std::vector<double> minv(m + 1, infinity);
    std::vector<char> used(m + 1, 0);
    do {
      used[j0] = 1;
      const size_t i0 = static_cast<size_t>(p[j0]);
      double delta = infinity;
      size_t j1 = 0;
      for (size_t j = 1; j <= m; ++j) {
        if (used[j]) continue;
        const double current = (transposed ? costs[j - 1][i0 - 1] : costs[i0 - 1][j - 1]) -
                               u[i0] - v[j];
        if (current < minv[j]) { minv[j] = current; way[j] = static_cast<int>(j0); }
        if (minv[j] < delta) { delta = minv[j]; j1 = j; }
      }
      for (size_t j = 0; j <= m; ++j) {
        if (used[j]) { u[p[j]] += delta; v[j] -= delta; }
        else minv[j] -= delta;
      }
      j0 = j1;
    } while (p[j0] != 0);
    do {
      const size_t j1 = static_cast<size_t>(way[j0]);
      p[j0] = p[j1];
      j0 = j1;
    } while (j0 != 0);
  }
  for (size_t j = 1; j <= m; ++j) {
    if (p[j] == 0) continue;
    const size_t row = static_cast<size_t>(p[j] - 1);
    const size_t column = j - 1;
    if (transposed) result[column] = static_cast<int>(row);
    else result[row] = static_cast<int>(column);
  }
  return result;
}

struct Matching
{
  std::vector<std::pair<size_t, size_t>> matches;
  std::vector<size_t> unmatchedRows;
  std::vector<size_t> unmatchedColumns;
};

Matching
linearAssignment(const std::vector<std::vector<double>>& costs, double threshold,
                  size_t columnCount)
{
  Matching result;
  const auto assignment = assign(costs);
  std::vector<char> rowUsed(costs.size(), 0);
  std::vector<char> columnUsed(columnCount, 0);
  for (size_t row = 0; row < assignment.size(); ++row) {
    const int column = assignment[row];
    if (column < 0 || costs[row][static_cast<size_t>(column)] > threshold) continue;
    result.matches.emplace_back(row, static_cast<size_t>(column));
    rowUsed[row] = 1;
    columnUsed[static_cast<size_t>(column)] = 1;
  }
  for (size_t row = 0; row < rowUsed.size(); ++row) {
    if (!rowUsed[row]) result.unmatchedRows.push_back(row);
  }
  for (size_t column = 0; column < columnUsed.size(); ++column) {
    if (!columnUsed[column]) result.unmatchedColumns.push_back(column);
  }
  return result;
}

void
fuseScores(std::vector<std::vector<double>>& costs, const std::vector<Track>& detections)
{
  for (auto& row : costs) {
    for (size_t column = 0; column < row.size(); ++column) {
      row[column] = 1.0 - (1.0 - row[column]) * detections[column].score;
    }
  }
}


void
removeByIds(std::vector<Track>& tracks, const std::vector<Track>& remove)
{
  tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&] (const Track& track) {
    return std::any_of(remove.begin(), remove.end(),
                       [&] (const Track& other) { return other.trackId == track.trackId; });
  }), tracks.end());
}

void
removeDuplicates(std::vector<Track>& tracked, std::vector<Track>& lost)
{
  if (tracked.empty() || lost.empty()) return;
  const auto costs = iouDistance(tracked, lost);
  std::vector<char> dropTracked(tracked.size(), 0);
  std::vector<char> dropLost(lost.size(), 0);
  for (size_t row = 0; row < tracked.size(); ++row) {
    for (size_t column = 0; column < lost.size(); ++column) {
      if (costs[row][column] >= 0.15) continue;
      const uint64_t trackedAge = tracked[row].frameId - tracked[row].startFrame;
      const uint64_t lostAge = lost[column].frameId - lost[column].startFrame;
      if (trackedAge > lostAge) dropLost[column] = 1;
      else dropTracked[row] = 1;
    }
  }
  size_t write = 0;
  for (size_t index = 0; index < tracked.size(); ++index) {
    if (!dropTracked[index]) tracked[write++] = std::move(tracked[index]);
  }
  tracked.resize(write);
  write = 0;
  for (size_t index = 0; index < lost.size(); ++index) {
    if (!dropLost[index]) lost[write++] = std::move(lost[index]);
  }
  lost.resize(write);
}

} // namespace

UavByteTracker::UavByteTracker(TrackerConfig config)
  : m_config(config)
{
  if (!(config.highThreshold > 0 && config.highThreshold <= 1) ||
      !(config.lowThreshold >= 0 && config.lowThreshold < config.highThreshold) ||
      !(config.newTrackThreshold > 0 && config.newTrackThreshold <= 1) ||
      !(config.matchThreshold > 0 && config.matchThreshold <= 1) ||
      config.trackBuffer == 0 || config.maximumTracks == 0) {
    throw std::invalid_argument("invalid ByteTrack configuration");
  }
}

void
UavByteTracker::activate(Track& track)
{
  track.trackId = m_nextId++;
  m_filter.initiate(xyahFromTlwh(track.initialTlwh), track.mean, track.covariance);
  track.initialized = true;
  track.trackletLength = 0;
  track.status = TrackStatus::TRACKED;
  if (m_frameId == 1) track.isActivated = true;
  track.frameId = m_frameId;
  track.startFrame = m_frameId;
}

void
UavByteTracker::predict(Track& track) const
{
  if (track.status != TrackStatus::TRACKED) track.mean[7] = 0;
  m_filter.predict(track.mean, track.covariance);
}

void
UavByteTracker::updateTrack(Track& track, const Detection& detection)
{
  track.frameId = m_frameId;
  ++track.trackletLength;
  if (!m_filter.update(track.mean, track.covariance, xyahFromTlwh(detection.tlwh))) {
    throw std::runtime_error("ByteTrack Kalman update is singular");
  }
  track.status = TrackStatus::TRACKED;
  track.isActivated = true;
  track.score = detection.score;
  track.cls = detection.cls;
  track.index = detection.index;
}

void
UavByteTracker::reActivate(Track& track, const Detection& detection)
{
  if (!m_filter.update(track.mean, track.covariance, xyahFromTlwh(detection.tlwh))) {
    throw std::runtime_error("ByteTrack Kalman re-activation is singular");
  }
  track.trackletLength = 0;
  track.status = TrackStatus::TRACKED;
  track.isActivated = true;
  track.frameId = m_frameId;
  track.score = detection.score;
  track.cls = detection.cls;
  track.index = detection.index;
}

void
UavByteTracker::markLost(Track& track)
{
  track.status = TrackStatus::LOST;
}

void
UavByteTracker::markRemoved(Track& track)
{
  track.status = TrackStatus::REMOVED;
}

std::vector<Track>
UavByteTracker::update(const std::vector<Detection>& detections)
{
  ++m_frameId;
  std::vector<Track> high;
  std::vector<Track> low;
  for (const auto& detection : detections) {
    if (!(detection.tlwh[2] > 0) || !(detection.tlwh[3] > 0)) continue;
    Track track;
    track.initialTlwh = detection.tlwh;
    track.score = detection.score;
    track.cls = detection.cls;
    track.index = detection.index;
    if (detection.score >= m_config.highThreshold) high.push_back(std::move(track));
    else if (detection.score > m_config.lowThreshold) low.push_back(std::move(track));
  }

  std::vector<Track> unconfirmed;
  std::vector<Track> pool;
  for (auto& track : m_tracked) {
    if (track.isActivated) pool.push_back(std::move(track));
    else unconfirmed.push_back(std::move(track));
  }
  m_tracked.clear();
  for (auto& track : m_lost) pool.push_back(std::move(track));
  m_lost.clear();
  for (auto& track : pool) predict(track);

  const auto detectionOf = [] (const Track& track) {
    Detection value;
    value.tlwh = track.initialTlwh;
    value.score = track.score;
    value.cls = track.cls;
    value.index = track.index;
    return value;
  };

  auto costs = iouDistance(pool, high);
  if (m_config.fuseScore) fuseScores(costs, high);
  const auto first = linearAssignment(costs, m_config.matchThreshold, high.size());
  for (const auto& [row, column] : first.matches) {
    if (pool[row].status == TrackStatus::TRACKED) {
      updateTrack(pool[row], detectionOf(high[column]));
    }
    else {
      reActivate(pool[row], detectionOf(high[column]));
    }
  }

  std::vector<size_t> remainingTracked;
  for (const auto row : first.unmatchedRows) {
    if (pool[row].status == TrackStatus::TRACKED) remainingTracked.push_back(row);
  }
  std::vector<Track> removedThisFrame;
  if (!remainingTracked.empty() && !low.empty()) {
    std::vector<Track> remaining;
    for (const auto row : remainingTracked) remaining.push_back(pool[row]);
    const auto second = linearAssignment(iouDistance(remaining, low), 0.5, low.size());
    std::vector<char> matched(remaining.size(), 0);
    for (const auto& [row, column] : second.matches) {
      updateTrack(pool[remainingTracked[row]], detectionOf(low[column]));
      matched[row] = 1;
    }
    for (size_t index = 0; index < remainingTracked.size(); ++index) {
      if (matched[index]) continue;
      markLost(pool[remainingTracked[index]]);
    }
  }
  else {
    for (const auto row : remainingTracked) {
      markLost(pool[row]);
    }
  }

  std::vector<Track> leftover;
  for (const auto column : first.unmatchedColumns) leftover.push_back(high[column]);
  std::vector<size_t> newDetectionIndices;
  std::vector<Track> keptUnconfirmed;
  if (!unconfirmed.empty() && !leftover.empty()) {
    const auto unconfirmedMatch = linearAssignment(iouDistance(unconfirmed, leftover), 0.7, leftover.size());
    std::vector<char> matched(unconfirmed.size(), 0);
    for (const auto& [row, column] : unconfirmedMatch.matches) {
      updateTrack(unconfirmed[row], detectionOf(leftover[column]));
      keptUnconfirmed.push_back(unconfirmed[row]);
      matched[row] = 1;
    }
    for (size_t index = 0; index < unconfirmed.size(); ++index) {
      if (matched[index]) continue;
      markRemoved(unconfirmed[index]);
      removedThisFrame.push_back(unconfirmed[index]);
    }
    for (const auto column : unconfirmedMatch.unmatchedColumns) newDetectionIndices.push_back(column);
  }
  else {
    for (auto& track : unconfirmed) {
      markRemoved(track);
      removedThisFrame.push_back(track);
    }
    for (size_t column = 0; column < leftover.size(); ++column) newDetectionIndices.push_back(column);
  }

  std::vector<Track> newTracks;
  for (const auto column : newDetectionIndices) {
    if (leftover[column].score < m_config.newTrackThreshold) continue;
    Track track = leftover[column];
    activate(track);
    newTracks.push_back(track);
  }

  for (auto& track : pool) {
    if (track.status == TrackStatus::LOST &&
        m_frameId - track.frameId > m_config.trackBuffer) {
      markRemoved(track);
    }
  }

  std::vector<Track> nextTracked;
  std::vector<Track> nextLost;
  std::vector<Track> nextRemoved = m_removed;
  for (auto& track : pool) {
    if (track.status == TrackStatus::TRACKED) nextTracked.push_back(std::move(track));
    else if (track.status == TrackStatus::LOST) nextLost.push_back(std::move(track));
    else nextRemoved.push_back(std::move(track));
  }
  for (auto& track : keptUnconfirmed) {
    if (track.status == TrackStatus::TRACKED) nextTracked.push_back(std::move(track));
  }
  for (auto& track : newTracks) nextTracked.push_back(std::move(track));
  for (auto& track : removedThisFrame) nextRemoved.push_back(std::move(track));

  auto dedupe = [] (std::vector<Track>& tracks) {
    std::vector<Track> unique;
    for (auto& track : tracks) {
      const bool seen = std::any_of(unique.begin(), unique.end(),
        [&] (const Track& other) { return other.trackId == track.trackId; });
      if (!seen) unique.push_back(std::move(track));
    }
    tracks = std::move(unique);
  };
  dedupe(nextTracked);
  dedupe(nextLost);
  removeByIds(nextLost, nextTracked);
  removeByIds(nextLost, nextRemoved);
  removeDuplicates(nextTracked, nextLost);
  if (nextRemoved.size() > 1000) {
    nextRemoved.erase(nextRemoved.begin(),
      nextRemoved.begin() + static_cast<std::ptrdiff_t>(nextRemoved.size() - 1000));
  }
  if (nextTracked.size() + nextLost.size() > m_config.maximumTracks) {
    throw std::runtime_error("ByteTrack track count exceeds its bound");
  }
  m_tracked = std::move(nextTracked);
  m_lost = std::move(nextLost);
  m_removed = std::move(nextRemoved);

  std::vector<Track> output;
  for (const auto& track : m_tracked) {
    if (track.isActivated) output.push_back(track);
  }
  return output;
}




namespace {

person_json::Json
trackJson(const Track& track, uint64_t currentFrame)
{
  using person_json::Json;
  Json tlwh = Json::array();
  for (double value : track.initialTlwh) tlwh.push_back(value);
  Json mean = Json::array();
  for (double value : track.mean) mean.push_back(value);
  Json covariance = Json::array();
  for (const auto& row : track.covariance) {
    Json values = Json::array();
    for (double value : row) values.push_back(value);
    covariance.push_back(std::move(values));
  }
  return Json{{"trackId", track.trackId}, {"isActivated", track.isActivated},
              {"state", static_cast<uint64_t>(track.status)}, {"tlwh", std::move(tlwh)},
              {"mean", std::move(mean)}, {"covariance", std::move(covariance)},
              {"score", track.score}, {"class", track.cls}, {"index", track.index},
              {"angle", nullptr}, {"startFrame", track.startFrame},
              {"frameId", track.frameId},
              {"timeSinceUpdate", currentFrame >= track.frameId ? currentFrame - track.frameId : 0},
              {"trackletLength", track.trackletLength}};
}

} // namespace

std::string
UavByteTracker::stateJson() const
{
  using person_json::Json;
  Json tracked = Json::array();
  for (const auto& track : m_tracked) tracked.push_back(trackJson(track, m_frameId));
  Json lost = Json::array();
  for (const auto& track : m_lost) lost.push_back(trackJson(track, m_frameId));
  Json removed = Json::array();
  for (const auto& track : m_removed) removed.push_back(track.trackId);
  return Json{{"frameId", m_frameId}, {"tracked", std::move(tracked)},
              {"lost", std::move(lost)}, {"removedIds", std::move(removed)},
              {"idHighWatermark", idHighWatermark()}}.dump();
}

void
UavByteTracker::restoreState(const std::string& stateJson)
{
  using person_json::Json;
  const Json state = person_json::parseBounded(stateJson, 8U * 1024U * 1024U);
  // Reject malformed count/shape/bounds before mutating this tracker.
  checkTracker(state, m_config.maximumTracks, BackendStateFormat::COMPACT_CURRENT);
  const uint64_t frameId = person_json::unsignedNumber(state.at("frameId"));
  const uint64_t high = person_json::unsignedNumber(state.at("idHighWatermark"));
  auto parseTrack = [&] (const Json& wire, TrackStatus expected) {
    person_json::exactKeys(wire, {"trackId", "isActivated", "state", "tlwh", "mean",
      "covariance", "score", "class", "index", "angle", "startFrame", "frameId",
      "timeSinceUpdate", "trackletLength"});
    Track track;
    track.trackId = person_json::unsignedNumber(wire.at("trackId"));
    track.isActivated = person_json::boolean(wire.at("isActivated"));
    track.status = static_cast<TrackStatus>(person_json::unsignedNumber(wire.at("state"), 3));
    if (track.status != expected) person_json::reject("restored track state");
    const auto& tlwh = wire.at("tlwh");
    for (size_t index = 0; index < 4; ++index) {
      track.initialTlwh[index] = person_json::finiteNumber(tlwh[index]);
    }
    const auto& mean = wire.at("mean");
    for (size_t index = 0; index < 8; ++index) {
      track.mean[index] = person_json::finiteNumber(mean[index]);
    }
    const auto& covariance = wire.at("covariance");
    for (size_t row = 0; row < 8; ++row) {
      for (size_t column = 0; column < 8; ++column) {
        track.covariance[row][column] =
          person_json::finiteNumber(covariance[row][column]);
      }
    }
    track.initialized = true;
    track.score = person_json::finiteNumber(wire.at("score"));
    track.cls = person_json::finiteNumber(wire.at("class"));
    track.index = person_json::finiteNumber(wire.at("index"));
    track.startFrame = person_json::unsignedNumber(wire.at("startFrame"));
    track.frameId = person_json::unsignedNumber(wire.at("frameId"));
    track.trackletLength = person_json::unsignedNumber(wire.at("trackletLength"));
    return track;
  };
  std::vector<Track> tracked;
  for (const auto& item : state.at("tracked")) tracked.push_back(parseTrack(item, TrackStatus::TRACKED));
  std::vector<Track> lost;
  for (const auto& item : state.at("lost")) lost.push_back(parseTrack(item, TrackStatus::LOST));
  std::vector<Track> removed;
  for (const auto& id : state.at("removedIds")) {
    Track track;
    track.trackId = person_json::unsignedNumber(id);
    track.status = TrackStatus::REMOVED;
    removed.push_back(track);
  }
  m_frameId = frameId;
  m_nextId = high + 1;
  m_tracked = std::move(tracked);
  m_lost = std::move(lost);
  m_removed = std::move(removed);
}

} // namespace ndnsf::examples::uav::byte_track

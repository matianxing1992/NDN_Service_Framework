#include "uav-vehicle-tracking-engine.hpp"

#include "../shared/uav-byte-track-state.hpp"
#include "../shared/UavDigest.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <locale>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace ndnsf::examples::uav {
namespace {

using person_json::Json;
using person_json::checkIdentifier;
using person_json::exactKeys;
using person_json::finiteNumber;
using person_json::isDigest;
using person_json::parseBounded;
using person_json::text;
using person_json::unsignedNumber;
constexpr size_t MAXIMUM_BYTES = 8 * 1024 * 1024;
constexpr size_t MAXIMUM_DETECTIONS = 300;
constexpr std::array<const char*, 3> CAMERAS = {"UAV1", "UAV2", "UAV3"};

[[noreturn]] void
rejectVehicle(const std::string& reason)
{
  throw std::invalid_argument("vehicle-backend-invalid: " + reason);
}

void
checkLabel(const std::string& value)
{
  if (value.empty() || value.size() > 1024 ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return ch >= 0x20 && ch != 0x7f;
      })) rejectVehicle("class label");
}

uint64_t
wireUnsigned(const Json& value)
{
  if (value.is_number_unsigned()) return value.get<uint64_t>();
  if (value.is_number_integer() && value.get<int64_t>() >= 0) {
    return static_cast<uint64_t>(value.get<int64_t>());
  }
  if (!value.is_string()) rejectVehicle("observation integer type");
  const auto wire = value.get<std::string>();
  uint64_t result = 0;
  const auto parsed = std::from_chars(wire.data(), wire.data() + wire.size(), result);
  if (wire.empty() || parsed.ec != std::errc() || parsed.ptr != wire.data() + wire.size() ||
      (wire.size() > 1 && wire.front() == '0')) rejectVehicle("observation integer spelling/range");
  return result;
}

void
checkContext(const VehicleWindowContext& context)
{
  checkIdentifier(context.runId);
  checkIdentifier(context.missionId);
  checkIdentifier(context.windowId);
  if (context.sequence == 0 || context.sequence > 600 ||
      !isDigest(context.nativeDecisionDigest) ||
      context.windowId != "window-" + std::to_string(context.sequence - 1)) {
    rejectVehicle("native window identity");
  }
  for (size_t i = 0; i < CAMERAS.size(); ++i) {
    const auto& frame = context.frames[i];
    if (frame.cameraId != CAMERAS[i] || frame.sequence != context.sequence ||
        frame.commonPtsUs < 0 || !isDigest(frame.inputDigest)) {
      rejectVehicle("native frame binding");
    }
    if (context.observationMode) {
      checkIdentifier(frame.sourceId);
      if (frame.sourceEpoch == 0 || frame.width <= 0 || frame.height <= 0 ||
          frame.observationName.empty() || !isDigest(frame.observationDigest) ||
          !frame.observation.is_object()) {
        rejectVehicle("native observation binding");
      }
      const auto& raw = frame.observation;
      const auto reason = raw.value("reason", Json());
      if (!reason.is_string() || reason.get_ref<const std::string&>().size() > 1024) {
        rejectVehicle("observation reason byte bound");
      }
      const auto& identity = raw.at("identity");
      const auto& key = raw.at("key");
      if (text(raw.at("schema")) != "uav-observation-v2" ||
          text(identity.at("sourceId")) != frame.sourceId ||
          text(identity.at("cameraId")) != frame.cameraId ||
          wireUnsigned(identity.at("epoch")) != frame.sourceEpoch ||
          text(key.at("sourceId")) != frame.sourceId || text(key.at("cameraId")) != frame.cameraId ||
          wireUnsigned(key.at("epoch")) != frame.sourceEpoch ||
          wireUnsigned(key.at("sequence")) != frame.sequence ||
          wireUnsigned(raw.at("captureTimeUs")) != static_cast<uint64_t>(frame.commonPtsUs) ||
          wireUnsigned(raw.at("width")) != static_cast<uint64_t>(frame.width) ||
          wireUnsigned(raw.at("height")) != static_cast<uint64_t>(frame.height)) {
        rejectVehicle("observation differs from native frame context");
      }
    }
  }
}

Json
contextIdentity(const VehicleWindowContext& context);

int
positiveDimension(const Json& value)
{
  const auto number = unsignedNumber(value, std::numeric_limits<int>::max());
  if (number == 0) rejectVehicle("empty image dimension");
  return static_cast<int>(number);
}

} // namespace

namespace {

using Vec3 = std::array<double, 3>;
using Quaternion = std::array<double, 4>;
using Covariance = std::array<double, 9>;
using Matrix = std::vector<std::vector<double>>;

double
number(const Json& value)
{
  if (value.is_number()) return finiteNumber(value);
  if (!value.is_string()) rejectVehicle("geometry numeric type");
  std::istringstream stream(value.get<std::string>());
  stream.imbue(std::locale::classic());
  double result;
  stream >> std::noskipws >> result;
  if (!stream || !stream.eof() || !std::isfinite(result)) {
    rejectVehicle("geometry numeric string");
  }
  return result;
}

template<size_t N>
std::array<double, N>
numbers(const Json& wire)
{
  if (!wire.is_array() || wire.size() != N) rejectVehicle("geometry array dimension");
  std::array<double, N> result;
  for (size_t i = 0; i < N; ++i) result[i] = number(wire.at(i));
  return result;
}

double
dot(const Vec3& a, const Vec3& b)
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vec3
cross(const Vec3& a, const Vec3& b)
{
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
          a[0] * b[1] - a[1] * b[0]};
}

Quaternion
multiplyQuaternion(const Quaternion& a, const Quaternion& b)
{
  return {a[0] * b[0] - a[1] * b[1] - a[2] * b[2] - a[3] * b[3],
          a[0] * b[1] + a[1] * b[0] + a[2] * b[3] - a[3] * b[2],
          a[0] * b[2] - a[1] * b[3] + a[2] * b[0] + a[3] * b[1],
          a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0]};
}

Vec3
rotate(const Quaternion& quaternion, const Vec3& vector)
{
  const Vec3 imaginary{quaternion[1], quaternion[2], quaternion[3]};
  const auto uv = cross(imaginary, vector);
  const auto uuv = cross(imaginary, uv);
  Vec3 result;
  for (size_t i = 0; i < 3; ++i) {
    result[i] = vector[i] + 2 * (quaternion[0] * uv[i] + uuv[i]);
  }
  return result;
}

void
checkUnit(const Quaternion& quaternion)
{
  double squared = 0;
  for (const auto value : quaternion) squared += value * value;
  if (std::abs(squared - 1.0) > 1e-6) rejectVehicle("INVALID_QUATERNION_OR_POSITION");
}

bool
roiContains(const Vec3& point, const Json& ground)
{
  const auto& vertices = ground.at("roiVertices");
  if (!vertices.is_array() || vertices.size() < 3) rejectVehicle("ROI_DEGENERATE");
  const auto origin = numbers<3>(vertices.at(0));
  const auto next = numbers<3>(vertices.at(1));
  Vec3 u;
  for (size_t i = 0; i < 3; ++i) u[i] = next[i] - origin[i];
  const auto length = std::sqrt(dot(u, u));
  if (length <= 1e-12) rejectVehicle("ROI_DEGENERATE");
  for (auto& value : u) value /= length;
  auto v = cross(numbers<3>(ground.at("normal")), u);
  const auto vLength = std::sqrt(dot(v, v));
  if (vLength <= 1e-12) rejectVehicle("ROI_DEGENERATE");
  for (auto& value : v) value /= vLength;
  const auto project = [&] (const Vec3& world) {
    Vec3 delta;
    for (size_t i = 0; i < 3; ++i) delta[i] = world[i] - origin[i];
    return std::array<double, 2>{dot(delta, u), dot(delta, v)};
  };
  const auto candidate = project(point);
  int sign = 0;
  for (size_t i = 0; i < vertices.size(); ++i) {
    const auto a = project(numbers<3>(vertices.at(i)));
    const auto b = project(numbers<3>(vertices.at((i + 1) % vertices.size())));
    const auto product = (b[0] - a[0]) * (candidate[1] - a[1]) -
                         (b[1] - a[1]) * (candidate[0] - a[0]);
    if (std::abs(product) <= 1e-8) continue;
    const auto current = product > 0 ? 1 : -1;
    if (sign != 0 && current != sign) return false;
    sign = current;
  }
  return true;
}

Vec3
projectRaw(const std::array<double, 2>& pixel, const Json& observation, bool checkRoi = true)
{
  const auto& calibration = observation.at("calibration");
  const auto& ground = observation.at("ground");
  const auto position = numbers<3>(observation.at("positionW"));
  const auto quaternion = numbers<4>(observation.at("qWC"));
  checkUnit(quaternion);
  const auto fx = number(calibration.at("fx"));
  const auto fy = number(calibration.at("fy"));
  if (fx <= 0 || fy <= 0) rejectVehicle("INVALID_FOCAL_LENGTH");
  const double xd = (pixel[0] - number(calibration.at("cx"))) / fx;
  const double yd = (pixel[1] - number(calibration.at("cy"))) / fy;
  double x = xd;
  double y = yd;
  if (calibration.value("imageTransform", std::string("raw")) != "rectified") {
    const auto coefficients = numbers<5>(calibration.at("distortion"));
    for (size_t iteration = 0; iteration < 8; ++iteration) {
      const auto r2 = x * x + y * y;
      const auto radial = 1 + coefficients[0] * r2 + coefficients[1] * r2 * r2 +
                          coefficients[4] * r2 * r2 * r2;
      if (!std::isfinite(radial) || std::abs(radial) < 1e-12) {
        rejectVehicle("DISTORTION_SINGULAR");
      }
      const auto dx = 2 * coefficients[2] * x * y + coefficients[3] * (r2 + 2 * x * x);
      const auto dy = coefficients[2] * (r2 + 2 * y * y) + 2 * coefficients[3] * x * y;
      x = (xd - dx) / radial;
      y = (yd - dy) / radial;
    }
  }
  Vec3 ray{x, y, 1};
  const auto rayLength = std::sqrt(dot(ray, ray));
  for (auto& value : ray) value /= rayLength;
  ray = rotate(quaternion, ray);
  const auto normal = numbers<3>(ground.at("normal"));
  if (std::abs(dot(normal, normal) - 1) > 1e-6) rejectVehicle("INVALID_GROUND_NORMAL");
  const auto denominator = dot(normal, ray);
  if (std::abs(denominator) < 0.1) rejectVehicle("RAY_NEAR_HORIZON");
  const auto distance = -(dot(normal, position) + number(ground.at("offsetM"))) / denominator;
  if (!std::isfinite(distance) || distance <= 0 || distance > 500) {
    rejectVehicle("GROUND_INTERSECTION_BEHIND_CAMERA");
  }
  Vec3 point;
  for (size_t i = 0; i < 3; ++i) {
    point[i] = position[i] + distance * ray[i];
    if (!std::isfinite(point[i]) || std::abs(point[i]) > 1e7) {
      rejectVehicle("GROUND_INTERSECTION_INVALID");
    }
  }
  if (checkRoi && !roiContains(point, ground)) rejectVehicle("GROUND_INTERSECTION_OUTSIDE_ROI");
  return point;
}

std::optional<Matrix>
covarianceMatrix(const Json& flat, size_t size)
{
  if (flat.is_null()) return std::nullopt;
  if (!flat.is_array() || flat.size() != size * size) rejectVehicle("covariance dimensions");
  Matrix matrix(size, std::vector<double>(size));
  double scale = 1;
  for (size_t row = 0; row < size; ++row) {
    for (size_t col = 0; col < size; ++col) {
      matrix[row][col] = number(flat.at(row * size + col));
      scale = std::max(scale, std::abs(matrix[row][col]));
    }
  }
  const auto tolerance = 1e-10 * scale;
  for (size_t row = 0; row < size; ++row) {
    for (size_t col = 0; col < row; ++col) {
      if (std::abs(matrix[row][col] - matrix[col][row]) > tolerance) return std::nullopt;
      matrix[row][col] = matrix[col][row] = (matrix[row][col] + matrix[col][row]) / 2;
    }
  }
  Matrix lower(size, std::vector<double>(size));
  std::vector<double> diagonal(size);
  for (size_t col = 0; col < size; ++col) {
    auto pivot = matrix[col][col];
    for (size_t index = 0; index < col; ++index) {
      pivot -= lower[col][index] * lower[col][index] * diagonal[index];
    }
    if (pivot < -tolerance) return std::nullopt;
    diagonal[col] = pivot <= tolerance ? 0 : pivot;
    for (size_t row = col + 1; row < size; ++row) {
      auto residual = matrix[row][col];
      for (size_t index = 0; index < col; ++index) {
        residual -= lower[row][index] * lower[col][index] * diagonal[index];
      }
      if (pivot <= tolerance) {
        if (std::abs(residual) > tolerance) return std::nullopt;
      }
      else lower[row][col] = residual / pivot;
    }
  }
  return matrix;
}

template<typename Projection>
Covariance
propagateCovariance(Projection projection, const std::vector<double>& values,
                    const std::vector<double>& steps, const Matrix& covariance)
{
  std::vector<Vec3> columns;
  for (size_t i = 0; i < values.size(); ++i) {
    auto before = values;
    auto after = values;
    before[i] -= steps[i];
    after[i] += steps[i];
    const auto low = projection(before);
    const auto high = projection(after);
    Vec3 column;
    for (size_t axis = 0; axis < 3; ++axis) column[axis] = (high[axis] - low[axis]) / (2 * steps[i]);
    columns.push_back(column);
  }
  Covariance result{};
  for (size_t row = 0; row < 3; ++row) {
    for (size_t col = 0; col < 3; ++col) {
      for (size_t i = 0; i < columns.size(); ++i) {
        for (size_t j = 0; j < columns.size(); ++j) {
          result[row * 3 + col] += columns[i][row] * covariance[i][j] * columns[j][col];
        }
      }
      if (!std::isfinite(result[row * 3 + col])) rejectVehicle("covariance overflow");
    }
  }
  return result;
}

struct GroundEstimate
{
  Vec3 point;
  Vec3 camera;
  double horizontalRange;
  double slantRange;
  std::optional<Covariance> pixelCovariance;
  std::optional<Covariance> systematicCovariance;
  std::optional<double> rangeSigma;
};

GroundEstimate
projectGround(const std::array<double, 2>& pixel, const Json& observation)
{
  if (observation.value("poseStatus", std::string()) != "aligned" ||
      observation.value("positionW", Json()).is_null() ||
      observation.value("qWC", Json()).is_null()) {
    const auto reason = observation.value("reason", Json());
    rejectVehicle(reason.is_string() && !reason.get<std::string>().empty()
                    ? reason.get<std::string>() : "POSE_UNAVAILABLE");
  }
  if (observation.value("calibration", Json()).is_null() ||
      observation.value("ground", Json()).is_null()) {
    rejectVehicle("CALIBRATION_OR_GROUND_MISSING");
  }
  if (number(observation.value("timeUncertaintyUs", Json(0))) != 0 &&
      observation.value("poseCovariance", Json()).is_null()) {
    rejectVehicle("TIME_UNCERTAINTY_UNBOUNDED");
  }
  GroundEstimate result;
  result.point = projectRaw(pixel, observation);
  result.camera = numbers<3>(observation.at("positionW"));
  Vec3 delta;
  for (size_t i = 0; i < 3; ++i) delta[i] = result.point[i] - result.camera[i];
  result.horizontalRange = std::hypot(delta[0], delta[1]);
  result.slantRange = std::sqrt(dot(delta, delta));
  const auto pose = covarianceMatrix(observation.value("poseCovariance", Json()), 6);
  const auto calibration = covarianceMatrix(
    observation.at("calibration").value("parameterCovariance", Json()), 9);
  const auto ground = covarianceMatrix(observation.at("ground").value("parameterCovariance", Json()), 4);
  const auto sigma = observation.value("anchorSigmaPixels", Json());
  if (!pose || !calibration || !ground || sigma.is_null()) return result;
  const auto pixelSigma = number(sigma);
  if (pixelSigma < 0) rejectVehicle("INVALID_ANCHOR_SIGMA");
  result.pixelCovariance = propagateCovariance([&] (const auto& values) {
    return projectRaw({values[0], values[1]}, observation, false);
  }, {pixel[0], pixel[1]}, {0.05, 0.05},
     {{pixelSigma * pixelSigma, 0}, {0, pixelSigma * pixelSigma}});
  const auto quaternion = numbers<4>(observation.at("qWC"));
  auto system = propagateCovariance([&] (const auto& values) {
    auto edited = observation;
    edited["positionW"] = {values[0], values[1], values[2]};
    Vec3 half{values[3] / 2, values[4] / 2, values[5] / 2};
    const auto angle = std::sqrt(dot(half, half));
    Quaternion dq{1, 0, 0, 0};
    if (angle > 1e-15) {
      dq[0] = std::cos(angle);
      for (size_t i = 0; i < 3; ++i) dq[i + 1] = half[i] * std::sin(angle) / angle;
    }
    edited["qWC"] = multiplyQuaternion(quaternion, dq);
    return projectRaw(pixel, edited, false);
  }, {result.camera[0], result.camera[1], result.camera[2], 0, 0, 0},
     {1e-4, 1e-4, 1e-4, 1e-5, 1e-5, 1e-5}, *pose);
  const auto& cal = observation.at("calibration");
  std::vector<double> calValues;
  for (const auto* key : {"fx", "fy", "cx", "cy"}) calValues.push_back(number(cal.at(key)));
  for (const auto value : numbers<5>(cal.at("distortion"))) calValues.push_back(value);
  std::vector<double> calSteps;
  for (const auto value : calValues) calSteps.push_back(std::max(std::abs(value) * 1e-6, 1e-8));
  const auto calCovariance = propagateCovariance([&] (const auto& values) {
    auto edited = observation;
    size_t index = 0;
    for (const auto* key : {"fx", "fy", "cx", "cy"}) edited["calibration"][key] = values[index++];
    edited["calibration"]["distortion"] =
      std::vector<double>(values.begin() + 4, values.end());
    return projectRaw(pixel, edited, false);
  }, calValues, calSteps, *calibration);
  const auto normal = numbers<3>(observation.at("ground").at("normal"));
  const auto groundCovariance = propagateCovariance([&] (const auto& values) {
    auto edited = observation;
    const Vec3 rawNormal{values[0], values[1], values[2]};
    const auto length = std::sqrt(dot(rawNormal, rawNormal));
    if (length <= 1e-12) rejectVehicle("INVALID_GROUND_NORMAL");
    edited["ground"]["normal"] = {values[0] / length, values[1] / length, values[2] / length};
    edited["ground"]["offsetM"] = values[3] / length;
    return projectRaw(pixel, edited, false);
  }, {normal[0], normal[1], normal[2], number(observation.at("ground").at("offsetM"))},
     {1e-6, 1e-6, 1e-6, 1e-5}, *ground);
  for (size_t i = 0; i < system.size(); ++i) system[i] += calCovariance[i] + groundCovariance[i];
  result.systematicCovariance = system;
  if (result.slantRange > 1e-12) {
    double variance = 0;
    for (size_t row = 0; row < 3; ++row) {
      for (size_t col = 0; col < 3; ++col) {
        variance += delta[row] * delta[col] / (result.slantRange * result.slantRange) *
                    ((*result.pixelCovariance)[row * 3 + col] + system[row * 3 + col]);
      }
    }
    result.rangeSigma = std::sqrt(std::max(0.0, variance));
  }
  return result;
}

} // namespace

namespace {

using MotionKey = std::tuple<std::string, std::string, uint64_t, uint64_t, uint64_t>;
using MotionSample = std::pair<int64_t, GroundEstimate>;

Json
optionalNumber(const std::optional<double>& value)
{
  return value ? Json(*value) : Json();
}

Json
optionalCovariance(const std::optional<Covariance>& value)
{
  return value ? Json(*value) : Json();
}

double
positionSigma(const GroundEstimate& ground)
{
  if (!ground.pixelCovariance || !ground.systematicCovariance) return 0;
  double variance = 0;
  for (const auto i : {0, 4, 8}) {
    variance += (*ground.pixelCovariance)[i] + (*ground.systematicCovariance)[i];
  }
  return std::sqrt(std::max(0.0, variance));
}

Json
groundState(const GroundEstimate& ground)
{
  return {{"positionW", ground.point}, {"cameraPositionW", ground.camera},
          {"horizontalRangeM", ground.horizontalRange}, {"slantRangeM", ground.slantRange},
          {"pixelCovariance", optionalCovariance(ground.pixelCovariance)},
          {"systemCovariance", optionalCovariance(ground.systematicCovariance)},
          {"rangeSigmaM", optionalNumber(ground.rangeSigma)}};
}

GroundEstimate
restoreGround(const Json& state)
{
  exactKeys(state, {"positionW", "cameraPositionW", "horizontalRangeM", "slantRangeM",
                   "pixelCovariance", "systemCovariance", "rangeSigmaM"});
  GroundEstimate result;
  result.point = numbers<3>(state.at("positionW"));
  result.camera = numbers<3>(state.at("cameraPositionW"));
  result.horizontalRange = finiteNumber(state.at("horizontalRangeM"));
  result.slantRange = finiteNumber(state.at("slantRangeM"));
  if (result.horizontalRange < 0 || result.slantRange < result.horizontalRange) {
    rejectVehicle("state range");
  }
  if (!state.at("pixelCovariance").is_null()) {
    result.pixelCovariance = numbers<9>(state.at("pixelCovariance"));
    if (!covarianceMatrix(state.at("pixelCovariance"), 3)) rejectVehicle("state pixel covariance");
  }
  if (!state.at("systemCovariance").is_null()) {
    result.systematicCovariance = numbers<9>(state.at("systemCovariance"));
    if (!covarianceMatrix(state.at("systemCovariance"), 3)) rejectVehicle("state system covariance");
  }
  if (!state.at("rangeSigmaM").is_null()) {
    result.rangeSigma = finiteNumber(state.at("rangeSigmaM"));
    if (*result.rangeSigma < 0) rejectVehicle("state range uncertainty");
  }
  return result;
}

std::string
canonicalDigest(const Json& value)
{
  ndn::util::Sha256 digest;
  digest << value.dump();
  return canonicalSha256Digest(digest.toString());
}

Json
sourceBindings(const VehicleWindowContext& context)
{
  Json result = Json::object();
  for (const auto& frame : context.frames) {
    Json binding{{"sourceId", frame.sourceId}, {"sourceEpoch", frame.sourceEpoch}};
    if (context.observationMode) {
      const auto& raw = frame.observation;
      const auto& identity = raw.at("identity");
      for (const auto* key : {"worldFrameId", "acquisitionKind", "deliveryMode"}) {
        binding[key] = text(identity.at(key));
      }
      binding["clockBindingDigest"] = canonicalDigest(raw.at("clockBinding"));
      binding["calibrationDigest"] = canonicalDigest(raw.at("calibration"));
      binding["groundDigest"] = canonicalDigest(raw.at("ground"));
    }
    result[frame.cameraId] = std::move(binding);
  }
  return result;
}

Json
unknownMeasurement(const VehicleFrameContext& frame, const Json& track, const std::string& reason)
{
  const auto& raw = frame.observation;
  const auto& identity = raw.at("identity");
  const auto calibration = raw.value("calibration", Json());
  return {{"schema", "uav-motion-result-v2"},
          {"trackKey", {{"sourceId", frame.sourceId}, {"cameraId", frame.cameraId},
                        {"sourceEpoch", frame.sourceEpoch}, {"localTrackId", track.at("localId")},
                        {"trackGeneration", track.at("trackGeneration")}}},
          {"globalId", track.at("globalId")},
          {"frameKey", {{"sourceId", frame.sourceId}, {"cameraId", frame.cameraId},
                        {"sourceEpoch", frame.sourceEpoch}, {"sequence", frame.sequence}}},
          {"observationRef", {{"name", frame.observationName}, {"digest", frame.observationDigest}}},
          {"captureTimeUs", frame.commonPtsUs}, {"estimateTimeUs", nullptr},
          {"rangeTimeUs", frame.commonPtsUs},
          {"velocityEastMps", nullptr}, {"velocityNorthMps", nullptr}, {"speedMps", nullptr},
          {"courseDeg", nullptr}, {"horizontalRangeM", nullptr}, {"slantRangeM", nullptr},
          {"sigmaSpeedMps", nullptr}, {"sigmaCourseDeg", nullptr}, {"sigmaRangeM", nullptr},
          {"speedStatus", "unknown"}, {"bearingStatus", "unknown"}, {"rangeStatus", "unknown"},
          {"quality", "unknown"}, {"reasonCodes", Json::array({reason})},
          {"acquisitionKind", identity.at("acquisitionKind")},
          {"deliveryMode", identity.at("deliveryMode")},
          {"geometryValidity", raw.at("geometryValidity")},
          {"accuracyQualification", raw.at("geometryValidity") == "analytic-fixture"
                                      ? "analytic-only" : "unvalidated"},
          {"uncertaintyKind", "unknown"}, {"anchorKind", "bbox-bottom-center"},
          {"anchorApproximation", true},
          {"calibrationRevision", calibration.is_object() ? calibration.value("revision", Json()) : Json()}};
}

std::array<double, 2>
projectWorld(const Vec3& point, const Json& observation)
{
  const auto& calibration = observation.at("calibration");
  const auto position = numbers<3>(observation.at("positionW"));
  const auto quaternion = numbers<4>(observation.at("qWC"));
  checkUnit(quaternion);
  Vec3 delta;
  for (size_t i = 0; i < 3; ++i) delta[i] = point[i] - position[i];
  const auto camera = rotate({quaternion[0], -quaternion[1], -quaternion[2], -quaternion[3]}, delta);
  if (camera[2] <= 1e-9) rejectVehicle("PROJECTED_POINT_BEHIND_CAMERA");
  double x = camera[0] / camera[2];
  double y = camera[1] / camera[2];
  if (calibration.value("imageTransform", std::string("raw")) == "raw") {
    const auto coefficients = numbers<5>(calibration.at("distortion"));
    const auto r2 = x * x + y * y;
    const auto radial = 1 + coefficients[0] * r2 + coefficients[1] * r2 * r2 +
                        coefficients[4] * r2 * r2 * r2;
    const auto xd = x * radial + 2 * coefficients[2] * x * y + coefficients[3] * (r2 + 2 * x * x);
    y = y * radial + coefficients[2] * (r2 + 2 * y * y) + 2 * coefficients[3] * x * y;
    x = xd;
  }
  std::array<double, 2> pixel{number(calibration.at("fx")) * x + number(calibration.at("cx")),
                             number(calibration.at("fy")) * y + number(calibration.at("cy"))};
  if (!std::isfinite(pixel[0]) || !std::isfinite(pixel[1])) rejectVehicle("PROJECTED_PIXEL_INVALID");
  return pixel;
}

Json
updateMotion(std::vector<MotionSample>& history, const VehicleTrackingPolicy& policy,
             const VehicleFrameContext& frame, const Json& track, const GroundEstimate& ground)
{
  const auto time = frame.commonPtsUs;
  if (!history.empty() &&
      (time <= history.back().first || time - history.back().first > policy.maximumGapUs)) {
    history.clear();
  }
  bool outlier = false;
  if (!history.empty()) {
    const auto& previous = history.back();
    const auto displacement = std::hypot(ground.point[0] - previous.second.point[0],
                                         ground.point[1] - previous.second.point[1]);
    if (displacement > policy.maximumSpeedMps * ((time - previous.first) / 1e6) +
                         3 * (positionSigma(ground) + positionSigma(previous.second))) {
      history.clear();
      outlier = true;
    }
  }
  history.emplace_back(time, ground);
  while (history.front().first < time - policy.historyWindowUs ||
         history.size() > policy.maximumPoints) history.erase(history.begin());
  auto result = unknownMeasurement(frame, track, "INSUFFICIENT_TRACK_HISTORY");
  result["rangeStatus"] = "valid";
  result["horizontalRangeM"] = ground.horizontalRange;
  result["slantRangeM"] = ground.slantRange;
  result["sigmaRangeM"] = optionalNumber(ground.rangeSigma);
  result["quality"] = "warming-up";
  result["imageArrow"] = nullptr;
  result["uncertaintyKind"] = ground.pixelCovariance && ground.systematicCovariance
                                ? "linearized-assumed-independent" : "unknown";
  if (outlier) {
    result["quality"] = "unknown";
    result["reasonCodes"] = Json::array({"OUTLIER"});
    return result;
  }
  const auto span = history.back().first - history.front().first;
  if (history.size() < policy.minimumPoints || span < policy.minimumTimeSpanUs) return result;
  result["estimateTimeUs"] = history.front().first + span / 2;
  std::vector<double> times;
  double mean = 0;
  for (const auto& sample : history) {
    times.push_back((sample.first - history.front().first) / 1e6);
    mean += times.back();
  }
  mean /= times.size();
  double denominator = 0;
  for (const auto value : times) denominator += (value - mean) * (value - mean);
  if (denominator <= 1e-12) {
    result["quality"] = "unknown";
    result["reasonCodes"] = Json::array({"INSUFFICIENT_TIME_SPAN"});
    return result;
  }
  Vec3 velocity{};
  Covariance covariance{};
  double systematicSigma = 0;
  bool complete = true;
  for (size_t i = 0; i < history.size(); ++i) {
    const auto weight = (times[i] - mean) / denominator;
    const auto& sample = history[i].second;
    for (size_t axis = 0; axis < 3; ++axis) velocity[axis] += weight * sample.point[axis];
    if (!sample.pixelCovariance || !sample.systematicCovariance) {
      complete = false;
      continue;
    }
    for (size_t entry = 0; entry < 9; ++entry) {
      covariance[entry] += weight * weight * (*sample.pixelCovariance)[entry];
    }
    const auto& system = *sample.systematicCovariance;
    systematicSigma += std::abs(weight) * std::sqrt(std::max(0.0, system[0] + system[4] + system[8]));
  }
  const auto speed = std::hypot(velocity[0], velocity[1]);
  if (!std::isfinite(speed)) rejectVehicle("motion overflow");
  std::optional<double> sigmaSpeed;
  std::optional<double> sigmaBearing;
  constexpr double DEGREES_PER_RADIAN = 180.0 / 3.14159265358979323846;
  if (complete) {
    for (const auto index : {0, 4, 8}) covariance[index] += systematicSigma * systematicSigma;
    if (speed > 1e-9) {
      sigmaSpeed = std::sqrt(std::max(0.0,
        (velocity[0] * velocity[0] * covariance[0] + velocity[1] * velocity[1] * covariance[4] +
         2 * velocity[0] * velocity[1] * covariance[1]) / (speed * speed)));
      const auto e = velocity[1] / (speed * speed);
      const auto n = -velocity[0] / (speed * speed);
      sigmaBearing = DEGREES_PER_RADIAN * std::sqrt(std::max(0.0,
        e * e * covariance[0] + n * n * covariance[4] + 2 * e * n * covariance[1]));
    }
    else sigmaSpeed = std::sqrt(std::max(0.0, covariance[0] + covariance[4]));
  }
  result["velocityEastMps"] = velocity[0];
  result["velocityNorthMps"] = velocity[1];
  result["speedMps"] = speed;
  result["speedStatus"] = "valid";
  result["sigmaSpeedMps"] = optionalNumber(sigmaSpeed);
  result["sigmaCourseDeg"] = optionalNumber(sigmaBearing);
  result["uncertaintyKind"] = sigmaSpeed ? "linearized-assumed-independent" : "unknown";
  if (sigmaSpeed && speed + 2 * *sigmaSpeed < policy.stationaryThresholdMps) {
    result["quality"] = "stationary";
    result["reasonCodes"] = Json::array({"STATIONARY"});
  }
  else if (speed < policy.stationaryThresholdMps) {
    result["quality"] = "degraded";
    result["reasonCodes"] = Json::array({"LOW_SPEED_DIRECTION_UNKNOWN"});
  }
  else {
    result["quality"] = "estimated";
    result["reasonCodes"] = Json::array({"BEARING_UNCERTAINTY"});
    if (sigmaSpeed && speed > 2 * *sigmaSpeed && sigmaBearing &&
        *sigmaBearing <= policy.maximumBearingSigmaDegrees) {
      result["courseDeg"] = std::fmod(DEGREES_PER_RADIAN * std::atan2(velocity[0], velocity[1]) + 360, 360);
      result["bearingStatus"] = "valid";
      result["reasonCodes"] = Json::array();
      Vec3 target = ground.point;
      const auto distance = std::max(0.5, std::min(5.0, ground.slantRange * 0.05));
      target[0] += velocity[0] / speed * distance;
      target[1] += velocity[1] / speed * distance;
      try {
        const auto tip = projectWorld(target, frame.observation);
        const auto box = track.at("box").get<std::array<int, 4>>();
        const std::array<double, 2> origin{box[0] / 2.0 + box[2] / 2.0, static_cast<double>(box[3])};
        const std::array<double, 2> direction{tip[0] - origin[0], tip[1] - origin[1]};
        if (std::hypot(direction[0], direction[1]) > 1e-6) {
          result["imageArrow"] = {{"originPx", origin}, {"directionPx", direction}};
        }
      }
      catch (const std::exception&) {
        // An unavailable display projection does not invalidate world motion.
      }
    }
  }
  return result;
}

} // namespace
namespace {

int64_t
integer(const Json& value, int64_t minimum, int64_t maximum)
{
  if (!value.is_number_integer() || value.is_boolean()) rejectVehicle("legacy integer type");
  if (value.is_number_unsigned() && value.get<uint64_t>() > static_cast<uint64_t>(maximum)) {
    rejectVehicle("legacy integer range");
  }
  const auto result = value.get<int64_t>();
  if (result < minimum || result > maximum) rejectVehicle("legacy integer range");
  return result;
}

Json
legacyTelemetry(const Json& input)
{
  Json result;
  result["droneId"] = text(input.at("droneId"));
  result["cameraId"] = text(input.at("cameraId"));
  checkIdentifier(text(result.at("droneId")));
  checkIdentifier(text(result.at("cameraId")));
  for (const auto* key : {"sourceEpoch", "sequence"}) {
    result[key] = integer(input.at(key), 1, std::numeric_limits<int64_t>::max());
  }
  for (const auto* key : {"sourceTimestampUs", "altitudeMm"}) {
    result[key] = integer(input.at(key), 0, std::numeric_limits<int64_t>::max());
  }
  result["latitudeE7"] = integer(input.at("latitudeE7"), -900000000, 900000000);
  result["longitudeE7"] = integer(input.at("longitudeE7"), -1800000000, 1800000000);
  result["headingCdeg"] = integer(input.at("headingCdeg"), 0, 35999);
  std::optional<int64_t> speed;
  if (input.contains("groundSpeedMmps") && !input.at("groundSpeedMmps").is_null()) {
    const auto raw = number(input.at("groundSpeedMmps"));
    if (raw < 0 || raw >= static_cast<double>(std::numeric_limits<int64_t>::max()) ||
        std::trunc(raw) != raw) rejectVehicle("legacy speed unit");
    speed = static_cast<int64_t>(raw);
  }
  if (input.contains("groundspeed_mps") && !input.at("groundspeed_mps").is_null()) {
    const auto raw = number(input.at("groundspeed_mps"));
    if (raw < 0 || raw * 1000 >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      rejectVehicle("legacy speed range");
    }
    const auto converted = static_cast<int64_t>(std::nearbyint(raw * 1000));
    if (speed && std::abs(static_cast<double>(*speed) - converted) > 1) {
      rejectVehicle("legacy speed unit disagreement");
    }
    speed = converted;
  }
  if (!speed) rejectVehicle("legacy speed missing");
  result["groundSpeedMmps"] = *speed;
  const auto freshness = input.value("freshness", std::string("fresh"));
  if (freshness != "fresh" && freshness != "stale" && freshness != "missing") {
    rejectVehicle("legacy telemetry freshness");
  }
  result["freshness"] = freshness;
  result["provenance"] = input.value("provenance", std::string("simulated-input"));
  checkIdentifier(text(result.at("provenance")));
  return result;
}

std::string
legacyFloat(double value)
{
  // The old profile digest uses Python JSON's shortest float spelling. Reuse
  // the vendored shortest digits, with Python's fixed/scientific cutover.
  auto wire = Json(value).dump();
  const bool negative = value < 0 || std::signbit(value);
  if (value == 0) return negative ? "-0.0" : "0.0";
  if (negative) wire.erase(wire.begin());
  const auto exponentPosition = wire.find('e');
  const int explicitExponent = exponentPosition == std::string::npos
    ? 0 : std::stoi(wire.substr(exponentPosition + 1));
  auto digits = wire.substr(0, exponentPosition);
  const auto dotPosition = digits.find('.');
  int decimalPosition = dotPosition == std::string::npos
    ? static_cast<int>(digits.size()) : static_cast<int>(dotPosition);
  if (dotPosition != std::string::npos) digits.erase(dotPosition, 1);
  while (digits.size() > 1 && digits.front() == '0') {
    digits.erase(digits.begin());
    --decimalPosition;
  }
  while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
  const int exponent = explicitExponent + decimalPosition - 1;
  std::string result = negative ? "-" : "";
  if (exponent < -4 || exponent >= 16) {
    result += digits.substr(0, 1);
    if (digits.size() > 1) result += "." + digits.substr(1);
    result += exponent < 0 ? "e-" : "e+";
    const auto magnitude = std::to_string(std::abs(exponent));
    if (magnitude.size() < 2) result += '0';
    return result + magnitude;
  }
  const int position = exponent + 1;
  if (position <= 0) return result + "0." + std::string(-position, '0') + digits;
  if (position >= static_cast<int>(digits.size())) {
    return result + digits + std::string(position - digits.size(), '0') + ".0";
  }
  return result + digits.substr(0, position) + "." + digits.substr(position);
}

std::string
legacyCalibrationDigest(const Json& profile)
{
  const auto stringValue = [&] (const char* key) { return profile.at(key).dump(-1, ' ', true); };
  std::string wire = "{\"cameraId\":" + stringValue("cameraId") +
    ",\"coordinateFrame\":" + stringValue("coordinateFrame") +
    ",\"distortion\":" + stringValue("distortion") + ",\"homography\":[";
  for (size_t row = 0; row < 3; ++row) {
    if (row != 0) wire += ',';
    wire += '[';
    for (size_t col = 0; col < 3; ++col) {
      if (col != 0) wire += ',';
      wire += legacyFloat(number(profile.at("homography").at(row).at(col)));
    }
    wire += ']';
  }
  wire += "],\"imageHeight\":" + profile.at("imageHeight").dump() +
          ",\"imageWidth\":" + profile.at("imageWidth").dump() + '}';
  ndn::util::Sha256 hash;
  hash << wire;
  return canonicalSha256Digest(hash.toString());
}

Json
legacyCalibration(const Json& input)
{
  Json result{{"cameraId", text(input.at("cameraId"))},
              {"imageWidth", positiveDimension(input.at("imageWidth"))},
              {"imageHeight", positiveDimension(input.at("imageHeight"))},
              {"coordinateFrame", input.value("coordinateFrame", std::string("ground-mm-ned"))},
              {"distortion", input.value("distortion", std::string("ignored-after-calibration"))}};
  if (result.at("coordinateFrame") != "ground-mm-ned") rejectVehicle("legacy calibration frame");
  const auto& wire = input.at("homography");
  if (!wire.is_array() || wire.size() != 3) rejectVehicle("legacy homography dimensions");
  std::array<Vec3, 3> matrix;
  for (size_t i = 0; i < matrix.size(); ++i) matrix[i] = numbers<3>(wire.at(i));
  if (dot(matrix[0], cross(matrix[1], matrix[2])) <= 1e-12) {
    rejectVehicle("homography is degenerate or reverses the ground frame");
  }
  result["homography"] = matrix;
  const auto digest = legacyCalibrationDigest(result);
  if (input.contains("calibrationDigest") && text(input.at("calibrationDigest")) != digest) {
    rejectVehicle("legacy calibration digest mismatch");
  }
  result["calibrationDigest"] = digest;
  return result;
}

Json
legacyEstimate(const Json& previous, const Json& current, const Json& previousTelemetry,
               const Json& currentTelemetry, const Json& calibration, bool compensate)
{
  const auto anchor = [] (const Json& track) {
    const auto box = numbers<4>(track.at("box"));
    return std::array<double, 2>{box[0] / 2 + box[2] / 2, box[3]};
  };
  const auto a = anchor(previous);
  const auto b = anchor(current);
  const auto from = previous.at("ptsUs").get<int64_t>();
  const auto to = current.at("ptsUs").get<int64_t>();
  Json result{{"vehicleId", std::to_string(current.at("globalId").get<uint64_t>())},
              {"globalId", current.at("globalId")}, {"fromPtsUs", from}, {"toPtsUs", to},
              {"pixelAnchors", Json::array({a, b})},
              {"groundPointsMm", Json::array({Json::array({0.0, 0.0}), Json::array({0.0, 0.0})})},
              {"distanceMm", nullptr}, {"vehicleSpeedMmps", nullptr}, {"unit", "mm/s"},
              {"provenance", "unknown"}, {"calibrationDigest", calibration.at("calibrationDigest")},
              {"egoMotionCompensation", "not-applied"}, {"uncertainty", "unknown"}, {"status", "unknown"}};
  const auto fail = [&] (const std::string& reason) { result["uncertainty"] = reason; return result; };
  if (previousTelemetry.at("cameraId") != currentTelemetry.at("cameraId")) return fail("telemetry camera mismatch");
  if (previousTelemetry.at("sourceEpoch") != currentTelemetry.at("sourceEpoch")) return fail("telemetry epoch mismatch");
  if (previousTelemetry.at("freshness") != "fresh" || currentTelemetry.at("freshness") != "fresh") return fail("telemetry is stale");
  if (to <= from || currentTelemetry.at("sourceTimestampUs").get<int64_t>() <=
                    previousTelemetry.at("sourceTimestampUs").get<int64_t>()) return fail("source PTS is not strictly increasing");
  if (currentTelemetry.at("sourceTimestampUs") != to) return fail("current telemetry is not source-time aligned");
  if (previousTelemetry.at("sourceTimestampUs") != from) return fail("previous telemetry is not source-time aligned");
  if (!compensate) return fail("UAV ego motion compensation is disabled");
  const auto project = [&] (const std::array<double, 2>& pixel) {
    const Vec3 point{pixel[0], pixel[1], 1};
    const auto& h = calibration.at("homography");
    const auto denominator = dot(numbers<3>(h.at(2)), point);
    if (std::abs(denominator) < 1e-12) rejectVehicle("homography projection has zero denominator");
    return std::array<double, 2>{dot(numbers<3>(h.at(0)), point) / denominator,
                                 dot(numbers<3>(h.at(1)), point) / denominator};
  };
  try {
    constexpr double RADIANS_PER_DEGREE = 3.14159265358979323846 / 180;
    const auto world = [&] (const std::array<double, 2>& local, const Json& sample) {
      const auto theta = number(sample.at("headingCdeg")) / 100 * RADIANS_PER_DEGREE;
      const auto east = (number(sample.at("longitudeE7")) - number(previousTelemetry.at("longitudeE7"))) /
                        1e7 * 111320 * std::cos(number(previousTelemetry.at("latitudeE7")) / 1e7 * RADIANS_PER_DEGREE) * 1000;
      const auto north = (number(sample.at("latitudeE7")) - number(previousTelemetry.at("latitudeE7"))) /
                         1e7 * 111320 * 1000;
      return std::array<double, 4>{east + local[0] * std::cos(theta) - local[1] * std::sin(theta),
                                   north + local[0] * std::sin(theta) + local[1] * std::cos(theta), east, north};
    };
    const auto worldA = world(project(a), previousTelemetry);
    const auto worldB = world(project(b), currentTelemetry);
    const auto distance = std::hypot(worldB[0] - worldA[0] - (worldB[2] - worldA[2]),
                                      worldB[1] - worldA[1] - (worldB[3] - worldA[3]));
    const auto speed = std::nearbyint(distance * 1e6 / (to - from));
    if (!std::isfinite(speed) || speed >= static_cast<double>(std::numeric_limits<int64_t>::max())) {
      return fail("legacy motion overflow");
    }
    result["groundPointsMm"] = Json::array({Json::array({worldA[0], worldA[1]}),
                                           Json::array({worldB[0], worldB[1]})});
    result["distanceMm"] = distance;
    result["vehicleSpeedMmps"] = static_cast<int64_t>(speed);
    result["provenance"] = "homography-estimated";
    result["egoMotionCompensation"] = "applied";
    result["uncertainty"] = "fixture-calibrated";
    result["status"] = "ok";
    return result;
  }
  catch (const std::exception& error) { return fail(error.what()); }
}

Json
contextIdentity(const VehicleWindowContext& context)
{
  Json frames = Json::array();
  for (const auto& frame : context.frames) {
    frames.push_back({{"cameraId", frame.cameraId}, {"inputDigest", frame.inputDigest},
                      {"sequence", frame.sequence}, {"commonPtsUs", frame.commonPtsUs},
                      {"imageName", frame.imageName}, {"observationName", frame.observationName},
                      {"observationDigest", frame.observationDigest},
                      {"width", frame.width}, {"height", frame.height}});
  }
  return {{"runId", context.runId}, {"missionId", context.missionId},
          {"windowId", context.windowId}, {"sequence", context.sequence},
          {"nativeDecisionDigest", context.nativeDecisionDigest},
          {"observationMode", context.observationMode}, {"sources", sourceBindings(context)},
          {"frames", frames}, {"legacyMotion", context.legacyMotion}};
}

} // namespace

struct UavVehicleTrackingEngine::Impl
{
  explicit Impl(const VehicleTrackingPolicy& configured)
    : policy(configured)
    , policyJson(Json::parse(configured.canonicalJson()))
  {
  }

  VehicleTrackingPolicy policy;
  Json policyJson;
  Json state;
  Json prepared;
  std::string resetReason;
  std::map<MotionKey, std::vector<MotionSample>> histories;


  Json emptyState(const VehicleWindowContext& context) const
  {
    Json nextLocal = Json::object();
    Json last = Json::object();
    Json pending = Json::object();
    Json cameras = Json::object();
    for (const auto* camera : CAMERAS) {
      nextLocal[camera] = uint64_t(1);
      last[camera] = Json::array();
      pending[camera] = Json::array();
      cameras[camera] = nullptr;
    }
    return {{"schema", "uav-vehicle-native-state-v1"}, {"runId", context.runId},
            {"missionId", context.missionId}, {"nativeDecisionDigest", context.nativeDecisionDigest},
            {"observationMode", context.observationMode}, {"policy", policyJson},
            {"sources", sourceBindings(context)}, {"sequence", context.sequence - 1},
            {"nextLocal", nextLocal}, {"last", last}, {"nextGlobal", uint64_t(1)},
            {"mapping", Json::array()}, {"pending", pending}, {"motion", Json::array()},
            {"previousLegacy", Json::object()},
            {"backendState", {{"schema", "uav-vehicle-backend-state-v1"}, {"cameras", cameras}}}};
  }

  static uint64_t positiveId(const Json& value)
  {
    const auto result = unsignedNumber(value, std::numeric_limits<uint64_t>::max() - 1);
    if (result == 0) rejectVehicle("state zero ID");
    return result;
  }

  static size_t cameraIndex(const std::string& camera)
  {
    for (size_t i = 0; i < CAMERAS.size(); ++i) {
      if (camera == CAMERAS[i]) return i;
    }
    rejectVehicle("state camera");
  }

  std::map<MotionKey, std::vector<MotionSample>> validateState(const Json& candidate)
  {
    exactKeys(candidate, {"schema", "runId", "missionId", "nativeDecisionDigest", "observationMode",
                          "policy", "sources", "sequence", "nextLocal", "last", "nextGlobal",
                          "mapping", "pending", "motion", "previousLegacy", "backendState"});
    if (text(candidate.at("schema")) != "uav-vehicle-native-state-v1" ||
        candidate.at("policy") != policyJson || !candidate.at("observationMode").is_boolean()) {
      rejectVehicle("native state schema/policy/mode");
    }
    checkIdentifier(text(candidate.at("runId")));
    checkIdentifier(text(candidate.at("missionId")));
    if (!isDigest(text(candidate.at("nativeDecisionDigest")))) rejectVehicle("state decision identity");
    const auto sequence = unsignedNumber(candidate.at("sequence"), 600);
    const auto nextGlobal = positiveId(candidate.at("nextGlobal"));
    const bool observationMode = candidate.at("observationMode").get<bool>();
    for (const auto* field : {"sources", "nextLocal", "last", "pending"}) {
      exactKeys(candidate.at(field), {"UAV1", "UAV2", "UAV3"});
    }
    const auto& backend = candidate.at("backendState");
    exactKeys(backend, {"schema", "cameras"});
    if (text(backend.at("schema")) != "uav-vehicle-backend-state-v1") rejectVehicle("state backend schema");
    exactKeys(backend.at("cameras"), {"UAV1", "UAV2", "UAV3"});
    std::map<std::pair<std::string, uint64_t>, uint64_t> mappings;
    const auto& mapping = candidate.at("mapping");
    if (!mapping.is_array()) rejectVehicle("state mapping type");
    for (const auto& item : mapping) {
      exactKeys(item, {"cameraId", "localId", "globalId"});
      const auto camera = text(item.at("cameraId"));
      cameraIndex(camera);
      const auto local = positiveId(item.at("localId"));
      const auto global = positiveId(item.at("globalId"));
      if (global >= nextGlobal || !mappings.emplace(std::make_pair(camera, local), global).second) {
        rejectVehicle("state duplicate/nonmonotonic mapping");
      }
    }
    for (const auto* camera : CAMERAS) {
      const auto& source = candidate.at("sources").at(camera);
      if (observationMode) {
        exactKeys(source, {"sourceId", "sourceEpoch", "worldFrameId", "acquisitionKind", "deliveryMode",
                           "clockBindingDigest", "calibrationDigest", "groundDigest"});
        checkIdentifier(text(source.at("sourceId")));
        if (unsignedNumber(source.at("sourceEpoch")) == 0) rejectVehicle("state zero source epoch");
        for (const auto* key : {"worldFrameId", "acquisitionKind", "deliveryMode"}) text(source.at(key));
        for (const auto* key : {"clockBindingDigest", "calibrationDigest", "groundDigest"}) {
          if (!isDigest(text(source.at(key)))) rejectVehicle("state source digest");
        }
      }
      else {
        exactKeys(source, {"sourceId", "sourceEpoch"});
        text(source.at("sourceId"));
        unsignedNumber(source.at("sourceEpoch"));
      }
      const auto nextLocal = positiveId(candidate.at("nextLocal").at(camera));
      const auto& tracks = candidate.at("last").at(camera);
      if (!tracks.is_array() || tracks.size() > MAXIMUM_DETECTIONS) rejectVehicle("state tracks bound");
      std::set<uint64_t> ids;
      for (const auto& track : tracks) {
        exactKeys(track, {"localId", "globalId", "class", "confidence", "box", "ptsUs", "trackGeneration"});
        const auto local = positiveId(track.at("localId"));
        const auto global = positiveId(track.at("globalId"));
        const auto found = mappings.find({camera, local});
        if (local >= nextLocal || !ids.insert(local).second || found == mappings.end() ||
            found->second != global || positiveId(track.at("trackGeneration")) != local) {
          rejectVehicle("state track identity");
        }
        checkLabel(text(track.at("class")));
        const auto confidence = finiteNumber(track.at("confidence"));
        if (confidence < 0 || confidence > 1) rejectVehicle("state track confidence");
        unsignedNumber(track.at("ptsUs"), std::numeric_limits<int64_t>::max());
        const auto& box = track.at("box");
        if (!box.is_array() || box.size() != 4) rejectVehicle("state box dimension");
        for (const auto& value : box) unsignedNumber(value, std::numeric_limits<int>::max());
        if (box.at(0) >= box.at(2) || box.at(1) >= box.at(3)) rejectVehicle("state box ordering");
      }
      for (const auto& item : mappings) {
        if (item.first.first == camera && item.first.second >= nextLocal) {
          rejectVehicle("state nextLocal below mapping");
        }
      }
      const auto& snapshot = backend.at("cameras").at(camera);
      if (!snapshot.is_null()) {
        if (!snapshot.at("removed").is_array() || snapshot.at("removed").size() > 1000) {
          rejectVehicle("state removed history bound");
        }
        byte_track::checkTracker(snapshot, std::numeric_limits<uint64_t>::max(),
                                 byte_track::BackendStateFormat::LEGACY_CONFIRMED);
      }
      const auto& pending = candidate.at("pending").at(camera);
      if (!pending.is_array()) rejectVehicle("state pending type");
      std::set<std::pair<std::string, uint64_t>> pendingIds;
      for (const auto& item : pending) {
        exactKeys(item, {"sourceCamera", "sourceLocalId", "globalId", "label", "point", "ptsUs", "lane"});
        const auto sourceCamera = text(item.at("sourceCamera"));
        const auto sourceIndex = cameraIndex(sourceCamera);
        if (sourceIndex >= 2 || cameraIndex(camera) != sourceIndex + 1) rejectVehicle("state route");
        const auto local = positiveId(item.at("sourceLocalId"));
        const auto found = mappings.find({sourceCamera, local});
        if (!pendingIds.emplace(sourceCamera, local).second || found == mappings.end() ||
            positiveId(item.at("globalId")) != found->second) rejectVehicle("state pending identity");
        checkLabel(text(item.at("label")));
        const auto point = numbers<2>(item.at("point"));
        for (const auto value : point) {
          if (value < 0 || value > 1) rejectVehicle("state normalized point");
        }
        const auto lane = text(item.at("lane"));
        if (lane != "upper" && lane != "lower") rejectVehicle("state lane");
        unsignedNumber(item.at("ptsUs"), std::numeric_limits<int64_t>::max());
      }
    }
    const auto& motion = candidate.at("motion");
    if (!motion.is_array() || motion.size() > 5000) rejectVehicle("state motion bound");
    std::map<MotionKey, std::vector<MotionSample>> restored;
    for (const auto& row : motion) {
      exactKeys(row, {"key", "samples"});
      const auto& key = row.at("key");
      if (!observationMode || !key.is_array() || key.size() != 5) rejectVehicle("state motion key");
      const auto camera = text(key.at(1));
      cameraIndex(camera);
      const auto source = text(key.at(0));
      const auto epoch = unsignedNumber(key.at(2));
      if (epoch == 0) rejectVehicle("state zero motion epoch");
      const auto local = positiveId(key.at(3));
      const auto generation = positiveId(key.at(4));
      const auto& binding = candidate.at("sources").at(camera);
      if (source != text(binding.at("sourceId")) || epoch != unsignedNumber(binding.at("sourceEpoch")) ||
          local != generation || mappings.count({camera, local}) == 0) rejectVehicle("state motion binding");
      const MotionKey identity{source, camera, epoch, local, generation};
      if (!restored.emplace(identity, std::vector<MotionSample>()).second) rejectVehicle("state duplicate motion key");
      const auto& samples = row.at("samples");
      if (!samples.is_array() || samples.size() > policy.maximumPoints) rejectVehicle("state sample bound");
      int64_t previous = -1;
      for (const auto& sample : samples) {
        exactKeys(sample, {"timeUs", "ground"});
        const auto time = static_cast<int64_t>(unsignedNumber(sample.at("timeUs"), std::numeric_limits<int64_t>::max()));
        if (time <= previous) rejectVehicle("state time ordering");
        previous = time;
        restored.at(identity).emplace_back(time, restoreGround(sample.at("ground")));
      }
    }
    if (!candidate.at("previousLegacy").is_object() ||
        (observationMode && !candidate.at("previousLegacy").empty()) || sequence == 0) {
      rejectVehicle("state previous legacy/sequence");
    }
    const auto& previousLegacy = candidate.at("previousLegacy");
    if (!previousLegacy.empty()) {
      exactKeys(previousLegacy, {"UAV1", "UAV2", "UAV3"});
      for (const auto* camera : CAMERAS) {
        const auto sample = legacyTelemetry(previousLegacy.at(camera));
        if (sample != previousLegacy.at(camera) || sample.at("cameraId") != camera ||
            sample.at("sequence") != sequence) rejectVehicle("state legacy telemetry binding");
      }
    }
    return restored;
  }

  std::pair<uint64_t, Json> associate(size_t index, uint64_t local, const VehicleDetection& detection,
                                     const VehicleFrameContext& frame, int width, int height)
  {
    const auto camera = CAMERAS[index];
    const std::array<double, 2> point{
      (detection.box[0] / 2.0 + detection.box[2] / 2.0) / width,
      (detection.box[1] / 2.0 + detection.box[3] / 2.0) / height};
    uint64_t global = 0;
    Json event;
    for (const auto& item : state.at("mapping")) {
      if (item.at("cameraId") == camera && item.at("localId") == local) {
        global = item.at("globalId").get<uint64_t>();
        break;
      }
    }
    auto& pending = state["pending"][camera];
    if (global == 0) {
      std::optional<size_t> best;
      double bestScore = 0;
      for (size_t i = 0; i < pending.size(); ++i) {
        const auto& item = pending.at(i);
        if (item.at("label") != detection.label) continue;
        const auto gap = frame.commonPtsUs - item.at("ptsUs").get<int64_t>();
        if (gap < 0 || gap > policy.associationMaxGapUs) continue;
        const auto sourceIndex = cameraIndex(text(item.at("sourceCamera")));
        const std::array<double, 4> rectangle = sourceIndex == 0
          ? std::array<double, 4>{0, 0.32, 0.22, 0.81}
          : std::array<double, 4>{0, 0.21, 0.22, 0.69};
        if (point[0] < rectangle[0] || point[0] > rectangle[2] ||
            point[1] < rectangle[1] || point[1] > rectangle[3]) continue;
        const auto threshold = sourceIndex == 0 ? 1225.0 / 2160 : 975.0 / 2160;
        if (item.at("lane") != (point[1] < threshold ? "upper" : "lower")) continue;
        const auto previous = numbers<2>(item.at("point"));
        const auto distance = std::hypot(point[0] - previous[0], point[1] - previous[1]);
        if (distance > policy.associationMaxCenterDistance) continue;
        const auto score = (1 - static_cast<double>(gap) / policy.associationMaxGapUs) + (1 - distance);
        if (!best || score > bestScore) {
          best = i;
          bestScore = score;
        }
      }
      if (best) {
        const auto item = pending.at(*best);
        global = item.at("globalId").get<uint64_t>();
        event = {{"sourceCamera", item.at("sourceCamera")}, {"sourceLocalId", item.at("sourceLocalId")},
                 {"targetCamera", camera}, {"targetLocalId", local}, {"globalId", global},
                 {"sourcePtsUs", item.at("ptsUs")}, {"targetPtsUs", frame.commonPtsUs},
                 {"reason", "overlap-lane-pts-center"}, {"score", std::nearbyint(bestScore * 1e6) / 1e6}};
        pending.erase(pending.begin() + *best);
      }
      else {
        global = positiveId(state.at("nextGlobal"));
        if (global >= std::numeric_limits<uint64_t>::max() - 1) {
          rejectVehicle("global ID exhausted");
        }
        state["nextGlobal"] = global + 1;
      }
      state["mapping"].push_back({{"cameraId", camera}, {"localId", local}, {"globalId", global}});
    }
    if (index < 2) {
      const std::array<double, 4> rectangle = index == 0
        ? std::array<double, 4>{0.78, 0.32, 1, 0.87}
        : std::array<double, 4>{0.78, 0.43, 1, 0.92};
      if (point[0] >= rectangle[0] && point[0] <= rectangle[2] &&
          point[1] >= rectangle[1] && point[1] <= rectangle[3]) {
        auto& destination = state["pending"][CAMERAS[index + 1]];
        bool exists = false;
        for (const auto& item : destination) {
          if (item.at("sourceLocalId") == local) exists = true;
        }
        if (!exists) {
          const auto threshold = index == 0 ? 1355.0 / 2160 : 1455.0 / 2160;
          destination.push_back({{"sourceCamera", camera}, {"sourceLocalId", local},
                                 {"globalId", global}, {"label", detection.label}, {"point", point},
                                 {"ptsUs", frame.commonPtsUs}, {"lane", point[1] < threshold ? "upper" : "lower"}});
        }
      }
    }
    for (const auto* destination : CAMERAS) {
      auto& entries = state["pending"][destination];
      entries.erase(std::remove_if(entries.begin(), entries.end(), [&] (const Json& item) {
        return frame.commonPtsUs - item.at("ptsUs").get<int64_t>() > policy.associationMaxGapUs;
      }), entries.end());
    }
    return {global, event};
  }

  std::pair<Json, Json> assignTracks(size_t index, const VehicleBackendFrame& backend,
                                    const VehicleFrameContext& frame)
  {
    const auto camera = CAMERAS[index];
    const auto previous = state.at("last").at(camera);
    auto next = positiveId(state.at("nextLocal").at(camera));
    std::set<uint64_t> previousIds;
    for (const auto& item : previous) previousIds.insert(positiveId(item.at("localId")));
    for (const auto& item : state.at("mapping")) {
      if (item.at("cameraId") == camera) next = std::max(next, positiveId(item.at("localId")) + 1);
    }
    std::set<uint64_t> used;
    Json tracks = Json::array();
    Json events = Json::array();
    for (const auto& detection : backend.detections) {
      const std::array<double, 2> center{detection.box[0] / 2.0 + detection.box[2] / 2.0,
                                        detection.box[1] / 2.0 + detection.box[3] / 2.0};
      std::optional<uint64_t> match;
      double bestDistance = std::numeric_limits<double>::infinity();
      for (const auto& item : previous) {
        const auto local = positiveId(item.at("localId"));
        if (used.count(local) != 0 || item.at("class") != detection.label) continue;
        const auto box = item.at("box").get<std::array<int, 4>>();
        const auto dx = center[0] - (box[0] / 2.0 + box[2] / 2.0);
        const auto dy = center[1] - (box[1] / 2.0 + box[3] / 2.0);
        const auto distance = dx * dx + dy * dy;
        if (distance < bestDistance && distance <= 100 * 100) {
          bestDistance = distance;
          match = local;
        }
      }
      uint64_t local;
      if (match) local = *match;
      else if (previous.empty() && detection.byteTrackId && used.count(*detection.byteTrackId) == 0) {
        local = *detection.byteTrackId;
        next = std::max(next, local + 1);
      }
      else {
        while (used.count(next) != 0 || previousIds.count(next) != 0) {
          if (next == std::numeric_limits<uint64_t>::max() - 1) rejectVehicle("local ID exhausted");
          ++next;
        }
        if (next >= std::numeric_limits<uint64_t>::max() - 1) rejectVehicle("local ID exhausted");
        local = next++;
      }
      used.insert(local);
      auto association = associate(index, local, detection, frame, backend.width, backend.height);
      if (!association.second.is_null()) events.push_back(std::move(association.second));
      tracks.push_back({{"localId", local}, {"globalId", association.first},
                         {"class", detection.label},
                         {"confidence", std::nearbyint(detection.confidence * 1e6) / 1e6},
                         {"box", detection.box}, {"ptsUs", frame.commonPtsUs}, {"trackGeneration", local}});
    }
    state["nextLocal"][camera] = next;
    state["last"][camera] = tracks;
    return {tracks, events};
  }
};

UavVehicleTrackingEngine::UavVehicleTrackingEngine(const VehicleTrackingPolicy& policy)
  : m_impl(std::make_unique<Impl>(policy))
{
}

UavVehicleTrackingEngine::~UavVehicleTrackingEngine() = default;

void
UavVehicleTrackingEngine::restoreState(const std::string& committedStateJson,
                                      const VehicleWindowContext& nextContext)
{
  checkContext(nextContext);
  const auto identity = contextIdentity(nextContext);
  Json candidate;
  std::string resetReason;
  std::map<MotionKey, std::vector<MotionSample>> restored;
  if (committedStateJson.empty()) {
    if (nextContext.sequence != 1) rejectVehicle("missing committed predecessor state");
    candidate = m_impl->emptyState(nextContext);
  }
  else {
    candidate = parseBounded(committedStateJson, MAXIMUM_BYTES);
    restored = m_impl->validateState(candidate);
    if (text(candidate.at("runId")) != nextContext.runId ||
        text(candidate.at("missionId")) != nextContext.missionId ||
        text(candidate.at("nativeDecisionDigest")) != nextContext.nativeDecisionDigest ||
        candidate.at("observationMode") != nextContext.observationMode ||
        unsignedNumber(candidate.at("sequence")) + 1 != nextContext.sequence) {
      rejectVehicle("committed state execution/computation/order binding");
    }
    if (candidate.at("sources") != identity.at("sources")) {
      for (const auto* camera : CAMERAS) {
        const auto& previous = candidate.at("sources").at(camera);
        const auto& incoming = identity.at("sources").at(camera);
        for (const auto* field : {"sourceId", "sourceEpoch", "worldFrameId", "acquisitionKind",
                                   "deliveryMode", "clockBindingDigest", "calibrationDigest", "groundDigest"}) {
          if (incoming.contains(field) && previous.at(field) != incoming.at(field) && resetReason.empty()) {
            resetReason = std::string("STATE_BINDING_MISMATCH:sources.") + camera + "." + field;
          }
        }
      }
      candidate = m_impl->emptyState(nextContext);
      restored.clear();
    }
  }
  m_impl->state = std::move(candidate);
  m_impl->histories = std::move(restored);
  m_impl->resetReason = std::move(resetReason);
  m_impl->prepared = identity;
}

std::string
UavVehicleTrackingEngine::backendStateJson() const
{
  if (m_impl->prepared.is_null()) rejectVehicle("backend export before native restore");
  return m_impl->state.at("backendState").dump();
}

VehicleWindowOutput
UavVehicleTrackingEngine::processWindow(const VehicleWindowContext& context,
                                       const VehicleBackendWindow& backend)
{
  checkContext(context);
  if (m_impl->prepared.is_null() || m_impl->prepared != contextIdentity(context)) {
    rejectVehicle("window differs from restored context");
  }
  if (backend.m_contextIdentity != m_impl->prepared.dump()) {
    rejectVehicle("candidate was admitted for a different native context");
  }
  const auto& backendState = backend.m_backendState;
  const auto previousTracks = m_impl->state.at("last");
  Json telemetry = Json::array();
  Json calibrations = Json::object();
  bool compensate = true;
  if (!context.legacyMotion.is_null()) {
    if (context.observationMode || !context.legacyMotion.is_object() ||
        context.legacyMotion.value("schema", std::string()) != "spec191-motion-window-v1") {
      rejectVehicle("legacy motion mode/schema");
    }
    const auto& rawTelemetry = context.legacyMotion.at("telemetry");
    const auto& rawCalibrations = context.legacyMotion.at("calibrations");
    if (!rawTelemetry.is_array() || rawTelemetry.size() != 3 ||
        !rawCalibrations.is_array() || rawCalibrations.size() != 3) {
      rejectVehicle("legacy motion camera count");
    }
    for (size_t i = 0; i < CAMERAS.size(); ++i) {
      const auto sample = legacyTelemetry(rawTelemetry.at(i));
      if (sample.at("cameraId") != CAMERAS[i] || sample.at("sequence") != context.sequence ||
          sample.at("sourceTimestampUs") != context.frames[i].commonPtsUs ||
          sample.at("sourceTimestampUs") != context.frames[0].commonPtsUs) {
        rejectVehicle("motion sample is not aligned with frame metadata");
      }
      telemetry.push_back(sample);
    }
    for (const auto& raw : rawCalibrations) {
      const auto calibration = legacyCalibration(raw);
      const auto camera = text(calibration.at("cameraId"));
      Impl::cameraIndex(camera);
      // Legacy homographies retain their declared pixel coordinates without implicit rescaling.
      if (calibrations.contains(camera)) rejectVehicle("legacy calibration duplicate");
      calibrations[camera] = calibration;
    }
    const auto policy = context.legacyMotion.value("motionPolicy", Json::object());
    if (!policy.is_object()) rejectVehicle("motion policy must be an object");
    compensate = policy.value("egoMotionCompensation", std::string("required")) != "disabled";
  }
  Json outputs = Json::array();
  Json allMeasurements = Json::array();
  Json lastAssociations = Json::array();
  Json legacyEstimates = Json::array();
  std::set<MotionKey> active;
  bool simulated = false;
  bool analytic = true;
  for (size_t i = 0; i < CAMERAS.size(); ++i) {
    const auto& frame = context.frames[i];
    auto decision = m_impl->assignTracks(i, backend.m_frames[i], frame);
    auto& tracks = decision.first;
    lastAssociations = decision.second;
    Json output{{"cameraId", frame.cameraId}, {"sequence", frame.sequence}, {"ptsUs", frame.commonPtsUs},
                {"associations", decision.second}, {"inputDigest", frame.inputDigest},
                {"algorithmProfile", "yolo-bytetrack-cpu-v1"},
                {"imageWidth", backend.m_frames[i].width}, {"imageHeight", backend.m_frames[i].height},
                {"imageRef", {{"name", frame.imageName}, {"digest", frame.inputDigest}}}};
    if (context.observationMode) {
      const auto& raw = frame.observation;
      const auto& identity = raw.at("identity");
      simulated = simulated || identity.at("acquisitionKind") == "simulated";
      analytic = analytic && raw.at("geometryValidity") == "analytic-fixture";
      for (const auto* key : {"acquisitionKind", "deliveryMode"}) output[key] = identity.at(key);
      output["sourceId"] = frame.sourceId;
      output["sourceEpoch"] = frame.sourceEpoch;
      output["captureTimeUs"] = frame.commonPtsUs;
      output["geometryValidity"] = raw.at("geometryValidity");
      output["observationName"] = frame.observationName;
      output["observationDigest"] = frame.observationDigest;
      output["measurements"] = Json::array();
      for (auto& track : tracks) {
        const auto local = track.at("localId").get<uint64_t>();
        const MotionKey key{frame.sourceId, frame.cameraId, frame.sourceEpoch, local, local};
        Json measurement;
        try {
          const auto box = track.at("box").get<std::array<int, 4>>();
          const auto ground = projectGround({box[0] / 2.0 + box[2] / 2.0, static_cast<double>(box[3])},
                                            frame.observation);
          measurement = updateMotion(m_impl->histories[key], m_impl->policy, frame, track, ground);
          active.insert(key);
        }
        catch (const std::exception& error) {
          m_impl->histories.erase(key);
          auto reason = std::string(error.what());
          const std::string prefix = "vehicle-backend-invalid: ";
          if (reason.compare(0, prefix.size(), prefix) == 0) reason.erase(0, prefix.size());
          measurement = unknownMeasurement(frame, track, reason);
        }
        track["measurement"] = measurement;
        output["measurements"].push_back(measurement);
        allMeasurements.push_back(std::move(measurement));
      }
      output["observation"] = {
        {"schema", "uav-observation-v2"}, {"identity", identity},
        {"clockBinding", raw.at("clockBinding")}, {"timeUncertaintyUs", raw.at("timeUncertaintyUs")},
        {"sourceId", frame.sourceId}, {"sourceEpoch", frame.sourceEpoch}, {"sequence", frame.sequence},
        {"captureTimeUs", frame.commonPtsUs}, {"acquisitionKind", identity.at("acquisitionKind")},
        {"deliveryMode", identity.at("deliveryMode")}, {"geometryValidity", raw.at("geometryValidity")},
        {"anchorApproximation", true},
        {"observationRef", {{"name", frame.observationName}, {"digest", frame.observationDigest}}}};
    }
    else if (!telemetry.empty()) {
      const auto& calibration = calibrations.at(frame.cameraId);
      const auto& sample = telemetry.at(i);
      output["telemetry"] = sample;
      output["calibrationDigest"] = calibration.at("calibrationDigest");
      const auto& previous = previousTracks.at(frame.cameraId);
      if (m_impl->state.at("previousLegacy").contains(frame.cameraId)) {
        const auto& previousSample = m_impl->state.at("previousLegacy").at(frame.cameraId);
        for (const auto& track : tracks) {
          for (const auto& old : previous) {
            if (old.at("globalId") == track.at("globalId")) {
              auto estimate = legacyEstimate(old, track, previousSample, sample, calibration, compensate);
              estimate["cameraId"] = frame.cameraId;
              legacyEstimates.push_back(std::move(estimate));
              break;
            }
          }
        }
      }
      m_impl->state["previousLegacy"][frame.cameraId] = sample;
    }
    output["tracks"] = std::move(tracks);
    outputs.push_back(std::move(output));
  }
  for (auto item = m_impl->histories.begin(); item != m_impl->histories.end();) {
    if (active.count(item->first) == 0) item = m_impl->histories.erase(item);
    else ++item;
  }
  Json result{{"schema", context.observationMode ? "spec210-tracking-result-v2" : "spec191-tracking-result-v1"},
              {"runId", context.runId}, {"missionId", context.missionId}, {"windowId", context.windowId},
              {"sequence", context.sequence}, {"algorithm", "yolo-bytetrack-cpu-v1"},
              {"decisionSchema", "uav-vehicle-native-decision-v1"},
              {"actualDevice", "cpu"},
              {"nativeDecisionDigest", context.nativeDecisionDigest},
              {"frames", outputs}, {"associations", lastAssociations}, {"terminal", true}};
  if (context.observationMode) {
    result["motion"] = {{"schema", "uav-motion-result-v2"}, {"sourceEpoch", context.frames[0].sourceEpoch},
                         {"measurements", allMeasurements}, {"inputProvenance", simulated ? "simulated" : "measured"},
                         {"accuracyQualification", analytic ? "analytic-only" : "unvalidated"}};
  }
  else if (!telemetry.empty()) {
    Json digests = Json::object();
    for (const auto* camera : CAMERAS) digests[camera] = calibrations.at(camera).at("calibrationDigest");
    auto oracle = context.legacyMotion.value("oracle", Json::object());
    if (!oracle.is_object()) rejectVehicle("legacy oracle type");
    oracle["provenance"] = "simulated-input";
    result["motion"] = {{"schema", "spec191-motion-result-v1"}, {"inputProvenance", "simulated-input"},
                         {"telemetry", telemetry}, {"calibrationDigests", digests}, {"estimates", legacyEstimates},
                         {"oracle", oracle}, {"motionProfile", context.legacyMotion.value("motionProfile", std::string("nominal"))},
                         {"egoMotionCompensation", compensate ? "required" : "disabled"}};
  }
  if (!m_impl->resetReason.empty()) result["stateResetReason"] = m_impl->resetReason;
  m_impl->state["sequence"] = context.sequence;
  m_impl->state["backendState"] = backendState;
  Json motion = Json::array();
  for (const auto& entry : m_impl->histories) {
    Json samples = Json::array();
    for (const auto& sample : entry.second) {
      samples.push_back({{"timeUs", sample.first}, {"ground", groundState(sample.second)}});
    }
    const auto& key = entry.first;
    motion.push_back({{"key", Json::array({std::get<0>(key), std::get<1>(key), std::get<2>(key),
                                           std::get<3>(key), std::get<4>(key)})}, {"samples", samples}});
  }
  m_impl->state["motion"] = std::move(motion);
  VehicleWindowOutput output{result.dump(), m_impl->state.dump()};
  if (output.resultJson.size() > MAXIMUM_BYTES || output.candidateStateJson.size() > MAXIMUM_BYTES) {
    rejectVehicle("native result/state byte bound");
  }
  m_impl->prepared = nullptr;
  m_impl->resetReason.clear();
  return output;
}

std::string
VehicleTrackingPolicy::canonicalJson() const
{
  if (associationMaxGapUs <= 0 || !std::isfinite(associationMaxCenterDistance) ||
      associationMaxCenterDistance <= 0 || associationMaxCenterDistance > 1 ||
      minimumPoints < 3 || maximumPoints < minimumPoints ||
      maximumPoints > 32 || maximumGapUs <= 0 || historyWindowUs <= 0 || minimumTimeSpanUs <= 0 ||
      !std::isfinite(maximumSpeedMps) || maximumSpeedMps <= 0 ||
      minimumTimeSpanUs > historyWindowUs ||
      !std::isfinite(stationaryThresholdMps) || stationaryThresholdMps <= 0 ||
      !std::isfinite(maximumBearingSigmaDegrees) || maximumBearingSigmaDegrees <= 0) {
    rejectVehicle("native policy");
  }
  return Json{{"associationMaxGapUs", associationMaxGapUs},
              {"associationMaxCenterDistance", associationMaxCenterDistance},
              {"minimumPoints", minimumPoints}, {"maximumPoints", maximumPoints},
              {"maximumGapUs", maximumGapUs}, {"historyWindowUs", historyWindowUs},
              {"minimumTimeSpanUs", minimumTimeSpanUs}, {"maximumSpeedMps", maximumSpeedMps},
              {"stationaryThresholdMps", stationaryThresholdMps},
              {"maximumBearingSigmaDegrees", maximumBearingSigmaDegrees}}.dump();
}

VehicleBackendWindow
parseUavVehicleBackendWindow(const std::string& wire, const VehicleWindowContext& expected)
{
  checkContext(expected);
  const auto root = parseBounded(wire, MAXIMUM_BYTES);
  exactKeys(root, {"schema", "runId", "missionId", "windowId", "sequence", "actualDevice",
                   "frames", "backendState"});
  if (text(root.at("schema")) != "uav-vehicle-backend-window-v1" ||
      text(root.at("runId")) != expected.runId ||
      text(root.at("missionId")) != expected.missionId ||
      text(root.at("windowId")) != expected.windowId ||
      unsignedNumber(root.at("sequence")) != expected.sequence ||
      text(root.at("actualDevice")) != "cpu") {
    rejectVehicle("window identity/device");
  }
  const auto& frames = root.at("frames");
  const auto& state = root.at("backendState");
  exactKeys(state, {"schema", "cameras"});
  if (text(state.at("schema")) != "uav-vehicle-backend-state-v1" ||
      !frames.is_array() || frames.size() != CAMERAS.size()) {
    rejectVehicle("backend schema/camera count");
  }
  const auto& cameras = state.at("cameras");
  exactKeys(cameras, {"UAV1", "UAV2", "UAV3"});
  VehicleBackendWindow result;
  for (size_t i = 0; i < CAMERAS.size(); ++i) {
    const auto& input = frames.at(i);
    exactKeys(input, {"cameraId", "width", "height", "detections"});
    auto& output = result.m_frames[i];
    output.cameraId = text(input.at("cameraId"));
    output.width = positiveDimension(input.at("width"));
    output.height = positiveDimension(input.at("height"));
    if (output.cameraId != CAMERAS[i] ||
        (expected.frames[i].width > 0 && output.width != expected.frames[i].width) ||
        (expected.frames[i].height > 0 && output.height != expected.frames[i].height)) {
      rejectVehicle("decoded frame binding");
    }
    const auto& snapshot = cameras.at(CAMERAS[i]);
    byte_track::TrackState admitted;
    if (!snapshot.is_null()) {
      // The byte bound limits full snapshots; preserve all stock removal history.
      if (!snapshot.at("removed").is_array() || snapshot.at("removed").size() > 1000) {
        rejectVehicle("removed history bound");
      }
      admitted = byte_track::checkTracker(snapshot, std::numeric_limits<uint64_t>::max(),
                                         byte_track::BackendStateFormat::LEGACY_CONFIRMED);
    }
    const auto& detections = input.at("detections");
    if (!detections.is_array() || detections.size() > MAXIMUM_DETECTIONS) {
      rejectVehicle("detection bound");
    }
    std::set<uint64_t> usedIds;
    for (const auto& candidate : detections) {
      exactKeys(candidate, {"box", "confidence", "class", "byteTrackId"});
      VehicleDetection detection;
      detection.label = text(candidate.at("class"));
      checkLabel(detection.label);
      detection.confidence = finiteNumber(candidate.at("confidence"));
      if (detection.confidence < 0 || detection.confidence > 1) {
        rejectVehicle("confidence range");
      }
      const auto& box = candidate.at("box");
      if (!box.is_array() || box.size() != detection.box.size()) rejectVehicle("box dimensions");
      for (size_t coordinate = 0; coordinate < detection.box.size(); ++coordinate) {
        detection.box[coordinate] = static_cast<int>(unsignedNumber(box.at(coordinate),
          static_cast<uint64_t>(coordinate % 2 == 0 ? output.width : output.height)));
      }
      if (detection.box[0] >= detection.box[2] || detection.box[1] >= detection.box[3]) {
        rejectVehicle("empty/reversed box");
      }
      if (!candidate.at("byteTrackId").is_null()) {
        const auto id = unsignedNumber(candidate.at("byteTrackId"),
                                       std::numeric_limits<uint64_t>::max() - 2);
        if (id == 0 || admitted.currentIds.count(id) == 0 || !usedIds.insert(id).second) {
          rejectVehicle("candidate ID absent/duplicate in current tracker snapshot");
        }
        detection.byteTrackId = id;
      }
      output.detections.push_back(std::move(detection));
    }
  }
  result.m_backendState = state;
  result.m_contextIdentity = contextIdentity(expected).dump();
  return result;
}


} // namespace ndnsf::examples::uav

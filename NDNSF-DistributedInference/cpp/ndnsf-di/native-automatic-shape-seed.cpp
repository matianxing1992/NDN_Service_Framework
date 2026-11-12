#include "native-automatic-shape-seed.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ndnsf::di {
namespace {

[[noreturn]] void
rejectSeed(const char* reason)
{
  throw std::invalid_argument(reason);
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    value.find_first_not_of("0123456789abcdef", 7) == std::string::npos;
}

std::uint64_t
unsignedNumber(const NativeJson& value)
{
  if (!value.is_number_integer() ||
      (!value.is_number_unsigned() && value.get<std::int64_t>() < 0))
    rejectSeed("AUTOMATIC_SHAPE_SEED_INTEGER");
  return value.get<std::uint64_t>();
}

void
checkLimit(const NativeJson& value, std::uint64_t byteLimit)
{
  const auto limit = std::min(byteLimit, NATIVE_AUTOMATIC_SHAPE_SEED_MAX_BYTES);
  if (limit == 0 || nativeCanonicalJson(value).size() > limit)
    rejectSeed("AUTOMATIC_SHAPE_SEED_LIMIT");
}

void
validateSeed(const NativeAutomaticShapeSeed& seed)
{
  if (!isDigest(seed.contractDigest) || !isDigest(seed.bindingDigest) ||
      !isDigest(seed.budgetDigest) || !isDigest(seed.envelopeDigest) || seed.maxContext == 0)
    rejectSeed("AUTOMATIC_SHAPE_SEED_IDENTITY");
  std::vector<TensorBundleEncodingMember> framing;
  framing.reserve(seed.inputShapes.size());
  for (const auto& [name, shape] : seed.inputShapes) {
    if (name.size() <= 6 || name.size() > 4096 || name.compare(0, 6, "graph/") != 0 ||
        name.find('\0') != std::string::npos)
      rejectSeed("AUTOMATIC_SHAPE_SEED_NAME");
    // The minimum scalar width checks existing geometry bounds only. Actual
    // dtype byte bounds remain the source adapter's responsibility.
    (void)tensorStorageByteSize(TensorElementType::UInt8, shape);
    // Zero lengths check metadata framing limits only, never payload estimates.
    framing.push_back({name.size() - 6, shape.size(), 0});
  }
  (void)tensorBundleEncodedSize(framing);
}

} // namespace

NativeJson
nativeAutomaticShapeSeedToJson(const NativeAutomaticShapeSeed& seed, std::uint64_t byteLimit)
{
  validateSeed(seed);
  NativeJson value{{"schema", "ndnsf-di-automatic-shape-seed-v1"},
    {"contractDigest", seed.contractDigest}, {"bindingDigest", seed.bindingDigest},
    {"budgetDigest", seed.budgetDigest}, {"envelopeDigest", seed.envelopeDigest},
    {"maxContext", seed.maxContext}, {"maxNewTokens", seed.maxNewTokens},
    {"inputShapes", seed.inputShapes}};
  checkLimit(value, byteLimit);
  return value;
}

NativeAutomaticShapeSeed
nativeAutomaticShapeSeedFromJson(const NativeJson& value, std::uint64_t byteLimit)
{
  if (!value.is_object() || value.size() != 8)
    rejectSeed("AUTOMATIC_SHAPE_SEED_FIELDS");
  for (const auto* key : {"schema", "contractDigest", "bindingDigest", "budgetDigest",
                           "envelopeDigest", "maxContext", "maxNewTokens", "inputShapes"})
    if (!value.contains(key)) rejectSeed("AUTOMATIC_SHAPE_SEED_FIELDS");
  if (value.at("schema") != NativeJson("ndnsf-di-automatic-shape-seed-v1") ||
      !value.at("inputShapes").is_object()) rejectSeed("AUTOMATIC_SHAPE_SEED_SCHEMA");
  checkLimit(value, byteLimit);
  NativeAutomaticShapeSeed seed;
  const auto text = [&](const char* key) {
    if (!value.at(key).is_string()) rejectSeed("AUTOMATIC_SHAPE_SEED_STRING");
    return value.at(key).get<std::string>();
  };
  seed.contractDigest = text("contractDigest"); seed.bindingDigest = text("bindingDigest");
  seed.budgetDigest = text("budgetDigest"); seed.envelopeDigest = text("envelopeDigest");
  seed.maxContext = unsignedNumber(value.at("maxContext"));
  seed.maxNewTokens = unsignedNumber(value.at("maxNewTokens"));
  for (const auto& item : value.at("inputShapes").items()) {
    if (!item.value().is_array()) rejectSeed("AUTOMATIC_SHAPE_SEED_DIMENSION");
    auto& shape = seed.inputShapes[item.key()];
    for (const auto& axis : item.value()) {
      const auto extent = unsignedNumber(axis);
      if (extent > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        rejectSeed("AUTOMATIC_SHAPE_SEED_DIMENSION");
      shape.push_back(static_cast<std::int64_t>(extent));
    }
  }
  validateSeed(seed);
  return seed;
}

} // namespace ndnsf::di

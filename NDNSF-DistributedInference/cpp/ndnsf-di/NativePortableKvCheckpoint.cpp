#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/NativePrivateCheckpointFile.hpp"

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <set>
#include <utility>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

constexpr char Magic[] = "NDKV0001";
constexpr std::size_t ChunkBytes = 64 * 1024;

[[noreturn]] void invalid()
{
  throw std::invalid_argument("native portable checkpoint invalid");
}

void require(bool condition) { if (!condition) invalid(); }

void cancelled(const NativePortableKvCheckpointLimitsV1& limits)
{
  if (limits.portable.cancelled && limits.portable.cancelled())
    throw std::runtime_error("native portable checkpoint cancelled");
}

void checkLimits(const NativePortableKvCheckpointLimitsV1& limits)
{
  validateNativePortableKvCheckpointLimits(limits);
  cancelled(limits);
}

void add(std::uint64_t& total, std::uint64_t value, std::uint64_t limit)
{
  require(total <= limit && value <= limit - total);
  total += value;
}

struct StringField
{
  const char* name;
  std::string DecodeStateIdentityV1::*member;
  std::size_t maxBytes;
  bool digest;
};

const std::array<StringField, 19> OriginStrings{{
  {"modelDigest", &DecodeStateIdentityV1::modelDigest, 71, true},
  {"graphSemanticDigest", &DecodeStateIdentityV1::graphSemanticDigest, 71, true},
  {"artifactDigest", &DecodeStateIdentityV1::artifactDigest, 71, true},
  {"adapterDigest", &DecodeStateIdentityV1::adapterDigest, 71, true},
  {"tokenizerDigest", &DecodeStateIdentityV1::tokenizerDigest, 71, true},
  {"runnerDigest", &DecodeStateIdentityV1::runnerDigest, 71, true},
  {"roleName", &DecodeStateIdentityV1::roleName, 1024, false},
  {"roleSplitDigest", &DecodeStateIdentityV1::roleSplitDigest, 71, true},
  {"prefixDigest", &DecodeStateIdentityV1::prefixDigest, 71, true},
  {"positionDigest", &DecodeStateIdentityV1::positionDigest, 71, true},
  {"precision", &DecodeStateIdentityV1::precision, 128, false},
  {"layoutDigest", &DecodeStateIdentityV1::layoutDigest, 71, true},
  {"stateSchemaDigest", &DecodeStateIdentityV1::stateSchemaDigest, 71, true},
  {"runtimeAbiDigest", &DecodeStateIdentityV1::runtimeAbiDigest, 71, true},
  {"securityDomainDigest", &DecodeStateIdentityV1::securityDomainDigest, 71, true},
  {"providerIdentity", &DecodeStateIdentityV1::providerIdentity, 4096, false},
  {"providerBootId", &DecodeStateIdentityV1::providerBootId, 4096, false},
  {"requestId", &DecodeStateIdentityV1::requestId, 4096, false},
  {"generationId", &DecodeStateIdentityV1::generationId, 4096, false},
}};

std::uint64_t unsignedValue(const NativeJson& value,
                            std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
{
  require(value.is_number_integer());
  if (!value.is_number_unsigned()) require(value.get<std::int64_t>() >= 0);
  const auto result = value.get<std::uint64_t>();
  require(result <= maximum);
  return result;
}

const std::string& text(const NativeJson& value, std::size_t maximum)
{
  require(value.is_string());
  const auto& result = value.get_ref<const NativeJson::string_t&>();
  require(!result.empty() && result.size() <= maximum);
  return result;
}

void keys(const NativeJson& value, std::initializer_list<const char*> expected)
{
  require(value.is_object() && value.size() == expected.size());
  for (const auto* key : expected) require(value.contains(key));
}

void checkText(const NativePortableKvSnapshotV1& snapshot,
               const NativePortableKvCheckpointLimitsV1& limits)
{
  require(!snapshot.originIdentity.stateComponentDigests.empty() &&
          snapshot.originIdentity.stateComponentDigests.size() <= 256 &&
          !snapshot.components.empty() && snapshot.components.size() <= 256);
  std::uint64_t bytes = 0;
  for (const auto& field : OriginStrings) {
    const auto& value = snapshot.originIdentity.*(field.member);
    require(!value.empty() && value.size() <= field.maxBytes);
    if (field.digest) decode_state_identity_detail::requireDigest(value, "checkpoint");
    add(bytes, value.size(), limits.maxMetadataBytes);
  }
  for (const auto& digest : snapshot.originIdentity.stateComponentDigests) {
    decode_state_identity_detail::requireDigest(digest, "checkpoint");
    add(bytes, digest.size(), limits.maxMetadataBytes);
  }
  decode_state_identity_detail::requireDigest(snapshot.stateIoDigest, "checkpoint");
  add(bytes, snapshot.stateIoDigest.size(), limits.maxMetadataBytes);
  for (const auto& component : snapshot.components) {
    require(!component.inputName.empty() && component.inputName.size() <= 1024 &&
            component.shape.size() <= 16);
    add(bytes, component.inputName.size(), limits.maxMetadataBytes);
    decode_state_identity_detail::requireDigest(component.contentDigest, "checkpoint");
    add(bytes, component.contentDigest.size(), limits.maxMetadataBytes);
  }
}

NativeJson metadata(const NativePortableKvSnapshotV1& snapshot)
{
  const auto& origin = snapshot.originIdentity;
  NativeJson identity = NativeJson::object();
  for (const auto& field : OriginStrings) identity[field.name] = origin.*(field.member);
  identity["layerBegin"] = origin.layerBegin;
  identity["layerEnd"] = origin.layerEnd;
  identity["prefixTokenCount"] = origin.prefixTokenCount;
  identity["stateInferenceEpoch"] = origin.stateInferenceEpoch;
  identity["cacheEpoch"] = origin.cacheEpoch;
  identity["attemptEpoch"] = origin.attemptEpoch;
  identity["predecessorInferenceEpoch"] = origin.predecessorInferenceEpoch ?
    NativeJson(*origin.predecessorInferenceEpoch) : NativeJson(nullptr);
  identity["stateComponentDigests"] = origin.stateComponentDigests;
  auto components = NativeJson::array();
  for (const auto& component : snapshot.components) {
    components.push_back({{"inputName", component.inputName},
      {"elementType", static_cast<std::uint32_t>(component.elementType)},
      {"shape", component.shape}, {"payloadBytes", component.payloadBytes},
      {"contentDigest", component.contentDigest}, {"wireBytes", component.tensorWire.size()}});
  }
  return {{"version", snapshot.version}, {"originIdentity", std::move(identity)},
    {"stateIoDigest", snapshot.stateIoDigest}, {"components", std::move(components)},
    {"logicalBytes", snapshot.logicalBytes}, {"encodedBytes", snapshot.encodedBytes}};
}

void checkDepth(const std::string& wire)
{
  unsigned depth = 0;
  bool string = false;
  for (std::size_t i = 0; i < wire.size(); ++i) {
    const char ch = wire[i];
    if (string && ch == '\\') { require(++i < wire.size()); continue; }
    if (ch == '"') { string = !string; continue; }
    if (string) continue;
    if (ch == '{' || ch == '[') require(++depth <= 8);
    else if (ch == '}' || ch == ']') { require(depth > 0); --depth; }
  }
  require(depth == 0 && !string);
}

DecodeStateIdentityV1 readIdentity(const NativeJson& value)
{
  require(value.is_object() && value.size() == OriginStrings.size() + 8);
  DecodeStateIdentityV1 result;
  for (const auto& field : OriginStrings) {
    require(value.contains(field.name));
    result.*(field.member) = text(value.at(field.name), field.maxBytes);
  }
  const auto u32 = std::numeric_limits<std::uint32_t>::max();
  result.layerBegin = unsignedValue(value.at("layerBegin"), u32);
  result.layerEnd = unsignedValue(value.at("layerEnd"), u32);
  result.prefixTokenCount = unsignedValue(value.at("prefixTokenCount"), u32);
  result.stateInferenceEpoch = unsignedValue(value.at("stateInferenceEpoch"));
  result.cacheEpoch = unsignedValue(value.at("cacheEpoch"));
  result.attemptEpoch = unsignedValue(value.at("attemptEpoch"));
  const auto& predecessor = value.at("predecessorInferenceEpoch");
  if (!predecessor.is_null()) result.predecessorInferenceEpoch = unsignedValue(predecessor);
  const auto& digests = value.at("stateComponentDigests");
  require(digests.is_array() && !digests.empty() && digests.size() <= 256);
  for (const auto& digest : digests) result.stateComponentDigests.push_back(text(digest, 71));
  result.validate();
  return result;
}

std::uint64_t width(TensorElementType type)
{
  switch (type) {
    case TensorElementType::Float32: return 4;
    case TensorElementType::Float16: return 2;
    case TensorElementType::Int64: return 8;
    case TensorElementType::Bool:
    case TensorElementType::UInt8: return 1;
  }
  invalid();
}

void transfer(int fd, std::uint8_t* bytes, std::size_t size, bool writing,
              const NativePortableKvCheckpointLimitsV1& limits)
{
  std::size_t offset = 0;
  while (offset < size) {
    cancelled(limits);
    const auto amount = std::min(ChunkBytes, size - offset);
    const auto count = writing ? ::write(fd, bytes + offset, amount) : ::read(fd, bytes + offset, amount);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) throw std::runtime_error("native portable checkpoint I/O failed");
    offset += static_cast<std::size_t>(count);
  }
}

void writeBytes(int fd, const void* bytes, std::size_t size,
                const NativePortableKvCheckpointLimitsV1& limits)
{
  // write(2) borrows a const buffer; transfer never modifies it in writing mode.
  transfer(fd, const_cast<std::uint8_t*>(static_cast<const std::uint8_t*>(bytes)), size, true, limits);
}

class ReadFd
{
public:
  explicit ReadFd(const std::filesystem::path& path)
    : fd(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW))
  {
    if (fd < 0) throw std::runtime_error("native portable checkpoint open failed");
  }
  ~ReadFd() { ::close(fd); }
  ReadFd(const ReadFd&) = delete;
  ReadFd& operator=(const ReadFd&) = delete;
  int fd;
};

} // namespace

void validateNativePortableKvCheckpointLimits(
  const NativePortableKvCheckpointLimitsV1& limits)
{
  require(limits.maxMetadataBytes > 0 && limits.maxMetadataBytes <= 1024 * 1024 &&
          limits.maxCheckpointBytes >= 16 && limits.portable.maxPayloadBytes > 0 &&
          limits.portable.maxEncodedBytes >= limits.portable.maxPayloadBytes);
}

void nativeExportPrivatePortableKvCheckpoint(
  const std::filesystem::path& destination, const NativePortableKvSnapshotV1& snapshot,
  const NativePortableKvCheckpointLimitsV1& limits, const NativeCheckpointExportOptions& options)
{
  checkLimits(limits);
  checkText(snapshot, limits);
  validateNativePortableKvSnapshot(snapshot, limits.portable);
  std::string wire;
  try { wire = nativeCanonicalJson(metadata(snapshot)); }
  catch (const NativeJson::exception&) { invalid(); }
  require(wire.size() <= limits.maxMetadataBytes);
  std::uint64_t total = 16;
  add(total, wire.size(), limits.maxCheckpointBytes);
  add(total, snapshot.encodedBytes, limits.maxCheckpointBytes);
  std::array<std::uint8_t, 16> header{};
  std::memcpy(header.data(), Magic, 8);
  auto length = static_cast<std::uint64_t>(wire.size());
  for (int i = 15; i >= 8; --i) { header[i] = length & 0xff; length >>= 8; }
  cancelled(limits);
  detail::writePrivateCheckpointFile(destination, options, [&] (int fd) {
    writeBytes(fd, header.data(), header.size(), limits);
    writeBytes(fd, wire.data(), wire.size(), limits);
    for (const auto& component : snapshot.components)
      writeBytes(fd, component.tensorWire.data(), component.tensorWire.size(), limits);
  });
}

NativePortableKvSnapshotV1 nativeReadPrivatePortableKvCheckpoint(
  const std::filesystem::path& source, const NativePortableKvCheckpointLimitsV1& limits)
{
  checkLimits(limits);
  ReadFd file(source);
  struct stat info{};
  require(::fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid() &&
          info.st_nlink == 1 && (info.st_mode & 0077) == 0 && (info.st_mode & S_IRUSR) != 0 &&
          info.st_size >= 16 && static_cast<std::uint64_t>(info.st_size) <= limits.maxCheckpointBytes);
  std::array<std::uint8_t, 16> header{};
  transfer(file.fd, header.data(), header.size(), false, limits);
  require(std::memcmp(header.data(), Magic, 8) == 0);
  std::uint64_t length = 0;
  for (unsigned i = 8; i < 16; ++i) length = (length << 8) | header[i];
  require(length > 0 && length <= limits.maxMetadataBytes &&
          length <= static_cast<std::uint64_t>(info.st_size) - 16);
  std::string wire(static_cast<std::size_t>(length), '\0');
  transfer(file.fd, reinterpret_cast<std::uint8_t*>(wire.data()), wire.size(), false, limits);
  NativePortableKvSnapshotV1 result;
  std::vector<std::size_t> sizes;
  try {
    checkDepth(wire);
    NativeJson json;
    try { json = nativeParseJson(wire); }
    catch (const std::invalid_argument&) { invalid(); }
    keys(json, {"version", "originIdentity", "stateIoDigest", "components", "logicalBytes", "encodedBytes"});
    require(unsignedValue(json.at("version")) == 1);
    result.originIdentity = readIdentity(json.at("originIdentity"));
    result.stateIoDigest = text(json.at("stateIoDigest"), 71);
    result.logicalBytes = unsignedValue(json.at("logicalBytes"));
    result.encodedBytes = unsignedValue(json.at("encodedBytes"));
    const auto& components = json.at("components");
    require(components.is_array() && !components.empty() && components.size() <= 256);
    std::uint64_t logical = 0, encoded = 0;
    std::set<std::string> names;
    for (const auto& value : components) {
      keys(value, {"inputName", "elementType", "shape", "payloadBytes", "contentDigest", "wireBytes"});
      NativePortableKvComponentV1 component;
      component.inputName = text(value.at("inputName"), 1024);
      require(names.insert(component.inputName).second);
      component.elementType = static_cast<TensorElementType>(unsignedValue(
        value.at("elementType"), std::numeric_limits<std::uint32_t>::max()));
      const auto elementWidth = width(component.elementType);
      const auto& shape = value.at("shape");
      require(shape.is_array() && shape.size() <= 16);
      std::uint64_t elements = 1;
      for (const auto& item : shape) {
        const auto dim = unsignedValue(item, 1ULL << 31);
        require(dim == 0 || elements <= std::numeric_limits<std::uint64_t>::max() / dim);
        elements *= dim;
        component.shape.push_back(static_cast<std::int64_t>(dim));
      }
      require(elements <= (512ULL * 1024 * 1024) / elementWidth);
      component.payloadBytes = unsignedValue(value.at("payloadBytes"));
      require(component.payloadBytes == elements * elementWidth);
      component.contentDigest = text(value.at("contentDigest"), 71);
      const auto size = unsignedValue(value.at("wireBytes"));
      require(size <= std::numeric_limits<std::size_t>::max() &&
              size == tensorBundleEncodedSize({{component.inputName.size(), component.shape.size(), component.payloadBytes}}));
      add(logical, component.payloadBytes, limits.portable.maxPayloadBytes);
      add(encoded, size, limits.portable.maxEncodedBytes);
      sizes.push_back(static_cast<std::size_t>(size));
      result.components.push_back(std::move(component));
    }
    require(logical > 0 && logical == result.logicalBytes && encoded == result.encodedBytes &&
            encoded == static_cast<std::uint64_t>(info.st_size) - 16 - length);
    checkText(result, limits);
    require(nativeCanonicalJson(json) == wire);
  }
  catch (const NativeJson::exception&) { invalid(); }
  for (std::size_t i = 0; i < sizes.size(); ++i) {
    cancelled(limits);
    result.components[i].tensorWire.resize(sizes[i]);
    transfer(file.fd, result.components[i].tensorWire.data(), sizes[i], false, limits);
  }
  std::uint8_t extra;
  ssize_t count;
  do { count = ::read(file.fd, &extra, 1); } while (count < 0 && errno == EINTR);
  require(count == 0);
  validateNativePortableKvSnapshot(result, limits.portable);
  return result;
}

} // namespace ndnsf::di

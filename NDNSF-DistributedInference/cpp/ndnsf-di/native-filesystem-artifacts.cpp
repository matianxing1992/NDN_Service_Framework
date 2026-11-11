#include "native-filesystem-artifacts.hpp"

#include "native-material-fragment-cache.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-owned-initializer-file.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <boost/scope_exit.hpp>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <fcntl.h>
#include <fstream>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

bool
validHex(const std::string& value)
{
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [] (char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string
componentText(const ndn::name::Component& component)
{
  if (component.type() != ndn::tlv::GenericNameComponent)
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_COMPONENT_INVALID");
  return {reinterpret_cast<const char*>(component.value()), component.value_size()};
}

std::uint64_t
decimal(const ndn::name::Component& component)
{
  const auto text = componentText(component);
  std::uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_RANGE_INVALID");
  return result;
}

std::filesystem::path
objectPath(const std::filesystem::path& root, const std::string& hex)
{
  return root / "objects" / "sha256" / hex.substr(0, 2) / hex;
}

} // namespace

NativeFilesystemSourceProvider::NativeFilesystemSourceProvider(std::filesystem::path root)
  : m_root(std::move(root))
{
  if (!m_root.is_absolute() || std::filesystem::canonical(m_root) != m_root)
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_SOURCE_DIRECTORY_INVALID");
}

NativeCanonicalSource
NativeFilesystemSourceProvider::load(const RepositorySourceRequest& request, const Fallback&) const
{
  const auto active = [&] {
    if (std::chrono::steady_clock::now() >= request.deadline)
      throw std::runtime_error("DI_FILESYSTEM_CACHE_SOURCE_TIMEOUT");
  };
  active();
  const auto catalog = nativeParseJson(request.catalogConfigurationJson);
  const auto& source = catalog.at("source");
  const auto sourceDigest = source.at("digest").get<std::string>();
  const auto bytes = source.at("bytes").get<std::uint64_t>();
  const auto maximum = std::min<std::uint64_t>(request.maxSourceBytes, 64ULL * 1024 * 1024);
  auto graph = NativeMaterialFragmentCache(m_root / "payloads")
    .read(sourceDigest, bytes, maximum);
  if (!graph) throw std::runtime_error("DI_FILESYSTEM_CACHE_SOURCE_INVALID");
  NativeCanonicalSource result;
  result.modelBytes = std::move(*graph);
  if (source.contains("initializer_digest")) {
    const auto digest = source.at("initializer_digest").get<std::string>();
    const auto size = source.at("initializer_bytes").get<std::uint64_t>();
    if (digest.size() != 71 || digest.substr(0, 7) != "sha256:" || !validHex(digest.substr(7)))
      throw std::runtime_error("DI_FILESYSTEM_CACHE_INITIALIZER_IDENTITY_INVALID");
    const auto path = m_root / "payloads" / "sha256" / digest.substr(7, 2) / digest.substr(7);
    result.initializerRangeSource = openNativeCanonicalInitializerFile(
      path.string(), size, digest,
      {request.deadline, active, request.maxSourceBytes, request.maxSourceBytes});
  }
  active();
  return result;
}

NativeFilesystemArtifactPublisher::NativeFilesystemArtifactPublisher(
  std::filesystem::path root, ndn::Name producer, std::string sourceNamespace)
  : m_root(std::move(root))
{
  if (!m_root.is_absolute() || std::filesystem::canonical(m_root) != m_root ||
      producer.empty() || sourceNamespace.size() != 71 ||
      sourceNamespace.substr(0, 7) != "sha256:" || !validHex(sourceNamespace.substr(7)))
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_CONFIGURATION_INVALID");
  m_prefix = ndn_service_framework::makeProducerDataPrefix(producer, "MODEL-CACHE")
    .append(sourceNamespace.substr(7));
}

NativePreparedCanonicalPublication
NativeFilesystemArtifactPublisher::publish(
  const std::string& modelKey, const std::string& serviceName,
  const NativeInspectedModel& model, const NativeCanonicalSource& source,
  const NativeCanonicalPublicationOptions& options, const NativeRequestControl& control) const
{
  control.requireActive();
  model.validate();
  if (modelKey.empty() || serviceName.empty() || !source.materialManifest)
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_MATERIALS_REQUIRED");
  const auto& manifest = *source.materialManifest;
  manifest.validate([&] { control.requireActive(); });
  if (manifest.sourceDigest != model.canonicalSourceDigest ||
      manifest.graphDigest != model.canonicalGraphDigest ||
      manifest.initializerDigest != model.canonicalInitializerDigest)
    throw std::invalid_argument("DI_FILESYSTEM_CACHE_MATERIAL_IDENTITY_MISMATCH");
  NativeMaterialFragmentCache objects(m_root / "objects");
  const auto store = [&] (const std::string& value) {
    control.requireActive();
    if (value.empty() || value.size() > 64ULL * 1024 * 1024)
      throw std::runtime_error("DI_FILESYSTEM_CACHE_OBJECT_LIMIT");
    const auto digest = nativePlanningDigest(value);
    if (!objects.write(digest, ndn::span<const std::uint8_t>(
          reinterpret_cast<const std::uint8_t*>(value.data()), value.size())))
      throw std::runtime_error("DI_FILESYSTEM_CACHE_WRITE_FAILED");
    return ndn::Name(m_prefix).append("OBJECT").append(digest.substr(7))
      .toUri(ndn::name::UriFormat::CANONICAL);
  };
  NativePreparedCanonicalPublication result;
  result.artifactPrefetchRequired = false;
  result.preparedMetadataJson = source.preparedMetadataJson;
  NativeJson records = NativeJson::array();
  for (const auto& payload : manifest.payloads) {
    control.requireActive();
    const auto bytes = payload.byteSize();
    if (payload.digest.size() != 71 || payload.digest.substr(0, 7) != "sha256:" ||
        !validHex(payload.digest.substr(7)) || bytes == 0 ||
        bytes > NativeCanonicalMaterialBundleMaxBytes)
      throw std::invalid_argument("DI_FILESYSTEM_CACHE_PAYLOAD_INVALID");
    std::string name;
    if (payload.rangeSource) {
      // The manifest authenticates the range; readers hash each selected slice.
      if (payload.rangeSource->size() != model.canonicalInitializerBytes ||
          payload.rangeOffset > payload.rangeSource->size() ||
          bytes > payload.rangeSource->size() - payload.rangeOffset)
        throw std::invalid_argument("DI_FILESYSTEM_CACHE_SOURCE_RANGE_MISMATCH");
      name = ndn::Name(m_prefix).append("RANGE").append(std::to_string(payload.rangeOffset))
        .append(std::to_string(bytes)).append(payload.digest.substr(7))
        .toUri(ndn::name::UriFormat::CANONICAL);
    }
    else {
      const auto value = payload.copyBytes([&] { control.requireActive(); });
      if (nativePlanningDigest(value.data(), value.size()) != payload.digest)
        throw std::runtime_error("DI_FILESYSTEM_CACHE_PAYLOAD_DIGEST_MISMATCH");
      name = store(std::string(value.begin(), value.end()));
    }
    result.materialPayloadIds.push_back(payload.payloadId);
    result.materialDataNames.push_back(name);
    result.materialDigests.push_back(payload.digest);
    records.push_back(NativeJson{{"payloadId", payload.payloadId}, {"dataName", name},
                                {"digest", payload.digest}, {"bytes", bytes}});
  }
  const auto manifestJson = manifest.canonicalJson();
  result.materialManifestDataName = store(manifestJson);
  result.materialManifestDigest = nativePlanningDigest(manifestJson);
  result.materialManifestBytes = manifestJson.size();
  const auto receipt = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-material-receipt-v1"},
    {"sourceDigest", manifest.sourceDigest}, {"graphDigest", manifest.graphDigest},
    {"materialIdentityDigest", manifest.manifestDigest},
    {"materialManifestDigest", result.materialManifestDigest},
    {"materialObjects", std::move(records)}});
  result.materialReceiptDataName = store(receipt);
  result.materialReceiptDigest = nativePlanningDigest(receipt);
  result.materialReceiptBytes = receipt.size();
  NativeJson metadata{{"canonicalSourceDigest", model.canonicalSourceDigest},
    {"cacheCompatibilityNamespace", "sha256:" + componentText(m_prefix[-1])},
    {"canonicalSourceBytes", model.canonicalSourceBytes},
    {"canonicalInitializerBytes", model.canonicalInitializerBytes},
    {"canonicalInitializerObjectDigest", model.canonicalInitializerObjectDigest},
    {"materialBacked", true}, {"materialIdentityDigest", manifest.manifestDigest},
    {"materialManifestDataName", result.materialManifestDataName},
    {"materialManifestDigest", result.materialManifestDigest},
    {"materialManifestBytes", result.materialManifestBytes},
    {"materialReceiptDataName", result.materialReceiptDataName},
    {"materialReceiptDigest", result.materialReceiptDigest},
    {"materialReceiptBytes", result.materialReceiptBytes}};
  if (!options.packageManifestDigest.empty())
    metadata["packageManifestDigest"] = options.packageManifestDigest;
  result.canonicalManifestJson = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-canonical-model-manifest-v1"}, {"state", "ACTIVE"},
    {"artifactProfileDigest", options.artifactProfileDigest},
    {"modelIdentityDigest", model.descriptor.contentDigest},
    {"modelSemanticIdentityDigest", model.descriptor.modelDigest()},
    {"modelName", model.descriptor.modelName}, {"metadata", std::move(metadata)}});
  result.manifestDigest = nativePlanningDigest(result.canonicalManifestJson);
  result.rootDataName = store(result.canonicalManifestJson);
  result.artifactProfileDigest = options.artifactProfileDigest;
  result.rollbackOwned = false;
  result.validate();
  control.requireActive();
  return result;
}

void
NativeFilesystemArtifactPublisher::rollback(const NativePreparedCanonicalPublication&) const noexcept
{
  // Immutable local objects own no live network publication to undo.
}

std::optional<ndn::Buffer>
readNativeFilesystemArtifact(const std::filesystem::path& root, const ndn::Name& prefix,
                             const ndn::Name& name, std::uint64_t maxBytes)
{
  if (!prefix.isPrefixOf(name) || maxBytes == 0 || name.size() < prefix.size() + 2)
    return std::nullopt;
  const auto hex = componentText(name[-1]);
  if (!validHex(hex)) return std::nullopt;
  maxBytes = std::min<std::uint64_t>(maxBytes, 64ULL * 1024 * 1024);
  if (name.size() == prefix.size() + 2 && componentText(name[-2]) == "OBJECT") {
    std::error_code error;
    const auto bytes = std::filesystem::file_size(objectPath(root, hex), error);
    if (error || bytes == 0 || bytes > maxBytes) return std::nullopt;
    auto value = NativeMaterialFragmentCache(root / "objects").read("sha256:" + hex, bytes, maxBytes);
    if (!value) return std::nullopt;
    ndn::Buffer buffer;
    buffer.swap(*value);
    return buffer;
  }
  if (name.size() != prefix.size() + 4 || componentText(name[-4]) != "RANGE")
    return std::nullopt;
  const auto offset = decimal(name[-3]);
  const auto bytes = decimal(name[-2]);
  if (bytes == 0 || bytes > maxBytes || bytes > NativeCanonicalMaterialBundleMaxBytes)
    return std::nullopt;
  const auto markerPath = root / "cache-identity.json";
  std::error_code error;
  if (std::filesystem::symlink_status(markerPath, error).type() !=
        std::filesystem::file_type::regular || error)
    return std::nullopt;
  const auto markerBytes = std::filesystem::file_size(markerPath, error);
  if (markerBytes > 65536 || markerBytes == 0 || error) return std::nullopt;
  std::ifstream marker(markerPath);
  std::string markerText(static_cast<std::size_t>(markerBytes), '\0');
  if (!marker.read(markerText.data(), markerText.size()) || marker.peek() != EOF)
    return std::nullopt;
  const auto identity = nativeParseJson(markerText);
  const auto initializer = identity.at("initializerDigest").get<std::string>();
  if (initializer.size() != 71 || initializer.substr(0, 7) != "sha256:" ||
      !validHex(initializer.substr(7))) return std::nullopt;
  const auto path = root / "payloads" / "sha256" / initializer.substr(7, 2) /
    initializer.substr(7);
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) return std::nullopt;
  BOOST_SCOPE_EXIT_ALL(fd) { ::close(fd); };
  struct stat info{};
  if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
      offset > static_cast<std::uint64_t>(info.st_size) ||
      bytes > static_cast<std::uint64_t>(info.st_size) - offset) {
    return std::nullopt;
  }
  ndn::Buffer result;
  result.resize(static_cast<std::size_t>(bytes));
  std::size_t done = 0;
  while (done < result.size()) {
    const auto count = ::pread(fd, result.data() + done, result.size() - done,
                               static_cast<off_t>(offset + done));
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return std::nullopt;
    done += static_cast<std::size_t>(count);
  }
  if (nativePlanningDigest(result.data(), result.size()) != "sha256:" + hex)
    return std::nullopt;
  return result;
}

} // namespace ndnsf::di

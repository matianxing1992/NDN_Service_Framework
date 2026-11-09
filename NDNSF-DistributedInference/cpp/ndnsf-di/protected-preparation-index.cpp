#include "protected-preparation-index.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/material-receipt.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

#include <ndn-cxx/util/scope.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <memory>
#include <openssl/rand.h>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ndnsf::di {
namespace {

constexpr std::size_t MAX_POINTER_BYTES = 64U << 10;
constexpr std::size_t MAX_POINTERS = 4096;

[[noreturn]] void
reject(const std::string& reason)
{
  throw RepositorySourceError(RepositorySourceError::Kind::Unavailable,
                              "DI_PROTECTED_PREPARATION_" + reason);
}

[[noreturn]] void
ioError(const char* reason)
{
  reject(std::string(reason) + ": errno=" + std::to_string(errno));
}

class OwnedFd
{
public:
  explicit OwnedFd(int value = -1) : m_value(value) {}
  ~OwnedFd() { if (m_value >= 0) ::close(m_value); }
  OwnedFd(const OwnedFd&) = delete;
  OwnedFd& operator=(const OwnedFd&) = delete;
  OwnedFd(OwnedFd&& other) noexcept : m_value(other.m_value) { other.m_value = -1; }
  int get() const noexcept { return m_value; }

private:
  int m_value;
};

void
checkFile(int fd, bool payload)
{
  struct stat status{};
  if (::fstat(fd, &status) != 0) ioError("FILE_STAT_FAILED");
  if (!S_ISREG(status.st_mode) || status.st_uid != ::geteuid() || status.st_nlink != 1 ||
      (status.st_mode & 07777) != 0600 || status.st_size < 0 ||
      static_cast<std::uint64_t>(status.st_size) > MAX_POINTER_BYTES ||
      (payload && status.st_size == 0))
    reject("PRIVATE_FILE_INVALID");
}

OwnedFd
openDirectory(const std::filesystem::path& path, bool create)
{
  int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0 && errno == ENOENT && create) {
    if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) ioError("DIRECTORY_CREATE_FAILED");
    fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  }
  if (fd < 0) {
    if (errno == ENOENT && !create) return OwnedFd{};
    ioError("DIRECTORY_OPEN_FAILED");
  }
  OwnedFd result(fd);
  struct stat status{};
  if (::fstat(fd, &status) != 0) ioError("DIRECTORY_STAT_FAILED");
  if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
      (status.st_mode & 07777) != 0700)
    reject("PRIVATE_DIRECTORY_INVALID");
  return result;
}

OwnedFd
lockDirectory(int directory, bool write)
{
  OwnedFd result(::openat(directory, ".lock", O_CLOEXEC | O_NOFOLLOW |
    (write ? O_RDWR | O_CREAT : O_RDONLY), 0600));
  if (result.get() < 0) ioError("LOCK_OPEN_FAILED");
  checkFile(result.get(), false);
  if (::flock(result.get(), (write ? LOCK_EX : LOCK_SH) | LOCK_NB) != 0)
    ioError("INDEX_BUSY");
  return result;
}

std::optional<std::string>
readPointer(int directory, const std::string& name)
{
  OwnedFd file(::openat(directory, name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
  if (file.get() < 0) {
    if (errno == ENOENT) return std::nullopt;
    ioError("POINTER_OPEN_FAILED");
  }
  checkFile(file.get(), true);
  std::string result;
  std::array<char, 4096> buffer{};
  for (;;) {
    const auto size = ::read(file.get(), buffer.data(), buffer.size());
    if (size < 0) {
      if (errno == EINTR) continue;
      ioError("POINTER_READ_FAILED");
    }
    if (size == 0) break;
    if (static_cast<std::size_t>(size) > MAX_POINTER_BYTES - result.size())
      reject("POINTER_SIZE_EXCEEDED");
    result.append(buffer.data(), static_cast<std::size_t>(size));
  }
  return result;
}

bool
isHex(const std::string& value)
{
  return std::all_of(value.begin(), value.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}

std::string
identityFor(const RepositoryPreparedLookupRequest& request)
{
  return nativeCanonicalPublicationIdentity(nativeParseJson(request.catalogConfigurationJson));
}

std::string
fileNameFor(const ndn::Name& owner, const RepositoryPreparedLookupRequest& request)
{
  return nativePlanningDigest(nativeCanonicalJson(NativeJson{
    {"owner", owner.toUri(ndn::name::UriFormat::CANONICAL)},
    {"service", ndn::Name(request.serviceName).toUri(ndn::name::UriFormat::CANONICAL)},
    {"catalogIdentity", identityFor(request)}})).substr(7) + ".json";
}

void
requireActive(const RepositoryPreparedLookupRequest& request)
{
  if (!request.requireActive) reject("CURRENT_OPERATION_REQUIRED");
  request.requireActive();
  if (std::chrono::steady_clock::now() >= request.deadline)
    throw RepositorySourceError(RepositorySourceError::Kind::Timeout,
                                  "DI_PROTECTED_PREPARATION_EXPIRED");
}

NativeJson
decodePointer(const std::string& wire, const ndn::Name& owner,
              const RepositoryPreparedLookupRequest& request)
{
  const auto value = nativeParseJson(wire);
  if (!value.is_object() || value.size() != 6 || nativeCanonicalJson(value) != wire ||
      value.value("schema", std::string{}) != "ndnsf-di-protected-preparation-pointer-v1" ||
      value.at("owner").get<std::string>() != owner.toUri(ndn::name::UriFormat::CANONICAL) ||
      ndn::Name(value.at("service").get<std::string>()) != ndn::Name(request.serviceName) ||
      value.at("catalogIdentity").get<std::string>() != identityFor(request))
    reject("POINTER_IDENTITY_INVALID");
  return value;
}

std::string
readProtected(const RepositoryPreparedLookupRequest& request,
              const ndn_service_framework::LargeDataPublishResult& reference,
              std::uint64_t maximum)
{
  requireActive(request);
  if (!request.readProtected || reference.plaintextSize > maximum ||
      (request.maxPublicationBytes != 0 && reference.plaintextSize > request.maxPublicationBytes))
    reject("OBJECT_READ_BOUND_INVALID");
  std::string result;
  bool consumed = false;
  request.readProtected(reference, maximum, [&](auto bytes) {
    requireActive(request);
    if (consumed || bytes.size() != reference.plaintextSize || bytes.size() > maximum)
      reject("OBJECT_READ_SIZE_INVALID");
    result.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    consumed = true;
  });
  requireActive(request);
  if (!consumed || nativePlanningDigest(result) != reference.contentDigest)
    reject("OBJECT_READ_IDENTITY_INVALID");
  return result;
}

NativeJson
validateRoot(const RepositoryPreparedLookupRequest& request, const std::string& wire,
             const ndn_service_framework::LargeDataPublishResult& metadataReference)
{
  const auto root = nativeParseJson(wire);
  const auto catalog = nativeParseJson(request.catalogConfigurationJson);
  const auto model = NativeModelDescriptor::fromCanonicalJson(nativeCanonicalJson(catalog.at("model")));
  const auto& source = catalog.at("source");
  const auto& metadata = root.at("metadata");
  if (wire.size() > 4096 || nativeCanonicalJson(root) != wire ||
      root.value("schema", std::string{}) != "ndnsf-di-canonical-model-manifest-v1" ||
      root.value("state", std::string{}) != "ACTIVE" ||
      root.at("publicationIdentityDigest").get<std::string>() != identityFor(request) ||
      root.at("modelIdentityDigest").get<std::string>() != model.contentDigest ||
      root.at("modelSemanticIdentityDigest").get<std::string>() != model.modelDigest() ||
      root.at("modelName").get<std::string>() != model.modelName ||
      root.at("artifactProfileDigest") != catalog.at("recipe").at("artifact_profile_digest") ||
      metadata.at("canonicalSourceDigest") != source.at("digest") ||
      metadata.at("canonicalSourceBytes").get<std::uint64_t>() !=
        source.value("bytes", metadata.at("canonicalSourceBytes").get<std::uint64_t>()) ||
      metadata.at("preparedMetadataDataName").get<std::string>() !=
        metadataReference.manifestName.toUri(ndn::name::UriFormat::CANONICAL) ||
      metadata.at("preparedMetadataDigest").get<std::string>() != metadataReference.contentDigest ||
      metadata.at("preparedMetadataBytes").get<std::uint64_t>() != metadataReference.plaintextSize)
    reject("ROOT_CATALOG_CONFLICT");
  return root;
}

} // namespace

ProtectedPreparationIndex::ProtectedPreparationIndex(std::filesystem::path sourceCacheRoot,
                                                     ndn::Name owner)
  : m_directory(std::move(sourceCacheRoot) / "prepared-owner-index")
  , m_owner(std::move(owner))
{
  if (!m_directory.is_absolute() || m_owner.empty())
    throw std::invalid_argument("protected preparation index requires its source cache and owner");
}

std::optional<NativePreparedCanonicalPublication>
ProtectedPreparationIndex::lookup(const RepositoryPreparedLookupRequest& request) const
try {
  requireActive(request);
  std::optional<std::string> pointer;
  {
    auto directory = openDirectory(m_directory, false);
    if (directory.get() < 0) return std::nullopt;
    const auto name = fileNameFor(m_owner, request);
    struct stat status{};
    if (::fstatat(directory.get(), name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0) {
      if (errno == ENOENT) return std::nullopt;
      ioError("POINTER_STAT_FAILED");
    }
    auto lock = lockDirectory(directory.get(), false);
    pointer = readPointer(directory.get(), name);
  }
  if (!pointer) return std::nullopt;
  if (!request.restoreProtected || !request.readProtected) reject("CORE_OWNER_PORT_REQUIRED");
  const auto value = decodePointer(*pointer, m_owner, request);
  const auto rootReference = nativeProtectedObjectFromReference(value.at("root"));
  const auto metadataReference = nativeProtectedObjectFromReference(value.at("metadata"));
  const auto rootWire = readProtected(request, rootReference, 4096);
  const auto root = validateRoot(request, rootWire, metadataReference);
  const auto& rootMetadata = root.at("metadata");
  NativePreparedCanonicalPublication result;
  result.rootDataName = rootReference.manifestName.toUri(ndn::name::UriFormat::CANONICAL);
  result.canonicalManifestJson = rootWire;
  result.manifestDigest = rootReference.contentDigest;
  result.artifactProfileDigest = root.at("artifactProfileDigest").get<std::string>();
  result.preparedMetadataDataName = metadataReference.manifestName.toUri(
    ndn::name::UriFormat::CANONICAL);
  result.preparedMetadataBytes = metadataReference.plaintextSize;
  result.preparedMetadataDigest = metadataReference.contentDigest;
  result.preparedMetadataJson = readProtected(request, metadataReference,
    NativeCanonicalMaterialPublicationMaxBytes);
  const auto metadata = nativeParseJson(result.preparedMetadataJson);
  if (nativeCanonicalJson(metadata) != result.preparedMetadataJson ||
      metadata.value("schema", std::string{}) != "ndnsf-di-prepared-metadata-v1" ||
      !metadata.at("protectedObjects").is_array() ||
      metadata.at("protectedObjects").size() > MAX_POINTERS - 2)
    reject("METADATA_INVALID");
  using Reference = ndn_service_framework::LargeDataPublishResult;
  std::map<std::string, Reference> objects;
  const auto add = [&](Reference reference) {
    const auto name = reference.manifestName.toUri(ndn::name::UriFormat::CANONICAL);
    if (!objects.emplace(name, std::move(reference)).second)
      reject("OBJECT_REFERENCE_DUPLICATED");
  };
  for (const auto& object : metadata.at("protectedObjects"))
    add(nativeProtectedObjectFromReference(object));
  add(metadataReference);
  add(rootReference);
  std::set<std::string> used{result.rootDataName, result.preparedMetadataDataName};
  const auto referenceFor = [&](const std::string& name, const std::string& digest,
                                 std::uint64_t bytes) -> const Reference& {
    const auto found = objects.find(name);
    if (found == objects.end() || found->second.contentDigest != digest ||
        found->second.plaintextSize != bytes)
      reject("OBJECT_REFERENCE_CONFLICT");
    used.insert(name);
    return found->second;
  };
  const auto initializerName = rootMetadata.value("canonicalInitializerDataName", std::string{});
  if (!initializerName.empty()) {
    referenceFor(initializerName,
      rootMetadata.at("canonicalInitializerObjectDigest").get<std::string>(),
      rootMetadata.at("canonicalInitializerBytes").get<std::uint64_t>());
    result.initializerDataName = initializerName;
  }
  const auto materialName = rootMetadata.value("materialManifestDataName", std::string{});
  if (materialName.empty()) {
    result.sourceDataName = rootMetadata.at("canonicalSourceDataName").get<std::string>();
    referenceFor(result.sourceDataName, rootMetadata.at("canonicalSourceDigest").get<std::string>(),
                  rootMetadata.at("canonicalSourceBytes").get<std::uint64_t>());
  }
  else {
    result.materialManifestDataName = materialName;
    result.materialManifestDigest = rootMetadata.at("materialManifestDigest").get<std::string>();
    result.materialManifestBytes = rootMetadata.at("materialManifestBytes").get<std::uint64_t>();
    const auto& materialReference = referenceFor(materialName, result.materialManifestDigest,
                                                result.materialManifestBytes);
    const auto materialWire = readProtected(request, materialReference,
      NativeCanonicalMaterialPublicationMaxBytes);
    auto material = parseNativeCanonicalMaterialManifest(
      std::vector<std::uint8_t>(materialWire.begin(), materialWire.end()));
    const auto catalog = nativeParseJson(request.catalogConfigurationJson);
    if (material->manifestDigest != rootMetadata.at("materialIdentityDigest").get<std::string>() ||
        material->sourceDigest != catalog.at("source").at("digest").get<std::string>() ||
        material->graphDigest != catalog.at("source").at("canonical_graph_digest").get<std::string>())
      reject("MATERIAL_IDENTITY_CONFLICT");
    result.materialManifest = material;
    result.materialReceiptDataName = rootMetadata.at("materialReceiptDataName").get<std::string>();
    result.materialReceiptDigest = rootMetadata.at("materialReceiptDigest").get<std::string>();
    result.materialReceiptBytes = rootMetadata.at("materialReceiptBytes").get<std::uint64_t>();
    const auto& receiptReference = referenceFor(result.materialReceiptDataName,
      result.materialReceiptDigest, result.materialReceiptBytes);
    const auto receiptWire = readProtected(request, receiptReference,
      NativeCanonicalMaterialPublicationMaxBytes);
    const auto receipt = parseNativeCanonicalMaterialReceipt(
      std::vector<std::uint8_t>(receiptWire.begin(), receiptWire.end()), material->sourceDigest,
      material->graphDigest, material->manifestDigest, result.materialManifestDigest);
    std::map<std::string, std::pair<std::string, std::uint64_t>> expected;
    for (const auto& item : material->references) {
      const auto entry = std::make_pair(item.digest, item.bytes);
      const auto inserted = expected.emplace(item.payloadId, entry);
      if (!inserted.second && inserted.first->second != entry) reject("MATERIAL_PAYLOAD_CONFLICT");
      for (const auto& id : item.chunkPayloadIds) expected.emplace(id, std::make_pair("", 0));
    }
    std::set<std::string> seen;
    std::map<std::string, std::vector<std::pair<std::uint64_t, std::uint64_t>>> ranges;
    for (const auto& item : receipt.get_child("materialObjects")) {
      requireActive(request);
      const auto id = item.second.get<std::string>("payloadId");
      const auto name = item.second.get<std::string>("dataName");
      const auto digest = item.second.get<std::string>("digest");
      const auto bytes = item.second.get<std::uint64_t>("bytes");
      const auto wanted = expected.find(id);
      if (wanted == expected.end() || !seen.insert(id).second || bytes == 0 ||
          bytes > NativeCanonicalMaterialBundleMaxBytes ||
          (wanted->second.second != 0 && wanted->second != std::make_pair(digest, bytes)) ||
          digest.size() != 71 || digest.compare(0, 7, "sha256:") != 0 || !isHex(digest.substr(7)))
        reject("MATERIAL_PAYLOAD_CONFLICT");
      const auto bundleDigest = item.second.get_optional<std::string>("bundleDigest");
      const auto bundleBytes = item.second.get_optional<std::uint64_t>("bundleBytes");
      const auto offset = item.second.get_optional<std::uint64_t>("bundleOffset");
      const bool bundled = bundleDigest || bundleBytes || offset;
      if (bundled && (!bundleDigest || !bundleBytes || !offset ||
          *bundleBytes > NativeCanonicalMaterialPublicationMaxBytes ||
          *offset > *bundleBytes || bytes > *bundleBytes - *offset))
        reject("MATERIAL_RANGE_INVALID");
      referenceFor(name, bundled ? *bundleDigest : digest, bundled ? *bundleBytes : bytes);
      ranges[name].emplace_back(bundled ? *offset : 0, bytes);
      result.materialPayloadIds.push_back(id);
      result.materialDataNames.push_back(name);
      result.materialDigests.push_back(digest);
    }
    if (seen.size() != expected.size()) reject("MATERIAL_PAYLOAD_MISSING");
    for (auto& range : ranges) {
      std::sort(range.second.begin(), range.second.end());
      std::uint64_t end = 0;
      for (const auto& item : range.second) {
        if (item.first != end) reject("MATERIAL_RANGE_INVALID");
        end += item.second;
      }
      if (end != objects.at(range.first).plaintextSize) reject("MATERIAL_RANGE_INVALID");
    }
  }
  if (used.size() != objects.size()) reject("UNREFERENCED_OBJECT");
  result.rollbackOwned = false;
  auto references = std::make_shared<std::vector<Reference>>();
  references->reserve(objects.size());
  for (const auto& item : objects) {
    requireActive(request);
    Reference restored;
    try {
      restored = request.restoreProtected(item.second);
    }
    catch (const std::runtime_error& error) {
      if (std::string(error.what()) != "OWNER_ORIGINAL_OBJECT_MISSING" ||
          item.first == result.rootDataName || item.first == result.preparedMetadataDataName ||
          item.first == result.materialManifestDataName || item.first == result.materialReceiptDataName)
        throw;
      result.missingDataNames.push_back(item.first);
      references->push_back(item.second);
      continue;
    }
    if (nativeProtectedObjectReference(restored) != nativeProtectedObjectReference(item.second))
      reject("RESTORED_REFERENCE_CONFLICT");
    if (!restored.servingLease) reject("SERVING_LEASE_REQUIRED");
    result.servingLeases.push_back(std::move(restored.servingLease));
    references->push_back(std::move(restored));
  }
  result.ownerRecoverableObjects = std::move(references);
  result.validate();
  requireActive(request);
  return result;
}
catch (const RepositorySourceError&) {
  throw;
}
catch (const std::exception& error) {
  reject(std::string("LOOKUP_INVALID: ") + error.what());
}

std::optional<NativePreparedCanonicalPublication>
ProtectedPreparationIndex::lookupWithLegacy(
  const RepositoryPreparedLookupRequest& request, const std::filesystem::path& legacyRoot) const
{
  if (auto prepared = lookup(request))
    return prepared;
  ProtectedPreparationIndex legacy(legacyRoot, m_owner);
  if (legacy.m_directory == m_directory)
    return std::nullopt;
  auto prepared = legacy.lookup(request);
  if (prepared && prepared->missingDataNames.empty())
    remember(request, *prepared);
  return prepared;
}

void
ProtectedPreparationIndex::remember(const RepositoryPreparedLookupRequest& request,
                                      const NativePreparedCanonicalPublication& publication) const
{
  requireActive(request);
  if (!publication.ownerRecoverableObjects || publication.ownerRecoverableObjects->empty() ||
      publication.preparedMetadataDataName.empty())
    return;
  publication.validate();
  if (!publication.missingDataNames.empty()) reject("INCOMPLETE_PUBLICATION");
  using Reference = ndn_service_framework::LargeDataPublishResult;
  const Reference* root = nullptr;
  const Reference* metadata = nullptr;
  for (const auto& object : *publication.ownerRecoverableObjects) {
    const auto name = object.manifestName.toUri(ndn::name::UriFormat::CANONICAL);
    if (name == publication.rootDataName) {
      if (root) reject("OBJECT_REFERENCE_DUPLICATED");
      root = &object;
    }
    if (name == publication.preparedMetadataDataName) {
      if (metadata) reject("OBJECT_REFERENCE_DUPLICATED");
      metadata = &object;
    }
  }
  if (!root || !metadata || root->contentDigest != publication.manifestDigest ||
      root->plaintextSize != publication.canonicalManifestJson.size() ||
      metadata->contentDigest != publication.preparedMetadataDigest ||
      metadata->plaintextSize != publication.preparedMetadataBytes ||
      ndn_service_framework::ProtectedBlobManifest::inferProducer(root->manifestName) != m_owner ||
      ndn_service_framework::ProtectedBlobManifest::inferProducer(metadata->manifestName) != m_owner)
    reject("PUBLICATION_REFERENCE_INVALID");
  (void)validateRoot(request, publication.canonicalManifestJson, *metadata);
  const auto wire = nativeCanonicalJson(NativeJson{
    {"schema", "ndnsf-di-protected-preparation-pointer-v1"},
    {"owner", m_owner.toUri(ndn::name::UriFormat::CANONICAL)},
    {"service", ndn::Name(request.serviceName).toUri(ndn::name::UriFormat::CANONICAL)},
    {"catalogIdentity", identityFor(request)},
    {"root", nativeProtectedObjectReference(*root)},
    {"metadata", nativeProtectedObjectReference(*metadata)}});
  if (wire.empty() || wire.size() > MAX_POINTER_BYTES) reject("POINTER_SIZE_EXCEEDED");
  auto directory = openDirectory(m_directory, true);
  auto lock = lockDirectory(directory.get(), true);
  const auto name = fileNameFor(m_owner, request);
  const auto previous = readPointer(directory.get(), name);
  if (previous) {
    const auto value = decodePointer(*previous, m_owner, request);
    (void)nativeProtectedObjectFromReference(value.at("root"));
    (void)nativeProtectedObjectFromReference(value.at("metadata"));
    if (*previous == wire) return;
  }
  const auto duplicate = ::dup(directory.get());
  if (duplicate < 0) ioError("DIRECTORY_DUP_FAILED");
  auto* raw = ::fdopendir(duplicate);
  if (!raw) {
    const auto error = errno;
    ::close(duplicate);
    errno = error;
    ioError("DIRECTORY_SCAN_FAILED");
  }
  std::unique_ptr<DIR, decltype(&::closedir)> scan(raw, ::closedir);
  std::size_t count = 0;
  std::size_t entries = 0;
  for (;;) {
    requireActive(request);
    errno = 0;
    const auto* entry = ::readdir(scan.get());
    if (!entry) {
      if (errno != 0) ioError("DIRECTORY_SCAN_FAILED");
      break;
    }
    const std::string filename(entry->d_name);
    if (filename == "." || filename == ".." || filename == ".lock") continue;
    if (++entries > 2 * MAX_POINTERS) reject("INDEX_ENTRY_LIMIT");
    if (filename.size() == 69 && filename.substr(64) == ".json" &&
        isHex(filename.substr(0, 64))) {
      if (++count > MAX_POINTERS) reject("INDEX_CAPACITY");
      continue;
    }
    if (filename.size() != 37 || filename.compare(0, 5, ".tmp-") != 0 ||
        !isHex(filename.substr(5)))
      reject("INDEX_ENTRY_INVALID");
    {
      OwnedFd temporary(::openat(directory.get(), filename.c_str(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
      if (temporary.get() < 0) ioError("TEMPORARY_OPEN_FAILED");
      checkFile(temporary.get(), false);
    }
    if (::unlinkat(directory.get(), filename.c_str(), 0) != 0) ioError("TEMPORARY_REMOVE_FAILED");
  }
  if (!previous && count >= MAX_POINTERS) reject("INDEX_CAPACITY");
  std::array<unsigned char, 16> random{};
  if (RAND_bytes(random.data(), static_cast<int>(random.size())) != 1)
    reject("TEMPORARY_RNG_FAILED");
  constexpr char HEX[] = "0123456789abcdef";
  std::string temporary = ".tmp-";
  for (const auto byte : random) {
    temporary.push_back(HEX[byte >> 4]);
    temporary.push_back(HEX[byte & 15]);
  }
  bool published = false;
  const auto removeTemporary = ndn::make_scope_exit([&] {
    if (!published) ::unlinkat(directory.get(), temporary.c_str(), 0);
  });
  {
    OwnedFd file(::openat(directory.get(), temporary.c_str(),
      O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (file.get() < 0) ioError("TEMPORARY_CREATE_FAILED");
    checkFile(file.get(), false);
    std::size_t offset = 0;
    while (offset < wire.size()) {
      requireActive(request);
      const auto size = ::write(file.get(), wire.data() + offset, wire.size() - offset);
      if (size < 0) {
        if (errno == EINTR) continue;
        ioError("POINTER_WRITE_FAILED");
      }
      if (size == 0) reject("POINTER_WRITE_STALLED");
      offset += static_cast<std::size_t>(size);
    }
    if (::fsync(file.get()) != 0) ioError("POINTER_SYNC_FAILED");
  }
  requireActive(request);
  if (::renameat(directory.get(), temporary.c_str(), directory.get(), name.c_str()) != 0)
    ioError("POINTER_PUBLISH_FAILED");
  published = true;
  if (::fsync(directory.get()) != 0) ioError("DIRECTORY_SYNC_FAILED");
  requireActive(request);
}

} // namespace ndnsf::di

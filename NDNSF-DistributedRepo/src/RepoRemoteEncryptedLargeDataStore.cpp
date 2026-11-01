#include "ndnsf-distributed-repo/RepoRemoteEncryptedLargeDataStore.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/random.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf_distributed_repo {
namespace {
using namespace ndn_service_framework;
using Clock = std::chrono::steady_clock;

struct PacketSpool
{
  std::filesystem::path path;
  std::FILE* file = nullptr;
  std::map<ndn::Name, std::pair<std::uint64_t, std::uint64_t>> index;
  mutable std::mutex mutex;
  std::shared_ptr<std::atomic<std::uint64_t>> budget;
  std::uint64_t reserved = 0;

  ~PacketSpool()
  {
    if (file) std::fclose(file);
    std::error_code error;
    if (!path.empty()) std::filesystem::remove(path, error);
    if (budget) budget->fetch_sub(reserved);
  }

  std::vector<uint8_t> packet(const ndn::Name& name) const
  {
    const auto found = index.find(name);
    if (found == index.end()) throw std::out_of_range("remote-spool-packet-not-found");
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<uint8_t> result(found->second.second);
    if (::fseeko(file, found->second.first, SEEK_SET) != 0 ||
        std::fread(result.data(), 1, result.size(), file) != result.size())
      throw std::runtime_error("remote-spool-read-failed");
    return result;
  }
};

struct ServingState
{
  std::mutex mutex;
  std::atomic<bool> cancelled{false};
  ndn::ScopedInterestFilterHandle filter;
};

struct ServingGuard
{
  std::shared_ptr<ServiceUser> writer;
  std::shared_ptr<ServingState> state;
  ~ServingGuard()
  {
    state->cancelled.store(true);
    // A late installation observes cancellation. The callback retains its
    // disk spool until removal has actually run.
    try { writer->postToIo([retained = state] {
      std::lock_guard<std::mutex> lock(retained->mutex);
      retained->filter.cancel();
    }); }
    catch (...) { /* Caller must keep the Face alive through worker drain. */ }
  }
};

class RemoteSource final : public EncryptedLargeDataRangeSource
{
public:
  std::shared_ptr<ServiceUser> writer;
  RepoRemoteEncryptedLargeDataStore::Options options;
  RepoUserStorageInsertRequest request;
  RepoObjectManifest committed;
  ProtectedBlobManifest root;
  std::shared_ptr<PacketSpool> spool;
  Clock::time_point deadline;

  int remaining() const
  {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    if (ms <= options.ackCollectionTimeMs)
      throw std::runtime_error("remote-packet-publication-deadline");
    return static_cast<int>(std::min<std::int64_t>(ms, options.timeoutMs));
  }
  RepoObjectManifest currentManifest(const std::function<void()>& current = {}) const
  {
    std::vector<uint8_t> wire;
    for (unsigned attempt = 0; ; ++attempt) {
      if (current) current();
      try {
        wire = RepoClient::requestSelectedService(*writer,
          ndn::Name(options.target.providerIdentity),
          makeRepoServiceName(ndn::Name(options.target.servicePrefix), "MANIFEST"),
          encodeUserStorageInsertRequest(request), options.ackCollectionTimeMs, remaining());
        break;
      }
      catch (const RepoNoAckObservations&) {
        if (current) current();
        if (attempt == 2) throw;
      }
    }
    if (current) current();
    return parseManifestJson(toString(wire));
  }
  std::uint64_t size() const noexcept override { return root.envelopeBytes; }
  bool isDurable() const noexcept override { return true; }
  void requireCurrentDurableStorageOwner(const ndn::Name& name,
                                         const std::string& owner) const override
  {
    if (name != ndn::Name(request.reference.objectName) || owner != request.storageBinding.ownerIdentity)
      throw std::runtime_error("DURABLE_OWNER_BINDING_MISMATCH");
    if (currentManifest().toJson() != committed.toJson())
      throw std::runtime_error("DURABLE_OWNER_CLAIM_UNAVAILABLE");
  }
  std::vector<ndn::Name> auxiliaryDataPacketNames() const override
  {
    std::vector<ndn::Name> result;
    for (std::size_t i = root.segmentCount + 1; i < committed.packetNames.size(); ++i)
      result.emplace_back(committed.packetNames[i]);
    return result;
  }
  std::vector<uint8_t> readOriginalDataPacket(const ndn::Name& name) const override
  {
    return spool->packet(name);
  }
  std::vector<uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > size() || length > size() - offset)
      throw std::out_of_range("remote-envelope-range-invalid");
    std::vector<uint8_t> result;
    result.reserve(length);
    while (result.size() < length) {
      const auto cursor = offset + result.size();
      auto name = root.dataPrefix;
      name.appendSegment(cursor / root.segmentBytes);
      const auto wire = spool->packet(name);
      const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size())));
      const auto& content = packet.getContent();
      const auto within = cursor % root.segmentBytes;
      if (within >= content.value_size()) throw std::runtime_error("remote-envelope-segment-invalid");
      const auto count = std::min<std::uint64_t>(length - result.size(), content.value_size() - within);
      result.insert(result.end(), content.value() + within, content.value() + within + count);
    }
    return result;
  }
};
} // namespace

RepoRemoteEncryptedLargeDataStore::RepoRemoteEncryptedLargeDataStore(
  std::shared_ptr<ServiceUser> writer, ndn::security::Certificate certificate, Options options)
  : m_writer(std::move(writer)), m_certificate(std::move(certificate)), m_options(std::move(options))
{
  struct stat attributes{};
  if (!m_writer || m_certificate.getIdentity() != m_writer->getName() ||
      !m_certificate.getValidityPeriod().isValid() || !m_options.scratchRoot.is_absolute() ||
      ::lstat(m_options.scratchRoot.c_str(), &attributes) != 0 || !S_ISDIR(attributes.st_mode) ||
      attributes.st_uid != ::geteuid() || (attributes.st_mode & 077) != 0 ||
      !m_options.maxWireBytes || m_options.maxWireBytes > REPO_USER_STORAGE_LIMIT_BYTES ||
      !m_options.maxPacketCount || m_options.maxPacketCount > 65536 ||
      m_options.ackCollectionTimeMs <= 0 || m_options.timeoutMs <= m_options.ackCollectionTimeMs ||
      m_options.timeoutMs > 600000)
    throw std::invalid_argument("remote-packet-store-config-invalid");
  for (const auto* value : {&m_options.target.repoNode, &m_options.target.providerIdentity,
                           &m_options.target.servicePrefix}) {
    if (value->empty() || value->size() > 4096 || ndn::Name(*value).empty() ||
        ndn::Name(*value).toUri(ndn::name::UriFormat::ALTERNATE) != *value)
      throw std::invalid_argument("remote-packet-store-target-invalid");
  }
}

std::shared_ptr<const EncryptedLargeDataRangeSource>
RepoRemoteEncryptedLargeDataStore::commitFile(const std::string&,
  const std::filesystem::path&, std::uint64_t, const std::function<void()>&)
{
  throw std::runtime_error("ORIGINAL_PACKET_STORAGE_REQUIRES_PACKET_COMMIT");
}

void
RepoRemoteEncryptedLargeDataStore::setPublicationDeadline(Clock::time_point deadline)
{
  std::lock_guard<std::mutex> lock(m_deadlineMutex);
  if (m_started || deadline <= Clock::now() || deadline > m_deadline)
    throw std::invalid_argument("remote-publication-deadline-cannot-extend-or-rebind");
  m_deadline = deadline;
}

std::shared_ptr<const EncryptedLargeDataRangeSource>
RepoRemoteEncryptedLargeDataStore::commitOriginalDataPackets(const ndn::Name& manifestName,
  const EncryptedLargeDataCommitOptions& options,
  const std::function<std::optional<std::pair<ndn::Name, std::vector<uint8_t>>>()>& next,
  const std::function<void()>& current)
{
  if (m_writer->isOnIoThread()) throw std::logic_error("remote-packet-commit-on-face-thread");
  if (!next || !options.storageActor || options.retention != EncryptedLargeDataRetention::Durable ||
      options.packetStorageFormat != "original-packet-set" ||
      options.storageActor->authenticatedWriterIdentity !=
        m_writer->getName().toUri(ndn::name::UriFormat::ALTERNATE) ||
      options.storageActor->grant.repoNode != m_options.target.repoNode ||
      options.packetWireBytes == 0 || options.packetWireBytes > m_options.maxWireBytes ||
      ndn::Name(options.manifestName) != manifestName)
    throw std::invalid_argument("remote-packet-commit-binding-invalid");
  if (current) current();
  auto source = std::make_shared<RemoteSource>();
  source->writer = m_writer;
  source->options = m_options;
  {
    std::lock_guard<std::mutex> lock(m_deadlineMutex);
    m_started = true;
    source->deadline = std::min(m_deadline,
      Clock::now() + std::chrono::milliseconds(m_options.timeoutMs));
  }
  auto spool = std::make_shared<PacketSpool>();
  auto reserved = m_spoolBytes->load();
  do {
    if (options.packetWireBytes > m_options.maxWireBytes - reserved)
      throw std::runtime_error("remote-packet-spool-budget-exceeded");
  } while (!m_spoolBytes->compare_exchange_weak(reserved, reserved + options.packetWireBytes));
  spool->budget = m_spoolBytes;
  spool->reserved = options.packetWireBytes;
  auto templatePath = (m_options.scratchRoot / ".original-packets-XXXXXX").string();
  std::vector<char> path(templatePath.begin(), templatePath.end());
  path.push_back('\0');
  const auto fd = ::mkstemp(path.data());
  if (fd < 0) throw std::runtime_error("remote-packet-spool-create-failed");
  spool->path = path.data();
  spool->file = ::fdopen(fd, "w+b");
  if (!spool->file) { ::close(fd); throw std::runtime_error("remote-packet-spool-open-failed"); }
  source->spool = spool;
  auto& request = source->request;
  request.operationId = "original-packets-" + std::to_string(ndn::random::generateSecureWord64());
  request.generation = 1;
  const auto& grant = options.storageActor->grant;
  request.storageBinding = {1, grant.ownerIdentity, grant.grantId, grant.ownerEpoch};
  auto& reference = request.reference;
  reference.objectName = manifestName.toUri(ndn::name::UriFormat::ALTERNATE);
  reference.dataPrefix = m_writer->getName().toUri(ndn::name::UriFormat::ALTERNATE);
  reference.forwardingHint = reference.dataPrefix;
  reference.expectedSize = options.packetWireBytes;
  reference.packetSetMetadata.emplace();
  auto& metadata = *reference.packetSetMetadata;
  metadata.objectName = reference.objectName;
  metadata.objectType = "original-packet-set";
  metadata.size = options.packetWireBytes;
  metadata.dataPrefix = options.dataPrefix;
  metadata.publicationIdentity = options.publicationIdentity;
  metadata.protectionEpoch = options.protectionEpoch;
  metadata.keyReferenceId = options.keyReferenceId;
  metadata.keyReferenceVersion = options.keyReferenceVersion;
  metadata.ciphertextManifestDigest = options.ciphertextManifestDigest;
  metadata.servingLocator = options.servingLocator;
  metadata.contentDigest = options.contentDigest;
  metadata.plaintextSize = options.plaintextSize;
  ndn::util::Sha256 digest;
  std::uint64_t bytes = 0;
  while (auto item = next()) {
    if (current) current();
    (void)source->remaining();
    if (metadata.packetNames.size() >= m_options.maxPacketCount || item->second.empty() ||
        item->second.size() > ndn::MAX_NDN_PACKET_SIZE ||
        item->second.size() > options.packetWireBytes - bytes ||
        !m_writer->getName().isPrefixOf(item->first) ||
        !spool->index.emplace(item->first, std::make_pair(bytes, item->second.size())).second)
      throw std::invalid_argument("remote-packet-spool-index-invalid");
    const ndn::Data packet(ndn::Block(ndn::span<const uint8_t>(item->second.data(), item->second.size())));
    if (packet.getName() != item->first || packet.wireEncode().size() != item->second.size() ||
        !m_certificate.getValidityPeriod().isValid() || !ndn::security::verifySignature(packet, m_certificate))
      throw std::invalid_argument("remote-packet-producer-signature-invalid");
    if (metadata.packetNames.empty()) {
      if (item->first != manifestName) throw std::invalid_argument("remote-packet-root-mismatch");
      const auto& content = packet.getContent();
      source->root = ProtectedBlobManifest::decode(
        std::vector<uint8_t>(content.value(), content.value() + content.value_size()),
        m_writer->getName(), manifestName);
      metadata.envelopeBytes = source->root.envelopeBytes;
    }
    metadata.packetNames.push_back(item->first.toUri(ndn::name::UriFormat::ALTERNATE));
    if (std::fwrite(item->second.data(), 1, item->second.size(), spool->file) != item->second.size())
      throw std::runtime_error("remote-packet-spool-write-failed");
    digest.update(ndn::span<const uint8_t>(item->second.data(), item->second.size()));
    bytes += item->second.size();
  }
  if (bytes != options.packetWireBytes || std::fflush(spool->file) != 0 ||
      source->root.segmentCount >= metadata.packetNames.size() ||
      source->root.segmentCount == 0 || source->root.segmentBytes == 0)
    throw std::runtime_error("remote-packet-spool-incomplete");
  reference.expectedSha256 = digest.toString();
  std::transform(reference.expectedSha256.begin(), reference.expectedSha256.end(),
    reference.expectedSha256.begin(), [](unsigned char c) { return std::tolower(c); });
  metadata.sha256 = reference.expectedSha256;
  reference.validateExactPacketSet();

  auto serving = std::make_shared<ServingState>();
  ServingGuard guard{m_writer, serving};
  auto installed = std::make_shared<std::promise<void>>();
  auto ready = installed->get_future();
  m_writer->postToIoWithFace([serving, spool, installed, prefix = m_writer->getName()](ndn::Face& face) {
    try {
      std::lock_guard<std::mutex> lock(serving->mutex);
      if (serving->cancelled.load())
        throw std::runtime_error("remote-packet-serving-install-cancelled");
      serving->filter = face.setInterestFilter(ndn::InterestFilter(prefix),
        [spool, &face](const ndn::InterestFilter&, const ndn::Interest& interest) {
          if (!spool->index.count(interest.getName())) return;
          try {
            const auto wire = spool->packet(interest.getName());
            face.put(ndn::Data(ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size()))));
          }
          catch (...) { /* No replacement Data on a failed original-packet read. */ }
        });
      installed->set_value();
    }
    catch (...) { installed->set_exception(std::current_exception()); }
  });
  if (ready.wait_until(source->deadline) != std::future_status::ready)
    throw std::runtime_error("remote-packet-serving-install-timeout");
  ready.get();
  if (current) current();
  const auto reply = RepoClient::requestSelectedService(*m_writer,
    ndn::Name(m_options.target.providerIdentity),
    makeRepoServiceName(ndn::Name(m_options.target.servicePrefix), "INSERT"),
    encodeUserStorageInsertRequest(request), m_options.ackCollectionTimeMs, source->remaining());
  const auto status = parseOperationStatusJson(toString(reply));
  if (status.operationId != request.operationId || status.operation != "INSERT" ||
      ndn::Name(status.objectName) != manifestName || status.state != "DONE")
    throw std::runtime_error("remote-packet-insert-failed: " + status.message);
  source->committed = source->currentManifest(current);
  const auto& committed = source->committed;
  if (committed.sha256 != reference.expectedSha256 || committed.size != bytes ||
      committed.packetNames.size() != metadata.packetNames.size() ||
      committed.envelopeBytes != metadata.envelopeBytes || committed.generation != request.generation)
    throw std::runtime_error("remote-packet-manifest-readback-mismatch");
  for (std::size_t i = 0; i < metadata.packetNames.size(); ++i) {
    if (current) current();
    const ndn::Name name(metadata.packetNames[i]);
    if (ndn::Name(committed.packetNames[i]) != name)
      throw std::runtime_error("remote-packet-order-readback-mismatch");
    const auto expected = spool->packet(name);
    const auto actual = RepoClient::fetchOriginalDataPacketFromUserStorage(*m_writer,
      m_options.target, name, expected.size(), [certificate = m_certificate](const ndn::Data& packet) {
        return certificate.getValidityPeriod().isValid() && ndn::security::verifySignature(packet, certificate);
      }, source->remaining());
    if (actual != expected) throw std::runtime_error("remote-original-packet-readback-mismatch");
  }
  source->requireCurrentDurableStorageOwner(manifestName, grant.ownerIdentity);
  if (current) current();
  return source;
}
} // namespace ndnsf_distributed_repo

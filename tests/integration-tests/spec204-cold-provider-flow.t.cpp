#include "tests/boost-test.hpp"
#include "tests/fixtures/spec204/native-causal-recovery-model.hpp"
#include "tests/unit-tests/generic-dynamic-api-fixture.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedArtifactStore.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRunnerPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePortableKvCheckpoint.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndnsf-distributed-repo/RepoClient.hpp"
#include "ndnsf-distributed-repo/RepoNode.hpp"
#include "ndnsf-distributed-repo/RepoProtocol.hpp"
#include "ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp"
#include "ndnsf-distributed-repo/FilesystemArtifactStore.hpp"

#include <openssl/pem.h>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>
#include <array>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <thread>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di::detail {
void waitForRecoveryActivationUntil(
  std::uint64_t expiresAtMs,
  const std::function<std::uint64_t()>& now,
  const std::function<void()>& executionGuard,
  const std::function<bool()>& pollAndAccept);
} // namespace ndnsf::di::detail

namespace ndnsf::di {
struct Spec212PreparedModelTestAccess
{
  static PreparedModel bind(
    NativeRequestCatalog catalog,
    std::shared_ptr<const CooperativeModelSplitStrategy> splitter,
    std::shared_ptr<const CooperativePlacementStrategy> placement,
    std::shared_ptr<NativeInferenceClient> client,
    const std::string& service, const std::string& securityDigest,
    const std::string& tokenizerDigest)
  {
    auto package = std::make_shared<PreparedModelPackage>();
    package->catalog = std::move(catalog);
    package->catalog.cooperativeSplitter = std::move(splitter);
    package->defaultPlacement = std::move(placement);
    package->manifest.taskName = "task";
    package->capabilities.streaming = true;
    package->capabilities.conversations = true;
    auto registration = std::make_shared<FrozenPreparationRegistration>();
    registration->configurationJson = nativeCanonicalJson(NativeJson{
      {"request", {{"service", service},
                    {"security_policy_digest", securityDigest},
                    {"tokenizer_digest", tokenizerDigest},
                    {"generation_mode", "TOKEN_STREAMING"}}}});
    package->registration = std::move(registration);
    return PreparedModel(std::move(package), PreparationReceipt{}, {},
      [client = std::move(client)](const auto&) { return client; });
  }
};
} // namespace ndnsf::di

namespace ndnsf::di::tests {
namespace {

class CausalResponseDroppingUser : public ndn_service_framework::ServiceUser
{
public:
  CausalResponseDroppingUser(ndn::Face& face, const ndn::Name& groupPrefix,
                             const ndn::security::Certificate& userCert,
                             const ndn::security::Certificate& aaCert,
                             const std::string& trustSchemaPath)
    : ndn_service_framework::ServiceUser(LocalMockTag{}, face, groupPrefix,
                                        userCert, aaCert, trustSchemaPath)
  {
  }

  void setResponseDropPredicate(
    std::function<bool(const ndn::svs::SVSPubSub::SubscriptionData&)> predicate)
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_predicate = std::move(predicate);
  }

  void OnResponse(const ndn::svs::SVSPubSub::SubscriptionData& subscription) override
  {
    std::function<bool(const ndn::svs::SVSPubSub::SubscriptionData&)> predicate;
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      predicate = m_predicate;
    }
    if (predicate && predicate(subscription))
      return;
    ndn_service_framework::ServiceUser::OnResponse(subscription);
  }

private:
  std::mutex m_mutex;
  std::function<bool(const ndn::svs::SVSPubSub::SubscriptionData&)> m_predicate;
};

struct CausalWatchCleanup
{
  std::optional<NativeDepartureWatchHandle>& firstWatch;
  std::optional<NativeDepartureWatchHandle>& secondWatch;
  std::optional<Conversation>& firstConversation;
  std::optional<Conversation>& secondConversation;

  ~CausalWatchCleanup()
  {
    if (secondWatch) secondWatch->close();
    if (firstWatch) firstWatch->close();
    if (secondConversation) secondConversation->close();
    if (firstConversation) firstConversation->close();
  }
};
using namespace ndn_service_framework;
using namespace std::chrono_literals;

std::string lowerHex(std::string value)
{
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::uint64_t causalNow()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}

std::shared_ptr<EVP_PKEY> causalKey(unsigned char seed)
{
  std::array<unsigned char, 32> bytes{}; bytes.fill(seed);
  auto* key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, bytes.data(), bytes.size());
  if (!key) throw std::runtime_error("causal fixture cannot construct Ed25519 key");
  return {key, EVP_PKEY_free};
}

std::string causalPublicRaw(const std::shared_ptr<EVP_PKEY>& key)
{
  std::string bytes(32, '\0'); std::size_t length = bytes.size();
  if (EVP_PKEY_get_raw_public_key(key.get(), reinterpret_cast<unsigned char*>(bytes.data()), &length) != 1 || length != 32)
    throw std::runtime_error("causal fixture cannot read Ed25519 public key");
  return bytes;
}

std::string causalPublicPem(const std::shared_ptr<EVP_PKEY>& key)
{
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), key.get()) != 1)
    throw std::runtime_error("causal fixture cannot encode public key");
  char* bytes = nullptr; const auto size = BIO_get_mem_data(bio.get(), &bytes);
  if (size <= 0) throw std::runtime_error("causal fixture public key is empty");
  return {bytes, static_cast<std::size_t>(size)};
}

std::string causalSign(const std::shared_ptr<EVP_PKEY>& key, const std::string& digest)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  std::array<unsigned char, 64> signature{}; std::size_t size = signature.size();
  if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1 ||
      EVP_DigestSign(context.get(), signature.data(), &size,
        reinterpret_cast<const unsigned char*>(digest.data()), digest.size()) != 1 || size != 64)
    throw std::runtime_error("causal fixture cannot sign offer");
  std::array<unsigned char, 89> encoded{};
  if (EVP_EncodeBlock(encoded.data(), signature.data(), size) != 88)
    throw std::runtime_error("causal fixture cannot encode signature");
  return {reinterpret_cast<const char*>(encoded.data()), 88};
}

struct CausalFiles
{
  std::filesystem::path root;
  bool preserveRoot = false;
  CausalFiles()
  {
    char pattern[] = "/tmp/spec204-causal-flow-XXXXXX";
    const auto* directory = ::mkdtemp(pattern);
    if (!directory) throw std::runtime_error("cannot claim causal fixture directory");
    root = directory;
  }
  ~CausalFiles()
  {
    if (!preserveRoot) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  }
  void write(const std::string& path, const std::string& text) const
  {
    const auto destination = root / path;
    std::filesystem::create_directories(destination.parent_path());
    std::ofstream output(destination, std::ios::binary);
    output.write(text.data(), text.size()); output.close();
    if (!output || ::chmod(destination.c_str(), 0600) != 0)
      throw std::runtime_error("cannot write causal fixture operator file");
  }
  void privateKey(const std::string& name, const std::shared_ptr<EVP_PKEY>& key) const
  {
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || PEM_write_bio_PrivateKey(bio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1)
      throw std::runtime_error("cannot encode causal fixture recipient key");
    char* bytes = nullptr; const auto size = BIO_get_mem_data(bio.get(), &bytes);
    if (size <= 0) throw std::runtime_error("causal recipient key is empty");
    write(name, {bytes, static_cast<std::size_t>(size)});
  }
};

struct CausalRepoReopenEvidence
{
  std::filesystem::path root;
  std::string repoName;
  std::vector<std::pair<ndn::Name, std::vector<std::uint8_t>>> originalPackets;
  std::weak_ptr<ndn_service_framework::test::NdnsfIntegrationEnvironment> environmentLifetime;
  std::weak_ptr<ndnsf_distributed_repo::RepoNode> repoLifetime;
  std::weak_ptr<ndnsf_distributed_repo::RepoStoreBackend> backendLifetime;
  std::weak_ptr<ndn_service_framework::EncryptedLargeDataRangeStore> rangeStoreLifetime;

  ~CausalRepoReopenEvidence()
  {
    if (!root.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root, error);
    }
  }
};

void verifyCausalRepoReopen(const CausalRepoReopenEvidence& evidence)
{
  BOOST_REQUIRE(!evidence.root.empty());
  BOOST_REQUIRE(!evidence.repoName.empty());
  BOOST_REQUIRE(!evidence.originalPackets.empty());
  const bool environmentExpired = evidence.environmentLifetime.expired();
  const bool repoExpired = evidence.repoLifetime.expired();
  const bool backendExpired = evidence.backendLifetime.expired();
  const bool rangeStoreExpired = evidence.rangeStoreLifetime.expired();
  BOOST_TEST_MESSAGE("Spec204 Repo teardown before reopen: environmentOwners="
    << evidence.environmentLifetime.use_count() << ", repoOwners="
    << evidence.repoLifetime.use_count() << ", backendOwners="
    << evidence.backendLifetime.use_count() << ", rangeStoreOwners="
    << evidence.rangeStoreLifetime.use_count());
  BOOST_REQUIRE(environmentExpired && repoExpired && backendExpired && rangeStoreExpired);
  ndnsf_distributed_repo::StorageCapability capability;
  capability.repoNode = evidence.repoName;
  capability.repoMode = "persistent";
  capability.freeBytes = 16U << 20;
  auto reopened = std::make_shared<ndnsf_distributed_repo::RepoNode>(
    ndn::Name(evidence.repoName), capability,
    ndnsf_distributed_repo::makeFilesystemRepoStore(
      (evidence.root / "repo-objects").string(), 4U << 20, 4U << 20),
    evidence.repoName, evidence.repoName, (evidence.root / "repo.sqlite").string());
  for (const auto& packet : evidence.originalPackets) {
    BOOST_CHECK(reopened->core().getDataPacket(packet.first.toUri()) == packet.second);
  }
}

class CausalRepoCommitGate
{
public:
  void parkOnce()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_consumed)
      return;
    m_consumed = true;
    m_entered = true;
    m_changed.notify_all();
    m_changed.wait(lock, [&] { return m_released; });
  }

  bool waitUntilEntered(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [&] { return m_entered; });
  }

  void release()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_changed.notify_all();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_consumed = false;
  bool m_entered = false;
  bool m_released = false;
};

class CausalBoundaryGate
{
public:
  void parkOnce()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_consumed)
      return;
    m_consumed = true;
    m_entered = true;
    m_changed.notify_all();
    m_changed.wait(lock, [&] { return m_released; });
  }

  bool waitUntilEntered(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [&] { return m_entered; });
  }

  void release()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_changed.notify_all();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_consumed = false;
  bool m_entered = false;
  bool m_released = false;
};

struct CausalBoundaryRelease
{
  std::shared_ptr<CausalBoundaryGate> gate;
  ~CausalBoundaryRelease() { if (gate) gate->release(); }
};

class CountingOriginalPacketStore final : public EncryptedLargeDataRangeStore
{
public:
  explicit CountingOriginalPacketStore(std::shared_ptr<EncryptedLargeDataRangeStore> delegate)
    : m_delegate(std::move(delegate))
  {
    if (!m_delegate) throw std::invalid_argument("Spec204 packet store delegate is required");
  }

  bool requiresDurableStorageActor() const noexcept override
  {
    return m_delegate->requiresDurableStorageActor();
  }
  std::shared_ptr<const EncryptedLargeDataRangeSource> retainDurableSource(
    const EncryptedLargeDataLookupResult& candidate, const EncryptedLargeDataCommitOptions& options,
    const std::function<void()>& active) override
  {
    return m_delegate->retainDurableSource(candidate, options, active);
  }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string& name, const std::filesystem::path& path, std::uint64_t size,
    const std::function<void()>& active) override
  {
    return m_delegate->commitFile(name, path, size, active);
  }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string& name, const std::filesystem::path& path, std::uint64_t size,
    const EncryptedLargeDataCommitOptions& options, const std::function<void()>& active) override
  {
    return m_delegate->commitFile(name, path, size, options, active);
  }
  std::optional<EncryptedLargeDataLookupResult> lookupDurable(
    const std::string& identity, const std::function<void()>& active) const override
  {
    return m_delegate->lookupDurable(identity, active);
  }
  bool supportsDurableRetention() const noexcept override
  {
    return m_delegate->supportsDurableRetention();
  }
  bool supportsOriginalDataPackets() const noexcept override
  {
    return m_delegate->supportsOriginalDataPackets();
  }
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitOriginalDataPackets(
    const ndn::Name& name, const EncryptedLargeDataCommitOptions& options,
    const std::function<std::optional<std::pair<ndn::Name, std::vector<std::uint8_t>>>()>& next,
    const std::function<void()>& active) override
  {
    const auto commitCall = ++m_packetCommitCalls;
    if (commitCall == m_failCommitCall.load()) {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
      while (m_successfulPacketCommitCalls.load() < m_requiredSuccessfulCommits.load() &&
             std::chrono::steady_clock::now() < deadline) {
        if (active) active();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (m_successfulPacketCommitCalls.load() < m_requiredSuccessfulCommits.load())
        throw std::runtime_error("Spec212 first Repo packet commit did not finish before injected failure");
      ++m_injectedCommitFailures;
      throw std::runtime_error("Spec212 injected second Repo packet commit failure");
    }
    std::shared_ptr<CausalRepoCommitGate> gate;
    {
      std::lock_guard<std::mutex> lock(m_gateMutex);
      gate = std::move(m_commitGate);
    }
    const auto gatedNext = [this, gate, next] {
      if (gate)
        gate->parkOnce();
      auto packet = next();
      if (packet) {
        ++m_packetCount;
        m_packetWireBytes += packet->second.size();
      }
      return packet;
    };
    auto source = m_delegate->commitOriginalDataPackets(name, options, gatedNext, active);
    ++m_successfulPacketCommitCalls;
    return source;
  }
  bool supportsAuxiliaryDataPackets() const noexcept override
  {
    return m_delegate->supportsAuxiliaryDataPackets();
  }

  std::size_t packetCommitCalls() const noexcept { return m_packetCommitCalls.load(); }
  std::size_t packetCount() const noexcept { return m_packetCount.load(); }
  std::uint64_t packetWireBytes() const noexcept { return m_packetWireBytes.load(); }
  std::size_t successfulPacketCommitCalls() const noexcept
  { return m_successfulPacketCommitCalls.load(); }
  std::size_t injectedCommitFailures() const noexcept
  { return m_injectedCommitFailures.load(); }
  void failSecondFuturePacketCommit()
  {
    m_requiredSuccessfulCommits = m_successfulPacketCommitCalls.load() + 1;
    m_failCommitCall = m_packetCommitCalls.load() + 2;
  }
  void armCommitGate(std::shared_ptr<CausalRepoCommitGate> gate)
  {
    std::lock_guard<std::mutex> lock(m_gateMutex);
    m_commitGate = std::move(gate);
  }

private:
  std::shared_ptr<EncryptedLargeDataRangeStore> m_delegate;
  std::mutex m_gateMutex;
  std::shared_ptr<CausalRepoCommitGate> m_commitGate;
  std::atomic<std::size_t> m_packetCommitCalls{0};
  std::atomic<std::size_t> m_packetCount{0};
  std::atomic<std::uint64_t> m_packetWireBytes{0};
  std::atomic<std::size_t> m_successfulPacketCommitCalls{0};
  std::atomic<std::size_t> m_failCommitCall{0};
  std::atomic<std::size_t> m_requiredSuccessfulCommits{0};
  std::atomic<std::size_t> m_injectedCommitFailures{0};
};

// Observes actual ORT owners; only the independent baseline opts into retention.
class CausalRunnerFailureGate
{
public:
  void blockAndFailOnce()
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (m_consumed)
      return;
    m_consumed = true;
    m_entered = true;
    m_changed.notify_all();
    m_changed.wait(lock, [&] { return m_released; });
    throw std::runtime_error("Spec212 injected first destination runner failure");
  }

  bool waitUntilEntered(std::chrono::milliseconds timeout)
  {
    std::unique_lock<std::mutex> lock(m_mutex);
    return m_changed.wait_for(lock, timeout, [&] { return m_entered; });
  }

  void release()
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_released = true;
    }
    m_changed.notify_all();
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_changed;
  bool m_consumed = false;
  bool m_entered = false;
  bool m_released = false;
};

class CausalRunnerFactory final : public NativeModelRunnerFactory
{
public:
  explicit CausalRunnerFactory(std::shared_ptr<std::atomic<bool>> createdSignal = {},
                               std::shared_ptr<CausalRunnerFailureGate> failureGate = {})
    : m_createdSignal(std::move(createdSignal)),
      m_failureGate(std::move(failureGate))
  { registerOnnxRuntimeBackend(m_registry); m_registry.freeze(); }
  std::shared_ptr<NativeModelRunner> create(const NativeModelRunnerSpec& spec) const override
  {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_attemptedRoles.push_back(spec.role);
    }
    if (m_failureGate)
      m_failureGate->blockAndFailOnce();
    auto observedSpec = spec;
    if (spec.role == "/LLM/Pipeline/Stage/1")
      observedSpec.metadata["outputAlias.stage1__logits"] = "logits";
    auto runner = m_registry.create(observedSpec);
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_created.push_back(runner);
      m_createdRoles.push_back(spec.role);
      if (m_retainNext) { m_retained = runner; m_retainNext = false; }
    }
    if (m_createdSignal) m_createdSignal->store(true);
    return runner;
  }
  void retainNextRunner()
  {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_retainNext || m_retained) throw std::logic_error("baseline runner observation already armed");
    m_retainNext = true;
  }
  std::shared_ptr<NativeModelRunner> takeRetainedRunner()
  { std::lock_guard<std::mutex> lock(m_mutex); return std::move(m_retained); }
  std::vector<std::weak_ptr<NativeModelRunner>> created() const
  { std::lock_guard<std::mutex> lock(m_mutex); return m_created; }
  std::vector<std::string> attemptedRoles() const
  { std::lock_guard<std::mutex> lock(m_mutex); return m_attemptedRoles; }
  std::vector<std::string> createdRoles() const
  { std::lock_guard<std::mutex> lock(m_mutex); return m_createdRoles; }
private:
  RegistryNativeModelRunnerFactory m_registry;
  std::shared_ptr<std::atomic<bool>> m_createdSignal;
  std::shared_ptr<CausalRunnerFailureGate> m_failureGate;
  mutable std::mutex m_mutex;
  mutable std::vector<std::weak_ptr<NativeModelRunner>> m_created;
  mutable std::vector<std::string> m_attemptedRoles;
  mutable std::vector<std::string> m_createdRoles;
  mutable bool m_retainNext = false;
  mutable std::shared_ptr<NativeModelRunner> m_retained;
};

class CausalSplitter final : public NativeModelSplitStrategy
  , public CooperativeModelSplitStrategy
{
public:
  explicit CausalSplitter(NativeSplitCandidate candidate) : m_candidate(std::move(candidate)) {}
  NativeStrategyIdentity identity() const override { return m_candidate.splitter; }
  std::vector<NativeSplitCandidate> enumerate(const NativeModelDescriptor&, const NativeGraphSnapshot&,
    const NativeCandidateBudget&) const override { return {m_candidate}; }
  std::vector<NativeSplitCandidate> enumerate(
    const NativeModelDescriptor&, const NativeGraphSnapshot&,
    const NativeCandidateBudget&, const ExtensionControl& control) const override
  {
    control.requireActive();
    return {m_candidate};
  }
private:
  NativeSplitCandidate m_candidate;
};

class CausalWrongDestinationPlacement final : public NativePlacementStrategy
{
public:
  explicit CausalWrongDestinationPlacement(std::string provider)
    : m_provider(std::move(provider))
    , m_identity{"spec204-wrong-destination-placement", "1",
        nativePlanningDigest("Spec204 wrong destination placement fixture"), true}
  {
  }

  NativeStrategyIdentity identity() const override { return m_identity; }

  NativeRolePlacementProposalV3 proposeRoles(
    const NativeOfferBindingContext& context, const std::string& ackClosedDigest,
    const std::vector<NativeSelectionRoleV3>& roles,
    const std::vector<NativeAdmittedOfferV3>& offers, std::uint64_t nowMs) const override
  {
    NativePreSplitFirstPlacement fallback;
    auto proposal = fallback.proposeRoles(context, ackClosedDigest, roles, offers, nowMs);
    const auto target = std::find_if(offers.begin(), offers.end(), [this](const auto& offer) {
      return offer.observation().provider == m_provider;
    });
    if (target == offers.end() || proposal.providerByRole.size() != 1)
      throw std::runtime_error("Spec204 wrong-map fixture lacks its requested one-role Provider offer");
    const auto role = proposal.providerByRole.begin()->first;
    proposal.strategy = m_identity;
    proposal.providerByRole.clear();
    proposal.providerByRole.emplace(role, m_provider);
    proposal.offerDigestByProvider.clear();
    proposal.offerDigestByProvider.emplace(m_provider, target->observation().offerDigest);
    return proposal;
  }

private:
  std::string m_provider;
  NativeStrategyIdentity m_identity;
};

NativeOnnxWorkerLocation causalAssemblyWorker()
{
  const auto* configured = std::getenv("NDNSF_DI_WORKER_BINARY");
  const auto path = std::filesystem::absolute(configured && *configured
    ? std::filesystem::path(configured) : std::filesystem::path("build-spec204/DI_NativeOnnxAssemblyWorker"));
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("Spec204 requires the actual native ONNX assembly worker");
  const std::string bytes(std::istreambuf_iterator<char>(input), {});
  return {path.string(), nativePlanningDigest(bytes)};
}

NativeApplicationInput causalInput(const NativeModelDescriptor& model, const std::vector<std::int64_t>& tokens,
                                  const std::string& tokenizer, unsigned maxTokens)
{
  std::vector<std::uint8_t> bytes(tokens.size() * sizeof(std::int64_t));
  std::memcpy(bytes.data(), tokens.data(), bytes.size());
  NativeApplicationInput application;
  application.taskName = "task";
  application.inputSchemaDigest = model.adapter.inputSchemaDigest;
  application.optionsSchemaDigest = model.adapter.optionsSchemaDigest;
  application.payload = encodeTensorBundle({NamedTensor{"input_ids", TensorElementType::Int64,
    {1, static_cast<std::int64_t>(tokens.size())}, std::move(bytes)}});
  std::vector<std::string> stateInputNames{
    "attention_kv_in", "recurrent_state_in", "convolution_state_in"};
  std::vector<std::string> stateOutputNames{
    "attention_kv_out", "recurrent_state_out", "convolution_state_out"};
  if (model.modelName == "Spec212TwoRoleTinyCausal") {
    const auto firstStageInputs = stateInputNames;
    const auto firstStageOutputs = stateOutputNames;
    for (const auto& name : firstStageInputs) stateInputNames.push_back("stage1__" + name);
    for (const auto& name : firstStageOutputs) stateOutputNames.push_back("stage1__" + name);
  }
  std::string stateSuccessorMap;
  for (std::size_t index = 0; index < stateInputNames.size(); ++index) {
    if (!stateSuccessorMap.empty()) stateSuccessorMap += ',';
    stateSuccessorMap += stateInputNames[index] + "=" + stateOutputNames[index];
  }
  const auto options = nativeCanonicalJson(NativeJson{
    {"useCache", true}, {"outputMode", "TOKEN_STREAMING"}, {"maxNewTokens", maxTokens},
    {"eosTokenIds", {2}}, {"tokenizerDigest", tokenizer}, {"tokenInputName", "input_ids"},
    {"stateInputNames", stateInputNames}, {"stateOutputNames", stateOutputNames},
    {"stateSuccessorMap", stateSuccessorMap}, {"greedy", true}});
  application.options.assign(options.begin(), options.end());
  return application;
}

void causalWait(ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
                const NativeInferenceHandle& request,
                const std::function<void()>& failureDiagnostics = {})
{
  const auto deadline = std::chrono::steady_clock::now() + 35s;
  while (request.status() == NativeRequestStatus::Pending && std::chrono::steady_clock::now() < deadline)
    environment.pumpUntilWithAttributeAuthority([&] { return request.status() != NativeRequestStatus::Pending; });
  if (request.status() != NativeRequestStatus::Succeeded) {
    try { (void)request.result(0ms); }
    catch (const std::exception& error) { BOOST_TEST_MESSAGE("Spec204 actual causal request: " << error.what()); }
    if (failureDiagnostics) failureDiagnostics();
  }
  BOOST_REQUIRE(request.status() == NativeRequestStatus::Succeeded);
}
} // namespace

BOOST_AUTO_TEST_SUITE(Spec204ColdProviderFlow)

BOOST_AUTO_TEST_CASE(Spec212RoleBoundControlsDoNotPoisonReplayState)
{
  using Guard = NativeProviderControlSequenceGuard;
  for (bool reverse : {false, true}) {
    Guard guards[2];
    unsigned accepted[2] = {0, 0};
    std::vector<std::pair<unsigned, std::uint64_t>> controls{{0, 6}, {1, 7}};
    if (reverse) std::reverse(controls.begin(), controls.end());
    for (const auto& control : controls) {
      for (unsigned role = 0; role != 2; ++role) {
        const auto decision = guards[role].observeBound(control.second, control.first == role);
        if (control.first == role) {
          BOOST_CHECK(decision == Guard::Decision::Accepted);
          if (decision == Guard::Decision::Accepted) ++accepted[role];
        }
        else {
          BOOST_CHECK(decision == Guard::Decision::Unbound);
        }
      }
    }
    for (unsigned role = 0; role != 2; ++role) {
      BOOST_CHECK_EQUAL(accepted[role], 1U);
      const std::uint64_t own = 6 + role;
      BOOST_CHECK(guards[role].observeBound(99, false) == Guard::Decision::Unbound);
      BOOST_CHECK(guards[role].observeBound(own, true) == Guard::Decision::Duplicate);
      BOOST_CHECK(guards[role].observeBound(own - 1, true) == Guard::Decision::Reordered);
      BOOST_CHECK(guards[role].observeBound(own + 2, true) == Guard::Decision::Accepted);
      BOOST_CHECK_EQUAL(guards[role].duplicateCount(), 1U);
      BOOST_CHECK_EQUAL(guards[role].reorderedCount(), 1U);
    }
  }
}

BOOST_AUTO_TEST_CASE(RecoveryActivationWaitExpiresAtParentDeadline)
{
  std::uint64_t nowMs = 1000;
  unsigned guards = 0;
  unsigned polls = 0;
  bool timedOut = false;
  try {
    detail::waitForRecoveryActivationUntil(1100,
      [&] { return nowMs; },
      [&] { ++guards; },
      [&] {
        ++polls;
        nowMs += 20; // The production Collaboration wait is bounded to 20 ms.
        return false;
      });
  }
  catch (const std::runtime_error& error) {
    timedOut = std::string(error.what()) == "KV_RECOVERY_ACTIVATION_TIMEOUT";
  }
  BOOST_CHECK(timedOut);
  BOOST_CHECK_EQUAL(nowMs, 1100U);
  BOOST_CHECK_EQUAL(polls, 5U);
  BOOST_CHECK_EQUAL(guards, 6U);

  nowMs = 2000;
  guards = 0;
  polls = 0;
  BOOST_CHECK_NO_THROW(detail::waitForRecoveryActivationUntil(2050,
    [&] { return nowMs; },
    [&] { ++guards; },
    [&] {
      ++polls;
      nowMs += 20;
      return true;
    }));
  BOOST_CHECK_EQUAL(nowMs, 2020U);
  BOOST_CHECK_EQUAL(polls, 1U);
  BOOST_CHECK_EQUAL(guards, 1U);
}

enum class CausalRecoveryFault {
  None, CancelBeforeImport, CancelAfterRunnerPrepared, FailFirstDestinationAppend,
  SynchronousFirstAppendDispatchError, CloseAfterPromotionBeforeAppend,
  CloseAfterCommittedAppendBeforeCompletion, FailAfterCommittedAppend,
  CloseDuringRepoCommit,
  CloseDuringOwnerPacketSetBoundary, DestroyOwnerClientAtBoundary,
  ControlBoundary64, ControlBacklog,
  OwnerJournalQuotaFailure, OwnerCasFailure,
  ProtectedWireBound, RejectedPublicationGrant, WrongOwnerPublicationGrant,
  WrongWriterPublicationGrant, ChangedRootPublicationGrant, ExpiredPublicationGrant,
  CancelledPublicationAtGrant, RejectedPublicationCommitGate, DeleteOwnerStorage,
  EventCapacityCompetition, RoleCapacityCompetition,
  MutateOwnerPolicyAfterResolution, UnrelatedConversationAfterSwitch,
  FailSecondRepoPacketCommit, TwoSourceCancelBeforeAllReady,
  ControlProgressWhileRoleParked, TwoEventCapacityCompetition,
  TwoRoleCapacityCompetition, SwitchFirstWithdrawalOverlap,
  WithdrawalDrainTimeoutAtBoundary, WithdrawalAfterCommittedAppend,
  HostStopWithoutWithdrawal, NoIntentWatchExpiry, NoIntentTransportDisconnect,
  WithdrawalMissingRepoRole,
  WithdrawalPublicationObserverFailure,
  PostCommitSecondChildDrainTimeout
};

CausalRepoReopenEvidence runActualCausalColdProvider(CausalRecoveryFault fault,
                                                      bool productionHandoff = false,
                                                      bool twoRole = false,
                                                      bool twoSource = false,
                                                      bool withdrawalAbort = false,
                                                      bool withdrawalSave = false,
                                                      bool withdrawalWatch = false,
                                                      bool withdrawalWatchNoIntent = false,
                                                      bool withdrawalBudget = false,
                                                      bool withdrawalForce = false,
                                                      bool withdrawalForceBudget = false,
                                                      bool providerScopeWithdrawal = false,
                                                      bool withdrawalWireBudget = false,
                                                      bool partialHostWithdrawal = false,
                                                      bool withdrawalLostConfirm = false,
                                                      bool withdrawalOwnerUnavailable = false,
                                                      bool twoNonemptyServiceWithdrawal = false,
                                                      bool postCommitChildFailure = false,
                                                      bool multipleOwnerWithdrawal = false,
                                                      bool multiOwnerLostConfirm = false,
                                                      bool withdrawalOverlapSwitch = false,
                                                      bool verifyMixedUriClaimManifest = false,
                                                      bool appendAfterWithdrawal = false)
{
  const std::string secondService = "/Spec212/B-Nonempty";
  const std::string mixedUriRepoName = "/spec212/mixed-uri/repo";
  BOOST_REQUIRE(!appendAfterWithdrawal ||
    (productionHandoff && withdrawalWatch && !twoRole && !twoSource &&
     !twoNonemptyServiceWithdrawal && fault == CausalRecoveryFault::None));
  const bool noIntentExpires = fault == CausalRecoveryFault::NoIntentWatchExpiry ||
    fault == CausalRecoveryFault::NoIntentTransportDisconnect;
  BOOST_REQUIRE(!postCommitChildFailure || twoNonemptyServiceWithdrawal);
  BOOST_REQUIRE(!multipleOwnerWithdrawal ||
    (twoNonemptyServiceWithdrawal && !postCommitChildFailure));
  BOOST_REQUIRE(!multiOwnerLostConfirm || multipleOwnerWithdrawal);
  BOOST_REQUIRE(!withdrawalOverlapSwitch ||
    (withdrawalWatch && productionHandoff && !providerScopeWithdrawal &&
     !twoNonemptyServiceWithdrawal && fault == CausalRecoveryFault::None));
  BOOST_REQUIRE(!verifyMixedUriClaimManifest ||
    (fault == CausalRecoveryFault::None && !productionHandoff));
  BOOST_REQUIRE(!twoNonemptyServiceWithdrawal ||
    (providerScopeWithdrawal && withdrawalWatch && productionHandoff &&
     !twoRole && !twoSource &&
     (fault == CausalRecoveryFault::None ||
      fault == CausalRecoveryFault::PostCommitSecondChildDrainTimeout)));
  BOOST_REQUIRE(!partialHostWithdrawal ||
    (providerScopeWithdrawal && withdrawalWireBudget));
  BOOST_REQUIRE(!withdrawalLostConfirm ||
    (withdrawalSave && !providerScopeWithdrawal));
  BOOST_REQUIRE(!withdrawalOwnerUnavailable ||
    (withdrawalAbort && !providerScopeWithdrawal));
  BOOST_REQUIRE(!providerScopeWithdrawal ||
    (productionHandoff && (withdrawalAbort || withdrawalSave ||
                           withdrawalBudget || withdrawalForceBudget ||
                           withdrawalWireBudget || withdrawalWatch)));
  BOOST_REQUIRE(!twoSource || (productionHandoff && twoRole));
  BOOST_REQUIRE(static_cast<unsigned>(withdrawalAbort) +
                static_cast<unsigned>(withdrawalSave) +
                static_cast<unsigned>(withdrawalWatch) +
                static_cast<unsigned>(withdrawalWatchNoIntent) +
                static_cast<unsigned>(withdrawalBudget) +
                static_cast<unsigned>(withdrawalForce) +
                static_cast<unsigned>(withdrawalForceBudget) +
                static_cast<unsigned>(withdrawalWireBudget) <= 1U);
  BOOST_REQUIRE(!(withdrawalAbort || withdrawalSave || withdrawalWatch ||
                  withdrawalWatchNoIntent || withdrawalBudget || withdrawalForce ||
                  withdrawalForceBudget || withdrawalWireBudget) ||
    (productionHandoff && (!twoRole ||
       (withdrawalWatch && fault == CausalRecoveryFault::WithdrawalMissingRepoRole)) &&
     !twoSource &&
     (fault == CausalRecoveryFault::None ||
      (withdrawalWatch && fault == CausalRecoveryFault::WithdrawalDrainTimeoutAtBoundary) ||
      (withdrawalWatch && fault == CausalRecoveryFault::WithdrawalAfterCommittedAppend) ||
      (withdrawalWatch && fault == CausalRecoveryFault::WithdrawalMissingRepoRole) ||
      (withdrawalWatch && fault == CausalRecoveryFault::WithdrawalPublicationObserverFailure) ||
      (withdrawalWatch && fault == CausalRecoveryFault::PostCommitSecondChildDrainTimeout) ||
      (withdrawalWatchNoIntent && fault == CausalRecoveryFault::HostStopWithoutWithdrawal) ||
      (withdrawalWatchNoIntent && noIntentExpires) ||
      (withdrawalWatch && fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch))));
  const bool twoConversationWithdrawalWatch = withdrawalWatch &&
    fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch;
  const std::size_t destinationIndex = twoSource ? 2U : 1U;
  std::vector<std::string> sourceRoles{"/LLM/Pipeline/Stage/0"};
  if (twoRole)
    sourceRoles.emplace_back("/LLM/Pipeline/Stage/1");
  const bool capacityCompetition =
    fault == CausalRecoveryFault::EventCapacityCompetition ||
    fault == CausalRecoveryFault::RoleCapacityCompetition ||
    fault == CausalRecoveryFault::ControlProgressWhileRoleParked ||
    fault == CausalRecoveryFault::TwoEventCapacityCompetition ||
    fault == CausalRecoveryFault::TwoRoleCapacityCompetition ||
    fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch;
  const bool capacityBoundaryCompetition =
    fault == CausalRecoveryFault::EventCapacityCompetition ||
    fault == CausalRecoveryFault::RoleCapacityCompetition ||
    fault == CausalRecoveryFault::ControlProgressWhileRoleParked ||
    fault == CausalRecoveryFault::TwoEventCapacityCompetition ||
    fault == CausalRecoveryFault::TwoRoleCapacityCompetition;
  const bool controlProgressWhileRoleParked =
    fault == CausalRecoveryFault::ControlProgressWhileRoleParked ||
    fault == CausalRecoveryFault::TwoEventCapacityCompetition ||
    fault == CausalRecoveryFault::TwoRoleCapacityCompetition;
  const bool twoEventCapacityCompetition =
    fault == CausalRecoveryFault::TwoEventCapacityCompetition;
  const bool twoRoleCapacityCompetition =
    fault == CausalRecoveryFault::TwoRoleCapacityCompetition;
  const bool twoSlotCapacityCompetition =
    twoEventCapacityCompetition || twoRoleCapacityCompetition;
  BOOST_REQUIRE(!twoSlotCapacityCompetition || (productionHandoff && twoRole && twoSource));
  const std::size_t preparedConversationCount =
    twoSlotCapacityCompetition ? 3U : (capacityCompetition ? 2U : 1U);
  const bool eventCapacityCompetition =
    fault == CausalRecoveryFault::EventCapacityCompetition;
  const bool roleCapacityCompetition =
    fault == CausalRecoveryFault::RoleCapacityCompetition;
  const bool closeDuringOwnerPacketSetBoundary =
    fault == CausalRecoveryFault::CloseDuringOwnerPacketSetBoundary;
  const bool switchFirstWithdrawalOverlap =
    fault == CausalRecoveryFault::SwitchFirstWithdrawalOverlap;
  const bool withdrawalDrainTimeout =
    fault == CausalRecoveryFault::WithdrawalDrainTimeoutAtBoundary;
  const bool withdrawalAfterCommittedAppend =
    fault == CausalRecoveryFault::WithdrawalAfterCommittedAppend;
  const bool postCommitSecondChildDrainTimeout =
    fault == CausalRecoveryFault::PostCommitSecondChildDrainTimeout;
  const bool destroyOwnerClientAtBoundary =
    fault == CausalRecoveryFault::DestroyOwnerClientAtBoundary;
  const bool ownerCasFailure = fault == CausalRecoveryFault::OwnerCasFailure;
  const bool mutateOwnerPolicyAfterResolution =
    fault == CausalRecoveryFault::MutateOwnerPolicyAfterResolution;
  const bool deletesOwnerStorage = fault == CausalRecoveryFault::DeleteOwnerStorage;
  const bool validatesRemoteStorage = fault == CausalRecoveryFault::None || deletesOwnerStorage;
  CausalFiles files;
  const auto spool = (files.root / "spool").string();
  const auto keyReferences = (files.root / "key-references").string();
  ndn_service_framework::test::ScopedEnvironmentValue spoolSetting("NDNSF_REQUEST_LARGE_DATA_DIR", spool.c_str());
  ndn_service_framework::test::ScopedEnvironmentValue referenceSetting("NDNSF_DURABLE_KEY_REFERENCE_DIR", keyReferences.c_str());
  ndn_service_framework::test::BootstrapProfile profile;
  profile.serviceName = ndn::Name("/Spec204/ActualCausal");
  profile.additionalServices.emplace_back("/spec204/causal/repo/USER_STORAGE_GRANT");
  profile.additionalServices.emplace_back("/spec204/causal/repo/USER_STORAGE_LIST");
  profile.additionalServices.emplace_back("/spec204/causal/repo/MANIFEST");
  if (verifyMixedUriClaimManifest) {
    profile.additionalServices.emplace_back(mixedUriRepoName + "/USER_STORAGE_GRANT");
    profile.additionalServices.emplace_back(mixedUriRepoName + "/USER_STORAGE_LIST");
    profile.additionalServices.emplace_back(mixedUriRepoName + "/MANIFEST");
  }
  if (twoNonemptyServiceWithdrawal) {
    profile.additionalServices.emplace_back(secondService);
    for (const auto& role : sourceRoles)
      profile.providerAdditionalServices.push_back(
        ndn::Name(secondService).append("ROLE").append(ndn::Name(role)));
  }
  if (deletesOwnerStorage)
    profile.additionalServices.emplace_back("/spec204/causal/repo/USER_STORAGE_DELETE");
  profile.deferBridgeDelivery = true;
  profile.providerFacesUseIndependentIoContexts = true;
  profile.validateSvsPackets = true;
  profile.providerCount = twoSource || multipleOwnerWithdrawal ? 3 : 2;
  profile.providerCanActAsUser = true;
  if (productionHandoff)
    profile.providerRoles = sourceRoles;
  auto environment = std::make_shared<ndn_service_framework::test::NdnsfIntegrationEnvironment>(profile);
  environment->bootstrap();
  std::optional<ndn_service_framework::PolicyStatusData> secondServiceControllerStatus;
  if (productionHandoff) {
    const auto now = causalNow();
    ndn_service_framework::PolicyStatusData status;
    status.setServiceName(profile.serviceName);
    status.setControllerVersion(ndn_service_framework::ControllerVersion{now, 1});
    status.setValidity(now - 1000, now + 600000);
    status.setPolicyDigest("sha256:" + std::string(64, '0'));
    status.setControllerCertificate(ndn::Name("/controller/spec212/causal"));
    status.setAbePublicParametersName(environment->attributeAuthorityPublicParametersName());
    status.setAbePublicParametersDigest(environment->attributeAuthorityPublicParametersDigest());
    BOOST_REQUIRE(environment->user().installControllerStatus(status));
    for (size_t index = 0; index < environment->providerCount(); ++index)
      BOOST_REQUIRE(environment->provider(index).installControllerStatus(status));
    if (twoNonemptyServiceWithdrawal) {
      auto secondStatus = status;
      secondStatus.setServiceName(ndn::Name(secondService));
      secondServiceControllerStatus = secondStatus;
      BOOST_REQUIRE(environment->user().installControllerStatus(secondStatus));
      for (size_t index = 0; index < environment->providerCount(); ++index)
        BOOST_REQUIRE(environment->provider(index).installControllerStatus(secondStatus));
    }
    environment->pumpUntilWithAttributeAuthority([] { return false; });
  }
  // Both the source and destination must run all selected role handlers while
  // a role waits for cross-role Data or the all-role recovery READY barrier.
  if (twoRole) {
    for (size_t index = 0; index < environment->providerCount(); ++index)
      environment->provider(index).setHandlerThreads(sourceRoles.size());
  }
  const auto service = profile.serviceName.toUri();
  const auto requester = environment->user().getName().toUri();
  const auto provider = environment->provider().getName().toUri();
  const auto secondOwner = multipleOwnerWithdrawal
    ? environment->provider(2).getName().toUri() : requester;
  const auto boot = environment->provider().getProviderBootEpoch();
  const auto secondSource = twoSource ? environment->provider(1).getName().toUri()
                                      : std::string{};
  const auto successor = environment->provider(destinationIndex).getName().toUri();
  auto observedResponseNames = std::make_shared<std::vector<std::string>>();
  auto observedResponseNamesMutex = std::make_shared<std::mutex>();
  auto providerFaceResponseNames = std::make_shared<std::vector<std::string>>();
  auto providerFaceResponseNamesMutex = std::make_shared<std::mutex>();
  auto providerFaceSyncDataNames = std::make_shared<std::vector<std::string>>();
  auto providerFaceSyncDataNamesMutex = std::make_shared<std::mutex>();
  auto providerFacePublishedPacketNames = std::make_shared<std::vector<std::string>>();
  auto providerFacePublishedPacketNamesMutex = std::make_shared<std::mutex>();
  auto providerFaceStreamEventNames = std::make_shared<std::vector<std::string>>();
  auto providerFaceStreamEventNamesMutex = std::make_shared<std::mutex>();
  std::vector<ndn::signal::ScopedConnection> providerFaceResponseObservers;
  providerFaceResponseObservers.reserve(environment->providerCount());
  auto providerFaceSyncInterestNames = std::make_shared<std::vector<std::string>>();
  auto providerFaceSyncInterestNamesMutex = std::make_shared<std::mutex>();
  std::vector<ndn::signal::ScopedConnection> providerFaceSyncInterestObservers;
  providerFaceSyncInterestObservers.reserve(environment->providerCount());
  for (size_t index = 0; index < environment->providerCount(); ++index) {
    auto* face = &environment->providerFace(index);
    providerFaceResponseObservers.emplace_back(face->onSendData.connect(
      [providerFaceResponseNames, providerFaceResponseNamesMutex](const ndn::Data& data) {
        if (!parseResponseNameV2(data.getName())) return;
        std::lock_guard<std::mutex> lock(*providerFaceResponseNamesMutex);
        providerFaceResponseNames->push_back(data.getName().toUri());
      }));
    providerFaceResponseObservers.emplace_back(face->onSendData.connect(
      [providerFaceSyncDataNames, providerFaceSyncDataNamesMutex,
       syncPrefix = profile.syncPrefix](const ndn::Data& data) {
        if (!syncPrefix.isPrefixOf(data.getName())) return;
        std::lock_guard<std::mutex> lock(*providerFaceSyncDataNamesMutex);
        providerFaceSyncDataNames->push_back(data.getName().toUri());
      }));
    providerFaceResponseObservers.emplace_back(face->onSendData.connect(
      [providerFacePublishedPacketNames, providerFacePublishedPacketNamesMutex,
       producerPrefix = profile.providerNode](const ndn::Data& data) {
        if (!producerPrefix.isPrefixOf(data.getName())) return;
        std::lock_guard<std::mutex> lock(*providerFacePublishedPacketNamesMutex);
        providerFacePublishedPacketNames->push_back(data.getName().toUri());
      }));
    providerFaceResponseObservers.emplace_back(face->onSendData.connect(
      [providerFaceStreamEventNames, providerFaceStreamEventNamesMutex](const ndn::Data& data) {
        const auto name = data.getName().toUri();
        if (name.find("/NDNSF/event/") == std::string::npos) return;
        std::lock_guard<std::mutex> lock(*providerFaceStreamEventNamesMutex);
        providerFaceStreamEventNames->push_back(name);
      }));
    providerFaceSyncInterestObservers.emplace_back(face->onSendInterest.connect(
      [providerFaceSyncInterestNames, providerFaceSyncInterestNamesMutex,
       syncPrefix = profile.syncPrefix](const ndn::Interest& interest) {
        if (!syncPrefix.isPrefixOf(interest.getName())) return;
        std::lock_guard<std::mutex> lock(*providerFaceSyncInterestNamesMutex);
        providerFaceSyncInterestNames->push_back(interest.getName().toUri());
      }));
  }
  environment->userPubSub().subscribeToProducer(
    profile.providerNode,
    [observedResponseNames, observedResponseNamesMutex](
        const ndn::svs::SVSPubSub::SubscriptionData& publication) {
      std::lock_guard<std::mutex> lock(*observedResponseNamesMutex);
      observedResponseNames->push_back(publication.name.toUri());
    });
  auto userResponseInterestNames = std::make_shared<std::vector<std::string>>();
  auto userResponseInterestNamesMutex = std::make_shared<std::mutex>();
  auto userProviderPacketInterestNames = std::make_shared<std::vector<std::string>>();
  auto userProviderPacketInterestNamesMutex = std::make_shared<std::mutex>();
  auto userStreamInterestNames = std::make_shared<std::vector<std::string>>();
  auto userStreamInterestNamesMutex = std::make_shared<std::mutex>();
  ndn::signal::ScopedConnection userStreamInterestObserver =
    environment->userFace().onSendInterest.connect(
      [userStreamInterestNames, userStreamInterestNamesMutex](const ndn::Interest& interest) {
        const auto name = interest.getName().toUri();
        if (name.find("/NDNSF/event/") == std::string::npos) return;
        std::lock_guard<std::mutex> lock(*userStreamInterestNamesMutex);
        userStreamInterestNames->push_back(name);
      });
  ndn::signal::ScopedConnection userProviderPacketInterestObserver =
    environment->userFace().onSendInterest.connect(
      [userProviderPacketInterestNames, userProviderPacketInterestNamesMutex,
       producerPrefix = profile.providerNode](const ndn::Interest& interest) {
        if (!producerPrefix.isPrefixOf(interest.getName())) return;
        std::lock_guard<std::mutex> lock(*userProviderPacketInterestNamesMutex);
        userProviderPacketInterestNames->push_back(interest.getName().toUri());
      });
  const auto providerResponsePrefix = ndn::Name(environment->provider().getName())
    .append("NDNSF").append("RESPONSE");
  ndn::signal::ScopedConnection userResponseInterestObserver =
    environment->userFace().onSendInterest.connect(
      [userResponseInterestNames, userResponseInterestNamesMutex,
       providerResponsePrefix](const ndn::Interest& interest) {
        if (!providerResponsePrefix.isPrefixOf(interest.getName())) return;
        std::lock_guard<std::mutex> lock(*userResponseInterestNamesMutex);
        userResponseInterestNames->push_back(interest.getName().toUri());
      });
  const auto now = causalNow();
  const std::string epoch = "spec204-causal-protected-v1";
  const std::string tokenizer = "sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6";
  const auto policyDigest = nativePlanningDigest("Spec204-actual-causal-policy");
  NativeRequestControl control{"spec204-source-inspection", 1, std::chrono::steady_clock::now() + 30s,
    [] { return false; }};
  ndnsf::di::test::spec204::NativeCausalRecoveryModel source(
    requester, epoch, control, twoRole);
  auto candidate = source.candidate;
  candidate.executionPlan.serviceName = service;
  candidate.candidateDigest = candidate.computedDigest();
  const auto roleOwnsTokenInput = [&](const std::string& roleName) {
    const auto roleContract = std::find_if(source.roles.begin(), source.roles.end(),
      [&](const auto& role) { return role.role == roleName; });
    BOOST_REQUIRE(roleContract != source.roles.end());
    return std::any_of(roleContract->expectedInputs.begin(), roleContract->expectedInputs.end(),
      [](const auto& tensor) {
        return tensor.name == "input_ids" && tensor.dtype == "int64" &&
          (tensor.shape.size() == 1 || tensor.shape.size() == 2);
      });
  };

  const auto offerKey = causalKey(0x31), authorityKey = causalKey(0x51), requesterKey = causalKey(0x41);
  const auto recipientKey = causalKey(0x61);
  const auto successorRecipientKey = causalKey(0x62);
  const auto secondSourceRecipientKey = causalKey(0x63);
  const auto offerKeyId = nativePlanningDigest(causalPublicRaw(offerKey));
  const auto trustedOfferEntry = [&](std::size_t index) {
    return NativeJson{{"provider", environment->provider(index).getName().toUri()},
      {"service", service},
      {"keyLocatorPrefix", environment->provider(index).getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment->provider(index).getSigningCertificateName().toUri()}};
  };
  NativeJson admissionEntries = NativeJson::array();
  admissionEntries.push_back(trustedOfferEntry(0));
  if (twoSource)
    admissionEntries.push_back(trustedOfferEntry(1));
  admissionEntries.push_back(trustedOfferEntry(destinationIndex));
  const auto admissionPolicy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"}, {"candidateId", "spec204-causal"},
    {"candidateDigest", policyDigest}, {"trustSchema", profile.groupPrefix.toUri() + "/trust"},
    {"entries", std::move(admissionEntries)}});
  auto admission = std::make_shared<NativeOfferAdmission>(admissionPolicy,
    std::map<std::string, std::string>{{offerKeyId, causalPublicPem(offerKey)}}, policyDigest);
  NativeProviderOfferV3Config offer;
  offer.provider = provider; offer.service = service; offer.bootEpoch = boot;
  offer.signerKeyId = offerKeyId; offer.acceptedRoles = sourceRoles;
  offer.backends = {"onnxruntime-cpu"}; offer.hasModel = false;
  offer.signDigest = [offerKey](const auto& digest) { return causalSign(offerKey, digest); };

  const auto authority = profile.attributeAuthority.toUri();
  NativeGrantIssuerConfig issuer;
  issuer.authorityIdentity = authority; issuer.requesterIdentity = requester;
  issuer.protectionEpoch = epoch; issuer.keyId = "spec204-causal-grant";
  issuer.authorityPrivateKey = authorityKey; issuer.requesterPublicKey = requesterKey;
  issuer.allowedModelManifests = {source.model.modelManifestDigest};
  issuer.publicationSources.emplace(source.model.modelManifestDigest, NativeGrantPublicationSource{
    source.model.descriptor.modelName, source.model.descriptor.contentDigest, source.model.canonicalSourceDigest,
    source.model.canonicalInitializerObjectDigest, source.roles.front().artifactProfileDigest});
  issuer.recipientPublicKeys = {{provider, recipientKey}, {successor, successorRecipientKey}};
  if (twoSource)
    issuer.recipientPublicKeys.emplace(secondSource, secondSourceRecipientKey);
  issuer.contentKey = [](const auto&, const auto&) { return std::vector<std::uint8_t>(32, 0x77); };
  const auto secondRequesterKey = causalKey(0x43);
  std::optional<NativeGrantIssuerConfig> secondIssuerConfig;
  if (multipleOwnerWithdrawal) {
    secondIssuerConfig = issuer;
    secondIssuerConfig->requesterIdentity = secondOwner;
    secondIssuerConfig->requesterPublicKey = secondRequesterKey;
  }
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuer));
  auto grantMutex = std::make_shared<std::mutex>();
  auto grantWires = std::make_shared<std::map<ndn::Name, std::string>>();
  // Exact grant transport is an explicit fixture port. Production grant signature,
  // current request/recipient bindings, unwrap and protected execution remain real.
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(requester, requesterKey, authority,
    causalPublicRaw(authorityKey), epoch,
    [grantIssuer](const auto& request, const auto& manifest, auto expires, const auto& grantControl) {
      grantControl.check(); return grantIssuer->issue(request, causalNow(), expires, manifest);
    },
    [grantWires, grantMutex](const auto& name, const auto& wire, const auto& grantControl) {
      grantControl.check(); std::lock_guard<std::mutex> lock(*grantMutex);
      (*grantWires)[ndn::Name(name)] = wire; return name;
    });
  std::shared_ptr<NativeAuthenticatedGrantClient> secondOwnerGrants;
  if (secondIssuerConfig) {
    auto secondGrantIssuer = std::make_shared<NativeArtifactGrantIssuer>(
      std::move(*secondIssuerConfig));
    secondOwnerGrants = std::make_shared<NativeAuthenticatedGrantClient>(
      secondOwner, secondRequesterKey, authority, causalPublicRaw(authorityKey), epoch,
      [secondGrantIssuer](const auto& request, const auto& manifest,
                          auto expires, const auto& grantControl) {
        grantControl.check();
        return secondGrantIssuer->issue(request, causalNow(), expires, manifest);
      },
      [grantWires, grantMutex](const auto& name, const auto& wire,
                              const auto& grantControl) {
        grantControl.check();
        std::lock_guard<std::mutex> lock(*grantMutex);
        (*grantWires)[ndn::Name(name)] = wire;
        return name;
      });
  }
  const auto authorityPem = causalPublicPem(authorityKey);
  files.write("authority.pub", authorityPem); files.privateKey("recipient.pem", recipientKey);
  files.privateKey("successor-recipient.pem", successorRecipientKey);
  NativeJson recipientFiles{
    {provider, (files.root / "recipient.pem").string()},
    {successor, (files.root / "successor-recipient.pem").string()}};
  if (twoSource) {
    files.privateKey("second-source-recipient.pem", secondSourceRecipientKey);
    recipientFiles[secondSource] = (files.root / "second-source-recipient.pem").string();
  }
  files.write("recipients.json", nativeCanonicalJson(recipientFiles));
  files.write("trust/trust-root-registry-v1.json", nativeCanonicalJson(NativeJson{
    {"schemaVersion", 1}, {"status", "CONFIGURED"}, {"artifactPolicyAuthority", {
      {"publicKeyAlgorithm", "ed25519"}, {"signatureAlgorithm", "ed25519"},
      {"grantSchema", "ndnsf-di-key-grant-v1"}, {"publicKeyPath", "authority.pub"},
      {"publicKeySha256", nativePlanningDigest(authorityPem)}, {"authorityId", authority},
      {"keyId", authority + "/KEY/1"}, {"acceptedModelFamilies", {source.model.descriptor.modelName}},
      {"protectionEpochs", {epoch}}}}}));
  const auto authorityHint = (files.root / "trust/authority.pub").string();
  const auto recipientMap = (files.root / "recipients.json").string();
  ndn_service_framework::test::ScopedEnvironmentValue authoritySetting("SPEC181_GRANT_AUTHORITY_PUBLIC_KEY", authorityHint.c_str());
  ndn_service_framework::test::ScopedEnvironmentValue recipientSetting("SPEC181_PROVIDER_RECIPIENT_KEY_MAP", recipientMap.c_str());

  auto factory = std::make_shared<CausalRunnerFactory>();
  auto successorRunnerPrepared = std::make_shared<std::atomic<bool>>(false);
  auto successorRunnerFailureGate = fault == CausalRecoveryFault::FailFirstDestinationAppend
    ? std::make_shared<CausalRunnerFailureGate>()
    : std::shared_ptr<CausalRunnerFailureGate>{};
  auto successorFactory = std::make_shared<CausalRunnerFactory>(
    fault == CausalRecoveryFault::CancelAfterRunnerPrepared
      ? successorRunnerPrepared : std::shared_ptr<std::atomic<bool>>{},
    successorRunnerFailureGate);
  const auto worker = causalAssemblyWorker();
  auto nativeFailures = std::make_shared<std::vector<NativeJson>>();
  auto nativeFailuresMutex = std::make_shared<std::mutex>();
  const auto makeConfig = [&](std::size_t index, const std::shared_ptr<CausalRunnerFactory>& runnerFactory) {
  const auto localProvider = environment->provider(index).getName().toUri();
  const auto localBoot = environment->provider(index).getProviderBootEpoch();
  NativeProviderHandlerConfig config;
  config.plan = candidate.executionPlan; config.runnerFactory = runnerFactory;
  config.localProviderName = localProvider; config.providerBootId = localBoot;
  config.nativeFailureObserver = std::make_shared<NativeProviderHandlerConfig::NativeFailureObserver>(
    [nativeFailures, nativeFailuresMutex, localProvider](const auto& role, const auto& reason) {
      std::lock_guard<std::mutex> lock(*nativeFailuresMutex);
      nativeFailures->push_back(NativeJson{
        {"provider", localProvider}, {"role", role}, {"reason", reason}});
    });
  // Cross-role decode dependencies can wait while another role publishes its
  // feedback, so the shared native runtime needs one worker per source role.
  config.workerCount = sourceRoles.size();
  config.fetchTimeoutMs = 10000; config.dependencyFetchTimeoutMs = 10000;
  config.enableNativeEpochCoordinator = true;
  config.requireGenerationTextOutput = true;
  config.generationDecodersFactory = [](const std::string& digest) {
    return makeNativeStandaloneTokenizerDecoders(
      {std::filesystem::absolute("tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json").string()}, digest);
  };
  config.protectedGrantFetcher = [grantWires, grantMutex](const auto& name, int, const auto& cancelled) {
    if (cancelled && cancelled()) throw std::runtime_error("causal grant fetch cancelled");
    std::lock_guard<std::mutex> lock(*grantMutex);
    return grantWires->at(ndn::Name(name));
  };
  config.runnerPreparationFactory = [root = files.root, provider = localProvider, boot = localBoot, now, worker, index,
    fixtureEnvironment = environment.get(), signingCertificate = environment->provider(index).getSigningCertificateName()](
    auto& ctx, const auto& projection, const auto& protectedRuntime) {
    NativeCanonicalOnnxAssemblerOptions assembly;
    assembly.cacheDir = (root / ("assembled-" + std::to_string(index))).string(); assembly.providerIdentity = provider;
    assembly.protectedRuntime = protectedRuntime; assembly.workerLocation = worker;
    assembly.roleAssemblySpecDigest = nativeAssemblyDigestFromValidatedProjection(projection);
    assembly.shouldCancel = [&ctx] { return ctx.isStreamed() && ctx.streamCancelled(); };
    assembly.signManifest = [fixtureEnvironment, signingCertificate, provider](const std::string& manifest) {
      ndn::Data packet(ndn::Name(provider).append("NDNSF").append("ASSEMBLY-MANIFEST"));
      packet.setContent(ndn::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(manifest.data()), manifest.size()));
      fixtureEnvironment->keyChain().sign(packet, ndn::security::signingByCertificate(signingCertificate));
      const auto& signature = packet.getSignatureValue();
      return selectionGatedHex(ndn::span<const std::uint8_t>(signature.value_begin(), signature.value_size()));
    };
    auto spec = prepareNativeCanonicalOnnxRole(ctx, projection, assembly);
    bindNativeRunnerPreparationContext(spec, projection, {provider, boot, now, assembly.cacheDir});
    return spec;
  };
  installNativeProtectedGrantFactory(config);
  installNativeProviderGroupCoordinatorFactory(config, localProvider,
    environment->provider(index).getSigningCertificateName().toUri(), &environment->keyChain());
  return config;
  };
  const auto installHandler = [&](std::size_t index, NativeProviderCollaborationRuntime& handlerRuntime,
                                  std::shared_ptr<std::atomic<unsigned>> returned = {},
                                  std::shared_ptr<std::optional<ServiceProvider::CollaborationContext>>
                                    observedContext = {}) {
    auto localOffer = offer;
    localOffer.provider = environment->provider(index).getName().toUri();
    localOffer.bootEpoch = environment->provider(index).getProviderBootEpoch();
    return environment->provider(index).addScopedCollaborationHandler(profile.serviceName, {},
    [offer = std::move(localOffer)](const RequestMessage& request) {
      const auto& payload = request.getPayload();
      const auto issued = issueNativeProviderOfferV3({payload.begin(), payload.end()}, offer, causalNow());
      ServiceProvider::AckDecision decision;
      if (issued) {
        decision.status = issued->status; decision.message = issued->message;
        decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
        decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      }
      return decision;
    }, ServiceProvider::AsyncAckStrategyHandler{},
    [handler = handlerRuntime.handler, returned, observedContext](ServiceProvider::CollaborationContext& ctx,
                                               const RequestMessage& request) {
      if (observedContext) observedContext->emplace(ctx);
      struct ReturnObservation {
        std::shared_ptr<std::atomic<unsigned>> counter;
        ~ReturnObservation() { if (counter) ++*counter; }
      } observation{returned};
      handler(ctx, request);
    });
  };

  // Match the production server lifecycle: register Repo services before
  // ServiceProvider::init() installs per-service request subscriptions.
  const std::string repoName = "/spec204/causal/repo";
  ndnsf_distributed_repo::StorageCapability capability;
  capability.repoNode = repoName; capability.repoMode = "persistent"; capability.freeBytes = 16U << 20;
  const auto journalPath = (files.root / "repo.sqlite").string();
  const auto repoProviderIdentity =
    environment->provider(destinationIndex).getName().toUri(ndn::name::UriFormat::ALTERNATE);
  auto repoBackend = ndnsf_distributed_repo::makeFilesystemRepoStore(
    (files.root / "repo-objects").string(), 4U << 20, 4U << 20);
  auto repo = std::make_shared<ndnsf_distributed_repo::RepoNode>(ndn::Name(repoName), capability,
    repoBackend,
    repoName, repoProviderIdentity, journalPath);
  auto journal = std::make_shared<ndnsf_distributed_repo::SqliteArtifactMetadataStore>(journalPath, true, 16, false);
  auto rangeStore = std::make_shared<CountingOriginalPacketStore>(
    repo->createEncryptedLargeDataStore());
  const auto producerCertificate = environment->keyChain().getPib().getIdentity(ndn::Name(provider))
    .getDefaultKey().getDefaultCertificate();
  const auto authorityCertificate = environment->keyChain().getPib().getIdentity(profile.attributeAuthority)
    .getDefaultKey().getDefaultCertificate();
  auto publisher = std::make_shared<ndn_service_framework::test::LocalServiceUser>(
    environment->providerFace(), profile.groupPrefix, producerCertificate, authorityCertificate, profile.trustSchemaPath);
  publisher->useSigningKeyChainForTest(environment->keyChain());
  publisher->attachLocalMockPubSubForTest(std::shared_ptr<ndn::svs::SVSPubSub>(
    &environment->providerPubSub(), [](ndn::svs::SVSPubSub*) {}));
  publisher->setEncryptedLargeDataRangeStore(rangeStore); publisher->init();
  environment->pumpUntilWithAttributeAuthority([&] { return publisher->isNacProducerReadyForTest(); });
  BOOST_REQUIRE(publisher->isNacProducerReadyForTest());
  // Keep the Repo management endpoint on the successor Provider so LIST and
  // MANIFEST remain reachable after the source Provider's transport is cut.
  repo->registerServices(environment->provider(destinationIndex), publisher);
  std::shared_ptr<ndnsf_distributed_repo::RepoNode> mixedUriRepo;
  std::shared_ptr<ndnsf_distributed_repo::SqliteArtifactMetadataStore> mixedUriJournal;
  std::optional<ndnsf_distributed_repo::RepoArtifactTarget> mixedUriTarget;
  if (verifyMixedUriClaimManifest) {
    ndnsf_distributed_repo::StorageCapability mixedCapability;
    mixedCapability.repoNode = mixedUriRepoName;
    mixedCapability.repoMode = "persistent";
    mixedCapability.freeBytes = 16U << 20;
    const auto mixedJournalPath = (files.root / "mixed-uri-repo.sqlite").string();
    auto mixedBackend = ndnsf_distributed_repo::makeFilesystemRepoStore(
      (files.root / "mixed-uri-repo-objects").string(), 4U << 20, 4U << 20);
    mixedUriRepo = std::make_shared<ndnsf_distributed_repo::RepoNode>(
      ndn::Name(mixedUriRepoName), mixedCapability, mixedBackend, mixedUriRepoName,
      repoProviderIdentity, mixedJournalPath);
    mixedUriJournal = std::make_shared<ndnsf_distributed_repo::SqliteArtifactMetadataStore>(
      mixedJournalPath, true, 16, false);
    // Register before Provider ingress initializes scoped subscriptions. This
    // node deliberately shares the destination Provider but has independent
    // physical storage and owner journal.
    mixedUriRepo->registerServices(environment->provider(destinationIndex), publisher);
    mixedUriTarget.emplace(ndnsf_distributed_repo::RepoArtifactTarget{
      mixedUriRepoName, repoProviderIdentity, mixedUriRepoName});
  }
  auto* requesterUser = &environment->user();
  const ndnsf_distributed_repo::RepoArtifactTarget storageTarget{
    repoName, environment->provider(destinationIndex).getName().toUri(ndn::name::UriFormat::ALTERNATE), repoName};

  // The fixture owns these embedded actors through `environment`.  Borrowed
  // shared_ptr views keep async callbacks from retaining the whole fixture
  // past its explicit client drain and teardown boundary.
  auto providerProducer = std::shared_ptr<ServiceProvider>(
    &environment->provider(), [](ServiceProvider*) {});
  NativeProviderCollaborationRuntime providerRuntime;
  NativeProviderCollaborationRuntime secondProviderRuntime;
  auto handoffOwnerContext = std::make_shared<
    std::optional<NativeConversationHandoffOwnerContextV1>>();
  auto secondHandoffOwnerContext = std::make_shared<
    std::optional<NativeConversationHandoffOwnerContextV1>>();
  auto secondOwnerCommitGate = std::make_shared<CausalBoundaryGate>();
  auto handoffResolverCalls = std::make_shared<std::atomic<unsigned>>(0);
  auto handoffProviderRequestsObserved = std::make_shared<std::atomic<unsigned>>(0);
  auto handoffProviderLifecycle = std::make_shared<std::vector<std::string>>();
  auto handoffProviderLifecycleMutex = std::make_shared<std::mutex>();
  if (productionHandoff) {
    environment->provider().setProviderRequestLifecycleCallback(
      [handoffProviderRequestsObserved, handoffProviderLifecycle,
       handoffProviderLifecycleMutex, serviceName = profile.serviceName](
          const ServiceProvider::ProviderRequestLifecycleStatus& status) {
        if (status.serviceName != serviceName) return;
        if (status.state == ServiceProvider::ProviderRequestLifecycleState::REQUEST_OBSERVED)
          ++*handoffProviderRequestsObserved;
        std::lock_guard<std::mutex> lock(*handoffProviderLifecycleMutex);
        handoffProviderLifecycle->push_back(
          status.requestId.toUri() + ":" +
          std::to_string(static_cast<unsigned>(status.state)) + ":selection=" +
          std::to_string(status.selectionReceivedTimestampUs) + ":execution-start=" +
          std::to_string(status.executionStartTimestampUs) + ":execution-done=" +
          std::to_string(status.executionDoneTimestampUs) + ":response-published=" +
          std::to_string(status.responsePublishedTimestampUs) + ":" + status.finalStatus);
      });
  }
  std::optional<ServiceProvider::ServiceRegistration> sourceRegistration;
  std::shared_ptr<NativeInferenceProvider> sourceHost;
  std::shared_ptr<NativeInferenceProvider> secondSourceHost;
  std::optional<NativeServiceRegistration> sourceNativeRegistration;
  std::optional<NativeServiceRegistration> emptySourceNativeRegistration;
  std::optional<NativeServiceRegistration> secondSourceNativeRegistration;
  NativeProviderCollaborationRuntime secondServiceRuntime;
  std::function<void()> closeSourceRegistration;
  std::function<bool()> drainSourceRegistration;
  if (productionHandoff) {
    sourceHost = std::make_shared<NativeInferenceProvider>(providerProducer,
      source.catalog->adapters());
    NativeServiceDefinition definition;
    definition.serviceName = service;
    definition.allowedRoles = twoSource ? std::vector<std::string>{sourceRoles.front()}
                                        : sourceRoles;
    if (withdrawalAbort || withdrawalSave || withdrawalWatch ||
        withdrawalWatchNoIntent || withdrawalBudget || withdrawalForce ||
        withdrawalForceBudget || withdrawalWireBudget || switchFirstWithdrawalOverlap)
      definition.withdrawalPersistencePolicyId = "spec212-causal-withdrawal-policy";
    auto localOffer = offer;
    localOffer.provider = provider;
    localOffer.bootEpoch = boot;
    localOffer.acceptedRoles = definition.allowedRoles;
    const auto makeOfferAck = [](NativeProviderOfferV3Config local) {
      return [local](const RequestMessage& request) {
        const auto& payload = request.getPayload();
        const auto issued = issueNativeProviderOfferV3(
          {payload.begin(), payload.end()}, local, causalNow());
        ServiceProvider::AckDecision decision;
        if (issued) {
          decision.status = issued->status;
          decision.message = issued->message;
          decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
          decision.pendingStateTtlMs = issued->pendingStateTtlMs;
        }
        return decision;
      };
    };
    definition.ackHandler = makeOfferAck(localOffer);
    definition.handoffOwnerContextResolver =
      [handoffOwnerContext, handoffResolverCalls](
          const ndn::Name&, const ndn::Name&, const std::string&) {
        ++*handoffResolverCalls;
        if (!*handoffOwnerContext)
          throw std::runtime_error("OWNER_UNAVAILABLE");
        return **handoffOwnerContext;
      };
    definition.runtimeObserver = [&providerRuntime](
        const NativeProviderCollaborationRuntime& runtimeView) {
      providerRuntime = runtimeView;
    };
    sourceNativeRegistration.emplace(sourceHost->serve(definition, makeConfig(0, factory)));
    if (partialHostWithdrawal || twoNonemptyServiceWithdrawal) {
      const std::string emptyService = twoNonemptyServiceWithdrawal
        ? secondService : "/Spec204/A-Empty";
      auto emptyDefinition = definition;
      emptyDefinition.serviceName = emptyService;
      auto secondOffer = localOffer;
      secondOffer.service = emptyService;
      emptyDefinition.ackHandler = makeOfferAck(secondOffer);
      if (postCommitChildFailure) {
        emptyDefinition.handoffOwnerContextResolver =
          [rangeStore, handoffResolverCalls](
              const ndn::Name&, const ndn::Name&, const std::string&) {
            ++*handoffResolverCalls;
            const auto commitDeadline = std::chrono::steady_clock::now() + 25s;
            while (rangeStore->packetCommitCalls() == 0 &&
                   std::chrono::steady_clock::now() < commitDeadline)
              std::this_thread::sleep_for(10ms);
            if (rangeStore->packetCommitCalls() == 0)
              throw std::runtime_error("TEST_FIRST_CHILD_COMMIT_MISSING");
            throw std::runtime_error("POSTCOMMIT_SECOND_CHILD_FAILED");
            return NativeConversationHandoffOwnerContextV1{};
          };
      }
      else if (multipleOwnerWithdrawal) {
        emptyDefinition.handoffOwnerContextResolver =
          [secondHandoffOwnerContext, handoffResolverCalls, secondOwner](
              const ndn::Name& authenticatedOwner,
              const ndn::Name&, const std::string&) {
            ++*handoffResolverCalls;
            if (authenticatedOwner != ndn::Name(secondOwner))
              throw std::runtime_error("SECOND_OWNER_IDENTITY_MISMATCH");
            if (!*secondHandoffOwnerContext)
              throw std::runtime_error("SECOND_OWNER_UNAVAILABLE");
            return **secondHandoffOwnerContext;
          };
      }
      emptyDefinition.runtimeObserver = twoNonemptyServiceWithdrawal
        ? std::function<void(const NativeProviderCollaborationRuntime&)>(
            [&secondServiceRuntime](const NativeProviderCollaborationRuntime& runtimeView) {
              secondServiceRuntime = runtimeView;
            })
        : std::function<void(const NativeProviderCollaborationRuntime&)>{};
      auto emptyConfig = makeConfig(0, factory);
      emptyConfig.plan.serviceName = emptyService;
      emptySourceNativeRegistration.emplace(
        sourceHost->serve(emptyDefinition, emptyConfig));
    }
    if (twoSource) {
      auto secondProducer = std::shared_ptr<ServiceProvider>(
        &environment->provider(1), [](ServiceProvider*) {});
      secondSourceHost = std::make_shared<NativeInferenceProvider>(secondProducer,
        source.catalog->adapters());
      auto secondDefinition = definition;
      secondDefinition.allowedRoles = {sourceRoles.back()};
      auto secondOffer = offer;
      secondOffer.provider = secondSource;
      secondOffer.bootEpoch = environment->provider(1).getProviderBootEpoch();
      secondOffer.acceptedRoles = secondDefinition.allowedRoles;
      secondDefinition.ackHandler = makeOfferAck(secondOffer);
      secondDefinition.handoffOwnerContextResolver =
        [secondHandoffOwnerContext, handoffResolverCalls](
            const ndn::Name&, const ndn::Name&, const std::string&) {
          ++*handoffResolverCalls;
          if (!*secondHandoffOwnerContext)
            throw std::runtime_error("Spec212 second source Owner context is not ready");
          return **secondHandoffOwnerContext;
        };
      secondDefinition.runtimeObserver = [&secondProviderRuntime](
          const NativeProviderCollaborationRuntime& runtimeView) {
        secondProviderRuntime = runtimeView;
      };
      secondSourceNativeRegistration.emplace(
        secondSourceHost->serve(secondDefinition, makeConfig(1, factory)));
    }
    closeSourceRegistration = [&] {
      sourceNativeRegistration->close();
      if (emptySourceNativeRegistration) emptySourceNativeRegistration->close();
      if (secondSourceNativeRegistration) secondSourceNativeRegistration->close();
    };
    drainSourceRegistration = [&] {
      const bool emptyDrained = !emptySourceNativeRegistration ||
        emptySourceNativeRegistration->drain(10s);
      const bool firstDrained = emptyDrained &&
        sourceNativeRegistration->drain(10s) && sourceHost->drain(10s);
      const bool secondDrained = !secondSourceNativeRegistration ||
        (secondSourceNativeRegistration->drain(10s) && secondSourceHost->drain(10s));
      return firstDrained && secondDrained;
    };
  }
  else {
    providerRuntime = makeNativeProviderCollaborationRuntime(makeConfig(0, factory));
    bindNativeProviderDurableKvPublisher(providerRuntime, providerProducer);
  }
  BOOST_REQUIRE(productionHandoff ||
    static_cast<bool>(providerRuntime.publishProtectedKvCheckpoint));
  if (!productionHandoff) {
    sourceRegistration.emplace(installHandler(0, providerRuntime));
    closeSourceRegistration = [&] { sourceRegistration->close(); };
    drainSourceRegistration = [] { return true; };
  }
  environment->enableProductionIngressForTest();
  std::shared_ptr<ServiceUser> secondOwnerUser;
  std::shared_ptr<CausalResponseDroppingUser> secondOwnerDroppingUser;
  std::optional<ndn::security::Certificate> secondOwnerCertificate;
  std::shared_ptr<ndn::svs::SVSPubSub> secondOwnerPubSub;
  auto secondOwnerProviderPublications = std::make_shared<std::atomic<unsigned>>(0);
  auto providerSecondOwnerPublications = std::make_shared<std::atomic<unsigned>>(0);
  auto secondOwnerPublicationNames = std::make_shared<std::vector<std::string>>();
  auto secondOwnerPublicationNamesMutex = std::make_shared<std::mutex>();
  auto providerSecondOwnerNames = std::make_shared<std::vector<std::string>>();
  auto providerSecondOwnerNamesMutex = std::make_shared<std::mutex>();
  const auto initializeSecondOwner = [&] {
    if (!multipleOwnerWithdrawal) return;
    const auto providerIdentity = ndn::Name(secondOwner);
    secondOwnerCertificate = environment->keyChain().getPib()
      .getIdentity(providerIdentity).getDefaultKey().getDefaultCertificate();
    const auto authorityCertificate = environment->keyChain().getPib()
      .getIdentity(profile.attributeAuthority).getDefaultKey().getDefaultCertificate();
    secondOwnerDroppingUser = std::make_shared<CausalResponseDroppingUser>(
      environment->providerFace(2), profile.groupPrefix,
      *secondOwnerCertificate, authorityCertificate, profile.trustSchemaPath);
    secondOwnerUser = secondOwnerDroppingUser;
    secondOwnerUser->useSigningKeyChainForTest(environment->keyChain());
    ndn::svs::SecurityOptions security(environment->keyChain());
    security.interestSigner = std::make_shared<ndn::svs::BaseSigner>();
    security.dataSigner->signingInfo =
      ndn::security::signingByCertificate(*secondOwnerCertificate);
    security.pubSigner->signingInfo =
      ndn::security::signingByCertificate(*secondOwnerCertificate);
    security.validator = std::make_shared<ndn::svs::BaseValidator>();
    security.encapsulatedDataValidator = std::make_shared<ndn::svs::BaseValidator>();
    ndn::svs::SVSPubSubOptions pubSubOptions;
    pubSubOptions.useTimestamp = false;
    secondOwnerPubSub = std::make_shared<ndn::svs::SVSPubSub>(
      profile.syncPrefix, ndn::Name(secondOwner).append("user").append("1"),
      environment->providerFace(2),
      [](const std::vector<ndn::svs::MissingDataInfo>&) {},
      pubSubOptions, security);
    secondOwnerPubSub->subscribeToProducer(profile.providerNode,
      [secondOwnerProviderPublications, secondOwnerPublicationNames,
       secondOwnerPublicationNamesMutex](
          const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        ++*secondOwnerProviderPublications;
        std::lock_guard<std::mutex> lock(*secondOwnerPublicationNamesMutex);
        secondOwnerPublicationNames->push_back(publication.name.toUri());
      }, false);
    environment->providerPubSub().subscribeToProducer(
      ndn::Name(secondOwner).append("user").append("1"),
      [providerSecondOwnerPublications, providerSecondOwnerNames,
       providerSecondOwnerNamesMutex, certificate = *secondOwnerCertificate,
       secondService](const ndn::svs::SVSPubSub::SubscriptionData& publication) {
        ++*providerSecondOwnerPublications;
        const auto actor = parseSvsProducerActor(publication.producerPrefix, "user");
        const bool signedByOwner = publication.packet &&
          ndn::security::verifySignature(*publication.packet, certificate);
        const bool isSecondServiceRequest = actor &&
          parseRequestNameV2(publication.name, *actor, ndn::Name(secondService));
        std::lock_guard<std::mutex> lock(*providerSecondOwnerNamesMutex);
        providerSecondOwnerNames->push_back(publication.name.toUri() +
          " signer-valid=" + (signedByOwner ? "1" : "0") +
          " service-request=" + (isSecondServiceRequest ? "1" : "0"));
      }, false);
    secondOwnerUser->attachLocalMockPubSubForTest(secondOwnerPubSub);
    ndn_service_framework::PermissionResponse permissions;
    permissions.setTargetIdentity(secondOwner);
    permissions.setPermissionKind(ndn_service_framework::tlv::UserPermission);
    permissions.setPolicyEpoch(1);
    for (std::size_t index = 0; index < environment->providerCount(); ++index) {
      for (const auto& allowedService :
           {ndn::Name(secondService),
            ndn::Name("/spec204/causal/repo/USER_STORAGE_GRANT"),
            ndn::Name("/spec204/causal/repo/USER_STORAGE_LIST"),
            ndn::Name("/spec204/causal/repo/MANIFEST")}) {
        ndn_service_framework::PermissionEntry entry;
        entry.setProviderName(environment->provider(index).getName().toUri());
        entry.setServiceName(allowedService.toUri());
        entry.setToken("");
        entry.setTtl(0);
        entry.setVersion(1);
        permissions.addEntry(entry);
      }
    }
    secondOwnerUser->applyPermissionResponse(permissions);
    BOOST_REQUIRE(secondServiceControllerStatus);
    BOOST_REQUIRE(secondOwnerUser->installControllerStatus(*secondServiceControllerStatus));
    secondOwnerUser->init();
    secondOwnerUser->refreshNacProducerForTest();
    environment->pumpUntilWithAttributeAuthority([&] {
      return secondOwnerUser->isNacConsumerReadyForTest() &&
        secondOwnerUser->isNacProducerReadyForTest();
    });
    BOOST_REQUIRE(secondOwnerUser->isNacConsumerReadyForTest());
    BOOST_REQUIRE(secondOwnerUser->isNacProducerReadyForTest());
    for (std::size_t index = 0; index < environment->providerCount(); ++index) {
      for (const auto* kind : {"ACK", "RESPONSE"}) {
        const auto key = environment->provider(index).prepareHybridSendKeyForTest(
          ndn::Name(secondService), kind);
        secondOwnerUser->cacheHybridReceiveKeyForTest(key.keyId, key.epochId, key.key);
      }
    }
    const auto ownerSelectionKey = secondOwnerUser->prepareHybridSendKeyForTest(
      ndn::Name(secondService), "SELECTION");
    for (std::size_t index = 0; index < environment->providerCount(); ++index)
      environment->provider(index).cacheHybridReceiveKeyForTest(
        ownerSelectionKey.keyId, ownerSelectionKey.epochId, ownerSelectionKey.key);
  };
  for (std::size_t index = 0; index < environment->providerCount(); ++index) {
    environment->provider(index).markHybridResponseKeyWrappedForTest(service);
    for (const auto* kind : {"ACK", "RESPONSE"}) {
      const auto key = environment->provider(index).prepareHybridSendKeyForTest(service, kind);
      environment->user().cacheHybridReceiveKeyForTest(key.keyId, key.epochId, key.key);
    }
    if (twoNonemptyServiceWithdrawal) {
      environment->provider(index).markHybridResponseKeyWrappedForTest(ndn::Name(secondService));
      for (const auto* kind : {"ACK", "RESPONSE"}) {
        const auto key = environment->provider(index).prepareHybridSendKeyForTest(
          ndn::Name(secondService), kind);
        environment->user().cacheHybridReceiveKeyForTest(key.keyId, key.epochId, key.key);
      }
    }
  }
  const auto selectionKey = environment->user().prepareHybridSendKeyForTest(service, "SELECTION");
  for (std::size_t index = 0; index < environment->providerCount(); ++index)
    environment->provider(index).cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);
  if (twoNonemptyServiceWithdrawal) {
    const auto secondSelectionKey = environment->user().prepareHybridSendKeyForTest(
      ndn::Name(secondService), "SELECTION");
    for (std::size_t index = 0; index < environment->providerCount(); ++index)
      environment->provider(index).cacheHybridReceiveKeyForTest(
        secondSelectionKey.keyId, secondSelectionKey.epochId, secondSelectionKey.key);
  }

  auto user = std::shared_ptr<ServiceUser>(
    &environment->user(), [](ServiceUser*) {});
  NativeConversationConfig conversation;
  conversation.journal = std::make_shared<NativeConversationJournal>(NativeConversationJournalConfig{
    files.root / "conversation", "spec204-causal-owner", {{"key", std::vector<std::uint8_t>(32, 0x5a)}},
    4U << 20, true});
  conversation.requesterIdentity = requester; conversation.serviceName = service;
  conversation.securityDomainDigest = policyDigest;
  auto postCommitGate = std::make_shared<CausalBoundaryGate>();
  auto postCommitAction = std::make_shared<std::atomic<int>>(0);
  if (fault == CausalRecoveryFault::CloseAfterCommittedAppendBeforeCompletion ||
      fault == CausalRecoveryFault::FailAfterCommittedAppend ||
      withdrawalAfterCommittedAppend) {
    conversation.afterDurableCommit = [postCommitGate, postCommitAction] {
      const auto action = postCommitAction->exchange(0);
      if (action == 1) postCommitGate->parkOnce();
      if (action == 2)
        std::this_thread::sleep_for(31s); // Expire the 30s request after durable commit.
    };
  }
  auto conversations = std::make_shared<NativeConversationCoordinator>(std::move(conversation));
  NativeRequestRuntime runtime;
  runtime.contract = {service, "task", source.model.descriptor.adapterId,
    source.model.descriptor.adapter.descriptorDigest(), nativePlanningDigest("spec204-composition"),
    nativePlanningDigest("spec204-task"), "TOKEN_STREAMING", tokenizer};
  runtime.requesterIdentity = requester; runtime.protectionEpoch = epoch;
  runtime.inputLayoutDigest = nativePlanningDigest("spec204-input-layout");
  runtime.security = {policyDigest, true}; runtime.budget = {1, 1000, 1};
  runtime.grants = grants; runtime.stateMapping = source.stateMapping; runtime.catalog = source.catalog;
  const auto ownerCertificate = environment->keyChain().getPib().getIdentity(environment->user().getName())
    .getDefaultKey().getDefaultCertificate();
  runtime.wrapProtectedKvKey = [fixtureEnvironment = environment.get(), ownerCertificate](
    const auto& checkpoint, const auto& publicKey, const auto& binding, const auto& requestControl, const auto& current) {
    requestControl.requireActive();
    return nativeWrapProtectedKvKeyForRecipient(checkpoint, ownerCertificate,
      fixtureEnvironment->keyChain(), publicKey, binding, current);
  };
  NativeInferenceClient client(user, source.catalog->adapters(), runtime, conversations,
    source.catalog->makePreparation(user, service), admission);
  struct DrainClient
  {
    NativeInferenceClient& client;
    ndn_service_framework::test::NdnsfIntegrationEnvironment& environment;
    std::function<void()> closeRegistration;
    std::function<bool()> drainRegistration;
    NativeProviderCollaborationRuntime& originalRuntime;
    NativeProviderCollaborationRuntime* secondRuntime;
    bool drained = false;
    bool clientDrained = false;
    bool registrationDrained = false;
    bool close()
    {
      if (drained) return true;
      closeRegistration(); originalRuntime = {};
      if (secondRuntime) *secondRuntime = {};
      client.close();
      auto work = std::async(std::launch::async, [&] {
        clientDrained = client.drain(10s);
        if (clientDrained)
          registrationDrained = drainRegistration();
        return clientDrained && registrationDrained;
      });
      while (work.wait_for(0ms) != std::future_status::ready)
        environment.pumpUntilWithAttributeAuthority([&] { return work.wait_for(0ms) == std::future_status::ready; });
      drained = work.get(); return drained;
    }
    ~DrainClient() { try { close(); } catch (...) {} }
  } drainClient{client, *environment, closeSourceRegistration,
                drainSourceRegistration, providerRuntime,
                twoSource ? &secondProviderRuntime : nullptr};
  struct DrainStandaloneClient
  {
    NativeInferenceClient& client;
    ndn_service_framework::test::NdnsfIntegrationEnvironment& environment;
    bool drained = false;
    DrainStandaloneClient(
      NativeInferenceClient& clientValue,
      ndn_service_framework::test::NdnsfIntegrationEnvironment& environmentValue)
      : client(clientValue), environment(environmentValue)
    {
    }
    bool close()
    {
      if (drained) return true;
      client.close();
      auto work = std::async(std::launch::async, [&] { return client.drain(10s); });
      while (work.wait_for(0ms) != std::future_status::ready)
        environment.pumpUntilWithAttributeAuthority([&] {
          return work.wait_for(0ms) == std::future_status::ready;
        });
      drained = work.get();
      return drained;
    }
    ~DrainStandaloneClient() { try { close(); } catch (...) {} }
  };
  NativeModelRef model; static_cast<NativeModelDescriptor&>(model) = source.model.descriptor;
  const auto input = causalInput(source.model.descriptor, {1, 3}, tokenizer, 2);
  NativeRequestOptions options;
  options.taskName = "task"; options.timeoutMs = 30000; options.ackTimeoutMs = 3000;
  options.outputMode = "TOKEN_STREAMING"; options.stream = StreamRequestOptions{};
  options.stream->generationId.fill(0x11);
  if (twoRole) {
    // COMMIT/ACK/FINALIZE may use the configured 10-second control window.
    options.stream->interestLifetimeMs = 3000;
    const auto streamGapBudgetMs = options.stream->interestLifetimeMs *
      (static_cast<unsigned>(options.stream->maxEventRetries) + 1U);
    BOOST_REQUIRE_GT(streamGapBudgetMs, 10000U);
    BOOST_REQUIRE_LT(streamGapBudgetMs, options.timeoutMs);
  }
  options.generation = nativeGenerationFromOptions(input.options, std::string(32, '1'));
  options.conversation = NativeConversationContinuation{"spec204-real-causal", 0, service, {}, {}, {},
    now + 180000, "FULL_CONTEXT", {}, std::string(32, '1'), {1, 3}, {}};
  options.providerNames = {ndn::Name(provider)};
  if (twoSource)
    options.providerNames.emplace_back(secondSource);
  auto splitter = std::make_shared<CausalSplitter>(candidate);
  const auto exportsBeforeFirst =
    providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts;
  const auto usageBeforeFirst = journal->userStorageUsage(requester, repoName);
  const auto claimsBeforeFirst = journal->listUserStorage(requester, repoName, "", 1000).size();
  BOOST_TEST_MESSAGE("Spec204 diagnostic: starting first inference request");
  const auto first = client.request(model, input, splitter, std::make_shared<NativePreSplitFirstPlacement>(), options);
  BOOST_TEST_MESSAGE("Spec204 diagnostic: first inference request returned; waiting for causal result");
  causalWait(*environment, first, [&] {
    BOOST_TEST_MESSAGE("Spec212 initial request id=" << first.requestId()
      << " status=" << static_cast<unsigned>(first.status())
      << " dropped-observations=" << first.diagnostics().observationDropped
      << " provider-requests-observed=" << handoffProviderRequestsObserved->load()
      << " handoff-owner-resolver-calls=" << handoffResolverCalls->load());
    std::vector<std::string> lifecycle;
    {
      std::lock_guard<std::mutex> lock(*handoffProviderLifecycleMutex);
      lifecycle = *handoffProviderLifecycle;
    }
    BOOST_TEST_MESSAGE("Spec212 provider lifecycle event count=" << lifecycle.size());
    for (const auto& event : lifecycle)
      BOOST_TEST_MESSAGE("Spec212 provider lifecycle: " << event);
    auto printRecentNames = [](const char* label,
                               const std::shared_ptr<std::vector<std::string>>& names,
                               const std::shared_ptr<std::mutex>& mutex) {
      std::vector<std::string> snapshot;
      {
        std::lock_guard<std::mutex> lock(*mutex);
        snapshot = *names;
      }
      BOOST_TEST_MESSAGE("Spec212 initial " << label << " count=" << snapshot.size());
      const auto firstRecent = snapshot.size() > 12 ? snapshot.size() - 12 : 0;
      for (std::size_t index = firstRecent; index < snapshot.size(); ++index)
        BOOST_TEST_MESSAGE("Spec212 initial " << label << ": " << snapshot[index]);
    };
    printRecentNames("Provider Data", providerFacePublishedPacketNames,
      providerFacePublishedPacketNamesMutex);
    printRecentNames("User Interest", userProviderPacketInterestNames,
      userProviderPacketInterestNamesMutex);
    printRecentNames("Provider Response", providerFaceResponseNames,
      providerFaceResponseNamesMutex);
    printRecentNames("Provider Event", providerFaceStreamEventNames,
      providerFaceStreamEventNamesMutex);
    printRecentNames("User Event Interest", userStreamInterestNames,
      userStreamInterestNamesMutex);
  });
  BOOST_TEST_MESSAGE("Spec204 diagnostic: first causal inference completed");
  initializeSecondOwner();
  BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), 0U);
  BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), 0U);
  if (withdrawalWatchNoIntent) {
    BOOST_CHECK_EQUAL(rangeStore->packetCount(), 0U);
    BOOST_CHECK_EQUAL(rangeStore->packetWireBytes(), 0U);
    BOOST_REQUIRE(providerRuntime.capacitySnapshot);
    const auto health = providerRuntime.capacitySnapshot();
    BOOST_CHECK_GT(health.workerCount, 0U);
    BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
                      exportsBeforeFirst);
    const auto usageAfterFirst = journal->userStorageUsage(requester, repoName);
    BOOST_CHECK_EQUAL(usageAfterFirst.committedBytes, usageBeforeFirst.committedBytes);
    BOOST_CHECK_EQUAL(usageAfterFirst.reservedBytes, usageBeforeFirst.reservedBytes);
    BOOST_CHECK_EQUAL(journal->listUserStorage(requester, repoName, "", 1000).size(), claimsBeforeFirst);
  }
  std::shared_ptr<NativeConversationCoordinator> secondServiceConversations;
  std::unique_ptr<NativeInferenceClient> secondServiceClient;
  std::unique_ptr<DrainStandaloneClient> secondServiceDrain;
  std::string secondServiceCheckpointDigest;
  if (twoNonemptyServiceWithdrawal) {
    auto secondAdmissionPolicy = nativeParseJson(admissionPolicy);
    for (auto& entry : secondAdmissionPolicy["entries"])
      entry["service"] = secondService;
    auto secondAdmission = std::make_shared<NativeOfferAdmission>(
      nativeCanonicalJson(secondAdmissionPolicy),
      std::map<std::string, std::string>{{offerKeyId, causalPublicPem(offerKey)}},
      policyDigest);
    NativeConversationConfig secondConversation;
    secondConversation.journal = std::make_shared<NativeConversationJournal>(
      NativeConversationJournalConfig{files.root / "conversation-second",
        "spec212-causal-owner-second-service", {{"key", std::vector<std::uint8_t>(32, 0x5a)}},
        4U << 20, true});
    secondConversation.requesterIdentity = secondOwner;
    secondConversation.serviceName = secondService;
    secondConversation.securityDomainDigest = policyDigest;
    secondServiceConversations = std::make_shared<NativeConversationCoordinator>(
      std::move(secondConversation));
    auto secondRequestRuntime = runtime;
    secondRequestRuntime.contract.serviceName = secondService;
    if (multipleOwnerWithdrawal) {
      BOOST_REQUIRE(secondOwnerUser);
      BOOST_REQUIRE(secondOwnerCertificate);
      BOOST_REQUIRE(secondOwnerGrants);
      secondRequestRuntime.requesterIdentity = secondOwner;
      secondRequestRuntime.grants = secondOwnerGrants;
      secondRequestRuntime.wrapProtectedKvKey =
        [fixtureEnvironment = environment.get(),
         ownerCertificate = *secondOwnerCertificate](
            const auto& checkpoint, const auto& publicKey, const auto& binding,
            const auto& requestControl, const auto& current) {
          requestControl.requireActive();
          return nativeWrapProtectedKvKeyForRecipient(checkpoint, ownerCertificate,
            fixtureEnvironment->keyChain(), publicKey, binding, current);
        };
    }
    auto requestUser = secondOwnerUser ? secondOwnerUser : user;
    secondServiceClient = std::make_unique<NativeInferenceClient>(
      requestUser, source.catalog->adapters(), std::move(secondRequestRuntime),
      secondServiceConversations,
      source.catalog->makePreparation(requestUser, secondService), secondAdmission);
    secondServiceDrain = std::make_unique<DrainStandaloneClient>(
      *secondServiceClient, *environment);
    auto secondCandidate = candidate;
    secondCandidate.executionPlan.serviceName = secondService;
    secondCandidate.candidateDigest = secondCandidate.computedDigest();
    auto secondOptions = options;
    secondOptions.stream->generationId.fill(0x44);
    secondOptions.generation = nativeGenerationFromOptions(
      input.options, std::string(32, '4'));
    secondOptions.conversation->conversationId = "spec212-second-service-conversation";
    secondOptions.conversation->serviceName = secondService;
    secondOptions.conversation->generationId = std::string(32, '4');
    const auto secondRequest = secondServiceClient->request(
      model, input, std::make_shared<CausalSplitter>(secondCandidate),
      std::make_shared<NativePreSplitFirstPlacement>(), secondOptions);
    causalWait(*environment, secondRequest, [&] {
      BOOST_TEST_MESSAGE("Spec212 second Owner SVS: provider publications="
        << secondOwnerProviderPublications->load()
        << "; Provider saw Owner publications="
        << providerSecondOwnerPublications->load());
      const auto diagnostics = secondOwnerUser->consumeRuntimeDiagnostics();
      BOOST_TEST_MESSAGE("Spec212 second Owner ACK latency samples="
        << diagnostics.ackLatenciesMs.size()
        << "; skipped callbacks=" << diagnostics.callbackSkippedNoPending);
      std::lock_guard<std::mutex> lock(*secondOwnerPublicationNamesMutex);
      const auto firstRecent = secondOwnerPublicationNames->size() > 12
        ? secondOwnerPublicationNames->size() - 12 : 0;
      for (std::size_t index = firstRecent;
           index < secondOwnerPublicationNames->size(); ++index)
        BOOST_TEST_MESSAGE("Spec212 second Owner saw: "
          << (*secondOwnerPublicationNames)[index]);
      {
        std::lock_guard<std::mutex> providerLock(*providerSecondOwnerNamesMutex);
        for (const auto& name : *providerSecondOwnerNames)
          BOOST_TEST_MESSAGE("Spec212 Provider saw second Owner: " << name);
      }
      const auto& crypto = environment->provider().getHybridCryptoCounters();
      BOOST_TEST_MESSAGE("Spec212 Provider crypto decrypt="
        << crypto.symmetric_decrypt_count.load()
        << "; NAC unwrap=" << crypto.nac_abe_key_unwrap_count.load());
    });
    const auto secondRecord = secondServiceConversations->find(
      "spec212-second-service-conversation");
    BOOST_REQUIRE(secondRecord);
    secondServiceCheckpointDigest = secondRecord->checkpoint.checkpointDigest;
    BOOST_REQUIRE_EQUAL(secondServiceRuntime.conversationStateSnapshot().saveEligibleEntries,
                        1U);
    BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), 0U);
  }
  std::string unrelatedConversationCheckpointWire;
  if (capacityCompetition) {
    auto competingOptions = options;
    competingOptions.stream->generationId.fill(0x22);
    competingOptions.generation = nativeGenerationFromOptions(input.options, std::string(32, '2'));
    competingOptions.conversation->conversationId = "spec212-capacity-competing";
    competingOptions.conversation->generationId = std::string(32, '2');
    const auto competingRequest = client.request(model, input, splitter,
      std::make_shared<NativePreSplitFirstPlacement>(), competingOptions);
    causalWait(*environment, competingRequest);
    const auto unrelatedBefore = conversations->find("spec212-capacity-competing");
    BOOST_REQUIRE(unrelatedBefore);
    if (fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch)
      unrelatedConversationCheckpointWire = unrelatedBefore->checkpoint.wire;
  }
  if (twoSlotCapacityCompetition) {
    auto thirdOptions = options;
    thirdOptions.stream->generationId.fill(0x33);
    thirdOptions.generation = nativeGenerationFromOptions(input.options, std::string(32, '3'));
    thirdOptions.conversation->conversationId = "spec212-capacity-third";
    thirdOptions.conversation->generationId = std::string(32, '3');
    const auto thirdRequest = client.request(model, input, splitter,
      std::make_shared<NativePreSplitFirstPlacement>(), thirdOptions);
    causalWait(*environment, thirdRequest);
    BOOST_REQUIRE(conversations->find("spec212-capacity-third"));
  }
  const auto result = first.result(0ms);
  const auto resultJson = nativeParseJson(std::string(result.payload.begin(), result.payload.end()));
  BOOST_CHECK(resultJson.at("tokenIds") == NativeJson::array({4, 5}));
  const auto record = conversations->find("spec204-real-causal");
  BOOST_REQUIRE(record); BOOST_REQUIRE(first.conversationCheckpoint());
  if (twoSource) {
    BOOST_CHECK_EQUAL(record->checkpoint.providersByRole.at(sourceRoles.front()), provider);
    BOOST_CHECK_EQUAL(record->checkpoint.providersByRole.at(sourceRoles.back()), secondSource);
  }
  BOOST_CHECK_EQUAL(record->checkpoint.wire, *first.conversationCheckpoint());
  BOOST_REQUIRE_EQUAL(record->checkpoint.transcript.at("providerRoleReceipts").size(),
                      sourceRoles.size());
  const auto receiptWire = nativeConversationBase64Decode(
    record->checkpoint.transcript.at("providerRoleReceipts").front().get<std::string>());
  const auto receipt = nativeReadProviderConversationStateReceipt(nativeParseJson(receiptWire));
  ConversationStateReferenceV1 reference{record->checkpoint.conversationId,
    record->checkpoint.successorContextEpoch, service, record->planRoleMapDigest,
    record->checkpoint.checkpointDigest, source.role, receipt.computedDigest(), receipt.expiresAtMs};
  const auto portable = providerRuntime.exportCommittedConversationState(reference, {1U << 20, 2U << 20, {}}, causalNow());
  BOOST_CHECK_EQUAL(portable.originIdentity.providerIdentity, provider);
  BOOST_CHECK_EQUAL(portable.originIdentity.providerBootId, boot);
  BOOST_CHECK_EQUAL(portable.originIdentity.prefixTokenCount, 4U);
  BOOST_CHECK_EQUAL(portable.components.size(), 3U);
  const auto sourceRunners = factory->created();
  const auto sourceRunnerRoles = factory->createdRoles();
  BOOST_REQUIRE_EQUAL(sourceRunners.size(),
    sourceRoles.size() * (preparedConversationCount +
      (twoNonemptyServiceWithdrawal ? 1U : 0U)));
  BOOST_REQUIRE_EQUAL(sourceRunnerRoles.size(), sourceRunners.size());
  std::optional<std::size_t> sourceTokenIngressIndex;
  std::size_t sourceTokenIngressCount = 0;
  for (std::size_t i = 0; i < sourceRunners.size(); ++i) {
    const bool ownsTokenInput = roleOwnsTokenInput(sourceRunnerRoles[i]);
    BOOST_CHECK_EQUAL(ownsTokenInput, sourceRunnerRoles[i] == source.role);
    const auto sourceRunner = sourceRunners[i].lock();
    BOOST_REQUIRE(sourceRunner);
    const auto sourceMetrics = sourceRunner->runtimeMetricsSnapshot();
    BOOST_REQUIRE(sourceMetrics);
    BOOST_CHECK_EQUAL(sourceMetrics->workCounterCertified, ownsTokenInput);
    if (ownsTokenInput) {
      if (!sourceTokenIngressIndex) sourceTokenIngressIndex = i;
      ++sourceTokenIngressCount;
    }
  }
  BOOST_REQUIRE_EQUAL(sourceTokenIngressCount,
    preparedConversationCount + (twoNonemptyServiceWithdrawal ? 1U : 0U));
  BOOST_REQUIRE(sourceTokenIngressIndex);
  const auto oldRunner = sourceRunners[*sourceTokenIngressIndex];
  auto held = oldRunner.lock(); BOOST_REQUIRE(held);
  const auto metrics = held->runtimeMetricsSnapshot(); BOOST_REQUIRE(metrics);
  BOOST_CHECK(metrics->workCounterCertified); BOOST_CHECK_EQUAL(metrics->executedPrefillTokens, 2U);
  if (withdrawalWatchNoIntent)
    BOOST_CHECK_GT(metrics->executedDecodeTokens, 0U);
  held.reset();

  // Persist while A is still online, with the real producing identity and an
  // exact Repo grant issued through the requester's authenticated Core path.
  // This remains a manually composed fixture, not the T006/T007 event API or
  // separate-process Repo qualification.
  const NativePortableKvCheckpointLimitsV1 portableLimits{{1U << 16, 1U << 17, {}}, 1U << 16, 1U << 18};
  const NativeProtectedKvCheckpointLimitsV1 protectedLimits{1U << 19, 1U << 20, {}};
  nativeExportPrivatePortableKvCheckpoint(files.root / "committed.ndkv", portable, portableLimits);
  const auto protectedCheckpoint = nativeProtectPortableKvCheckpoint(files.root / "committed.ndkv",
    files.root / "committed.ndkve", ownerCertificate, ndn::Name(provider),
    nativeProtectedKvRoleBinding(requester, reference), protectedLimits);
  const auto expectedPublicationIdentity = [](const NativeProtectedKvCheckpointReferenceV1& checkpoint) {
    return nativePlanningDigest(nativeCanonicalJson(NativeJson{
      {"version", 1U}, {"purpose", "NDNSF-DI/KV-PUBLICATION/V1"},
      {"producer", checkpoint.producerPrefix.toUri(ndn::name::UriFormat::CANONICAL)},
      {"owner", checkpoint.ownerIdentity.toUri(ndn::name::UriFormat::CANONICAL)},
      {"ownerCertificateName", checkpoint.ownerCertificateName.toUri(ndn::name::UriFormat::CANONICAL)},
      {"ownerCertificateDigest", checkpoint.ownerCertificateDigest},
      {"checkpointIdentityDigest", checkpoint.checkpointIdentityDigest},
      {"wireDigest", checkpoint.wireDigest}, {"wireBytes", checkpoint.wireBytes}}));
  };
  const auto grantDeadline = std::chrono::steady_clock::now() +
    (productionHandoff ? 45s : 12s);
  auto grantBarrier = std::make_shared<NativeDurableStorageGrantBarrierV1>(requester,
    NativeDurableStorageGrantBarrierV1::ProducerPrefixesByRole{
      {source.role, ndn::Name(provider)}}, grantDeadline);
  std::shared_ptr<NativeDurableStorageGrantBarrierV1> secondGrantBarrier;
  if (multipleOwnerWithdrawal)
    secondGrantBarrier = std::make_shared<NativeDurableStorageGrantBarrierV1>(
      secondOwner,
      NativeDurableStorageGrantBarrierV1::ProducerPrefixesByRole{
        {source.role, ndn::Name(provider)}}, grantDeadline);
  struct PendingGrantRequest
  {
    ndn::Name objectName;
    std::string writerIdentity;
    std::string ownerIdentity;
    std::chrono::steady_clock::time_point deadline;
  };
  std::mutex grantQueueMutex;
  std::deque<std::shared_ptr<PendingGrantRequest>> grantQueue;
  auto resolvedRoleGrant = std::make_shared<std::optional<NativeDurableStorageRoleGrantV1>>();
  auto publicationCancelled = std::make_shared<std::atomic<bool>>(false);
  auto roleGrantExpiresAtMs = std::make_shared<std::atomic<std::uint64_t>>(0);
  std::optional<DurableStorageGrantChallengeV1> rejectedPublicationChallenge;
  const auto takePendingGrant = [&]() -> std::shared_ptr<PendingGrantRequest> {
    std::lock_guard<std::mutex> lock(grantQueueMutex);
    if (grantQueue.empty()) return {};
    auto pending = std::move(grantQueue.front());
    grantQueue.pop_front();
    return pending;
  };
  const auto hasPendingGrant = [&] {
    std::lock_guard<std::mutex> lock(grantQueueMutex);
    return !grantQueue.empty();
  };
  std::optional<ndn::Name> publicationChallengeDataPrefix;
  std::mutex publicationChallengeMutex;
  const auto recordPublicationChallenge = [&](const DurableStorageGrantChallengeV1& challenge) {
    std::lock_guard<std::mutex> lock(publicationChallengeMutex);
    publicationChallengeDataPrefix = challenge.dataPrefix;
    rejectedPublicationChallenge = challenge;
  };
  NativeDurableStorageOwner storageOwner{requester, grantBarrier->resolverFor(source.role),
    [&grantQueue, &grantQueueMutex, &recordPublicationChallenge, grantDeadline, requester](
        const DurableStorageGrantChallengeV1& challenge) {
      recordPublicationChallenge(challenge);
      auto pending = std::make_shared<PendingGrantRequest>();
      pending->objectName = challenge.manifestName;
      pending->writerIdentity = challenge.writerIdentity;
      pending->ownerIdentity = requester;
      pending->deadline = grantDeadline;
      std::lock_guard<std::mutex> lock(grantQueueMutex);
      grantQueue.push_back(std::move(pending));
    }};
  std::optional<NativeDurableStorageOwner> secondStorageOwner;
  if (secondGrantBarrier) {
    secondStorageOwner.emplace(NativeDurableStorageOwner{
      secondOwner, secondGrantBarrier->resolverFor(source.role),
      [&grantQueue, &grantQueueMutex, &recordPublicationChallenge,
       grantDeadline, secondOwner](const DurableStorageGrantChallengeV1& challenge) {
        recordPublicationChallenge(challenge);
        auto pending = std::make_shared<PendingGrantRequest>();
        pending->objectName = challenge.manifestName;
        pending->writerIdentity = challenge.writerIdentity;
        pending->ownerIdentity = secondOwner;
        pending->deadline = grantDeadline;
        std::lock_guard<std::mutex> lock(grantQueueMutex);
        grantQueue.push_back(std::move(pending));
      }});
  }
  auto commitGateCalls = std::make_shared<std::atomic<unsigned>>(0);
  auto commitGateObservedGrant = std::make_shared<std::atomic<bool>>(false);
  auto ownerBoundaryGate = std::make_shared<CausalBoundaryGate>();
  auto secondSourceBoundaryGate = std::make_shared<CausalBoundaryGate>();
  auto firstSourceBoundaryCalls = std::make_shared<std::atomic<unsigned>>(0);
  storageOwner.onDurablePacketSetBoundary = [commitGateCalls, commitGateObservedGrant,
                                               resolvedRoleGrant, fault, ownerBoundaryGate,
                                               capacityBoundaryCompetition,
                                               closeDuringOwnerPacketSetBoundary,
                                               switchFirstWithdrawalOverlap,
                                               withdrawalDrainTimeout,
                                               destroyOwnerClientAtBoundary,
                                                mutateOwnerPolicyAfterResolution,
                                                ownerCasFailure] {
    ++*commitGateCalls;
    commitGateObservedGrant->store(resolvedRoleGrant->has_value());
    if (fault == CausalRecoveryFault::RejectedPublicationCommitGate)
      throw std::runtime_error("Spec204 Owner closed the durable packet commit gate");
    if (capacityBoundaryCompetition || closeDuringOwnerPacketSetBoundary ||
        switchFirstWithdrawalOverlap || withdrawalDrainTimeout ||
        destroyOwnerClientAtBoundary || ownerCasFailure ||
        mutateOwnerPolicyAfterResolution)
      ownerBoundaryGate->parkOnce();
  };
  if (fault == CausalRecoveryFault::RejectedPublicationGrant) {
    storageOwner.resolveGrant = [](const ndn::Name&, const std::string&,
                                   const std::function<void()>&)
        -> ndn_service_framework::DurableStorageGrantSelector {
      throw std::runtime_error("Spec204 publication grant rejected by Owner");
    };
    storageOwner.onGrantRequired = [&recordPublicationChallenge](
        const DurableStorageGrantChallengeV1& challenge) {
      recordPublicationChallenge(challenge);
    };
  }
  else if (fault == CausalRecoveryFault::CancelledPublicationAtGrant) {
    storageOwner.onGrantRequired = [
      &recordPublicationChallenge, publicationCancelled](
        const DurableStorageGrantChallengeV1& challenge) {
      recordPublicationChallenge(challenge);
      publicationCancelled->store(true);
    };
  }
  else if (fault == CausalRecoveryFault::WrongOwnerPublicationGrant) {
    const auto resolveGrant = storageOwner.resolveGrant;
    storageOwner.resolveGrant = [resolveGrant](const ndn::Name& manifest,
        const std::string& writer, const std::function<void()>& active) {
      auto selector = resolveGrant(manifest, writer, active);
      selector.ownerIdentity = "/Spec204/WrongOwner";
      return selector;
    };
    const auto queueGrant = storageOwner.onGrantRequired;
    storageOwner.onGrantRequired = [queueGrant, &recordPublicationChallenge](
        const DurableStorageGrantChallengeV1& challenge) {
      recordPublicationChallenge(challenge);
      queueGrant(challenge);
    };
  }
  else if (fault == CausalRecoveryFault::ExpiredPublicationGrant) {
    const auto resolveGrant = storageOwner.resolveGrant;
    storageOwner.resolveGrant = [resolveGrant, roleGrantExpiresAtMs](
        const ndn::Name& manifest, const std::string& writer,
        const std::function<void()>& active) {
      auto selector = resolveGrant(manifest, writer, active);
      const auto expiresAtMs = roleGrantExpiresAtMs->load();
      if (expiresAtMs == 0)
        throw std::runtime_error("Spec204 expired-grant fixture lacks expiry");
      while (causalNow() < expiresAtMs) {
        if (active) active();
        std::this_thread::sleep_for(1ms);
      }
      return selector;
    };
  }
  const auto issuePendingGrant = [&] {
    auto pending = takePendingGrant();
    if (!pending) return false;
    const auto activeBarrier = secondGrantBarrier &&
      pending->ownerIdentity == secondOwner ? secondGrantBarrier : grantBarrier;
    auto* activeRequesterUser = secondGrantBarrier &&
      pending->ownerIdentity == secondOwner ? secondOwnerUser.get() : requesterUser;
    try {
      const auto challenges = activeBarrier->waitForAllChallenges();
      if (challenges.size() != 1 || challenges.front().roleName != source.role ||
          challenges.front().manifestName != pending->objectName ||
          challenges.front().writerIdentity != pending->writerIdentity)
        throw std::runtime_error("Spec204 complete exact Repo grant challenge mismatch");
      const auto& name = pending->objectName;
      const auto& writer = pending->writerIdentity;
      if (ndn::Name(writer) != ndn::Name(provider) || !ndn::Name(provider).isPrefixOf(name))
        throw std::runtime_error("Spec204 exact Producer storage grant mismatch");
      const auto issuedAt = causalNow();
      const auto grantId = nativePlanningDigest(name.toUri(ndn::name::UriFormat::CANONICAL));
      const auto objectName = name.toUri(ndn::name::UriFormat::ALTERNATE);
      const auto grantExpiresAtMs = issuedAt +
        (fault == CausalRecoveryFault::ExpiredPublicationGrant ? 3000 : 180000);
      roleGrantExpiresAtMs->store(grantExpiresAtMs);
      const ndnsf_distributed_repo::RepoUserStorageGrantRequest request{
        1, grantId, writer, objectName, grantExpiresAtMs};
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        pending->deadline - std::chrono::steady_clock::now());
      constexpr auto responsePumpMargin = 1000ms;
      if (remaining <= responsePumpMargin)
        throw std::runtime_error("Spec204 Repo grant request missed its shared deadline");
      auto requestTimeout = remaining - responsePumpMargin;
      if (requestTimeout > 8000ms) requestTimeout = 8000ms;
      const auto requestId = ndnsf_distributed_repo::RepoClient::requestUserStorageGrant(
        *activeRequesterUser, storageTarget, request, static_cast<int>(requestTimeout.count()),
        [activeBarrier](const ndn::Name&) {
          activeBarrier->abort(std::make_exception_ptr(
            std::runtime_error("Spec204 Repo grant request timed out")));
        },
        [activeBarrier, sourceRole = source.role, storageTarget,
         ownerIdentity = pending->ownerIdentity, request,
         resolvedRoleGrant, fault]
        (const ndn_service_framework::ResponseMessage& response) {
          if (!response.getStatus()) {
            activeBarrier->abort(std::make_exception_ptr(std::runtime_error(
              "Spec204 Repo grant request failed: " + response.getErrorInfo())));
            return;
          }
          try {
            const auto payload = response.getPayload();
            const std::string wire(payload.begin(), payload.end());
            const auto reply = ndnsf_distributed_repo::RepoUserStorageGrantReply::fromJson(wire);
            reply.validateExpected(storageTarget, ownerIdentity, request);
            const auto& grant = reply.grant;
            NativeDurableStorageRoleGrantV1 roleGrant{
              grant.repoNode, grant.ownerIdentity, grant.grantId, grant.writerIdentity,
              grant.objectName, grant.ownerEpoch};
            if (fault == CausalRecoveryFault::WrongWriterPublicationGrant)
              roleGrant.writerIdentity = "/Spec204/WrongWriter";
            else if (fault == CausalRecoveryFault::ChangedRootPublicationGrant)
              roleGrant.objectName = ndn::Name(roleGrant.objectName).append("rebound")
                .toUri(ndn::name::UriFormat::ALTERNATE);
            *resolvedRoleGrant = roleGrant;
            activeBarrier->releaseAll({{sourceRole, roleGrant}});
          }
          catch (...) { activeBarrier->abort(std::current_exception()); }
        });
      if (requestId.empty())
        activeBarrier->abort(std::make_exception_ptr(
          std::runtime_error("Spec204 Repo grant request was not admitted by ServiceUser")));
    }
    catch (...) { activeBarrier->abort(std::current_exception()); }
    return true;
  };
  auto publicationObserverCalls = std::make_shared<std::atomic<unsigned>>(0);
  if (productionHandoff) {
    BOOST_REQUIRE(sourceNativeRegistration);
    auto ownerContext = NativeConversationHandoffOwnerContextV1{};
    ownerContext.ownerEncryptionCertificate = ownerCertificate;
    ownerContext.durableStorageOwner = storageOwner;
    if (fault == CausalRecoveryFault::WithdrawalPublicationObserverFailure) {
      ownerContext.durableStorageOwner.onGrantRequired =
        [publicationObserverCalls](const DurableStorageGrantChallengeV1&) {
          ++*publicationObserverCalls;
          throw std::runtime_error("HANDOFF_PUBLICATION_OBSERVER_REJECTED");
        };
    }
    ownerContext.originalPacketStore = rangeStore;
    ownerContext.saveBatchLimits = {64, 1U << 20};
    ownerContext.portableLimits = {{1U << 20, 2U << 20, {}},
      1U << 20, protectedLimits.maxPlaintextBytes};
    ownerContext.protectedCheckpointLimits = protectedLimits;
    ownerContext.policy.enabled = true;
    ownerContext.policy.maxConcurrentEvents =
      eventCapacityCompetition ? 1 : (twoRoleCapacityCompetition ? 3 : 2);
    ownerContext.policy.maxRolesPerEvent = sourceRoles.size();
    ownerContext.policy.maxConcurrentRoleSlots =
      roleCapacityCompetition ? 1 :
      twoEventCapacityCompetition ? 3 * sourceRoles.size() :
      twoRoleCapacityCompetition ? 2 :
      controlProgressWhileRoleParked ? 2 * sourceRoles.size() : 2;
    ownerContext.policy.maxPacketCountPerRole = 64;
    ownerContext.policy.maxPacketSetWireBytesPerRole = 2U << 20;
    ownerContext.policy.maxAggregateBytes = sourceRoles.size() * (2U << 20);
    // Each role reserves at most 2 MiB of portable state and 1 MiB of
    // protected wire, so size the admission budget for the selected roles.
    ownerContext.policy.maxScratchBytesPerEvent = sourceRoles.size() * (3U << 20);
    ownerContext.policy.scratchQuotaBytes =
      twoSlotCapacityCompetition ? 24U << 20 :
      controlProgressWhileRoleParked && twoRole ? 12U << 20 : 8U << 20;
    if (twoSlotCapacityCompetition) {
      BOOST_REQUIRE_GT(ownerContext.policy.scratchQuotaBytes,
        3 * ownerContext.policy.maxScratchBytesPerEvent);
      if (twoEventCapacityCompetition)
        BOOST_REQUIRE_GE(ownerContext.policy.maxConcurrentRoleSlots,
          3 * sourceRoles.size());
      else
        BOOST_REQUIRE_GE(ownerContext.policy.maxConcurrentEvents, 3U);
    }
    ownerContext.policy.maxSaveDurationMs = 300000;
    ownerContext.policy.scratchRoot = files.root / "spec212-handoff-scratch";
    std::filesystem::create_directories(ownerContext.policy.scratchRoot);
    for (const auto& role : sourceRoles) {
      ownerContext.repoTargetByRole.emplace(role,
        NativeDurableStorageGrantRepoTargetV1{
          storageTarget.repoNode, storageTarget.providerIdentity,
          storageTarget.servicePrefix});
    }
    if (twoSource) {
      auto secondContext = ownerContext;
      ownerContext.repoTargetByRole.erase(sourceRoles.back());
      secondContext.repoTargetByRole.erase(sourceRoles.front());
      const auto firstBoundary = ownerContext.durableStorageOwner.onDurablePacketSetBoundary;
      ownerContext.durableStorageOwner.onDurablePacketSetBoundary =
        [firstBoundary, firstSourceBoundaryCalls] {
          if (firstBoundary) firstBoundary();
          ++*firstSourceBoundaryCalls;
        };
      if (fault == CausalRecoveryFault::TwoSourceCancelBeforeAllReady) {
        const auto previousBoundary = secondContext.durableStorageOwner.onDurablePacketSetBoundary;
        secondContext.durableStorageOwner.onDurablePacketSetBoundary =
          [previousBoundary, secondSourceBoundaryGate] {
            if (previousBoundary) previousBoundary();
            secondSourceBoundaryGate->parkOnce();
          };
      }
      secondHandoffOwnerContext->emplace(std::move(secondContext));
    }
    else if (multipleOwnerWithdrawal) {
      BOOST_REQUIRE(secondOwnerCertificate);
      BOOST_REQUIRE(secondStorageOwner);
      auto secondContext = ownerContext;
      secondContext.ownerEncryptionCertificate = *secondOwnerCertificate;
      secondContext.durableStorageOwner = *secondStorageOwner;
      secondContext.policy.scratchRoot = files.root / "spec212-second-owner-scratch";
      std::filesystem::create_directories(secondContext.policy.scratchRoot);
      if (postCommitSecondChildDrainTimeout) {
        const auto previousBoundary =
          secondContext.durableStorageOwner.onDurablePacketSetBoundary;
        secondContext.durableStorageOwner.onDurablePacketSetBoundary =
          [previousBoundary, secondOwnerCommitGate] {
            if (previousBoundary) previousBoundary();
            secondOwnerCommitGate->parkOnce();
          };
      }
      secondHandoffOwnerContext->emplace(std::move(secondContext));
    }
    if (fault == CausalRecoveryFault::WithdrawalMissingRepoRole) {
      BOOST_REQUIRE_EQUAL(sourceRoles.size(), 2U);
      ownerContext.repoTargetByRole.erase(sourceRoles.back());
    }
    handoffOwnerContext->emplace(std::move(ownerContext));

    repo->registerOriginalDataServing(environment->providerFace(destinationIndex));
    ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits fetchLimits;
    // The two-source parked TSAN path bootstraps another MANIFEST service
    // while both save workers are active; keep ACK collection inside the
    // unchanged absolute switch deadline rather than racing a 1s startup.
    fetchLimits.ackCollectionTimeMs = controlProgressWhileRoleParked && twoSource ? 3000 : 1000;
    NativeRemoteProtectedKvRoleTrustPolicyV1 sourceTrust;
    sourceTrust.target = storageTarget;
    sourceTrust.producerVerifierPolicyDigest = nativePlanningDigest(
      "Spec212-causal-provider-producer-trust");
    sourceTrust.verifyProducerSignature = [producerCertificate](const ndn::Data& data) {
      return ndn::security::verifySignature(data, producerCertificate);
    };
    sourceTrust.limits = fetchLimits;
    std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1> trustByRole;
    for (const auto& role : sourceRoles) {
      auto roleTrust = sourceTrust;
      if (twoSource && role == sourceRoles.back()) {
        const auto secondCertificate = environment->keyChain().getPib()
          .getIdentity(ndn::Name(secondSource)).getDefaultKey().getDefaultCertificate();
        roleTrust.verifyProducerSignature = [secondCertificate](const ndn::Data& data) {
          return ndn::security::verifySignature(data, secondCertificate);
        };
      }
      trustByRole.emplace(role, std::move(roleTrust));
    }
    std::map<std::string, std::string> destinationProviders;
    for (const auto& role : sourceRoles)
      destinationProviders.emplace(role, successor);
    if (productionHandoff) {
      const auto saveSnapshot = providerRuntime.conversationStateSnapshot();
      const auto expectedEntries =
        sourceRoles.size() * preparedConversationCount;
      BOOST_REQUIRE_EQUAL(saveSnapshot.entries,
        twoSource ? preparedConversationCount : expectedEntries);
      BOOST_REQUIRE_EQUAL(saveSnapshot.saveEligibleEntries,
        twoSource ? preparedConversationCount : expectedEntries);
      BOOST_REQUIRE_EQUAL(saveSnapshot.pinnedEntries, 0U);
      if (twoSource) {
        const auto secondSnapshot = secondProviderRuntime.conversationStateSnapshot();
        BOOST_REQUIRE_EQUAL(secondSnapshot.entries, preparedConversationCount);
        BOOST_REQUIRE_EQUAL(secondSnapshot.saveEligibleEntries, preparedConversationCount);
        BOOST_REQUIRE_EQUAL(secondSnapshot.pinnedEntries, 0U);
      }
      BOOST_TEST_MESSAGE("Spec212 source save state before switch: entries="
        << saveSnapshot.entries << "; eligible=" << saveSnapshot.saveEligibleEntries
        << "; pinned=" << saveSnapshot.pinnedEntries
        << "; committed promotions=" << saveSnapshot.promotions);
    }
    if (withdrawalAbort || withdrawalSave || withdrawalWatch ||
        withdrawalWatchNoIntent || withdrawalBudget || withdrawalForce ||
        withdrawalForceBudget || withdrawalWireBudget) {
      BOOST_REQUIRE(sourceNativeRegistration);
      std::optional<Conversation> watchedConversation;
      std::optional<NativeDepartureWatchHandle> ownerWatch;
      std::optional<Conversation> secondWatchedConversation;
      std::optional<NativeDepartureWatchHandle> secondOwnerWatch;
      CausalWatchCleanup watchCleanup{ownerWatch, secondOwnerWatch,
                                     watchedConversation, secondWatchedConversation};
      std::string secondCheckpointDigest;
      std::string secondConversationId;
      if (withdrawalWatch || withdrawalWatchNoIntent) {
        const auto packetCommitsBeforeWatch = rangeStore->packetCommitCalls();
        const auto exportsBeforeWatch =
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts;
        const auto providerRequestsBeforeWatch =
          handoffProviderRequestsObserved->load();
        NativeRequestCatalog watchCatalog;
        watchCatalog.model.descriptor = source.model.descriptor;
        watchCatalog.preparation = source.catalog;
        watchCatalog.cooperativeSplitter = splitter;
        watchCatalog.stateMapping = source.stateMapping;
        auto watchClient = std::shared_ptr<NativeInferenceClient>(
          &client, [](NativeInferenceClient*) {});
        auto watchModel = Spec212PreparedModelTestAccess::bind(
          std::move(watchCatalog), splitter,
          std::make_shared<NativePreSplitFirstPlacement>(),
          watchClient, service, policyDigest, tokenizer);
        ConversationOptions watchConversationOptions;
        watchConversationOptions.conversationId = reference.conversationId;
        watchedConversation.emplace(watchModel.openConversation(watchConversationOptions));
        NativeDepartureWatchOptionsV1 watchOptions;
        watchOptions.deadline = std::chrono::steady_clock::now() +
          (noIntentExpires ? 5s : 45s);
        watchOptions.pollInterval = 100ms;
        watchOptions.queryTimeout = 4s;
        watchOptions.saveTimeout = 25s;
        watchOptions.trustedSourcePolicyByRole = trustByRole;
        ownerWatch = watchedConversation->watchProviderDeparture(watchOptions);
        BOOST_REQUIRE(ownerWatch->valid());
        if (twoConversationWithdrawalWatch) {
          const auto secondRecord = conversations->find("spec212-capacity-competing");
          BOOST_REQUIRE(secondRecord);
          secondCheckpointDigest = secondRecord->checkpoint.checkpointDigest;
          secondConversationId = "spec212-capacity-competing";
          watchConversationOptions.conversationId = secondConversationId;
          secondWatchedConversation.emplace(
            watchModel.openConversation(watchConversationOptions));
          secondOwnerWatch = secondWatchedConversation->watchProviderDeparture(watchOptions);
          BOOST_REQUIRE(secondOwnerWatch->valid());
        }
        if (twoNonemptyServiceWithdrawal && !postCommitChildFailure) {
          BOOST_REQUIRE(secondServiceClient);
          secondConversationId = "spec212-second-service-conversation";
          secondCheckpointDigest = secondServiceCheckpointDigest;
          auto secondCandidate = candidate;
          secondCandidate.executionPlan.serviceName = secondService;
          secondCandidate.candidateDigest = secondCandidate.computedDigest();
          auto secondSplitter = std::make_shared<CausalSplitter>(secondCandidate);
          NativeRequestCatalog secondWatchCatalog;
          secondWatchCatalog.model.descriptor = source.model.descriptor;
          secondWatchCatalog.preparation = source.catalog;
          secondWatchCatalog.cooperativeSplitter = secondSplitter;
          secondWatchCatalog.stateMapping = source.stateMapping;
          auto secondWatchClient = std::shared_ptr<NativeInferenceClient>(
            secondServiceClient.get(), [](NativeInferenceClient*) {});
          auto secondWatchModel = Spec212PreparedModelTestAccess::bind(
            std::move(secondWatchCatalog), secondSplitter,
            std::make_shared<NativePreSplitFirstPlacement>(), secondWatchClient,
            secondService, policyDigest, tokenizer);
          watchConversationOptions.conversationId = secondConversationId;
          secondWatchedConversation.emplace(
            secondWatchModel.openConversation(watchConversationOptions));
          secondOwnerWatch = secondWatchedConversation->watchProviderDeparture(watchOptions);
          BOOST_REQUIRE(secondOwnerWatch->valid());
        }
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoffProviderRequestsObserved->load() > providerRequestsBeforeWatch;
        });
        BOOST_CHECK(ownerWatch->status() == NativeDepartureWatchStatusV1::Watching);
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeWatch);
        if (withdrawalWatchNoIntent) {
          if (fault == CausalRecoveryFault::HostStopWithoutWithdrawal)
            sourceHost->stop();
          if (fault == CausalRecoveryFault::NoIntentTransportDisconnect)
            environment->disconnectProviderTransportForTest(0);
          if (noIntentExpires) {
            // A fixture pump has a finite iteration budget shorter than this
            // watch deadline; keep driving Faces until expiry is observable.
            for (unsigned round = 0; round < 3 &&
                 ownerWatch->status() != NativeDepartureWatchStatusV1::Expired; ++round) {
              environment->pumpUntilWithAttributeAuthority([&] {
                return ownerWatch->status() == NativeDepartureWatchStatusV1::Expired;
              });
            }
          }
          else {
            BOOST_REQUIRE(ownerWatch->cancel());
            environment->pumpUntilWithAttributeAuthority([&] {
              return ownerWatch->status() == NativeDepartureWatchStatusV1::Cancelled;
            });
          }
          const auto watchResult = ownerWatch->result(0ms);
          BOOST_CHECK_EQUAL(watchResult.reason, noIntentExpires ? "NO_POSITIVE_INTENT" : "CANCELLED");
          BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeWatch);
          if (fault != CausalRecoveryFault::HostStopWithoutWithdrawal)
            BOOST_CHECK(!sourceNativeRegistration->closed());
          watchedConversation->close();
          if (fault != CausalRecoveryFault::HostStopWithoutWithdrawal)
            closeSourceRegistration();
          BOOST_CHECK(drainSourceRegistration());
          BOOST_CHECK(sourceNativeRegistration->closed());
          BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeWatch);
          BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), 0U);
          BOOST_CHECK_EQUAL(rangeStore->packetCount(), 0U);
          BOOST_CHECK_EQUAL(rangeStore->packetWireBytes(), 0U);
          const auto usageAfterClose = journal->userStorageUsage(requester, repoName);
          BOOST_CHECK_EQUAL(usageAfterClose.committedBytes, usageBeforeFirst.committedBytes);
          BOOST_CHECK_EQUAL(usageAfterClose.reservedBytes, usageBeforeFirst.reservedBytes);
          BOOST_CHECK_EQUAL(journal->listUserStorage(requester, repoName, "", 1000).size(),
                            claimsBeforeFirst);
          BOOST_CHECK_EQUAL(
            providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
            exportsBeforeWatch);
          const auto unchanged = conversations->find(reference.conversationId);
          BOOST_REQUIRE(unchanged);
          BOOST_CHECK(!unchanged->protectedKvReferences);
          BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
          BOOST_CHECK(drainClient.close());
          return {};
        }
      }
      NativeWithdrawalOptionsV1 options;
      options.deadline = std::chrono::steady_clock::now() +
        (withdrawalSave || withdrawalWatch || withdrawalWireBudget ? 35s : 10s);
      options.drainDeadline = options.deadline +
        (withdrawalDrainTimeout ? 1s : 5s);
      options.maxConversations =
        (twoConversationWithdrawalWatch || twoNonemptyServiceWithdrawal) ? 2 : 1;
      options.maxTotalBytes = 1024 * 1024;
      options.persistencePolicyId = "spec212-causal-withdrawal-policy";
      if (withdrawalForce || withdrawalForceBudget)
        options.failurePolicy = NativeWithdrawalFailurePolicyV1::ForceUnsaved;
      if (withdrawalBudget || withdrawalForceBudget || withdrawalWireBudget) {
        auto probeFence = providerRuntime.beginServiceWithdrawalSaveFence(
          profile.serviceName.toUri(), "withdrawal-budget-probe",
          sourceNativeRegistration->generation());
        BOOST_REQUIRE(probeFence);
        auto probe = providerRuntime.tryAcquireCommittedSaveBatch(
          *probeFence, {64, 1024 * 1024}, 1);
        BOOST_REQUIRE(probe.status == ConversationSaveBatchStatusV1::Ready);
        BOOST_REQUIRE(probe.batch);
        BOOST_CHECK_GT(probe.totalBytes, probe.metadataBytes);
        options.maxTotalBytes = withdrawalWireBudget
          ? probe.totalBytes : probe.metadataBytes;
        probe.batch.reset();
        probeFence.reset();
      }
      const auto packetCommitsBefore = rangeStore->packetCommitCalls();
      const auto successfulCommitsBefore = rangeStore->successfulPacketCommitCalls();
      std::shared_ptr<CausalRepoCommitGate> overlapCommitGate;
      if (withdrawalOverlapSwitch) {
        overlapCommitGate = std::make_shared<CausalRepoCommitGate>();
        rangeStore->armCommitGate(overlapCommitGate);
      }
      const auto exportAttemptsBefore =
        providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts;
      std::optional<NativeConversationHandoffOwnerContextV1> withheldOwnerContext;
      if (withdrawalOwnerUnavailable) {
        BOOST_REQUIRE(*handoffOwnerContext);
        withheldOwnerContext.emplace(**handoffOwnerContext);
        handoffOwnerContext->reset();
      }
      if (withdrawalWatch)
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      auto droppedSecondConfirmation = std::make_shared<std::atomic<unsigned>>(0);
      auto secondConfirmationNames = std::make_shared<std::vector<ndn::Name>>();
      auto secondConfirmationMutex = std::make_shared<std::mutex>();
      auto blockedSecondConfirmationRequestId =
        std::make_shared<std::optional<ndn::Name>>();
      if (multiOwnerLostConfirm) {
        BOOST_REQUIRE(secondOwnerDroppingUser);
        secondOwnerDroppingUser->setResponseDropPredicate(
          [secondServiceConversations, secondConversationId,
           droppedSecondConfirmation, secondConfirmationNames,
           secondConfirmationMutex, blockedSecondConfirmationRequestId,
           provider = ndn::Name(provider),
           secondOwner = ndn::Name(secondOwner),
           service = ndn::Name(secondService)](
              const ndn::svs::SVSPubSub::SubscriptionData& subscription) {
            const auto response = parseResponseNameV2(subscription.name);
            if (!response || response->providerName != provider ||
                response->requesterName != secondOwner ||
                response->serviceName != service)
              return false;
            const auto promoted = secondServiceConversations->find(secondConversationId);
            if (!promoted || !promoted->protectedKvReferences)
              return false;
            std::lock_guard<std::mutex> lock(*secondConfirmationMutex);
            secondConfirmationNames->push_back(subscription.name);
            if (!*blockedSecondConfirmationRequestId)
              *blockedSecondConfirmationRequestId = response->requestId;
            if (response->requestId != **blockedSecondConfirmationRequestId)
              return false;
            ++*droppedSecondConfirmation;
            return true;
          });
      }
      std::string withdrawalCheckpointDigest = reference.checkpointDigest;
      RequestHandle appendBeforeWithdrawal;
      CausalBoundaryRelease releasePostCommit{postCommitGate};
      if (withdrawalAfterCommittedAppend) {
        const auto delta = causalInput(source.model.descriptor, {6}, tokenizer, 1);
        RequestOptions appendOptions;
        appendOptions.timeout = 30s;
        appendOptions.ackTimeout = 3s;
        appendOptions.outputMode = "TOKEN_STREAMING";
        appendOptions.stream = StreamOptions{};
        appendOptions.generation = GenerationOptions{1};
        postCommitAction->store(1);
        appendBeforeWithdrawal = watchedConversation->request(
          Input::inlineBytes(delta.payload, delta.options), appendOptions);
        const auto appendDeadline = std::chrono::steady_clock::now() + 30s;
        while (!postCommitGate->waitUntilEntered(0ms) &&
               appendBeforeWithdrawal.status() == RequestStatus::Pending &&
               std::chrono::steady_clock::now() < appendDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return postCommitGate->waitUntilEntered(0ms) ||
              appendBeforeWithdrawal.status() != RequestStatus::Pending ||
              std::chrono::steady_clock::now() >= appendDeadline;
          });
        BOOST_REQUIRE_MESSAGE(postCommitGate->waitUntilEntered(0ms),
          "second APPEND did not reach the Owner durable commit boundary");
        BOOST_CHECK(appendBeforeWithdrawal.status() == RequestStatus::Pending);
      }
      auto withdrawal = providerScopeWithdrawal
        ? sourceHost->withdraw(options)
        : sourceNativeRegistration->withdraw(options);
      BOOST_REQUIRE(withdrawal.valid());
      if (fault == CausalRecoveryFault::WithdrawalPublicationObserverFailure) {
        const auto failureDeadline = std::chrono::steady_clock::now() + 10s;
        while (withdrawal.status() != NativeWithdrawalStatusV1::Aborted &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < failureDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return withdrawal.status() == NativeWithdrawalStatusV1::Aborted ||
                   withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
                   std::chrono::steady_clock::now() >= failureDeadline;
          });
        BOOST_REQUIRE_MESSAGE(
          withdrawal.status() == NativeWithdrawalStatusV1::Aborted ||
          withdrawal.status() == NativeWithdrawalStatusV1::Failed,
          "publication observer failure did not settle before the save deadline");
        const auto outcome = withdrawal.result(0ms);
        BOOST_CHECK_GT(publicationObserverCalls->load(), 0U);
        BOOST_CHECK_NE(outcome.reason.find("HANDOFF_PUBLICATION_OBSERVER_REJECTED"),
                       std::string::npos);
        BOOST_CHECK(!outcome.closed);
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        const auto retainedState = providerRuntime.conversationStateSnapshot();
        BOOST_CHECK_EQUAL(retainedState.saveEligibleEntries, 1U);
        BOOST_CHECK_EQUAL(retainedState.pinnedEntries, 0U);
        const auto unchanged = conversations->find(reference.conversationId);
        BOOST_REQUIRE(unchanged);
        BOOST_CHECK(!unchanged->protectedKvReferences);
        BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
        BOOST_REQUIRE(ownerWatch);
        const auto ownerFailureDeadline = std::chrono::steady_clock::now() + 8s;
        while ((ownerWatch->status() == NativeDepartureWatchStatusV1::Watching ||
                ownerWatch->status() == NativeDepartureWatchStatusV1::Saving) &&
               std::chrono::steady_clock::now() < ownerFailureDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return ownerWatch->status() != NativeDepartureWatchStatusV1::Saving ||
                   std::chrono::steady_clock::now() >= ownerFailureDeadline;
          });
        BOOST_CHECK_MESSAGE(ownerWatch->status() == NativeDepartureWatchStatusV1::Failed,
          "Owner watch did not receive the Provider publication failure");
        if (ownerWatch) ownerWatch->close();
        if (watchedConversation) watchedConversation->close();
        const bool drained = drainClient.close();
        BOOST_CHECK_MESSAGE(drained,
          "publication failure cleanup clientDrained=" << drainClient.clientDrained <<
          " registrationDrained=" << drainClient.registrationDrained);
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        return {};
      }
      if (fault == CausalRecoveryFault::WithdrawalMissingRepoRole) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return withdrawal.status() == NativeWithdrawalStatusV1::Aborted;
        });
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::Aborted);
        const auto outcome = withdrawal.result(0ms);
        BOOST_CHECK_EQUAL(outcome.reason, "WITHDRAWAL_REPO_ROLE_SET_MISMATCH");
        BOOST_CHECK(!outcome.closed);
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK_EQUAL(
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          exportAttemptsBefore);
        const auto unchanged = conversations->find(reference.conversationId);
        BOOST_REQUIRE(unchanged);
        BOOST_CHECK(!unchanged->protectedKvReferences);
        BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
        if (ownerWatch) ownerWatch->close();
        if (watchedConversation) watchedConversation->close();
        BOOST_CHECK(drainClient.close());
        return {};
      }
      if (postCommitSecondChildDrainTimeout) {
        CausalBoundaryRelease releaseSecondBoundary{secondOwnerCommitGate};
        while (!secondOwnerCommitGate->waitUntilEntered(0ms) &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return secondOwnerCommitGate->waitUntilEntered(0ms) ||
              withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
              std::chrono::steady_clock::now() >= options.drainDeadline;
          });
        BOOST_REQUIRE_MESSAGE(secondOwnerCommitGate->waitUntilEntered(0ms),
          "second Owner save did not reach its pre-COMMIT boundary");
        while (ownerWatch->status() != NativeDepartureWatchStatusV1::Saved &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return ownerWatch->status() == NativeDepartureWatchStatusV1::Saved ||
              std::chrono::steady_clock::now() >= options.drainDeadline;
          });
        BOOST_REQUIRE(ownerWatch->status() == NativeDepartureWatchStatusV1::Saved);
        BOOST_CHECK_GT(rangeStore->successfulPacketCommitCalls(),
                       successfulCommitsBefore);
        BOOST_REQUIRE(secondOwnerWatch);
        BOOST_CHECK(secondOwnerWatch->status() != NativeDepartureWatchStatusV1::Saved);
        const auto firstSaved = conversations->find(reference.conversationId);
        const auto secondUnsaved = secondServiceConversations->find(secondConversationId);
        BOOST_REQUIRE(firstSaved);
        BOOST_REQUIRE(secondUnsaved);
        BOOST_CHECK(firstSaved->protectedKvReferences.has_value());
        BOOST_CHECK(!secondUnsaved->protectedKvReferences);
        while (withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < options.drainDeadline + 5s)
          environment->pumpUntilWithAttributeAuthority([&] {
            return withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
              std::chrono::steady_clock::now() >= options.drainDeadline + 5s;
          });
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::Failed);
        const auto outcome = withdrawal.result(0ms);
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT timeout reason=" << outcome.reason);
        BOOST_CHECK_EQUAL(outcome.reason, "DRAIN_TIMEOUT");
        BOOST_CHECK(!outcome.closed);
        BOOST_REQUIRE_GE(outcome.conversationResults.size(), 1U);
        BOOST_REQUIRE_LE(outcome.conversationResults.size(), 2U);
        bool firstConfirmed = false;
        for (const auto& row : outcome.conversationResults) {
          if (row.conversationId == reference.conversationId) {
            BOOST_CHECK_EQUAL(row.classification, "CONFIRMED");
            firstConfirmed = true;
          }
          else {
            BOOST_CHECK_EQUAL(row.conversationId, secondConversationId);
            BOOST_CHECK_EQUAL(row.classification, "FAILED");
          }
        }
        BOOST_CHECK(firstConfirmed);
        BOOST_CHECK(!withdrawal.cancel());
        BOOST_CHECK(sourceNativeRegistration->closed());
        BOOST_CHECK(!emptySourceNativeRegistration->closed());
        auto conflictingFence = secondServiceRuntime.beginConversationSaveFence(
          secondOwner, secondService, secondConversationId);
        BOOST_CHECK(!conflictingFence);
        const auto commitsAtTimeout = rangeStore->successfulPacketCommitCalls();
        auto changed = options;
        changed.deadline = std::chrono::steady_clock::now() + 10s;
        changed.drainDeadline = changed.deadline + 5s;
        ++changed.maxTotalBytes;
        BOOST_CHECK_EXCEPTION(sourceHost->withdraw(changed), std::logic_error,
          [](const std::logic_error& error) {
            return std::string(error.what()) == "WITHDRAWAL_OPERATION_CONFLICT";
          });
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: release second boundary");
        secondOwnerCommitGate->release();
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: close watches");
        secondOwnerWatch->close();
        ownerWatch->close();
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: close conversations");
        secondWatchedConversation->close();
        watchedConversation->close();
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: drain second client");
        BOOST_CHECK(secondServiceDrain->close());
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: drain source client");
        BOOST_CHECK(drainClient.close());
        BOOST_TEST_MESSAGE("Spec212 post-COMMIT cleanup: drained");
        BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), commitsAtTimeout);
        const auto secondAfterCleanup = secondServiceConversations->find(secondConversationId);
        BOOST_REQUIRE(secondAfterCleanup);
        BOOST_CHECK(!secondAfterCleanup->protectedKvReferences);
        return {};
      }
      if (withdrawalAfterCommittedAppend) {
        postCommitGate->release();
        const auto appendDeadline = std::chrono::steady_clock::now() + 30s;
        while (appendBeforeWithdrawal.status() == RequestStatus::Pending &&
               std::chrono::steady_clock::now() < appendDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return appendBeforeWithdrawal.status() != RequestStatus::Pending ||
              std::chrono::steady_clock::now() >= appendDeadline;
          });
        BOOST_REQUIRE(appendBeforeWithdrawal.status() == RequestStatus::Succeeded);
        const auto committed = conversations->find(reference.conversationId);
        BOOST_REQUIRE(committed);
        BOOST_CHECK_NE(committed->checkpoint.checkpointDigest,
                       reference.checkpointDigest);
        withdrawalCheckpointDigest = committed->checkpoint.checkpointDigest;
      }
      if (withdrawalOverlapSwitch) {
        while (!overlapCommitGate->waitUntilEntered(0ms) &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return overlapCommitGate->waitUntilEntered(0ms) ||
              withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
              std::chrono::steady_clock::now() >= options.drainDeadline;
          });
        const bool commitEntered = overlapCommitGate->waitUntilEntered(0ms);
        if (commitEntered) {
          BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                            successfulCommitsBefore);
          BOOST_CHECK_EXCEPTION(
            watchedConversation->switchProviders(destinationProviders, trustByRole,
                                                  options.drainDeadline),
            NativeDiError,
            [](const NativeDiError& error) {
              return error.code() == "CONVERSATION_HANDOFF_CONFLICT";
            });
          BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                            successfulCommitsBefore);
        }
        overlapCommitGate->release();
        BOOST_REQUIRE_MESSAGE(commitEntered,
          "Owner withdrawal save did not reach the Repo COMMIT boundary");
      }
      if (withdrawalDrainTimeout) {
        CausalBoundaryRelease releaseBoundary{ownerBoundaryGate};
        while (!ownerBoundaryGate->waitUntilEntered(0ms) &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return ownerBoundaryGate->waitUntilEntered(0ms) ||
              withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
              std::chrono::steady_clock::now() >= options.drainDeadline;
          });
        const bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
        if (boundaryEntered) {
          while (withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
                 std::chrono::steady_clock::now() < options.drainDeadline + 3s)
            environment->pumpUntilWithAttributeAuthority([&] {
              return withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
                std::chrono::steady_clock::now() >= options.drainDeadline + 3s;
            });
          if (withdrawal.status() == NativeWithdrawalStatusV1::Failed) {
            const auto outcome = withdrawal.result(0ms);
            BOOST_CHECK_EQUAL(outcome.reason, "DRAIN_TIMEOUT");
            BOOST_CHECK(!outcome.closed);
            BOOST_CHECK(!sourceNativeRegistration->closed());
            BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                              successfulCommitsBefore);
            auto conflictingFence = providerRuntime.beginConversationSaveFence(
              requester, service, reference.conversationId);
            BOOST_CHECK(!conflictingFence);
            auto changed = options;
            changed.deadline = std::chrono::steady_clock::now() + 10s;
            changed.drainDeadline = changed.deadline + 1s;
            ++changed.maxTotalBytes;
            BOOST_CHECK_THROW(sourceNativeRegistration->withdraw(changed),
                              std::logic_error);
          }
        }
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(boundaryEntered,
          "withdrawal save did not reach the Owner packet-set boundary");
        BOOST_CHECK(withdrawal.status() == NativeWithdrawalStatusV1::Failed);
        if (ownerWatch) ownerWatch->close();
        if (watchedConversation) watchedConversation->close();
        closeSourceRegistration();
        auto sourceDrain = std::async(std::launch::async, [&] {
          return drainSourceRegistration();
        });
        const auto sourceDrainDeadline = std::chrono::steady_clock::now() + 15s;
        while (sourceDrain.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < sourceDrainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return sourceDrain.wait_for(0ms) == std::future_status::ready ||
              std::chrono::steady_clock::now() >= sourceDrainDeadline;
          });
        BOOST_CHECK(sourceDrain.get());
        BOOST_CHECK(drainClient.close());
        BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                          successfulCommitsBefore);
        return {};
      }
      if (postCommitChildFailure) {
        const auto failureDeadline = options.drainDeadline + 5s;
        while (withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < failureDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return withdrawal.status() == NativeWithdrawalStatusV1::Failed ||
              std::chrono::steady_clock::now() >= failureDeadline;
          });
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::Failed);
        const auto outcome = withdrawal.result(0ms);
        BOOST_CHECK(!outcome.closed);
        BOOST_CHECK(!outcome.forced);
        BOOST_CHECK_EQUAL(outcome.reason, "POSTCOMMIT_SECOND_CHILD_FAILED");
        BOOST_REQUIRE_EQUAL(outcome.conversationResults.size(), 1U);
        BOOST_CHECK_EQUAL(outcome.conversationResults.front().conversationId,
                          reference.conversationId);
        BOOST_CHECK_EQUAL(outcome.conversationResults.front().classification,
                          "CONFIRMED");
        BOOST_CHECK_GT(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK(!withdrawal.cancel());
        BOOST_CHECK(sourceNativeRegistration->closed());
        BOOST_CHECK(!emptySourceNativeRegistration->closed());
        auto conflictingRetry = options;
        conflictingRetry.deadline = std::chrono::steady_clock::now() + 10s;
        conflictingRetry.drainDeadline = conflictingRetry.deadline + 5s;
        BOOST_CHECK_EXCEPTION(sourceHost->withdraw(conflictingRetry), std::logic_error,
          [](const std::logic_error& error) {
            return std::string(error.what()) == "WITHDRAWAL_OPERATION_CONFLICT";
          });
        BOOST_REQUIRE(ownerWatch);
        BOOST_CHECK(ownerWatch->status() == NativeDepartureWatchStatusV1::Saved);
        const auto saved = conversations->find(reference.conversationId);
        BOOST_REQUIRE(saved);
        BOOST_REQUIRE(saved->protectedKvReferences);
        const auto unsaved = secondServiceConversations->find(
          "spec212-second-service-conversation");
        BOOST_REQUIRE(unsaved);
        BOOST_CHECK(!unsaved->protectedKvReferences);
        ownerWatch->close();
        watchedConversation->close();
        BOOST_CHECK(secondServiceDrain->close());
        BOOST_CHECK(drainClient.close());
        return {};
      }
      if (withdrawalForceBudget) {
        const auto forceWaitDeadline = options.drainDeadline + 5s;
        while (withdrawal.status() != NativeWithdrawalStatusV1::ForcedUnsaved &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < forceWaitDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved ||
                   withdrawal.status() == NativeWithdrawalStatusV1::Failed;
          });
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved ||
                      withdrawal.status() == NativeWithdrawalStatusV1::Failed);
        const auto outcome = withdrawal.result(0ms);
        BOOST_REQUIRE_MESSAGE(
          withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved,
          "budget force withdrawal stopped: " << outcome.reason);
        BOOST_CHECK(outcome.closed);
        BOOST_CHECK(outcome.forced);
        BOOST_CHECK(outcome.scope == (providerScopeWithdrawal
          ? NativeWithdrawalScopeV1::Provider
          : NativeWithdrawalScopeV1::Service));
        BOOST_REQUIRE_EQUAL(outcome.conversationResults.size(), 1U);
        BOOST_CHECK_EQUAL(outcome.conversationResults.front().classification,
                          "FORCED_UNSAVED");
        BOOST_CHECK_EQUAL(outcome.conversationResults.front().reason,
                          "BUDGET_EXCEEDED");
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK_EQUAL(
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          exportAttemptsBefore);
        BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().pinnedEntries, 0U);
        BOOST_CHECK(sourceNativeRegistration->closed());
        BOOST_CHECK(sourceNativeRegistration->drain(10s));
        BOOST_CHECK(drainClient.close());
        return {};
      }
      if (withdrawalBudget || withdrawalOwnerUnavailable) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return withdrawal.status() == NativeWithdrawalStatusV1::Aborted;
        });
        const auto outcome = withdrawal.result(5s);
        BOOST_CHECK_EQUAL(outcome.reason, withdrawalOwnerUnavailable
          ? "OWNER_UNAVAILABLE" : "BUDGET_EXCEEDED");
        BOOST_CHECK(!outcome.closed);
        BOOST_CHECK(outcome.scope == (providerScopeWithdrawal
          ? NativeWithdrawalScopeV1::Provider
          : NativeWithdrawalScopeV1::Service));
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK_EQUAL(
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          exportAttemptsBefore);
        BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().pinnedEntries, 0U);
        if (withheldOwnerContext)
          handoffOwnerContext->emplace(std::move(*withheldOwnerContext));
      }
      else {
      if (!withdrawalOverlapSwitch && !withdrawalAfterCommittedAppend) {
        const auto readyDeadline = std::chrono::steady_clock::now() + 5s;
        while (withdrawal.status() == NativeWithdrawalStatusV1::Quiescing &&
               std::chrono::steady_clock::now() < readyDeadline)
          std::this_thread::sleep_for(10ms);
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::AwaitingOwners);
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      }
      if (withdrawalWatch) {
        while ((ownerWatch->status() == NativeDepartureWatchStatusV1::Watching ||
                ownerWatch->status() == NativeDepartureWatchStatusV1::Saving ||
                (secondOwnerWatch &&
                 (secondOwnerWatch->status() == NativeDepartureWatchStatusV1::Watching ||
                  secondOwnerWatch->status() == NativeDepartureWatchStatusV1::Saving))) &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return ownerWatch->status() == NativeDepartureWatchStatusV1::Saved &&
              (!secondOwnerWatch ||
               secondOwnerWatch->status() == NativeDepartureWatchStatusV1::Saved);
          });
        if (withdrawalAfterCommittedAppend &&
            ownerWatch->status() != NativeDepartureWatchStatusV1::Saved) {
          const auto watchStatus = ownerWatch->status();
          BOOST_TEST_MESSAGE("Spec212 new-checkpoint watch status=" <<
            static_cast<int>(watchStatus) << " withdrawal status=" <<
            static_cast<int>(withdrawal.status()) << " repo commits=" <<
            rangeStore->packetCommitCalls());
          if (watchStatus != NativeDepartureWatchStatusV1::Watching &&
              watchStatus != NativeDepartureWatchStatusV1::Saving)
            BOOST_TEST_MESSAGE("Spec212 new-checkpoint watch reason=" <<
              ownerWatch->result(0ms).reason);
          const auto withdrawalStatus = withdrawal.status();
          if (withdrawalStatus == NativeWithdrawalStatusV1::Aborted ||
              withdrawalStatus == NativeWithdrawalStatusV1::Failed ||
              withdrawalStatus == NativeWithdrawalStatusV1::Closed)
            BOOST_TEST_MESSAGE("Spec212 new-checkpoint withdrawal reason=" <<
              withdrawal.result(0ms).reason);
        }
        BOOST_REQUIRE(ownerWatch->status() == NativeDepartureWatchStatusV1::Saved);
        const auto watchResult = ownerWatch->result(0ms);
        BOOST_CHECK_EQUAL(watchResult.conversationId, reference.conversationId);
        BOOST_CHECK_EQUAL(watchResult.checkpointDigest, withdrawalCheckpointDigest);
        BOOST_CHECK_EQUAL(watchResult.providerIdentity, provider);
        if (twoNonemptyServiceWithdrawal || providerScopeWithdrawal) {
          BOOST_CHECK(!watchResult.withdrawalId.empty());
          BOOST_CHECK_NE(watchResult.withdrawalId, withdrawal.operationId());
        }
        else
          BOOST_CHECK_EQUAL(watchResult.withdrawalId, withdrawal.operationId());
        BOOST_CHECK(!watchResult.saveOperationId.empty());
        const auto promoted = conversations->find(reference.conversationId);
        BOOST_REQUIRE(promoted);
        BOOST_REQUIRE(promoted->protectedKvReferences);
        BOOST_CHECK_EQUAL(promoted->protectedKvReferences->size(), sourceRoles.size());
        BOOST_CHECK(promoted->nextTurnProvidersByRole.empty());
        std::string secondWithdrawalId;
        std::string secondSaveOperationId;
        if (secondOwnerWatch) {
          if (secondOwnerWatch->status() != NativeDepartureWatchStatusV1::Saved) {
            const auto watchStatus = secondOwnerWatch->status();
            BOOST_TEST_MESSAGE("Spec212 second Owner watch status="
              << static_cast<int>(watchStatus) << "; host status="
              << static_cast<int>(withdrawal.status()) << "; packet commits="
              << rangeStore->packetCommitCalls());
            if (watchStatus == NativeDepartureWatchStatusV1::Failed ||
                watchStatus == NativeDepartureWatchStatusV1::Expired ||
                watchStatus == NativeDepartureWatchStatusV1::Cancelled)
              BOOST_TEST_MESSAGE("Spec212 second Owner watch reason="
                << secondOwnerWatch->result(0ms).reason);
          }
          BOOST_REQUIRE(secondOwnerWatch->status() == NativeDepartureWatchStatusV1::Saved);
          const auto secondResult = secondOwnerWatch->result(0ms);
          BOOST_CHECK_EQUAL(secondResult.conversationId, secondConversationId);
          BOOST_CHECK_EQUAL(secondResult.checkpointDigest, secondCheckpointDigest);
          BOOST_CHECK_EQUAL(secondResult.providerIdentity, provider);
          secondWithdrawalId = secondResult.withdrawalId;
          if (twoNonemptyServiceWithdrawal) {
            BOOST_CHECK(!secondWithdrawalId.empty());
            BOOST_CHECK_NE(secondWithdrawalId, withdrawal.operationId());
            BOOST_CHECK_NE(secondWithdrawalId, watchResult.withdrawalId);
          }
          else
            BOOST_CHECK_EQUAL(secondWithdrawalId, withdrawal.operationId());
          BOOST_CHECK(!secondResult.saveOperationId.empty());
          secondSaveOperationId = secondResult.saveOperationId;
          const auto secondPromoted = twoNonemptyServiceWithdrawal
            ? secondServiceConversations->find(secondConversationId)
            : conversations->find(secondConversationId);
          BOOST_REQUIRE(secondPromoted);
          BOOST_REQUIRE(secondPromoted->protectedKvReferences);
          BOOST_CHECK_EQUAL(secondPromoted->protectedKvReferences->size(), sourceRoles.size());
          BOOST_CHECK(secondPromoted->nextTurnProvidersByRole.empty());
          if (multiOwnerLostConfirm) {
            secondOwnerDroppingUser->setResponseDropPredicate({});
            BOOST_CHECK_GE(droppedSecondConfirmation->load(), 1U);
            std::lock_guard<std::mutex> lock(*secondConfirmationMutex);
            BOOST_REQUIRE_GE(secondConfirmationNames->size(), 2U);
            BOOST_REQUIRE(*blockedSecondConfirmationRequestId);
            bool freshRequestIdSeen = false;
            for (const auto& name : *secondConfirmationNames) {
              const auto response = parseResponseNameV2(name);
              BOOST_REQUIRE(response);
              if (response->requestId != **blockedSecondConfirmationRequestId)
                freshRequestIdSeen = true;
            }
            BOOST_CHECK(freshRequestIdSeen);
            BOOST_CHECK_EQUAL(
              secondServiceRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
              1U);
          }
        }
        BOOST_CHECK_GT(rangeStore->packetCommitCalls(), packetCommitsBefore);
        while ((!sourceNativeRegistration->closed() ||
                (twoNonemptyServiceWithdrawal &&
                 !emptySourceNativeRegistration->closed())) &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return sourceNativeRegistration->closed() &&
              (!twoNonemptyServiceWithdrawal || emptySourceNativeRegistration->closed());
          });
        BOOST_REQUIRE(sourceNativeRegistration->closed());
        if (twoNonemptyServiceWithdrawal)
          BOOST_REQUIRE(emptySourceNativeRegistration->closed());
        const auto withdrawalResult = withdrawal.result(5s);
        BOOST_CHECK(withdrawalResult.closed);
        BOOST_CHECK_EQUAL(withdrawalResult.reason, "CONFIRMED");
        BOOST_CHECK_EQUAL(withdrawalResult.conversationResults.size(),
                          (twoConversationWithdrawalWatch || twoNonemptyServiceWithdrawal)
                            ? 2U : 1U);
        if (twoNonemptyServiceWithdrawal) {
          BOOST_CHECK_EQUAL(withdrawalResult.withdrawalId, withdrawal.operationId());
          for (const auto& row : withdrawalResult.conversationResults) {
            BOOST_CHECK_EQUAL(row.classification, "CONFIRMED");
            if (row.conversationId == reference.conversationId) {
              BOOST_CHECK_EQUAL(row.ownerIdentity, requester);
              BOOST_CHECK_EQUAL(row.operationId, watchResult.saveOperationId);
            }
            else if (row.conversationId == secondConversationId) {
              BOOST_CHECK_EQUAL(row.ownerIdentity, secondOwner);
              BOOST_CHECK_EQUAL(row.operationId, secondSaveOperationId);
            }
            else
              BOOST_CHECK_MESSAGE(false, "unexpected conversation in host withdrawal result");
          }
        }
        if (secondOwnerWatch)
          secondOwnerWatch->close();
        if (secondWatchedConversation)
          secondWatchedConversation->close();
        ownerWatch->close();
        if (appendAfterWithdrawal) {
          BOOST_REQUIRE(promoted->nextTurnProvidersByRole.empty());
          BOOST_REQUIRE(drainSourceRegistration());
          std::filesystem::create_directory(files.root / "spec212-recovery-B");
          BOOST_REQUIRE_EQUAL(::chmod((files.root / "spec212-recovery-B").c_str(), 0700), 0);
          auto destinationConfig = makeConfig(destinationIndex, successorFactory);
          destinationConfig.kvRecovery = NativeProviderKvRecoveryConfigV1{
            files.root / "spec212-recovery-B", {ndn::Name(repoName)},
            protectedLimits, portableLimits, 2U << 20};
          auto destinationRuntime = makeNativeProviderCollaborationRuntime(
            std::move(destinationConfig));
          auto destinationRegistration = installHandler(destinationIndex, destinationRuntime);
          const auto delta = causalInput(source.model.descriptor, {6}, tokenizer, 1);
          RequestOptions appendOptions;
          appendOptions.timeout = 30s;
          appendOptions.ackTimeout = 3s;
          appendOptions.outputMode = "TOKEN_STREAMING";
          appendOptions.stream = StreamOptions{};
          appendOptions.generation = GenerationOptions{1};
          BOOST_REQUIRE(appendOptions.providerNames.empty());
          const auto append = watchedConversation->request(
            Input::inlineBytes(delta.payload, delta.options), appendOptions);
          const auto appendDeadline = std::chrono::steady_clock::now() + 35s;
          while (append.status() == RequestStatus::Pending &&
                 std::chrono::steady_clock::now() < appendDeadline)
            environment->pumpUntilWithAttributeAuthority([&] {
              return append.status() != RequestStatus::Pending;
            });
          if (append.status() != RequestStatus::Succeeded) {
            try { (void)append.result(0ms); }
            catch (const std::exception& error) {
              BOOST_TEST_MESSAGE("Spec212 post-departure APPEND error=" << error.what());
            }
          }
          BOOST_REQUIRE(append.status() == RequestStatus::Succeeded);
          BOOST_CHECK(!append.result(0ms).payload.empty());
          BOOST_REQUIRE_EQUAL(successorFactory->created().size(), 1U);
          const auto runner = successorFactory->created().front().lock();
          BOOST_REQUIRE(runner);
          const auto metrics = runner->runtimeMetricsSnapshot();
          BOOST_REQUIRE(metrics);
          BOOST_CHECK(metrics->workCounterCertified);
          BOOST_CHECK_EQUAL(metrics->executedPrefillTokens, 1U);
          BOOST_CHECK_EQUAL(metrics->executedDecodeTokens, 1U);
          BOOST_CHECK_EQUAL(metrics->stateInputMisses, 0U);
          BOOST_CHECK_GE(metrics->stateInputHits, 3U);
          const auto completed = conversations->find(reference.conversationId);
          BOOST_REQUIRE(completed);
          BOOST_CHECK(completed->checkpoint.providersByRole == destinationProviders);
          BOOST_CHECK_GT(completed->checkpoint.successorContextEpoch,
                         promoted->checkpoint.successorContextEpoch);
          BOOST_CHECK(sourceNativeRegistration->closed());
          const auto waitTargetDrain = destinationRegistration.makeTargetedHandlerDrainWaiter();
          destinationRegistration.close();
          BOOST_REQUIRE(waitTargetDrain(10s));
          destinationRuntime = {};
        }
        watchedConversation->close();
        if (secondServiceDrain)
          BOOST_CHECK(secondServiceDrain->close());
        BOOST_CHECK(drainClient.close());
        return {};
      }
      const auto queryDeadline = std::chrono::steady_clock::now() + 4s;
      auto query = std::async(std::launch::async, [&] {
        return client.queryProviderWithdrawalIntent(
          reference.conversationId, reference.checkpointDigest, provider,
          [producerCertificate](const ndn::Data& data) {
            return ndn::security::verifySignature(data, producerCertificate);
          }, queryDeadline);
      });
      environment->pumpUntilWithAttributeAuthority([&] {
        return query.wait_for(0ms) == std::future_status::ready;
      });
      const auto intentReply = query.get();
      BOOST_REQUIRE(intentReply.status == NativeWithdrawalQueryStatusV1::Intent);
      BOOST_REQUIRE(intentReply.intent);
      BOOST_CHECK_EQUAL(intentReply.intent->ownerIdentity, requester);
      BOOST_CHECK_EQUAL(intentReply.intent->conversationId, reference.conversationId);
      BOOST_CHECK_EQUAL(intentReply.intent->checkpointDigest, reference.checkpointDigest);
      if (providerScopeWithdrawal)
        BOOST_CHECK_NE(intentReply.intent->withdrawalId, withdrawal.operationId());
      else
        BOOST_CHECK_EQUAL(intentReply.intent->withdrawalId, withdrawal.operationId());
      BOOST_CHECK_EQUAL(intentReply.intent->localRoles.size(), 1U);
      BOOST_CHECK(!intentReply.originalProofWire.empty());
      if (withdrawalWireBudget) {
        const auto saveDeadline = std::chrono::steady_clock::now() + 25s;
        auto save = client.saveConversationForWithdrawal(
          reference.conversationId, reference.checkpointDigest,
          record->checkpoint.providersByRole, trustByRole, provider,
          intentReply, saveDeadline);
        environment->pumpUntilWithAttributeAuthority([&] {
          return save.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= saveDeadline;
        });
        BOOST_REQUIRE(save.status() == ConversationHandoffStatus::Failed);
        environment->pumpUntilWithAttributeAuthority([&] {
          return withdrawal.status() == (partialHostWithdrawal
                   ? NativeWithdrawalStatusV1::Failed
                   : NativeWithdrawalStatusV1::Aborted) ||
            std::chrono::steady_clock::now() >= options.drainDeadline;
        });
        BOOST_REQUIRE(withdrawal.status() == (partialHostWithdrawal
          ? NativeWithdrawalStatusV1::Failed
          : NativeWithdrawalStatusV1::Aborted));
        const auto outcome = withdrawal.result(0ms);
        BOOST_CHECK_EQUAL(outcome.reason, "BUDGET_EXCEEDED");
        BOOST_CHECK(!outcome.closed);
        BOOST_CHECK(outcome.scope == (providerScopeWithdrawal
          ? NativeWithdrawalScopeV1::Provider
          : NativeWithdrawalScopeV1::Service));
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK_GT(
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          exportAttemptsBefore);
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().pinnedEntries, 0U);
        if (partialHostWithdrawal) {
          BOOST_REQUIRE(emptySourceNativeRegistration);
          BOOST_CHECK(emptySourceNativeRegistration->closed());
          BOOST_CHECK(!withdrawal.cancel());
          auto replay = sourceHost->withdraw(options);
          BOOST_CHECK_EQUAL(replay.operationId(), withdrawal.operationId());
          auto changed = options;
          ++changed.maxTotalBytes;
          BOOST_CHECK_THROW(sourceHost->withdraw(changed), std::logic_error);
          closeSourceRegistration();
          BOOST_CHECK(drainSourceRegistration());
          BOOST_CHECK(drainClient.close());
          return {};
        }
      }
      else {
      if (withdrawalForce) {
        const auto forceWaitDeadline = options.drainDeadline + 5s;
        while (withdrawal.status() != NativeWithdrawalStatusV1::ForcedUnsaved &&
               withdrawal.status() != NativeWithdrawalStatusV1::Failed &&
               std::chrono::steady_clock::now() < forceWaitDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved ||
                   withdrawal.status() == NativeWithdrawalStatusV1::Failed;
          });
        BOOST_REQUIRE(withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved ||
                      withdrawal.status() == NativeWithdrawalStatusV1::Failed);
        const auto outcome = withdrawal.result(0ms);
        BOOST_REQUIRE_MESSAGE(
          withdrawal.status() == NativeWithdrawalStatusV1::ForcedUnsaved,
          "force withdrawal stopped: " << outcome.reason);
        BOOST_CHECK(outcome.closed);
        BOOST_CHECK(outcome.forced);
        BOOST_CHECK(outcome.scope == NativeWithdrawalScopeV1::Service);
        BOOST_CHECK_EQUAL(outcome.conversationResults.size(), 1U);
        BOOST_CHECK_EQUAL(outcome.conversationResults.front().classification,
                          "FORCED_UNSAVED");
        BOOST_CHECK(!outcome.conversationResults.front().reason.empty());
        BOOST_CHECK(sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
        BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().pinnedEntries, 0U);
        BOOST_CHECK(sourceNativeRegistration->drain(10s));
        BOOST_CHECK(drainClient.close());
        return {};
      }
      if (withdrawalSave) {
        const auto saveDeadline = std::chrono::steady_clock::now() + 25s;
        const auto repoUsageBeforeWithdrawal = journal->userStorageUsage(requester, repoName);
        const auto repoClaimsBeforeWithdrawal =
          journal->listUserStorage(requester, repoName, "", 1000);
        std::shared_ptr<CausalRepoCommitGate> pendingCommitGate;
        auto droppedConfirmation = std::make_shared<std::atomic<unsigned>>(0);
        auto droppedConfirmationName = std::make_shared<std::string>();
        auto droppedConfirmationMutex = std::make_shared<std::mutex>();
        auto confirmationResponseNames = std::make_shared<std::vector<ndn::Name>>();
        if (withdrawalLostConfirm) {
          environment->setResponseDropPredicateForTest(
            [conversations, droppedConfirmation, droppedConfirmationName,
             droppedConfirmationMutex, confirmationResponseNames,
             conversationId = reference.conversationId,
             producer = ndn::Name(provider)](
                const ndn::svs::SVSPubSub::SubscriptionData& subscription) {
            if (!producer.isPrefixOf(subscription.name) ||
                !parseResponseNameV2(subscription.name))
              return false;
            const auto promoted = conversations->find(conversationId);
            if (!promoted || !promoted->protectedKvReferences)
              return false;
            {
              std::lock_guard<std::mutex> lock(*droppedConfirmationMutex);
              confirmationResponseNames->push_back(subscription.name);
            }
            unsigned expected = 0;
            if (!droppedConfirmation->compare_exchange_strong(expected, 1))
              return false;
            std::lock_guard<std::mutex> lock(*droppedConfirmationMutex);
            *droppedConfirmationName = subscription.name.toUri();
            return true;
          });
        }
        if (providerScopeWithdrawal) {
          pendingCommitGate = std::make_shared<CausalRepoCommitGate>();
          rangeStore->armCommitGate(pendingCommitGate);
        }
        auto save = client.saveConversationForWithdrawal(
          reference.conversationId, reference.checkpointDigest,
          record->checkpoint.providersByRole, trustByRole, provider,
          intentReply, saveDeadline);
        if (pendingCommitGate) {
          environment->pumpUntilWithAttributeAuthority([&] {
            return pendingCommitGate->waitUntilEntered(0ms) ||
              save.status() != ConversationHandoffStatus::Pending ||
              std::chrono::steady_clock::now() >= saveDeadline;
          });
          const bool commitEntered = pendingCommitGate->waitUntilEntered(0ms);
          if (commitEntered) {
            BOOST_CHECK(withdrawal.status() == NativeWithdrawalStatusV1::Saving);
            BOOST_CHECK(!withdrawal.cancel());
          }
          pendingCommitGate->release();
          BOOST_REQUIRE_MESSAGE(commitEntered,
            "Provider withdrawal did not reach the first Repo packet commit");
        }
        do {
          environment->pumpUntilWithAttributeAuthority([&] {
            return save.status() != ConversationHandoffStatus::Pending;
          });
        } while (save.status() == ConversationHandoffStatus::Pending &&
                 std::chrono::steady_clock::now() < saveDeadline);
        if (withdrawalLostConfirm) {
          BOOST_TEST_MESSAGE("Spec212 lost-confirm save status=" <<
            static_cast<int>(save.status()) << " drops=" <<
            droppedConfirmation->load() << " withdrawal=" <<
            static_cast<int>(withdrawal.status()));
          if (save.status() != ConversationHandoffStatus::Succeeded) {
            try { (void)save.result(0ms); }
            catch (const NativeDiError& error) {
              BOOST_TEST_MESSAGE("Spec212 lost-confirm save error=" <<
                error.code() << ":" << error.what());
            }
            catch (const std::exception& error) {
              BOOST_TEST_MESSAGE("Spec212 lost-confirm save exception=" << error.what());
            }
            environment->setResponseDropPredicateForTest({});
            closeSourceRegistration();
            BOOST_CHECK(drainSourceRegistration());
            BOOST_CHECK(drainClient.close());
          }
        }
        BOOST_REQUIRE(save.status() == ConversationHandoffStatus::Succeeded);
        BOOST_CHECK_NO_THROW(save.result(0ms));
        if (fault == CausalRecoveryFault::None && sourceRoles.size() == 1 &&
            !providerScopeWithdrawal && !withdrawalLostConfirm &&
            !twoNonemptyServiceWithdrawal && !multipleOwnerWithdrawal) {
          BOOST_REQUIRE_EQUAL(repoUsageBeforeWithdrawal.committedBytes, 0U);
          BOOST_REQUIRE_EQUAL(repoUsageBeforeWithdrawal.reservedBytes, 0U);
          BOOST_REQUIRE(repoClaimsBeforeWithdrawal.empty());

          std::optional<DurableStorageGrantChallengeV1> committedChallenge;
          {
            std::lock_guard<std::mutex> lock(publicationChallengeMutex);
            committedChallenge = rejectedPublicationChallenge;
          }
          BOOST_REQUIRE(committedChallenge.has_value());
          const auto claims = journal->listUserStorage(requester, repoName, "", 1000);
          BOOST_REQUIRE_EQUAL(claims.size(), 1U);
          const auto& claim = claims.front();
          BOOST_CHECK(claim.state == ndnsf_distributed_repo::RepoUserStorageClaimState::Committed);
          BOOST_CHECK_EQUAL(claim.repoNode, repoName);
          BOOST_CHECK_EQUAL(claim.ownerIdentity, requester);
          BOOST_CHECK_EQUAL(claim.object.objectName,
            committedChallenge->manifestName.toUri(ndn::name::UriFormat::ALTERNATE));
          BOOST_CHECK_EQUAL(claim.object.payloadWireBytes, committedChallenge->packetWireBytes);
          BOOST_CHECK_GT(claim.object.manifestBytes, 0U);

          const auto repoUsageAfterWithdrawal = journal->userStorageUsage(requester, repoName);
          BOOST_CHECK_EQUAL(repoUsageAfterWithdrawal.reservedBytes, 0U);
          BOOST_CHECK_EQUAL(repoUsageAfterWithdrawal.committedBytes,
            committedChallenge->packetWireBytes + claim.object.manifestBytes);
        }
        if (withdrawalLostConfirm) {
          environment->setResponseDropPredicateForTest({});
          BOOST_CHECK_EQUAL(droppedConfirmation->load(), 1U);
          std::lock_guard<std::mutex> lock(*droppedConfirmationMutex);
          BOOST_CHECK(parseResponseNameV2(ndn::Name(*droppedConfirmationName)));
          BOOST_REQUIRE_GE(confirmationResponseNames->size(), 2U);
          const auto firstReply = parseResponseNameV2(
            confirmationResponseNames->front());
          const auto retryReply = parseResponseNameV2(
            confirmationResponseNames->at(1));
          BOOST_REQUIRE(firstReply && retryReply);
          BOOST_CHECK(firstReply->providerName == ndn::Name(provider));
          BOOST_CHECK(firstReply->serviceName == profile.serviceName);
          BOOST_CHECK(firstReply->requesterName == ndn::Name(requester));
          BOOST_CHECK(firstReply->requestId != retryReply->requestId);
        }
        const auto promoted = conversations->find(reference.conversationId);
        BOOST_REQUIRE(promoted);
        BOOST_REQUIRE(promoted->protectedKvReferences);
        BOOST_CHECK_EQUAL(promoted->protectedKvReferences->size(), sourceRoles.size());
        BOOST_CHECK(promoted->nextTurnProvidersByRole.empty());
        BOOST_CHECK_GT(rangeStore->packetCommitCalls(), packetCommitsBefore);
        NativeJson journalBody{{"checkpoint",
          nativeConversationBase64Encode(promoted->checkpoint.wire)},
          {"references", *promoted->protectedKvReferences}};
        NativeWithdrawalConfirmV1 repeatedConfirm;
        repeatedConfirm.withdrawalId = intentReply.intent->withdrawalId;
        repeatedConfirm.registrationGeneration =
          intentReply.intent->registrationGeneration;
        repeatedConfirm.conversationId = reference.conversationId;
        repeatedConfirm.checkpointDigest = reference.checkpointDigest;
        repeatedConfirm.operationId = save.operationId();
        repeatedConfirm.completeRoleSetDigest = nativePlanningDigest(
          nativeCanonicalJson(*promoted->protectedKvReferences));
        repeatedConfirm.journalDigest = nativePlanningDigest(
          nativeConversationCanonicalJson(journalBody));
        repeatedConfirm.deadlineMs = intentReply.intent->deadlineMs;
        const auto repeatedWire = nativeConversationCanonicalJson(
          nativeWithdrawalConfirmV1ToJson(repeatedConfirm));
        ndn_service_framework::RequestMessage repeatedRequest;
        ndn_service_framework::RequestCapabilities repeatedCapabilities;
        repeatedCapabilities.setField("RequestScopedConfidentialityV1", "required");
        repeatedCapabilities.setField("NativeConversationHandoffV2", "required");
        repeatedRequest.setRequestCapabilities(std::move(repeatedCapabilities));
        ndn::Buffer repeatedPayload(repeatedWire.begin(), repeatedWire.end());
        repeatedRequest.setPayload(repeatedPayload, repeatedPayload.size());
        bool repeatedDone = false;
        bool repeatedTimedOut = false;
        ndn_service_framework::ResponseMessage repeatedResponse;
        const auto repeatedRequestName = requesterUser->RequestServiceTargeted(
          ndn::Name(provider), profile.serviceName, std::move(repeatedRequest), 4000,
          [&] (const ndn::Name&) {
            repeatedTimedOut = true;
            repeatedDone = true;
          },
          [&] (const ndn_service_framework::ResponseMessage& response) {
            repeatedResponse = response;
            repeatedDone = true;
          });
        BOOST_REQUIRE(!repeatedRequestName.empty());
        environment->pumpUntilWithAttributeAuthority([&] { return repeatedDone; });
        BOOST_REQUIRE(repeatedDone);
        BOOST_REQUIRE(!repeatedTimedOut);
        BOOST_REQUIRE(repeatedResponse.getStatus());
        const auto repeatedReplyPayload = repeatedResponse.getPayload();
        const auto repeatedReply = nativeWithdrawalConfirmReplyV1FromJson(
          std::string(repeatedReplyPayload.begin(), repeatedReplyPayload.end()));
        BOOST_CHECK(nativeWithdrawalConfirmV1ToJson(repeatedReply.confirm) ==
                    nativeWithdrawalConfirmV1ToJson(repeatedConfirm));
        while (!sourceNativeRegistration->closed() &&
               std::chrono::steady_clock::now() < options.drainDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return sourceNativeRegistration->closed();
          });
        BOOST_REQUIRE(sourceNativeRegistration->closed());
        const auto withdrawalResult = withdrawal.result(5s);
        BOOST_CHECK(withdrawalResult.closed);
        BOOST_CHECK_EQUAL(withdrawalResult.reason, "CONFIRMED");
        BOOST_CHECK(withdrawalResult.scope == (providerScopeWithdrawal
          ? NativeWithdrawalScopeV1::Provider
          : NativeWithdrawalScopeV1::Service));
        BOOST_CHECK(withdrawal.status() == NativeWithdrawalStatusV1::Closed);
        BOOST_CHECK(sourceNativeRegistration->drain(10s));
        BOOST_CHECK(drainClient.close());
        return {};
      }
      BOOST_REQUIRE(withdrawal.cancel());
      const auto withdrawalResult = withdrawal.result(5s);
      BOOST_CHECK_EQUAL(withdrawalResult.reason, "CANCELLED");
      BOOST_CHECK(!withdrawalResult.closed);
      BOOST_CHECK(withdrawal.status() == NativeWithdrawalStatusV1::Aborted);
      if (providerScopeWithdrawal) {
        auto retry = sourceHost->withdraw(options);
        BOOST_CHECK_NE(retry.operationId(), withdrawal.operationId());
        BOOST_REQUIRE(retry.cancel());
        const auto retried = retry.result(5s);
        BOOST_CHECK_EQUAL(retried.reason, "CANCELLED");
        BOOST_CHECK(!retried.closed);
      }
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().saveEligibleEntries,
                        1U);
      }
      }
    }
    if (destroyOwnerClientAtBoundary) {
      auto ownerClient = std::make_shared<NativeInferenceClient>(
        user, source.catalog->adapters(), runtime, conversations,
        source.catalog->makePreparation(user, service), admission);
      const auto packetCommitsBefore = rangeStore->packetCommitCalls();
      const auto handoffDeadline = std::chrono::steady_clock::now() + 45s;
      const auto handoff = ownerClient->switchConversationProviders(
        reference.conversationId, reference.checkpointDigest,
        record->checkpoint.providersByRole, destinationProviders, trustByRole,
        handoffDeadline);
      struct ReleaseBoundary
      {
        std::shared_ptr<CausalBoundaryGate> gate;
        ~ReleaseBoundary() { gate->release(); }
      } releaseBoundary{ownerBoundaryGate};
      bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      while (!boundaryEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < handoffDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= handoffDeadline;
        });
        boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      }
      BOOST_REQUIRE_MESSAGE(boundaryEntered,
        "handoff did not reach the Owner packet-set boundary before client destruction");
      auto destroy = std::async(std::launch::async,
        [clientToDestroy = std::move(ownerClient)]() mutable {
          clientToDestroy.reset();
        });
      const bool destroyedWhileBoundaryParked =
        destroy.wait_for(1s) == std::future_status::ready;
      BOOST_CHECK_MESSAGE(destroyedWhileBoundaryParked,
        "NativeInferenceClient destruction waited for a worker blocked on the Face");
      const auto cancelDeadline = std::chrono::steady_clock::now() + 10s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < cancelDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= cancelDeadline;
        });
      }
      ownerBoundaryGate->release();
      destroy.get();
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Cancelled);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      BOOST_CHECK(drainClient.close());
      return {};
    }
    const auto providerRequestsBeforeHandoff = handoffProviderRequestsObserved->load();
    const auto providerEncryptCountBeforeHandoff =
      environment->provider().getHybridCryptoCounters().symmetric_encrypt_count.load(
        std::memory_order_relaxed);
    auto& providerSvsCore = environment->providerPubSub().getSVSync().getCore();
    const auto providerSvsNode = providerSvsCore.getNodeId();
    const auto providerSvsSeqBeforeHandoff = providerSvsCore.getSeqNo(providerSvsNode);
    const auto userProviderSvsSeqBeforeHandoff = environment->userPubSub()
      .getSVSync().getCore().getSeqNo(providerSvsNode);
    std::size_t userPublicationCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*observedResponseNamesMutex);
      userPublicationCountBeforeHandoff = observedResponseNames->size();
    }
    std::size_t providerLifecycleBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*handoffProviderLifecycleMutex);
      providerLifecycleBeforeHandoff = handoffProviderLifecycle->size();
    }
    std::size_t providerSyncInterestCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*providerFaceSyncInterestNamesMutex);
      providerSyncInterestCountBeforeHandoff = providerFaceSyncInterestNames->size();
    }
    std::size_t providerSyncDataCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*providerFaceSyncDataNamesMutex);
      providerSyncDataCountBeforeHandoff = providerFaceSyncDataNames->size();
    }
    std::size_t providerPacketCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*providerFacePublishedPacketNamesMutex);
      providerPacketCountBeforeHandoff = providerFacePublishedPacketNames->size();
    }
    std::size_t userResponseInterestCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*userResponseInterestNamesMutex);
      userResponseInterestCountBeforeHandoff = userResponseInterestNames->size();
    }
    std::size_t userProviderPacketInterestCountBeforeHandoff = 0;
    {
      std::lock_guard<std::mutex> lock(*userProviderPacketInterestNamesMutex);
      userProviderPacketInterestCountBeforeHandoff = userProviderPacketInterestNames->size();
    }
    std::shared_ptr<NativeInferenceClient> publicClient;
    std::optional<DrainStandaloneClient> publicClientDrain;
    std::filesystem::path ownerJournalPath = files.root / "conversation" /
      "spec204-causal-owner" / "journal.jsonl";
    std::filesystem::path isolatedOwnerJournalPath;
    std::uintmax_t ownerJournalBytesBeforeHandoff = 0;
    if (fault == CausalRecoveryFault::OwnerJournalQuotaFailure) {
      const auto isolatedRoot = files.root / "spec212-owner-journal-quota";
      std::filesystem::create_directory(isolatedRoot);
      BOOST_REQUIRE_EQUAL(::chmod(isolatedRoot.c_str(), 0700), 0);
      const auto isolatedIdentityRoot = isolatedRoot / "spec204-causal-owner";
      std::filesystem::create_directory(isolatedIdentityRoot);
      BOOST_REQUIRE_EQUAL(::chmod(isolatedIdentityRoot.c_str(), 0700), 0);
      isolatedOwnerJournalPath = isolatedIdentityRoot / "journal.jsonl";
      ownerJournalBytesBeforeHandoff = std::filesystem::file_size(ownerJournalPath);
      std::filesystem::copy_file(ownerJournalPath, isolatedOwnerJournalPath);
      NativeConversationConfig reopenedConversation;
      reopenedConversation.journal = std::make_shared<NativeConversationJournal>(
        NativeConversationJournalConfig{isolatedRoot, "spec204-causal-owner",
          {{"key", std::vector<std::uint8_t>(32, 0x5a)}},
          static_cast<std::size_t>(ownerJournalBytesBeforeHandoff + 1), true});
      reopenedConversation.requesterIdentity = requester;
      reopenedConversation.serviceName = service;
      reopenedConversation.securityDomainDigest = policyDigest;
      conversations = std::make_shared<NativeConversationCoordinator>(
        std::move(reopenedConversation));
      BOOST_REQUIRE(conversations->find(reference.conversationId));
      publicClient = std::make_shared<NativeInferenceClient>(
        user, source.catalog->adapters(), runtime, conversations,
        source.catalog->makePreparation(user, service), admission);
      publicClientDrain.emplace(*publicClient, *environment);
    }
    else {
      publicClient = std::shared_ptr<NativeInferenceClient>(
        &client, [](NativeInferenceClient*) {});
    }
    NativeRequestCatalog publicCatalog;
    publicCatalog.model.descriptor = source.model.descriptor;
    publicCatalog.preparation = source.catalog;
    publicCatalog.cooperativeSplitter = splitter;
    publicCatalog.stateMapping = source.stateMapping;
    auto publicModel = Spec212PreparedModelTestAccess::bind(
      std::move(publicCatalog), splitter,
      std::make_shared<NativePreSplitFirstPlacement>(),
      publicClient, service, policyDigest, tokenizer);
    ConversationOptions conversationOptions;
    conversationOptions.conversationId = reference.conversationId;
    auto publicConversation = publicModel.openConversation(conversationOptions);
    auto repoCommitGate = std::make_shared<CausalRepoCommitGate>();
    if (fault == CausalRecoveryFault::CloseDuringRepoCommit)
      rangeStore->armCommitGate(repoCommitGate);
    if (fault == CausalRecoveryFault::FailSecondRepoPacketCommit)
      rangeStore->failSecondFuturePacketCommit();
#if defined(__clang__)
    constexpr bool threadSanitizerProfile = __has_feature(thread_sanitizer);
#else
    constexpr bool threadSanitizerProfile = false;
#endif
    const auto parkedControlBudget = threadSanitizerProfile ? 180s : 90s;
    const auto productiveControlBudget = threadSanitizerProfile ? 90s : 45s;
    if (fault == CausalRecoveryFault::ControlProgressWhileRoleParked) {
      BOOST_TEST_MESSAGE((threadSanitizerProfile ?
        "Spec212PositiveControlProfile=tsan parked=180 competitor=90" :
        "Spec212PositiveControlProfile=normal-asan parked=90 competitor=45"));
    }
    const auto publicSwitchDeadline = std::chrono::steady_clock::now() +
      (fault == CausalRecoveryFault::ControlProgressWhileRoleParked ? parkedControlBudget :
       fault == CausalRecoveryFault::CloseDuringRepoCommit || twoSlotCapacityCompetition ||
       controlProgressWhileRoleParked ? 90s : 45s);
    const auto successfulPacketCommitsBeforeHandoff =
      rangeStore->successfulPacketCommitCalls();
    const auto packetCommitsBeforeHandoff = rangeStore->packetCommitCalls();
    const auto repoUsageBeforeHandoff = journal->userStorageUsage(requester, repoName);
    const auto handoff = publicConversation.switchProviders(
      destinationProviders, trustByRole, publicSwitchDeadline);
    const auto duplicateHandoff = publicConversation.switchProviders(
      destinationProviders, trustByRole, publicSwitchDeadline);
    BOOST_CHECK_EQUAL(duplicateHandoff.operationId(), handoff.operationId());
    auto conflictingProviders = destinationProviders;
    conflictingProviders[source.role] = ndn::Name(
      "/Spec204/ConflictingProvider").toUri(ndn::name::UriFormat::ALTERNATE);
    bool conflictingSwitchRejected = false;
    try {
      static_cast<void>(publicConversation.switchProviders(
        conflictingProviders, trustByRole, publicSwitchDeadline));
    }
    catch (const std::exception& error) {
      conflictingSwitchRejected = std::string(error.what()).find(
        "different Provider switch is already unsettled") != std::string::npos;
    }
    BOOST_CHECK(conflictingSwitchRejected);
    const auto issuePendingGrant = [&] {
      auto pending = takePendingGrant();
      if (!pending) return false;
      // The public handoff path owns a distinct grant barrier and the native
      // Owner client obtains/submits grants from the challenge over the
      // handoff control protocol.  This observer queue only lets the fixture
      // validate the callback; waiting on the manually composed publication
      // barrier here can never be satisfied and stalls the Face pump.
      if (productionHandoff) {
        const auto writerName = ndn::Name(pending->writerIdentity);
        const bool knownWriter = writerName == ndn::Name(provider) ||
          (twoSource && writerName == ndn::Name(secondSource));
        if (!knownWriter || pending->objectName.empty() ||
            !writerName.isPrefixOf(pending->objectName))
          throw std::runtime_error("Spec212 public handoff grant challenge mismatch");
        return true;
      }
      try {
        const auto challenges = grantBarrier->waitForAllChallenges();
        if (challenges.size() != 1 || challenges.front().roleName != source.role ||
            challenges.front().manifestName != pending->objectName ||
            challenges.front().writerIdentity != pending->writerIdentity)
          throw std::runtime_error("Spec204 complete exact Repo grant challenge mismatch");
        const auto& name = pending->objectName;
        const auto& writer = pending->writerIdentity;
        if (ndn::Name(writer) != ndn::Name(provider) || !ndn::Name(provider).isPrefixOf(name))
          throw std::runtime_error("Spec204 exact Producer storage grant mismatch");
        const auto issuedAt = causalNow();
        const auto grantId = nativePlanningDigest(name.toUri(ndn::name::UriFormat::CANONICAL));
        const auto objectName = name.toUri(ndn::name::UriFormat::ALTERNATE);
        const auto grantExpiresAtMs = issuedAt +
          (fault == CausalRecoveryFault::ExpiredPublicationGrant ? 3000 : 180000);
        roleGrantExpiresAtMs->store(grantExpiresAtMs);
        const ndnsf_distributed_repo::RepoUserStorageGrantRequest request{
          1, grantId, writer, objectName, grantExpiresAtMs};
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          pending->deadline - std::chrono::steady_clock::now());
        constexpr auto responsePumpMargin = 1000ms;
        if (remaining <= responsePumpMargin)
          throw std::runtime_error("Spec204 Repo grant request missed its shared deadline");
        auto requestTimeout = remaining - responsePumpMargin;
        if (requestTimeout > 8000ms) requestTimeout = 8000ms;
        const auto requestId = ndnsf_distributed_repo::RepoClient::requestUserStorageGrant(
          *requesterUser, storageTarget, request, static_cast<int>(requestTimeout.count()),
          [grantBarrier](const ndn::Name&) {
            grantBarrier->abort(std::make_exception_ptr(
              std::runtime_error("Spec204 Repo grant request timed out")));
          },
          [grantBarrier, sourceRole = source.role, storageTarget, requester, request,
           resolvedRoleGrant, fault]
          (const ndn_service_framework::ResponseMessage& response) {
            if (!response.getStatus()) {
              grantBarrier->abort(std::make_exception_ptr(std::runtime_error(
                "Spec204 Repo grant request failed: " + response.getErrorInfo())));
              return;
            }
            try {
              const auto payload = response.getPayload();
              const std::string wire(payload.begin(), payload.end());
              const auto reply = ndnsf_distributed_repo::RepoUserStorageGrantReply::fromJson(wire);
              reply.validateExpected(storageTarget, requester, request);
              const auto& grant = reply.grant;
              NativeDurableStorageRoleGrantV1 roleGrant{
                grant.repoNode, grant.ownerIdentity, grant.grantId, grant.writerIdentity,
                grant.objectName, grant.ownerEpoch};
              if (fault == CausalRecoveryFault::WrongWriterPublicationGrant)
                roleGrant.writerIdentity = "/Spec204/WrongWriter";
              else if (fault == CausalRecoveryFault::ChangedRootPublicationGrant)
                roleGrant.objectName = ndn::Name(roleGrant.objectName).append("rebound")
                  .toUri(ndn::name::UriFormat::ALTERNATE);
              *resolvedRoleGrant = roleGrant;
              grantBarrier->releaseAll({{sourceRole, roleGrant}});
            }
            catch (...) { grantBarrier->abort(std::current_exception()); }
          });
        if (requestId.empty())
          grantBarrier->abort(std::make_exception_ptr(
            std::runtime_error("Spec204 Repo grant request was not admitted by ServiceUser")));
      }
      catch (...) { grantBarrier->abort(std::current_exception()); }
      return true;
    };
    std::optional<std::future<bool>> ownerBoundarySourceDrain;
    if (switchFirstWithdrawalOverlap) {
      const auto boundaryDeadline = std::chrono::steady_clock::now() + 45s;
      while (!ownerBoundaryGate->waitUntilEntered(0ms) &&
             handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < boundaryDeadline) {
        (void)issuePendingGrant();
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) || hasPendingGrant() ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= boundaryDeadline;
        });
      }
      const bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      if (boundaryEntered) {
        BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                          successfulPacketCommitsBeforeHandoff);
        const auto exportsBeforeWithdrawal =
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts;
        NativeWithdrawalOptionsV1 options;
        options.deadline = std::chrono::steady_clock::now() + 20s;
        options.drainDeadline = options.deadline + 5s;
        options.maxConversations = 1;
        options.maxTotalBytes = 1024 * 1024;
        options.persistencePolicyId = "spec212-causal-withdrawal-policy";
        bool rejected = false;
        try {
          auto withdrawal = sourceNativeRegistration->withdraw(options);
          (void)withdrawal.cancel();
        }
        catch (const std::logic_error& error) {
          rejected = std::string(error.what()) == "WITHDRAWAL_SCOPE_BUSY";
        }
        BOOST_CHECK_MESSAGE(rejected,
          "withdrawal must not acquire a service fence during an active user switch");
        BOOST_CHECK(!sourceNativeRegistration->closed());
        BOOST_CHECK_EQUAL(
          providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          exportsBeforeWithdrawal);
        BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                          successfulPacketCommitsBeforeHandoff);
      }
      ownerBoundaryGate->release();
      BOOST_REQUIRE_MESSAGE(boundaryEntered,
        "user switch did not reach the Owner packet-set boundary");
    }
    if (fault == CausalRecoveryFault::TwoSourceCancelBeforeAllReady) {
      const auto boundaryDeadline = std::chrono::steady_clock::now() + 35s;
      const auto bothReachedBoundary = [&] {
        return firstSourceBoundaryCalls->load() == 1U &&
          secondSourceBoundaryGate->waitUntilEntered(0ms);
      };
      while (!bothReachedBoundary() &&
             handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < boundaryDeadline) {
        (void)issuePendingGrant();
        environment->pumpUntilWithAttributeAuthority([&] {
          return bothReachedBoundary() || hasPendingGrant() ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= boundaryDeadline;
        });
      }
      const bool boundaryReached = bothReachedBoundary();
      const auto packetCallsAtBoundary = rangeStore->packetCommitCalls();
      const bool cancelled = boundaryReached && handoff.cancel();
      secondSourceBoundaryGate->release();
      BOOST_CHECK_MESSAGE(boundaryReached,
        "both source Providers did not reach the coordinated precommit boundary");
      BOOST_CHECK_EQUAL(packetCallsAtBoundary, packetCommitsBeforeHandoff);
      BOOST_CHECK(cancelled);
      const auto settleDeadline = std::chrono::steady_clock::now() + 20s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < settleDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending;
        });
      }
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Cancelled);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Cancelled);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeHandoff);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulPacketCommitsBeforeHandoff);
      const auto repoUsageAfterCancel = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_EQUAL(repoUsageAfterCancel.committedBytes,
                        repoUsageBeforeHandoff.committedBytes);
      const auto unchanged = conversations->find(reference.conversationId);
      BOOST_REQUIRE(unchanged);
      BOOST_CHECK_EQUAL(unchanged->checkpoint.wire, record->checkpoint.wire);
      BOOST_CHECK(!unchanged->protectedKvReferences);
      BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
      BOOST_CHECK(drainClient.close());
      return {};
    }
    if (closeDuringOwnerPacketSetBoundary) {
      const auto boundaryDeadline = std::chrono::steady_clock::now() + 45s;
      bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      while (!boundaryEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < boundaryDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= boundaryDeadline;
        });
        boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      }
      if (!boundaryEntered) {
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(false,
          "production handoff did not park at the Owner packet-set boundary");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      struct OwnerBoundaryRelease
      {
        std::shared_ptr<CausalBoundaryGate> gate;
        ~OwnerBoundaryRelease() { gate->release(); }
      } releaseOwnerBoundary{ownerBoundaryGate};
      BOOST_CHECK_EQUAL(commitGateCalls->load(), 1U);
      const auto packetCommitsBefore = rangeStore->packetCommitCalls();
      closeSourceRegistration();
      ownerBoundarySourceDrain.emplace(std::async(std::launch::async, [&] {
        return drainSourceRegistration();
      }));
      const bool drainWaitedForBoundary =
        ownerBoundarySourceDrain->wait_for(100ms) == std::future_status::timeout;
      ownerBoundaryGate->release();
      BOOST_CHECK(drainWaitedForBoundary);
      const auto sourceDrainDeadline = publicSwitchDeadline + 15s;
      while (ownerBoundarySourceDrain->wait_for(0ms) != std::future_status::ready &&
             std::chrono::steady_clock::now() < sourceDrainDeadline) {
        (void)issuePendingGrant();
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundarySourceDrain->wait_for(0ms) == std::future_status::ready ||
            hasPendingGrant() || std::chrono::steady_clock::now() >= sourceDrainDeadline;
        });
      }
      bool sourceDrained =
        ownerBoundarySourceDrain->wait_for(0ms) == std::future_status::ready &&
        ownerBoundarySourceDrain->get();
      while (!sourceDrained && std::chrono::steady_clock::now() < sourceDrainDeadline) {
        auto retryDrain = std::async(std::launch::async, [&] {
          return drainSourceRegistration();
        });
        while (retryDrain.wait_for(0ms) != std::future_status::ready &&
               std::chrono::steady_clock::now() < sourceDrainDeadline) {
          environment->pumpUntilWithAttributeAuthority([&] {
            return retryDrain.wait_for(0ms) == std::future_status::ready ||
              std::chrono::steady_clock::now() >= sourceDrainDeadline;
          });
        }
        sourceDrained = retryDrain.wait_for(0ms) == std::future_status::ready &&
          retryDrain.get();
      }
      BOOST_CHECK_MESSAGE(sourceDrained,
        "source registration did not drain after Owner boundary close");
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      const auto currentConversation = conversations->find(reference.conversationId);
      BOOST_REQUIRE(currentConversation);
      BOOST_CHECK(currentConversation->nextTurnProvidersByRole != destinationProviders);
      const auto handoffCancelDeadline = publicSwitchDeadline + 15s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < handoffCancelDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= handoffCancelDeadline;
        });
      }
      BOOST_CHECK(handoff.status() != ConversationHandoffStatus::Pending);
      BOOST_CHECK(duplicateHandoff.status() != ConversationHandoffStatus::Pending);
      BOOST_CHECK_MESSAGE(drainClient.close(),
        "client did not drain after Provider boundary cancellation");
      return {};
    }
    if (mutateOwnerPolicyAfterResolution) {
      const auto policyGateDeadline = std::chrono::steady_clock::now() + 45s;
      bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      while (!boundaryEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < policyGateDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= policyGateDeadline;
        });
        boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      }
      if (!boundaryEntered) {
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(false,
          "policy mutation test did not reach the post-resolution publication boundary");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      if (!*handoffOwnerContext ||
          (**handoffOwnerContext).policy.maxAggregateBytes <= 1U) {
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(false,
          "resolved Owner policy context was unavailable for the mutation oracle");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      auto& resolverOwnedContext = **handoffOwnerContext;
      resolverOwnedContext.policy.maxAggregateBytes = 1;
      BOOST_CHECK_EQUAL(resolverOwnedContext.policy.maxAggregateBytes, 1U);
      ownerBoundaryGate->release();
    }
    if (capacityBoundaryCompetition) {
      const auto packetCommitsBefore = rangeStore->packetCommitCalls();
      const auto capacityDeadline = controlProgressWhileRoleParked ?
        publicSwitchDeadline : std::chrono::steady_clock::now() + 30s;
      bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      while (!boundaryEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < capacityDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= capacityDeadline;
        });
        boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      }
      if (!boundaryEntered) {
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(false,
          "first production switch did not reach the Owner packet-set boundary");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      if (controlProgressWhileRoleParked && twoRole) {
        while (commitGateCalls->load() < sourceRoles.size() &&
               std::chrono::steady_clock::now() < capacityDeadline) {
          environment->pumpUntilWithAttributeAuthority([&] {
            return commitGateCalls->load() >= sourceRoles.size() ||
              std::chrono::steady_clock::now() >= capacityDeadline;
          });
        }
      }
      BOOST_CHECK_EQUAL(commitGateCalls->load(),
        controlProgressWhileRoleParked ? sourceRoles.size() : 1U);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      const auto saveSnapshotBeforeCompetitor =
        providerRuntime.conversationStateSnapshot();
      const auto pinnedBeforeCompetitor = saveSnapshotBeforeCompetitor.pinnedEntries;
      const auto exportsBeforeCompetitor =
        saveSnapshotBeforeCompetitor.saveBatchRowExportAttempts;
      std::size_t secondPinnedBeforeCompetitor = 0;
      std::uint64_t secondExportsBeforeCompetitor = 0;
      if (twoSource) {
        const auto secondSnapshot = secondProviderRuntime.conversationStateSnapshot();
        secondPinnedBeforeCompetitor = secondSnapshot.pinnedEntries;
        secondExportsBeforeCompetitor = secondSnapshot.saveBatchRowExportAttempts;
        BOOST_CHECK_EQUAL(secondPinnedBeforeCompetitor, 1U);
        BOOST_CHECK_EQUAL(secondExportsBeforeCompetitor, 1U);
      }
      BOOST_CHECK_EQUAL(pinnedBeforeCompetitor, twoSource ? 1U : sourceRoles.size());
      BOOST_CHECK_EQUAL(exportsBeforeCompetitor, twoSource ? 1U : sourceRoles.size());

      auto competingClient = std::make_shared<NativeInferenceClient>(
        user, source.catalog->adapters(), runtime, conversations,
        source.catalog->makePreparation(user, service), admission);
      NativeRequestCatalog competingCatalog;
      competingCatalog.model.descriptor = source.model.descriptor;
      competingCatalog.preparation = source.catalog;
      competingCatalog.cooperativeSplitter = splitter;
      competingCatalog.stateMapping = source.stateMapping;
      auto competingModel = Spec212PreparedModelTestAccess::bind(
        std::move(competingCatalog), splitter,
        std::make_shared<NativePreSplitFirstPlacement>(), competingClient,
        service, policyDigest, tokenizer);
      ConversationOptions competingConversationOptions;
      competingConversationOptions.conversationId = "spec212-capacity-competing";
      auto competingConversation = competingModel.openConversation(competingConversationOptions);
      // The later productive event owns its full budget while the first stays parked.
      const auto competingSwitchDeadline =
        fault == CausalRecoveryFault::ControlProgressWhileRoleParked ?
          std::chrono::steady_clock::now() + productiveControlBudget : publicSwitchDeadline;
      const auto competingProgressDeadline =
        fault == CausalRecoveryFault::ControlProgressWhileRoleParked ?
          competingSwitchDeadline : capacityDeadline;
      const auto competingSwitch = competingConversation.switchProviders(
        destinationProviders, trustByRole, competingSwitchDeadline);
      if (twoSlotCapacityCompetition) {
        while (std::chrono::steady_clock::now() < capacityDeadline) {
          const auto firstSource = providerRuntime.conversationStateSnapshot();
          const auto secondSourceState = secondProviderRuntime.conversationStateSnapshot();
          if (firstSource.pinnedEntries == 2 && secondSourceState.pinnedEntries == 2 &&
              firstSource.saveBatchRowExportAttempts == 2 &&
              secondSourceState.saveBatchRowExportAttempts == 2)
            break;
          environment->pumpUntilWithAttributeAuthority([&] {
            const auto first = providerRuntime.conversationStateSnapshot();
            const auto second = secondProviderRuntime.conversationStateSnapshot();
            return (first.pinnedEntries == 2 && second.pinnedEntries == 2 &&
                    first.saveBatchRowExportAttempts == 2 &&
                    second.saveBatchRowExportAttempts == 2) ||
              std::chrono::steady_clock::now() >= capacityDeadline;
          });
        }
        const auto firstSource = providerRuntime.conversationStateSnapshot();
        const auto secondSourceState = secondProviderRuntime.conversationStateSnapshot();
        BOOST_REQUIRE_EQUAL(firstSource.pinnedEntries, 2U);
        BOOST_REQUIRE_EQUAL(secondSourceState.pinnedEntries, 2U);
        BOOST_REQUIRE_EQUAL(firstSource.saveBatchRowExportAttempts, 2U);
        BOOST_REQUIRE_EQUAL(secondSourceState.saveBatchRowExportAttempts, 2U);
        const auto admittedBoundaries = 2U * sourceRoles.size();
        while (commitGateCalls->load() < admittedBoundaries &&
               std::chrono::steady_clock::now() < capacityDeadline)
          environment->pumpUntilWithAttributeAuthority([&] {
            return commitGateCalls->load() >= admittedBoundaries ||
              std::chrono::steady_clock::now() >= capacityDeadline;
          });
        BOOST_REQUIRE_EQUAL(commitGateCalls->load(), admittedBoundaries);
        const auto boundariesBeforeRejection = commitGateCalls->load();

        auto thirdClient = std::make_shared<NativeInferenceClient>(
          user, source.catalog->adapters(), runtime, conversations,
          source.catalog->makePreparation(user, service), admission);
        DrainStandaloneClient drainThird{*thirdClient, *environment};
        NativeRequestCatalog thirdCatalog;
        thirdCatalog.model.descriptor = source.model.descriptor;
        thirdCatalog.preparation = source.catalog;
        thirdCatalog.cooperativeSplitter = splitter;
        thirdCatalog.stateMapping = source.stateMapping;
        auto thirdModel = Spec212PreparedModelTestAccess::bind(
          std::move(thirdCatalog), splitter,
          std::make_shared<NativePreSplitFirstPlacement>(), thirdClient,
          service, policyDigest, tokenizer);
        ConversationOptions thirdConversationOptions;
        thirdConversationOptions.conversationId = "spec212-capacity-third";
        auto thirdConversation = thirdModel.openConversation(thirdConversationOptions);
        const auto thirdSwitch = thirdConversation.switchProviders(
          destinationProviders, trustByRole, publicSwitchDeadline);
        while (thirdSwitch.status() == ConversationHandoffStatus::Pending &&
               std::chrono::steady_clock::now() < capacityDeadline) {
          environment->pumpUntilWithAttributeAuthority([&] {
            return thirdSwitch.status() != ConversationHandoffStatus::Pending ||
              std::chrono::steady_clock::now() >= capacityDeadline;
          });
        }
        BOOST_CHECK(thirdSwitch.status() == ConversationHandoffStatus::Failed);
        bool typedCapacityFailure = false;
        try { (void)thirdSwitch.result(0ms); }
        catch (const NativeDiError& error) {
          typedCapacityFailure = error.code() == "BUDGET_EXCEEDED";
        }
        BOOST_CHECK(typedCapacityFailure);
        BOOST_CHECK_EQUAL(commitGateCalls->load(), boundariesBeforeRejection);
        // The admitted second switch may finish its Repo commits while the
        // third switch is rejected; only commits beyond that set are invalid.
        BOOST_CHECK_LE(rangeStore->packetCommitCalls(),
                       packetCommitsBefore + sourceRoles.size());
        BOOST_CHECK_EQUAL(providerRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          firstSource.saveBatchRowExportAttempts);
        BOOST_CHECK_EQUAL(secondProviderRuntime.conversationStateSnapshot().saveBatchRowExportAttempts,
          secondSourceState.saveBatchRowExportAttempts);
        const auto rejectedRecord = conversations->find("spec212-capacity-third");
        BOOST_REQUIRE(rejectedRecord);
        BOOST_CHECK(rejectedRecord->nextTurnProvidersByRole.empty());
        BOOST_CHECK(drainThird.close());
      }
      unsigned grantsDispatchedWhileParked = 0;
      while (competingSwitch.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < competingProgressDeadline) {
        if (controlProgressWhileRoleParked && issuePendingGrant())
          ++grantsDispatchedWhileParked;
        environment->pumpUntilWithAttributeAuthority([&] {
          return competingSwitch.status() != ConversationHandoffStatus::Pending ||
            (controlProgressWhileRoleParked && hasPendingGrant()) ||
            std::chrono::steady_clock::now() >= competingProgressDeadline;
        });
      }
      if (controlProgressWhileRoleParked) {
        if (competingSwitch.status() != ConversationHandoffStatus::PromotedAwaitingAppend) {
          BOOST_TEST_MESSAGE("Spec212 parked competitor status="
            << static_cast<unsigned>(competingSwitch.status())
            << " grants=" << grantsDispatchedWhileParked
            << " boundaries=" << commitGateCalls->load()
            << " packet commits=" << rangeStore->packetCommitCalls());
          try { (void)competingSwitch.result(0ms); }
          catch (const std::exception& error) {
            BOOST_TEST_MESSAGE("Spec212 parked competitor error: " << error.what());
          }
        }
        BOOST_CHECK_MESSAGE(
          competingSwitch.status() == ConversationHandoffStatus::PromotedAwaitingAppend,
          "GRANT, COMMIT, and Owner promotion did not finish while the first role was parked");
        const auto competingRecord = conversations->find("spec212-capacity-competing");
        BOOST_REQUIRE(competingRecord);
        BOOST_CHECK(competingRecord->nextTurnProvidersByRole == destinationProviders);
        const auto stillParkedRecord = conversations->find(reference.conversationId);
        BOOST_REQUIRE(stillParkedRecord);
        BOOST_CHECK(stillParkedRecord->nextTurnProvidersByRole ==
          record->nextTurnProvidersByRole);
        BOOST_CHECK_GT(grantsDispatchedWhileParked, 0U);
        BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Pending);
        BOOST_CHECK_EQUAL(commitGateCalls->load(), 2U * sourceRoles.size());
        BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
          packetCommitsBefore + sourceRoles.size());
      }
      else {
        BOOST_CHECK(competingSwitch.status() == ConversationHandoffStatus::Failed);
        bool typedCapacityFailure = false;
        try {
          competingSwitch.result(0ms);
        }
        catch (const NativeDiError& error) {
          typedCapacityFailure = error.code() == "BUDGET_EXCEEDED";
        }
        BOOST_CHECK(typedCapacityFailure);
        BOOST_CHECK_EQUAL(commitGateCalls->load(), 1U);
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore);
      }
      const auto saveSnapshotAfterCompetitor =
        providerRuntime.conversationStateSnapshot();
      BOOST_CHECK_EQUAL(saveSnapshotAfterCompetitor.pinnedEntries, pinnedBeforeCompetitor);
      BOOST_CHECK_EQUAL(saveSnapshotAfterCompetitor.saveBatchRowExportAttempts,
        exportsBeforeCompetitor + (controlProgressWhileRoleParked ?
          (twoSource ? 1U : sourceRoles.size()) : 0U));
      if (twoSource) {
        const auto secondSnapshot = secondProviderRuntime.conversationStateSnapshot();
        BOOST_CHECK_EQUAL(secondSnapshot.pinnedEntries, secondPinnedBeforeCompetitor);
        BOOST_CHECK_EQUAL(secondSnapshot.saveBatchRowExportAttempts,
          secondExportsBeforeCompetitor + (controlProgressWhileRoleParked ? 1U : 0U));
      }

      BOOST_CHECK(handoff.cancel());
      const auto parkedCancelDeadline = std::chrono::steady_clock::now() + 5s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < parkedCancelDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= parkedCancelDeadline;
        });
      }
      BOOST_CHECK_MESSAGE(handoff.status() == ConversationHandoffStatus::Cancelled,
        "CANCEL control did not settle while the Provider boundary callback was parked");
      ownerBoundaryGate->release();
      const auto cancelDeadline = std::chrono::steady_clock::now() + 20s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < cancelDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending;
        });
      }
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Cancelled);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Cancelled);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(),
        packetCommitsBefore + (controlProgressWhileRoleParked ? sourceRoles.size() : 0U));
      std::function<ConversationStateSnapshot()> firstSnapshotAfterDrain;
      std::function<ConversationStateSnapshot()> secondSnapshotAfterDrain;
      if (twoSlotCapacityCompetition) {
        firstSnapshotAfterDrain = providerRuntime.conversationStateSnapshot;
        secondSnapshotAfterDrain = secondProviderRuntime.conversationStateSnapshot;
      }
      competingClient->close();
      BOOST_CHECK(competingClient->drain(5s));
      BOOST_CHECK_MESSAGE(drainClient.close(),
        "client and source registration did not finish draining after capacity cancellation");
      if (twoSlotCapacityCompetition) {
        BOOST_CHECK_EQUAL(commitGateCalls->load(), 2U * sourceRoles.size());
        BOOST_CHECK_EQUAL(firstSnapshotAfterDrain().saveBatchRowExportAttempts, 2U);
        BOOST_CHECK_EQUAL(secondSnapshotAfterDrain().saveBatchRowExportAttempts, 2U);
        BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(),
                          packetCommitsBefore + sourceRoles.size());
      }
      return {};
    }
    if (ownerCasFailure) {
      const auto casDeadline = std::chrono::steady_clock::now() + 45s;
      bool boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      while (!boundaryEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < casDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundaryGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= casDeadline;
        });
        boundaryEntered = ownerBoundaryGate->waitUntilEntered(0ms);
      }
      if (!boundaryEntered) {
        ownerBoundaryGate->release();
        BOOST_CHECK_MESSAGE(false,
          "public switch did not reach the durable Repo packet-set boundary for Owner CAS race");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      struct OwnerCasBoundaryRelease
      {
        std::shared_ptr<CausalBoundaryGate> gate;
        ~OwnerCasBoundaryRelease() { gate->release(); }
      } releaseOwnerCasBoundary{ownerBoundaryGate};
      BOOST_CHECK_EQUAL(commitGateCalls->load(), 1U);
      const auto packetCommitsBeforeConcurrentTurn = rangeStore->packetCommitCalls();
      const auto successfulCommitsBeforeConcurrentTurn =
        rangeStore->successfulPacketCommitCalls();
      BOOST_CHECK_EQUAL(packetCommitsBeforeConcurrentTurn, packetCommitsBeforeHandoff);

      std::string concurrentHeadWire;
      try {
      // This synthetic NativeCompletedAttempt exercises the trusted Owner
      // completion boundary. The test does not claim a second Provider ran;
      // the stale switch below still traverses public Provider/Repo/Owner APIs.
      const auto parentTokens = record->checkpoint.transcript.at("canonicalTokenIds")
        .get<std::vector<std::int64_t>>();
      auto inputTokens = parentTokens;
      inputTokens.push_back(6);
      auto nextTokens = inputTokens;
      nextTokens.push_back(7);
      NativeConversationContinuation concurrentContinuation;
      concurrentContinuation.conversationId = reference.conversationId;
      concurrentContinuation.parentContextEpoch = record->checkpoint.successorContextEpoch;
      concurrentContinuation.serviceName = record->serviceName;
      concurrentContinuation.planRoleMapDigest = record->planRoleMapDigest;
      concurrentContinuation.parentCheckpointDigest = record->checkpoint.checkpointDigest;
      concurrentContinuation.requestContractDigest = nativePlanningDigest(
        "spec212-owner-cas-concurrent-turn");
      concurrentContinuation.retentionDeadlineMs = record->retentionDeadlineMs;
      concurrentContinuation.mode = "APPEND_DELTA";
      concurrentContinuation.parentCheckpointWire = record->checkpoint.wire;
      concurrentContinuation.generationId = std::string(32, '2');
      concurrentContinuation.canonicalTokenIds = inputTokens;
      concurrentContinuation.expectedRoles = record->checkpoint.expectedRoles;
      const std::string concurrentRequestId = "/Spec212/OwnerCas/concurrent-turn";
      auto concurrentTurn = conversations->beginTurn(
        concurrentContinuation, concurrentRequestId);
      conversations->acceptTokenPrefix(concurrentTurn, {7});

      NativeCompletedAttempt concurrentCompletion;
      concurrentCompletion.requestId = concurrentTurn.executionRequestId;
      concurrentCompletion.attempt = concurrentTurn.attempt;
      concurrentCompletion.tokenIds = nextTokens;
      concurrentCompletion.complete = true;
      concurrentCompletion.generationId = concurrentContinuation.generationId;
      const auto checkpointJson = nativeParseJson(record->checkpoint.wire);
      concurrentCompletion.modelContractDigest = checkpointJson.at("modelContractDigest");
      const auto& transcript = record->checkpoint.transcript;
      concurrentCompletion.tokenizerDigest = transcript.at("tokenizerDigest");
      concurrentCompletion.chatTemplateDigest = transcript.at("chatTemplateDigest");
      concurrentCompletion.applicationMessages = nativeConversationBase64Decode(
        transcript.at("applicationMessages").get<std::string>());
      const auto receiptPrefix = nativeConversationPrefixDigest(nextTokens);
      for (const auto& encoded : transcript.at("providerRoleReceipts")) {
        auto receipt = nativeParseJson(nativeConversationBase64Decode(
          encoded.get<std::string>()));
        receipt["originRequestId"] = concurrentRequestId;
        receipt["originGenerationId"] = concurrentContinuation.generationId;
        receipt["parentContextEpoch"] = concurrentContinuation.parentContextEpoch;
        receipt["successorContextEpoch"] = concurrentContinuation.parentContextEpoch + 1;
        receipt["prefixDigest"] = receiptPrefix;
        receipt["prefixTokenCount"] = nextTokens.size();
        auto receiptUnsigned = receipt;
        receiptUnsigned.erase("receiptDigest");
        receiptUnsigned.erase("signature");
        receipt["receiptDigest"] = nativePlanningDigest(
          nativeConversationCanonicalJson(receiptUnsigned));
        concurrentCompletion.authenticatedReceipts.push_back(std::move(receipt));
      }
      concurrentCompletion.commitProviderState = [](const std::string&) {};
      concurrentCompletion.rollbackProviderState = [] {};
      const auto preparedConcurrent = conversations->prepareCheckpoint(
        concurrentTurn, concurrentCompletion);
      const auto advanced = conversations->commitTurn(concurrentTurn, preparedConcurrent);
      concurrentHeadWire = advanced.checkpoint.wire;
      BOOST_CHECK(concurrentHeadWire != record->checkpoint.wire);
      BOOST_CHECK(advanced.checkpoint.providersByRole == record->checkpoint.providersByRole);
      }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("synthetic competing Owner completion failed: " << error.what());
        ownerBoundaryGate->release();
        const auto settleDeadline = std::chrono::steady_clock::now() + 35s;
        while (handoff.status() == ConversationHandoffStatus::Pending &&
               std::chrono::steady_clock::now() < settleDeadline) {
          environment->pumpUntilWithAttributeAuthority([&] {
            return handoff.status() != ConversationHandoffStatus::Pending ||
              std::chrono::steady_clock::now() >= settleDeadline;
          });
        }
        BOOST_CHECK(drainClient.close());
        throw;
      }

      ownerBoundaryGate->release();
      const auto handoffDeadline = std::chrono::steady_clock::now() + 35s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < handoffDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= handoffDeadline;
        });
      }
      if (handoff.status() != ConversationHandoffStatus::Failed) {
        BOOST_TEST_MESSAGE(
          "stale public Provider switch did not settle as a failure before the test deadline");
        (void)handoff.cancel();
        BOOST_CHECK(drainClient.close());
        BOOST_CHECK_MESSAGE(false,
          "stale public Provider switch unexpectedly replaced the concurrent Owner checkpoint");
        return {};
      }
      bool staleOwnerHeadRejected = false;
      try {
        handoff.result(0ms);
      }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("stale public Provider switch failed with: " << error.what());
        staleOwnerHeadRejected = std::string(error.what()).find(
          "protected KV publication unavailable") != std::string::npos;
      }
      BOOST_CHECK(staleOwnerHeadRejected);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeConcurrentTurn + 1);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulCommitsBeforeConcurrentTurn + 1);
      const auto usageAfterCasRace = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_GT(usageAfterCasRace.committedBytes, repoUsageBeforeHandoff.committedBytes);
      BOOST_CHECK_EQUAL(usageAfterCasRace.reservedBytes, repoUsageBeforeHandoff.reservedBytes);
      const auto afterCasRace = conversations->find(reference.conversationId);
      BOOST_REQUIRE(afterCasRace);
      BOOST_CHECK_EQUAL(afterCasRace->checkpoint.wire, concurrentHeadWire);
      BOOST_CHECK(afterCasRace->checkpoint.providersByRole == record->checkpoint.providersByRole);
      BOOST_CHECK(!afterCasRace->protectedKvReferences);
      BOOST_CHECK(afterCasRace->nextTurnProvidersByRole.empty());
      BOOST_CHECK(drainClient.close());
      return {};
    }
    if (fault == CausalRecoveryFault::CloseDuringRepoCommit) {
      const auto packetCommitsBefore = rangeStore->packetCommitCalls();
      const auto successfulCommitsBefore = rangeStore->successfulPacketCommitCalls();
      const auto usageBefore = journal->userStorageUsage(requester, repoName);
      const auto commitDeadline = std::chrono::steady_clock::now() + 45s;
      bool commitEntered = repoCommitGate->waitUntilEntered(0ms);
      while (!commitEntered && handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < commitDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return repoCommitGate->waitUntilEntered(0ms) ||
            handoff.status() != ConversationHandoffStatus::Pending ||
            std::chrono::steady_clock::now() >= commitDeadline;
        });
        commitEntered = repoCommitGate->waitUntilEntered(0ms);
      }
      if (!commitEntered) {
        repoCommitGate->release();
        BOOST_CHECK_MESSAGE(false,
          "handoff did not enter the Repo original-packet commit callback");
        closeSourceRegistration();
        BOOST_CHECK(drainSourceRegistration());
        return {};
      }
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBefore + 1);
      closeSourceRegistration();
      auto sourceDrain = std::async(std::launch::async, [&] {
        return drainSourceRegistration();
      });
      const bool drainWaitedForCommit =
        sourceDrain.wait_for(100ms) == std::future_status::timeout;
      repoCommitGate->release();
      BOOST_CHECK(drainWaitedForCommit);
      const auto sourceDrainDeadline = std::chrono::steady_clock::now() + 25s;
      while (sourceDrain.wait_for(0ms) != std::future_status::ready &&
             std::chrono::steady_clock::now() < sourceDrainDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return sourceDrain.wait_for(0ms) == std::future_status::ready;
        });
      }
      const bool sourceDrained = sourceDrain.wait_for(0ms) == std::future_status::ready &&
        sourceDrain.get();
      BOOST_CHECK_MESSAGE(sourceDrained,
        "source registration did not drain after the Repo commit callback was released");
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), successfulCommitsBefore + 1);
      const auto usageAfter = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_GT(usageAfter.committedBytes, usageBefore.committedBytes);
      BOOST_CHECK_EQUAL(usageAfter.reservedBytes, usageBefore.reservedBytes);
      const auto handoffDeadline = publicSwitchDeadline + 5s;
      while (handoff.status() == ConversationHandoffStatus::Pending &&
             std::chrono::steady_clock::now() < handoffDeadline) {
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::Pending;
        });
      }
      BOOST_CHECK_MESSAGE(handoff.status() != ConversationHandoffStatus::Pending,
        "handoff remained active after source Repo commit drain completed");
      BOOST_CHECK_MESSAGE(drainClient.close(),
        "client and source registration did not finish draining before fixture teardown");
      return {};
    }
    const auto handoffDeadline = std::chrono::steady_clock::now() + 50s;
    while (handoff.status() == ConversationHandoffStatus::Pending &&
           std::chrono::steady_clock::now() < handoffDeadline) {
      (void)issuePendingGrant();
      environment->pumpUntilWithAttributeAuthority([&] {
        return handoff.status() != ConversationHandoffStatus::Pending || hasPendingGrant();
      });
    }
    if (handoff.status() == ConversationHandoffStatus::Failed) {
      try {
        handoff.result(0ms);
      }
      catch (const std::exception& error) {
        const auto providerSeq9SegmentName = ndn::Name(
          environment->providerPubSub().getSVSync().getDataName(
            providerSvsNode, providerSvsCore.getBootstrapTime(), 9))
          .appendVersion(0).appendSegment(0);
        const auto providerSeq9Stored = environment->providerPubSub().getSVSync()
          .getDataStore().find(ndn::Interest(providerSeq9SegmentName)) != nullptr;
        auto providerSeq9FetchInterest = ndn::Interest(
          environment->providerPubSub().getSVSync().getDataName(
            providerSvsNode, providerSvsCore.getBootstrapTime(), 9));
        providerSeq9FetchInterest.setCanBePrefix(true);
        const auto providerSeq9FetchMatch = environment->providerPubSub().getSVSync()
          .getDataStore().find(providerSeq9FetchInterest);
        std::string providerSeq9SegmentStoreState;
        for (uint64_t segment = 0; segment < 3; ++segment) {
          const auto segmentName = ndn::Name(
            environment->providerPubSub().getSVSync().getDataName(
              providerSvsNode, providerSvsCore.getBootstrapTime(), 9))
            .appendVersion(0).appendSegment(segment);
          const auto stored = environment->providerPubSub().getSVSync().getDataStore()
            .find(ndn::Interest(segmentName)) != nullptr;
          if (!providerSeq9SegmentStoreState.empty()) providerSeq9SegmentStoreState += ",";
          providerSeq9SegmentStoreState += std::to_string(segment) + "=" +
            (stored ? "present" : "missing");
        }
        std::string providerLifecycle;
        {
          std::lock_guard<std::mutex> lock(*handoffProviderLifecycleMutex);
          for (std::size_t i = providerLifecycleBeforeHandoff;
               i < handoffProviderLifecycle->size(); ++i) {
            if (!providerLifecycle.empty()) providerLifecycle += ",";
            providerLifecycle += handoffProviderLifecycle->at(i);
          }
        }
        std::string userLifecycle;
        const auto firstState = providerLifecycle.find(":0:");
        if (firstState != std::string::npos) {
          const auto userStatus = environment->user().getRequestStatus(
            ndn::Name(providerLifecycle.substr(0, firstState)));
          if (userStatus) {
            userLifecycle = std::to_string(static_cast<unsigned>(userStatus->state)) +
              ":published=" + std::to_string(userStatus->publishTimestampUs) +
              ":observed=" + std::to_string(userStatus->responseObservedTimestampUs) +
              ":decrypted=" + std::to_string(userStatus->responseDecryptedTimestampUs) +
              ":callback=" + std::to_string(userStatus->callbackTimestampUs) +
              ":cleanup=" + userStatus->finalCleanupReason;
          }
        }
        std::string userPubSubResponses;
        {
          std::lock_guard<std::mutex> lock(*observedResponseNamesMutex);
          for (std::size_t i = userPublicationCountBeforeHandoff;
               i < observedResponseNames->size(); ++i) {
            if (!userPubSubResponses.empty()) userPubSubResponses += ",";
            userPubSubResponses += observedResponseNames->at(i);
          }
        }
        std::string providerFaceResponses;
        {
          std::lock_guard<std::mutex> lock(*providerFaceResponseNamesMutex);
          for (const auto& responseName : *providerFaceResponseNames) {
            if (!providerFaceResponses.empty()) providerFaceResponses += ",";
            providerFaceResponses += responseName;
          }
        }
        std::string providerFaceSyncInterests;
        {
          std::lock_guard<std::mutex> lock(*providerFaceSyncInterestNamesMutex);
          for (std::size_t i = providerSyncInterestCountBeforeHandoff;
               i < providerFaceSyncInterestNames->size(); ++i) {
            if (!providerFaceSyncInterests.empty()) providerFaceSyncInterests += ",";
            providerFaceSyncInterests += providerFaceSyncInterestNames->at(i);
          }
        }
        std::string providerFaceSyncData;
        {
          std::lock_guard<std::mutex> lock(*providerFaceSyncDataNamesMutex);
          for (std::size_t i = providerSyncDataCountBeforeHandoff;
               i < providerFaceSyncDataNames->size(); ++i) {
            if (!providerFaceSyncData.empty()) providerFaceSyncData += ",";
            providerFaceSyncData += providerFaceSyncDataNames->at(i);
          }
        }
        std::string providerFacePublishedPackets;
        {
          std::lock_guard<std::mutex> lock(*providerFacePublishedPacketNamesMutex);
          for (std::size_t i = providerPacketCountBeforeHandoff;
               i < providerFacePublishedPacketNames->size(); ++i) {
            if (!providerFacePublishedPackets.empty()) providerFacePublishedPackets += ",";
            providerFacePublishedPackets += providerFacePublishedPacketNames->at(i);
          }
        }
        std::string userResponseInterests;
        {
          std::lock_guard<std::mutex> lock(*userResponseInterestNamesMutex);
          for (std::size_t i = userResponseInterestCountBeforeHandoff;
               i < userResponseInterestNames->size(); ++i) {
            if (!userResponseInterests.empty()) userResponseInterests += ",";
            userResponseInterests += userResponseInterestNames->at(i);
          }
        }
        std::string userProviderPacketInterests;
        {
          std::lock_guard<std::mutex> lock(*userProviderPacketInterestNamesMutex);
          for (std::size_t i = userProviderPacketInterestCountBeforeHandoff;
               i < userProviderPacketInterestNames->size(); ++i) {
            if (!userProviderPacketInterests.empty()) userProviderPacketInterests += ",";
            userProviderPacketInterests += userProviderPacketInterestNames->at(i);
          }
        }
        BOOST_TEST_MESSAGE("Spec212 production handoff failure: " << error.what()
          << "; Owner resolver calls=" << handoffResolverCalls->load()
          << "; Provider requests observed after switch="
          << handoffProviderRequestsObserved->load() - providerRequestsBeforeHandoff
          << "; Provider symmetric encryptions after switch="
          << environment->provider().getHybridCryptoCounters().symmetric_encrypt_count.load(
               std::memory_order_relaxed) - providerEncryptCountBeforeHandoff
          << "; Provider SVS node=" << providerSvsNode
          << "; Provider SVS sequence before/after=" << providerSvsSeqBeforeHandoff
          << "/" << providerSvsCore.getSeqNo(providerSvsNode)
          << "; Provider response seq=9 segment stored=" << providerSeq9Stored
          << "; Provider seq=9 can-prefix fetch match="
          << (providerSeq9FetchMatch ? providerSeq9FetchMatch->getName().toUri() : "none")
          << "; Provider seq=9 segment store states=" << providerSeq9SegmentStoreState
          << "; User view of Provider SVS sequence before/after="
          << userProviderSvsSeqBeforeHandoff << "/"
          << environment->userPubSub().getSVSync().getCore().getSeqNo(providerSvsNode)
          << "; Provider lifecycle(id:state:final)=" << providerLifecycle
          << "; User lifecycle(state:times:cleanup)=" << userLifecycle
          << "; Provider Face response packets=" << providerFaceResponses
          << "; Provider Face sync Interests after switch=" << providerFaceSyncInterests
          << "; Provider Face sync Data after switch=" << providerFaceSyncData
          << "; Provider Face published packets after switch=" << providerFacePublishedPackets
          << "; User response Interests after switch=" << userResponseInterests
          << "; User Provider-packet Interests after switch=" << userProviderPacketInterests
          << "; independent user-PubSub publications after switch=" << userPubSubResponses);
      }
    }
    if (fault == CausalRecoveryFault::FailSecondRepoPacketCommit) {
      BOOST_REQUIRE(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeHandoff + 2);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulPacketCommitsBeforeHandoff + 1);
      BOOST_CHECK_EQUAL(rangeStore->injectedCommitFailures(), 1U);
      const auto repoUsageAfterHandoff = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_GT(repoUsageAfterHandoff.committedBytes,
                     repoUsageBeforeHandoff.committedBytes);
      const auto unchanged = conversations->find(reference.conversationId);
      BOOST_REQUIRE(unchanged);
      BOOST_CHECK_EQUAL(unchanged->checkpoint.wire, record->checkpoint.wire);
      BOOST_CHECK(unchanged->checkpoint.providersByRole == record->checkpoint.providersByRole);
      BOOST_CHECK(!unchanged->protectedKvReferences);
      BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
      BOOST_CHECK(drainClient.close());
      return {};
    }
    if (fault == CausalRecoveryFault::OwnerJournalQuotaFailure) {
      BOOST_REQUIRE(handoff.status() == ConversationHandoffStatus::Failed);
      bool journalQuotaFailure = false;
      try {
        static_cast<void>(handoff.result(0ms));
      }
      catch (const std::exception& error) {
        journalQuotaFailure =
          std::string(error.what()).find("conversation journal quota exceeded") !=
          std::string::npos;
      }
      BOOST_CHECK(journalQuotaFailure);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_EQUAL(handoffResolverCalls->load(), 1U);
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeHandoff + 1);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulPacketCommitsBeforeHandoff + 1);
      const auto repoUsageAfterHandoff = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_GT(repoUsageAfterHandoff.committedBytes,
                     repoUsageBeforeHandoff.committedBytes);
      BOOST_CHECK_EQUAL(repoUsageAfterHandoff.reservedBytes,
                        repoUsageBeforeHandoff.reservedBytes);
      const auto unchanged = conversations->find(reference.conversationId);
      BOOST_REQUIRE(unchanged);
      BOOST_CHECK(unchanged->checkpoint.providersByRole ==
                  record->checkpoint.providersByRole);
      BOOST_CHECK(!unchanged->protectedKvReferences);
      BOOST_CHECK(unchanged->nextTurnProvidersByRole.empty());
      BOOST_CHECK_EQUAL(std::filesystem::file_size(isolatedOwnerJournalPath),
                        ownerJournalBytesBeforeHandoff);
      BOOST_REQUIRE(publicClientDrain);
      BOOST_CHECK(publicClientDrain->close());
      BOOST_CHECK(drainClient.close());
      return {};
    }
    BOOST_REQUIRE_MESSAGE(
      handoff.status() == ConversationHandoffStatus::PromotedAwaitingAppend,
      "production Owner switch did not promote the saved Repo packet set");
    const auto promoted = conversations->find(reference.conversationId);
    BOOST_REQUIRE(promoted);
    BOOST_REQUIRE(promoted->protectedKvReferences);
    if (twoSource) {
      BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), packetCommitsBeforeHandoff + 2);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulPacketCommitsBeforeHandoff + 2);
    }
    BOOST_CHECK(promoted->checkpoint.providersByRole == record->checkpoint.providersByRole);
    BOOST_CHECK(promoted->nextTurnProvidersByRole == destinationProviders);
    BOOST_REQUIRE_EQUAL(promoted->protectedKvReferences->size(), sourceRoles.size());
    for (const auto& role : sourceRoles) {
      const auto publishedManifestName = ndn::Name(
        promoted->protectedKvReferences->at(role).at("manifestName").get<std::string>());
      const auto& sourceProducer = twoSource && role == sourceRoles.back()
        ? secondSource : provider;
      BOOST_CHECK(ndn::Name(sourceProducer).isPrefixOf(publishedManifestName));
    }

    if (fault == CausalRecoveryFault::CloseAfterPromotionBeforeAppend) {
      BOOST_CHECK(!handoff.cancel());
      publicConversation.close();
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      bool closedFailure = false;
      try { handoff.result(0ms); }
      catch (const DiError& error) { closedFailure = error.code() == "RUNTIME_CLOSED"; }
      BOOST_CHECK(closedFailure);
      BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(),
                        successfulPacketCommitsBeforeHandoff + 1);
      BOOST_CHECK(drainClient.close());
      return {};
    }

    std::filesystem::create_directory(files.root / "spec212-recovery-B");
    BOOST_REQUIRE_EQUAL(::chmod((files.root / "spec212-recovery-B").c_str(), 0700), 0);
    auto destinationConfig = makeConfig(destinationIndex, successorFactory);
    destinationConfig.kvRecovery = NativeProviderKvRecoveryConfigV1{
      files.root / "spec212-recovery-B", {ndn::Name(repoName)},
      protectedLimits, portableLimits, 2U << 20};
    auto destinationRuntime = makeNativeProviderCollaborationRuntime(
      std::move(destinationConfig));
    auto destinationRegistration = installHandler(destinationIndex, destinationRuntime);

    const auto delta = causalInput(source.model.descriptor, {6}, tokenizer, 1);
    RequestOptions continuationOptions;
    continuationOptions.timeout = 30s;
    continuationOptions.ackTimeout = 3s;
    continuationOptions.outputMode = "TOKEN_STREAMING";
    continuationOptions.stream = StreamOptions{};
    if (twoRole) {
      // The two-role native runner can take over 1.5 seconds between a token
      // and End under TSAN. Keep a bounded gap budget within the 30s request.
      continuationOptions.stream->interestLifetimeMs = 1000;
    }
    continuationOptions.generation = GenerationOptions{1};
    const auto deltaInput = Input::inlineBytes(delta.payload, delta.options);
    const auto pumpRequestUntilTerminal = [&](const RequestHandle& request) {
      const auto deadline = std::chrono::steady_clock::now() + 35s;
      while (request.status() == RequestStatus::Pending &&
             std::chrono::steady_clock::now() < deadline)
        environment->pumpUntilWithAttributeAuthority([&] {
          return request.status() != RequestStatus::Pending;
        });
    };
    RequestHandle continued;
    if (fault == CausalRecoveryFault::SynchronousFirstAppendDispatchError) {
      auto invalidOptions = continuationOptions;
      invalidOptions.providerNames = {provider};
      bool dispatchRejected = false;
      try {
        static_cast<void>(publicConversation.request(deltaInput, invalidOptions));
      }
      catch (const std::exception& error) {
        dispatchRejected = std::string(error.what()).find(
          "providerNames conflicts with the Owner-selected next-turn map") !=
          std::string::npos;
      }
      BOOST_CHECK(dispatchRejected);
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
      continued = publicConversation.request(deltaInput, continuationOptions);
    }
    else if (fault == CausalRecoveryFault::FailFirstDestinationAppend) {
      const auto failedAppend = publicConversation.request(deltaInput, continuationOptions);
      BOOST_CHECK(failedAppend.status() == RequestStatus::Pending);
      auto releaseFailureGate = std::async(std::launch::async, [successorRunnerFailureGate] {
        const bool entered = successorRunnerFailureGate->waitUntilEntered(10s);
        successorRunnerFailureGate->release();
        return entered;
      });
      pumpRequestUntilTerminal(failedAppend);
      BOOST_REQUIRE(releaseFailureGate.get());
      BOOST_REQUIRE(failedAppend.status() == RequestStatus::Failed);
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_THROW(failedAppend.result(0ms), std::exception);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
      continued = publicConversation.request(deltaInput, continuationOptions);
    }
    else if (fault == CausalRecoveryFault::CloseAfterCommittedAppendBeforeCompletion) {
      postCommitAction->store(1);
      continued = publicConversation.request(deltaInput, continuationOptions);
      struct GateRelease {
        std::shared_ptr<CausalBoundaryGate> gate;
        ~GateRelease() { gate->release(); }
      } release{postCommitGate};
      const auto gateDeadline = std::chrono::steady_clock::now() + 35s;
      while (!postCommitGate->waitUntilEntered(0ms) &&
             std::chrono::steady_clock::now() < gateDeadline)
        environment->pumpUntilWithAttributeAuthority([&] {
          return postCommitGate->waitUntilEntered(0ms);
        });
      BOOST_REQUIRE(postCommitGate->waitUntilEntered(0ms));
      BOOST_CHECK(continued.status() == RequestStatus::Pending);
      publicConversation.close();
      std::optional<Conversation> retired(std::move(publicConversation));
      retired.reset();
      postCommitGate->release();
    }
    else if (fault == CausalRecoveryFault::FailAfterCommittedAppend) {
      postCommitAction->store(2);
      continued = publicConversation.request(deltaInput, continuationOptions);
    }
    else {
      continued = publicConversation.request(deltaInput, continuationOptions);
    }
    pumpRequestUntilTerminal(continued);
    if (fault == CausalRecoveryFault::FailAfterCommittedAppend) {
      BOOST_REQUIRE(continued.status() == RequestStatus::Failed);
      const auto handoffDeadline = std::chrono::steady_clock::now() + 10s;
      while (handoff.status() == ConversationHandoffStatus::PromotedAwaitingAppend &&
             std::chrono::steady_clock::now() < handoffDeadline)
        environment->pumpUntilWithAttributeAuthority([&] {
          return handoff.status() != ConversationHandoffStatus::PromotedAwaitingAppend;
        });
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_THROW(continued.result(0ms), std::exception);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
      const auto committed = conversations->find(reference.conversationId);
      BOOST_REQUIRE(committed);
      BOOST_CHECK_EQUAL(committed->checkpoint.successorContextEpoch,
                        reference.contextEpoch + 1);
      BOOST_CHECK(committed->checkpoint.providersByRole == destinationProviders);
      BOOST_CHECK(drainClient.close());
      return {};
    }
    if (continued.status() != RequestStatus::Succeeded) {
      std::vector<NativeJson> observedFailures;
      {
        std::lock_guard<std::mutex> lock(*nativeFailuresMutex);
        observedFailures = *nativeFailures;
      }
      for (const auto& failure : observedFailures)
        BOOST_TEST_MESSAGE("Spec212 native failure=" << nativeCanonicalJson(failure));
      const auto capacity = destinationRuntime.capacitySnapshot();
      BOOST_TEST_MESSAGE("Spec212 destination capacity: handlerThreads="
        << environment->provider(destinationIndex).getHandlerThreads()
        << " handlerQueue=" << environment->provider(destinationIndex).getHandlerQueueDepth()
        << " nativeWorkers=" << capacity.workerCount
        << " readyQueue=" << capacity.readyQueueDepth
        << " activeWorkers=" << capacity.activeWorkerCount
        << " waitingForInput=" << capacity.waitingForInputCount
        << " dependencyWaitActive=" << capacity.dependencyWaitActiveCount);
      const auto attemptedRoles = successorFactory->attemptedRoles();
      const auto createdRoles = successorFactory->createdRoles();
      BOOST_TEST_MESSAGE("Spec212 first APPEND terminal status=" <<
        static_cast<int>(continued.status()) << " handoffStatus=" <<
        static_cast<int>(handoff.status()) << " destinationRunnersCreated=" <<
        successorFactory->created().size());
      for (const auto& role : attemptedRoles)
        BOOST_TEST_MESSAGE("Spec212 destination runner attempt=" << role);
      for (const auto& role : createdRoles)
        BOOST_TEST_MESSAGE("Spec212 destination runner created=" << role);
      if (const auto lifecycle = user->getRequestStatus(ndn::Name(continued.id())))
        BOOST_TEST_MESSAGE("Spec212 APPEND User lifecycle state=" << static_cast<unsigned>(lifecycle->state)
          << " published=" << lifecycle->publishTimestampUs
          << " responseObserved=" << lifecycle->responseObservedTimestampUs
          << " responseDecrypted=" << lifecycle->responseDecryptedTimestampUs
          << " callback=" << lifecycle->callbackTimestampUs);
      BOOST_TEST_MESSAGE("Spec212 User handler threads=" << user->getHandlerThreads()
        << " queueDepth=" << user->getHandlerQueueDepth());
      try {
        const auto streamMetrics = user->getStreamMetricsForTest(ndn::Name(continued.id()));
        BOOST_TEST_MESSAGE("Spec212 User stream metrics delivered=" << streamMetrics.deliveredEvents
          << " retries=" << streamMetrics.retryCount
          << " duplicates=" << streamMetrics.duplicateCount);
      }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("Spec212 User stream metrics unavailable=" << error.what());
      }
      {
        std::lock_guard<std::mutex> lock(*providerFacePublishedPacketNamesMutex);
        std::size_t reported = 0;
        for (auto it = providerFacePublishedPacketNames->rbegin();
             it != providerFacePublishedPacketNames->rend() && reported < 12; ++it) {
          if (it->find("/NDNSF/event/") == std::string::npos) continue;
          BOOST_TEST_MESSAGE("Spec212 Provider event Data=" << *it);
          ++reported;
        }
      }
      const auto userDiagnostics = user->consumeRuntimeDiagnostics();
      BOOST_TEST_MESSAGE("Spec212 User response diagnostics callbackSkippedNoPending="
        << userDiagnostics.callbackSkippedNoPending << " responseAfterPendingTimeout="
        << userDiagnostics.responseAfterPendingTimeout);
      try {
        const auto readyRecords = user->waitForVerifiedCollaborationData(
          ndn::Name(continued.id()), "ndnsf-di-conversation-state-v1",
          ndn::Name("/ndnsf-di/conversation/ready"), 1, 20, true);
        BOOST_TEST_MESSAGE("Spec212 verified recovery READY records=" << readyRecords.size());
        for (const auto& ready : readyRecords)
          BOOST_TEST_MESSAGE("Spec212 verified recovery READY role=" << ready.producerRole);
      }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("Spec212 recovery READY diagnostic failed=" << error.what());
      }
      try {
        const auto receiptRecords = user->waitForVerifiedCollaborationData(
          ndn::Name(continued.id()), "ndnsf-di-conversation-state-v1",
          ndn::Name("/ndnsf-di/conversation/receipt"), 1, 20, true);
        BOOST_TEST_MESSAGE("Spec212 verified recovery receipt records=" << receiptRecords.size());
        for (const auto& receipt : receiptRecords)
          BOOST_TEST_MESSAGE("Spec212 verified recovery receipt role=" << receipt.producerRole);
      }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("Spec212 recovery receipt diagnostic failed=" << error.what());
      }
      try { (void)continued.result(0ms); }
      catch (const std::exception& error) {
        BOOST_TEST_MESSAGE("Spec212 first APPEND error=" << error.what());
      }
    }
    BOOST_REQUIRE(continued.status() == RequestStatus::Succeeded);
    const auto continuedResult = continued.result(0ms);
    BOOST_CHECK(!continuedResult.payload.empty());
    if (fault == CausalRecoveryFault::None || mutateOwnerPolicyAfterResolution ||
        switchFirstWithdrawalOverlap ||
        fault == CausalRecoveryFault::CloseAfterCommittedAppendBeforeCompletion ||
        fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch) {
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Succeeded);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Succeeded);
      BOOST_CHECK_EQUAL(handoffResolverCalls->load(),
        twoSource ? 2U : (withdrawalWireBudget ? 3U : (withdrawalAbort ? 2U : 1U)));
      BOOST_CHECK_NO_THROW(handoff.result(0ms));
    }
    else {
      BOOST_CHECK(handoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK(duplicateHandoff.status() == ConversationHandoffStatus::Failed);
      BOOST_CHECK_THROW(handoff.result(0ms), std::exception);
    }
    const auto createdRunners = successorFactory->created();
    const auto createdRoles = successorFactory->createdRoles();
    BOOST_REQUIRE_EQUAL(createdRunners.size(), sourceRoles.size());
    BOOST_REQUIRE_EQUAL(createdRoles.size(), createdRunners.size());
    std::optional<NativeRuntimeMetrics> continuedMetrics;
    for (std::size_t i = 0; i < createdRunners.size(); ++i) {
      auto roleRunner = createdRunners[i].lock();
      BOOST_REQUIRE(roleRunner);
      const auto roleMetrics = roleRunner->runtimeMetricsSnapshot();
      BOOST_REQUIRE(roleMetrics);
      const bool ownsTokenInput = roleOwnsTokenInput(createdRoles[i]);
      // Downstream activation work remains unclassified, not zero execution.
      BOOST_CHECK_EQUAL(ownsTokenInput, createdRoles[i] == source.role);
      BOOST_CHECK_EQUAL(roleMetrics->workCounterCertified, ownsTokenInput);
      BOOST_CHECK_EQUAL(roleMetrics->stateInputMisses, 0U);
      BOOST_CHECK_GE(roleMetrics->stateInputHits, 3U);
      if (ownsTokenInput) {
        BOOST_CHECK_EQUAL(roleMetrics->executedPrefillTokens, 1U);
        BOOST_CHECK_EQUAL(roleMetrics->executedDecodeTokens, 1U);
        BOOST_REQUIRE(!continuedMetrics);
        continuedMetrics = roleMetrics;
      }
    }
    BOOST_REQUIRE(continuedMetrics);
    BOOST_TEST_MESSAGE("Spec212 token-ingress APPEND work trace: certified="
      << continuedMetrics->workCounterCertified
      << "; prefix prefill tokens recomputed=0; delta prefill tokens="
      << continuedMetrics->executedPrefillTokens
      << "; decode tokens=" << continuedMetrics->executedDecodeTokens
      << "; KV component hits=" << continuedMetrics->stateInputHits);
    const auto completed = conversations->find(reference.conversationId);
    BOOST_REQUIRE(completed);
    BOOST_CHECK(completed->checkpoint.providersByRole == destinationProviders);
    if (fault == CausalRecoveryFault::UnrelatedConversationAfterSwitch) {
      const auto unrelated = conversations->find("spec212-capacity-competing");
      BOOST_REQUIRE(unrelated);
      BOOST_CHECK_EQUAL(unrelated->checkpoint.wire, unrelatedConversationCheckpointWire);
      BOOST_CHECK(unrelated->checkpoint.providersByRole.at(source.role) == provider);
      BOOST_CHECK(!unrelated->protectedKvReferences);
      BOOST_CHECK(unrelated->nextTurnProvidersByRole.empty());
    }
    if (fault != CausalRecoveryFault::CloseAfterCommittedAppendBeforeCompletion) {
      const auto publicCheckpoint = publicConversation.checkpoint();
      const auto publicCheckpointBytes = publicCheckpoint.bytes();
      const std::string publicCheckpointWire(publicCheckpointBytes.begin(),
                                             publicCheckpointBytes.end());
      BOOST_CHECK_EQUAL(completed->checkpoint.wire, publicCheckpointWire);
    }

    if (ownerBoundarySourceDrain) {
      const auto sourceDrainDeadline = std::chrono::steady_clock::now() + 25s;
      while (ownerBoundarySourceDrain->wait_for(0ms) != std::future_status::ready &&
             std::chrono::steady_clock::now() < sourceDrainDeadline) {
        (void)issuePendingGrant();
        environment->pumpUntilWithAttributeAuthority([&] {
          return ownerBoundarySourceDrain->wait_for(0ms) == std::future_status::ready ||
            hasPendingGrant() || std::chrono::steady_clock::now() >= sourceDrainDeadline;
        });
      }
      const bool sourceDrained =
        ownerBoundarySourceDrain->wait_for(0ms) == std::future_status::ready &&
        ownerBoundarySourceDrain->get();
      BOOST_CHECK_MESSAGE(sourceDrained,
        "source registration did not drain after Owner packet-set close");
    }

    destinationRegistration.close();
    destinationRuntime = {};
    return {};
  }
  const ndn::Name publicationServiceName(service);
  const ndn::Name publicationRequestId("spec204-cold-provider-event");
  PreparedServiceRequest preparedPublication;
  preparedPublication.serviceName = publicationServiceName;
  preparedPublication.requestId = publicationRequestId;
  const auto packetCommitsBeforeInvalidContext = rangeStore->packetCommitCalls();
  BOOST_CHECK_EXCEPTION(
    providerRuntime.publishProtectedKvCheckpoint(
      ndn::Name(requester), ndn::Name(), publicationRequestId,
      files.root / "committed.ndkve", protectedCheckpoint, storageOwner, rangeStore,
      protectedLimits, {}),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()).find("Core service and request identities") !=
             std::string::npos;
    });
  BOOST_CHECK_EXCEPTION(
    providerRuntime.publishProtectedKvCheckpoint(
      ndn::Name(requester), publicationServiceName, ndn::Name(),
      files.root / "committed.ndkve", protectedCheckpoint, storageOwner, rangeStore,
      protectedLimits, {}),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()).find("Core service and request identities") !=
             std::string::npos;
    });
  BOOST_TEST(rangeStore->packetCommitCalls() == packetCommitsBeforeInvalidContext);
  BOOST_CHECK_EXCEPTION(
    providerRuntime.publishProtectedKvCheckpoint(
      ndn::Name("/Spec204/NotTheOwner"), publicationServiceName,
      publicationRequestId, files.root / "committed.ndkve",
      protectedCheckpoint, storageOwner, rangeStore, protectedLimits, {}),
    std::invalid_argument,
    [](const std::invalid_argument& error) {
      return std::string(error.what()).find("authenticated requester") != std::string::npos;
    });
  BOOST_TEST_MESSAGE("Spec204 diagnostic: starting durable publication");
  auto publicationWork = std::async(std::launch::async, [&] {
    return providerRuntime.publishProtectedKvCheckpoint(ndn::Name(requester),
      publicationServiceName, publicationRequestId,
      files.root / "committed.ndkve", protectedCheckpoint, storageOwner, rangeStore, protectedLimits,
      [publicationCancelled] {
        if (publicationCancelled->load()) throw std::runtime_error("causal publication fixture timed out");
      });
  });
  const auto publicationDeadline = std::chrono::steady_clock::now() + 30s;
  while (publicationWork.wait_for(0ms) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= publicationDeadline) publicationCancelled->store(true);
    (void)issuePendingGrant();
    environment->pumpUntilWithAttributeAuthority([&] {
      return publicationWork.wait_for(0ms) == std::future_status::ready || hasPendingGrant() ||
        std::chrono::steady_clock::now() >= publicationDeadline;
    });
  }
  BOOST_TEST_MESSAGE("Spec204 diagnostic: durable publication worker returned");
  if (fault == CausalRecoveryFault::RejectedPublicationGrant ||
      fault == CausalRecoveryFault::WrongOwnerPublicationGrant ||
      fault == CausalRecoveryFault::WrongWriterPublicationGrant ||
      fault == CausalRecoveryFault::ChangedRootPublicationGrant ||
      fault == CausalRecoveryFault::ExpiredPublicationGrant ||
      fault == CausalRecoveryFault::CancelledPublicationAtGrant ||
      fault == CausalRecoveryFault::RejectedPublicationCommitGate) {
    if (fault == CausalRecoveryFault::WrongOwnerPublicationGrant) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(publicationWork.get()), std::runtime_error,
        [](const std::runtime_error& error) {
          BOOST_TEST_MESSAGE("Rejected Owner publication grant: " << error.what());
          return std::string(error.what()) ==
            "protected KV publication unavailable: DURABLE_OWNER_BINDING_MISMATCH";
        });
    }
    else if (fault == CausalRecoveryFault::WrongWriterPublicationGrant ||
             fault == CausalRecoveryFault::ChangedRootPublicationGrant) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(publicationWork.get()), std::runtime_error,
        [](const std::runtime_error& error) {
          BOOST_TEST_MESSAGE("Rejected publication grant binding: " << error.what());
          return std::string(error.what()) ==
            "protected KV publication unavailable: DURABLE_GRANT_BATCH_MISMATCH";
        });
    }
    else if (fault == CausalRecoveryFault::ExpiredPublicationGrant) {
      BOOST_CHECK_EXCEPTION(static_cast<void>(publicationWork.get()), std::runtime_error,
        [](const std::runtime_error& error) {
          BOOST_TEST_MESSAGE("Rejected expired publication grant: " << error.what());
          return std::string(error.what()) ==
            "protected KV publication unavailable: repo-user-storage-grant-expired";
        });
    }
    else if (fault == CausalRecoveryFault::RejectedPublicationCommitGate) {
      BOOST_CHECK_THROW(static_cast<void>(publicationWork.get()), std::runtime_error);
    }
    else {
      BOOST_CHECK_THROW(static_cast<void>(publicationWork.get()), std::runtime_error);
    }
    BOOST_REQUIRE(rejectedPublicationChallenge.has_value());
    BOOST_CHECK(!rejectedPublicationChallenge->manifestName.empty());
    BOOST_CHECK(ndn::Name(provider).isPrefixOf(rejectedPublicationChallenge->manifestName));
    BOOST_CHECK_EQUAL(rejectedPublicationChallenge->writerIdentity, provider);
    BOOST_CHECK_EQUAL(rejectedPublicationChallenge->publicationIdentity,
      expectedPublicationIdentity(protectedCheckpoint));
    BOOST_CHECK(!rejectedPublicationChallenge->dataPrefix.empty());
    BOOST_CHECK_GT(rejectedPublicationChallenge->packetWireBytes, 0U);
    if (fault == CausalRecoveryFault::CancelledPublicationAtGrant)
      BOOST_CHECK(publicationCancelled->load());
    BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(),
      fault == CausalRecoveryFault::ExpiredPublicationGrant ? 1U : 0U);
    BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), 0U);
    BOOST_CHECK_EQUAL(commitGateCalls->load(),
      fault == CausalRecoveryFault::ExpiredPublicationGrant ||
      fault == CausalRecoveryFault::RejectedPublicationCommitGate ? 1U : 0U);
    BOOST_CHECK_EQUAL(commitGateObservedGrant->load(),
      fault == CausalRecoveryFault::ExpiredPublicationGrant ||
      fault == CausalRecoveryFault::RejectedPublicationCommitGate);
    const auto usage = journal->userStorageUsage(requester, repoName);
    BOOST_CHECK_EQUAL(usage.committedBytes, 0U);
    BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
    return {};
  }
  auto publication = publicationWork.get(); publication.requireCurrent();
  BOOST_TEST_MESSAGE("Spec204 diagnostic: durable publication committed");
  BOOST_REQUIRE(rejectedPublicationChallenge.has_value());
  BOOST_CHECK_EQUAL(publication.inputPublicationIdentity(),
    rejectedPublicationChallenge->publicationIdentity);
  BOOST_CHECK_EQUAL(commitGateCalls->load(), 1U);
  BOOST_CHECK(commitGateObservedGrant->load());
  BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), 1U);
  BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), 1U);
  BOOST_CHECK(ndn::Name(provider).isPrefixOf(publication.transport.manifestName));
  const auto originalManifest = publication.transport.durableStorageSource->readOriginalDataPacket(publication.transport.manifestName);
  BOOST_REQUIRE(!originalManifest.empty());
  std::vector<std::pair<ndn::Name, std::vector<std::uint8_t>>> persistedPackets;
  persistedPackets.emplace_back(publication.transport.manifestName, originalManifest);
  BOOST_CHECK(originalManifest == repo->core().getDataPacket(publication.transport.manifestName.toUri()));
  const ndn::Data originalManifestPacket(ndn::Block(ndn::span<const std::uint8_t>(
    originalManifest.data(), originalManifest.size())));
  const auto& manifestContent = originalManifestPacket.getContent();
  const std::vector<std::uint8_t> manifestPayload(
    manifestContent.value(), manifestContent.value() + manifestContent.value_size());
  const auto manifest = ProtectedBlobManifest::decode(
    manifestPayload, ndn::Name(provider), publication.transport.manifestName);
  BOOST_REQUIRE(publicationChallengeDataPrefix.has_value());
  BOOST_CHECK(publicationChallengeDataPrefix.value() == manifest.dataPrefix);
  for (std::uint64_t segment = 0; segment < manifest.segmentCount; ++segment) {
    auto segmentName = manifest.dataPrefix;
    segmentName.appendSegment(segment);
    const auto packet = publication.transport.durableStorageSource->readOriginalDataPacket(segmentName);
    BOOST_REQUIRE(!packet.empty());
    persistedPackets.emplace_back(segmentName, packet);
    BOOST_CHECK(packet == repo->core().getDataPacket(segmentName.toUri()));
    const ndn::Data decoded(ndn::Block(ndn::span<const std::uint8_t>(packet.data(), packet.size())));
    BOOST_REQUIRE(decoded.getFinalBlock());
    BOOST_CHECK(decoded.getFinalBlock()->isSegment());
    BOOST_CHECK_EQUAL(decoded.getFinalBlock()->toSegment(), manifest.segmentCount - 1);
  }
  for (const auto& packetName : publication.transport.auxiliaryPacketNames) {
    const auto packet = publication.transport.durableStorageSource->readOriginalDataPacket(packetName);
    BOOST_REQUIRE(!packet.empty());
    persistedPackets.emplace_back(packetName, packet);
    BOOST_CHECK(packet == repo->core().getDataPacket(packetName.toUri()));
  }
  const auto usageBeforeRetry = journal->userStorageUsage(requester, repoName);
  BOOST_REQUIRE(resolvedRoleGrant->has_value());
  const auto pinnedGrant = resolvedRoleGrant->value();
  NativeDurableStorageOwner retryOwner{requester,
    [pinnedGrant](const ndn::Name& manifestName, const std::string& writer,
                  const std::function<void()>& active) {
      if (active) active();
      if (manifestName.toUri(ndn::name::UriFormat::ALTERNATE) != pinnedGrant.objectName ||
          ndn::Name(writer) != ndn::Name(pinnedGrant.writerIdentity))
        throw std::runtime_error("Spec204 retry does not match the granted Producer object");
      return DurableStorageGrantSelector{pinnedGrant.repoNode, pinnedGrant.ownerIdentity,
        pinnedGrant.grantId, pinnedGrant.ownerEpoch};
    }, {}};
  auto retryBoundaryCalls = std::make_shared<std::atomic<unsigned>>(0);
  retryOwner.onDurablePacketSetBoundary = [retryBoundaryCalls] { ++*retryBoundaryCalls; };
  BOOST_TEST_MESSAGE("Spec204 diagnostic: starting idempotent publication retry");
  auto retryCancelled = std::make_shared<std::atomic<bool>>(false);
  auto retryWork = std::async(std::launch::async, [&] {
    return nativePublishProtectedPortableKvCheckpoint(providerProducer, preparedPublication,
      files.root / "committed.ndkve", protectedCheckpoint, retryOwner, rangeStore, protectedLimits,
      [retryCancelled] {
        if (retryCancelled->load()) throw std::runtime_error("causal retry fixture timed out");
      });
  });
  const auto retryDeadline = std::chrono::steady_clock::now() + 30s;
  while (retryWork.wait_for(0ms) != std::future_status::ready) {
    if (std::chrono::steady_clock::now() >= retryDeadline) retryCancelled->store(true);
    environment->pumpUntilWithAttributeAuthority([&] {
      return retryWork.wait_for(0ms) == std::future_status::ready;
    });
  }
  auto retry = retryWork.get();
  retry.requireCurrent();
  BOOST_TEST_MESSAGE("Spec204 diagnostic: idempotent publication retry returned");
  BOOST_CHECK_EQUAL(retryBoundaryCalls->load(), 1U);
  BOOST_CHECK_EQUAL(rangeStore->packetCommitCalls(), 1U);
  BOOST_CHECK_EQUAL(rangeStore->successfulPacketCommitCalls(), 1U);
  BOOST_CHECK(retry.transport.manifestName == publication.transport.manifestName);
  BOOST_CHECK(retry.transport.dataPrefix == publication.transport.dataPrefix);
  BOOST_CHECK_EQUAL(retry.transport.durableStorageSource->size(),
    publication.transport.durableStorageSource->size());
  BOOST_CHECK(retry.transport.durableStorageSource->readOriginalDataPacket(
    retry.transport.manifestName) == originalManifest);
  for (std::uint64_t segment = 0; segment < manifest.segmentCount; ++segment) {
    auto segmentName = manifest.dataPrefix;
    segmentName.appendSegment(segment);
    const auto retryPacket = retry.transport.durableStorageSource->readOriginalDataPacket(segmentName);
    BOOST_REQUIRE(!retryPacket.empty());
    BOOST_CHECK(retryPacket == publication.transport.durableStorageSource->readOriginalDataPacket(segmentName));
    const ndn::Data decoded(ndn::Block(ndn::span<const std::uint8_t>(
      retryPacket.data(), retryPacket.size())));
    BOOST_REQUIRE(decoded.getFinalBlock());
    BOOST_CHECK(decoded.getFinalBlock()->isSegment());
    BOOST_CHECK_EQUAL(decoded.getFinalBlock()->toSegment(), manifest.segmentCount - 1);
  }
  for (const auto& packetName : publication.transport.auxiliaryPacketNames) {
    BOOST_CHECK(retry.transport.durableStorageSource->readOriginalDataPacket(packetName) ==
      publication.transport.durableStorageSource->readOriginalDataPacket(packetName));
  }
  const auto usage = journal->userStorageUsage(requester, repoName);
  BOOST_CHECK_EQUAL(usage.committedBytes, usageBeforeRetry.committedBytes);
  BOOST_CHECK_EQUAL(usage.reservedBytes, usageBeforeRetry.reservedBytes);
  BOOST_CHECK_EQUAL(usage.limitBytes, 1000000000ULL); BOOST_CHECK_GT(usage.committedBytes, 0U);
  BOOST_CHECK_EQUAL(usage.reservedBytes, 0U);
  const auto authorizeProducer = [&producerCertificate] (const ndn::Data& data) {
    return ndn::security::verifySignature(data, producerCertificate);
  };
  const auto runWithEnvironmentPump = [&] (auto operation) {
    auto work = std::async(std::launch::async, std::move(operation));
    while (work.wait_for(0ms) != std::future_status::ready) {
      environment->pumpUntilWithAttributeAuthority([&] {
        return work.wait_for(0ms) == std::future_status::ready;
      });
    }
    return work.get();
  };
  // Every recovery scenario needs the same authorized remote handoff before
  // exercising its import/cancellation boundary on the successor.
  repo->registerOriginalDataServing(environment->providerFace(1));
  const auto ownerReference = nativeProtectedKvPublicationReference(publication);
  ndnsf_distributed_repo::RepoUserStoragePacketSetFetchLimits remoteFetchLimits;
  // First use of the scoped MANIFEST service bootstraps its NAC-ABE state.
  remoteFetchLimits.ackCollectionTimeMs = 1000;
  const std::map<std::string, NativeRemoteProtectedKvRoleSourceV1> remoteSources{
    {source.role, {storageTarget, ownerReference, authorizeProducer, remoteFetchLimits}}};
  const std::map<std::string, std::string> destinationProviders{{source.role, successor}};
  std::uint64_t actualRepoManifestBytes = 0;
  if (validatesRemoteStorage) {
    ndn::util::Sha256 sourcePacketDigest;
    for (const auto& packet : persistedPackets) {
      sourcePacketDigest.update(ndn::span<const std::uint8_t>(
        packet.second.data(), packet.second.size()));
    }
    const ndn::Name packetSetObjectName(
      ownerReference.at("manifestName").get<std::string>());
    const auto repoPacketManifest = repo->core().getManifest(
      packetSetObjectName.toUri(ndn::name::UriFormat::ALTERNATE));
    BOOST_CHECK_EQUAL(repoPacketManifest.sha256, lowerHex(sourcePacketDigest.toString()));
    BOOST_REQUIRE_EQUAL(repoPacketManifest.packetNames.size(), persistedPackets.size());
    for (std::size_t i = 0; i < persistedPackets.size(); ++i) {
      BOOST_CHECK_EQUAL(repoPacketManifest.packetNames[i],
        persistedPackets[i].first.toUri(ndn::name::UriFormat::CANONICAL));
    }

    ndn::util::Sha256 fetchedPacketDigest;
    for (const auto& packet : persistedPackets) {
      const auto fetchedWire = runWithEnvironmentPump([&] {
        return ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketFromUserStorage(
          *requesterUser, storageTarget, packet.first, packet.second.size(),
          authorizeProducer, 2000);
      });
      BOOST_CHECK(fetchedWire == packet.second);
      fetchedPacketDigest.update(ndn::span<const std::uint8_t>(
        fetchedWire.data(), fetchedWire.size()));
    }
    BOOST_CHECK_EQUAL(lowerHex(fetchedPacketDigest.toString()), repoPacketManifest.sha256);
    BOOST_TEST_MESSAGE("Spec204 diagnostic: fetching remote publication proof");
    {
      auto remoteProof = runWithEnvironmentPump([&] {
        return nativeFetchAndValidateRemoteProtectedKvPublicationV1(
          *requesterUser, storageTarget, ownerReference, authorizeProducer, 20000,
          remoteFetchLimits);
      });
      const auto observedClaim = remoteProof.keyGraph().packetSet().claim();
      actualRepoManifestBytes = observedClaim.object.manifestBytes;
      BOOST_REQUIRE_GT(actualRepoManifestBytes, 1U);
      auto reboundClaim = observedClaim;
      ++reboundClaim.ownerEpoch;
      BOOST_CHECK_THROW(runWithEnvironmentPump([&] {
        return ndnsf_distributed_repo::RepoClient::revalidateCurrentUserStorageClaim(
          *requesterUser, storageTarget, reboundClaim, 5000);
      }), std::runtime_error);
    } // A fetched proof cannot be retained and directly promoted.
    BOOST_TEST_MESSAGE("Spec204 diagnostic: remote publication proof validated");
    if (verifyMixedUriClaimManifest) {
      BOOST_REQUIRE(mixedUriRepo);
      BOOST_REQUIRE(mixedUriJournal);
      BOOST_REQUIRE(mixedUriTarget.has_value());

      const auto objectName = packetSetObjectName.toUri(ndn::name::UriFormat::ALTERNATE);
      const auto grantId = nativePlanningDigest("spec212-mixed-uri-owner-grant:" + objectName);
      const auto grantRequest = ndnsf_distributed_repo::RepoUserStorageGrantRequest{
        1, grantId, provider, objectName, causalNow() + 180000};
      auto grantPromise = std::make_shared<std::promise<
        ndnsf_distributed_repo::RepoUserStorageGrant>>();
      auto grantFuture = grantPromise->get_future();
      const auto grantRequestId =
        ndnsf_distributed_repo::RepoClient::requestUserStorageGrant(
          *requesterUser, *mixedUriTarget, grantRequest, 10000,
          [grantPromise] (const ndn::Name&) {
            try {
              grantPromise->set_exception(std::make_exception_ptr(
                std::runtime_error("mixed-URI Repo owner grant timed out")));
            }
            catch (const std::future_error&) {}
          },
          [grantPromise, target = *mixedUriTarget, requester, grantRequest]
          (const ndn_service_framework::ResponseMessage& response) {
            try {
              if (!response.getStatus())
                throw std::runtime_error("mixed-URI Repo owner grant failed: " +
                                         response.getErrorInfo());
              const auto payload = response.getPayload();
              const auto reply = ndnsf_distributed_repo::RepoUserStorageGrantReply::fromJson(
                std::string(payload.begin(), payload.end()));
              reply.validateExpected(target, requester, grantRequest);
              grantPromise->set_value(reply.grant);
            }
            catch (...) {
              try { grantPromise->set_exception(std::current_exception()); }
              catch (const std::future_error&) {}
            }
          });
      BOOST_REQUIRE(!grantRequestId.empty());
      const auto grant = runWithEnvironmentPump([&] { return grantFuture.get(); });

      std::vector<std::vector<std::uint8_t>> packetWires;
      packetWires.reserve(persistedPackets.size());
      for (const auto& packet : persistedPackets) {
        const auto accepted = runWithEnvironmentPump([&] {
          return environment->provider(destinationIndex).validateProducerPacketWireUntil(
            packet.second, ndn::Name(provider),
            std::chrono::steady_clock::now() + 10000ms);
        });
        BOOST_REQUIRE_MESSAGE(accepted,
          "mixed-URI regression input must pass the configured Producer TrustSchema");
        packetWires.push_back(packet.second);
      }

      ndnsf_distributed_repo::RepoDataReference mixedReference;
      mixedReference.objectName = objectName;
      mixedReference.dataPrefix = ndn::Name(provider).toUri(ndn::name::UriFormat::ALTERNATE);
      mixedReference.expectedSha256 = repoPacketManifest.sha256;
      mixedReference.expectedSize = repoPacketManifest.size;
      mixedReference.storeWirePackets = true;
      mixedReference.objectType = "original-packet-set";
      mixedReference.packetSetMetadata = repoPacketManifest;
      ndnsf_distributed_repo::RepoUserStorageInsertRequest insertRequest;
      insertRequest.operationId = nativePlanningDigest(
        "spec212-mixed-uri-owned-insert:" + grant.grantId);
      insertRequest.generation = 1;
      insertRequest.reference = std::move(mixedReference);
      insertRequest.storageBinding = ndnsf_distributed_repo::RepoUserStorageBinding{
        1, requester, grant.grantId, grant.ownerEpoch};
      const auto insertStatus = mixedUriRepo->insertWirePackets(
        insertRequest, packetWires, provider);
      BOOST_REQUIRE_EQUAL(insertStatus.state, "DONE");
      BOOST_REQUIRE_EQUAL(insertStatus.completedSegments, persistedPackets.size());

      const auto mixedManifest = mixedUriRepo->core().getManifest(objectName);
      BOOST_REQUIRE_EQUAL(mixedManifest.objectName,
        packetSetObjectName.toUri(ndn::name::UriFormat::CANONICAL));
      const auto mixedClaims = mixedUriJournal->listUserStorage(requester,
        mixedUriRepoName, "", 1000);
      const auto mixedClaim = std::find_if(mixedClaims.begin(), mixedClaims.end(),
        [&objectName] (const auto& claim) {
          return claim.object.objectName == objectName;
        });
      BOOST_REQUIRE(mixedClaim != mixedClaims.end());
      BOOST_REQUIRE_EQUAL(mixedClaim->object.objectName, objectName);
      BOOST_REQUIRE_NE(mixedClaim->object.objectName, mixedManifest.objectName);
      BOOST_REQUIRE(ndn::Name(mixedClaim->object.objectName) ==
                    ndn::Name(mixedManifest.objectName));
      for (const auto& packet : persistedPackets) {
        BOOST_CHECK(mixedUriRepo->core().getDataPacket(packet.first.toUri()) == packet.second);
      }
      const auto mixedUsage = mixedUriJournal->userStorageUsage(requester, mixedUriRepoName);
      BOOST_CHECK_GT(mixedUsage.committedBytes, 0U);
      BOOST_CHECK_EQUAL(mixedUsage.reservedBytes, 0U);
      const auto claimCountBeforeNegative = mixedClaims.size();

      auto mixedProof = runWithEnvironmentPump([&] {
        return nativeFetchAndValidateRemoteProtectedKvPublicationV1(
          *requesterUser, *mixedUriTarget, ownerReference, authorizeProducer, 20000,
          remoteFetchLimits);
      });
      BOOST_CHECK(mixedProof.keyGraph().packetSet().target() == *mixedUriTarget);
      BOOST_CHECK(ndn::Name(mixedProof.keyGraph().packetSet().claim().object.objectName) ==
                  ndn::Name(mixedProof.keyGraph().packetSet().manifest().objectName));
      BOOST_CHECK(mixedProof.keyGraph().packetSet().packetWires() == packetWires);

      const auto usageBeforeChangedName = mixedUriJournal->userStorageUsage(
        requester, mixedUriRepoName);
      auto changedNameReference = ownerReference;
      auto changedManifestName = packetSetObjectName;
      changedManifestName.append("spec212-changed-object-name");
      changedNameReference["manifestName"] =
        changedManifestName.toUri(ndn::name::UriFormat::CANONICAL);
      BOOST_CHECK_THROW(runWithEnvironmentPump([&] {
        return nativeFetchAndValidateRemoteProtectedKvPublicationV1(
          *requesterUser, *mixedUriTarget, changedNameReference, authorizeProducer,
          20000, remoteFetchLimits);
      }), std::exception);
      const auto usageAfterChangedName = mixedUriJournal->userStorageUsage(
        requester, mixedUriRepoName);
      BOOST_CHECK_EQUAL(usageAfterChangedName.committedBytes,
        usageBeforeChangedName.committedBytes);
      BOOST_CHECK_EQUAL(usageAfterChangedName.reservedBytes,
        usageBeforeChangedName.reservedBytes);
      BOOST_CHECK_EQUAL(mixedUriJournal->listUserStorage(
        requester, mixedUriRepoName, "", 1000).size(), claimCountBeforeNegative);
    }
    auto budgetVerifierCalls = std::make_shared<std::atomic<unsigned>>(0);
    auto preflightSources = remoteSources;
    preflightSources.at(source.role).verifyProducerSignature =
      [budgetVerifierCalls, &producerCertificate] (const ndn::Data& data) {
        ++*budgetVerifierCalls;
        return ndn::security::verifySignature(data, producerCertificate);
      };
    auto missingRoleSources = preflightSources;
    missingRoleSources.erase(source.role);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, missingRoleSources, 20000), std::invalid_argument);
    auto extraRoleSources = preflightSources;
    extraRoleSources.emplace("/unexpected-role", preflightSources.at(source.role));
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, extraRoleSources, 20000), std::runtime_error);
    auto unexpectedRoleSources = preflightSources;
    const auto unexpectedRoleSource = unexpectedRoleSources.at(source.role);
    unexpectedRoleSources.clear();
    unexpectedRoleSources.emplace("/unexpected-role", unexpectedRoleSource);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, unexpectedRoleSources, 20000), std::runtime_error);
    auto wrongRepoEndpointSources = preflightSources;
    wrongRepoEndpointSources.at(source.role).target.providerIdentity = "/provider/wrong";
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, wrongRepoEndpointSources, 20000), std::runtime_error);
    auto overBudgetSources = preflightSources;
    overBudgetSources.at(source.role).limits.maxPacketWireBytes = (256ULL << 20) + 1;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overBudgetSources, 20000), std::runtime_error);
    auto overPageBudgetSources = preflightSources;
    overPageBudgetSources.at(source.role).limits.maxListingPagesPerScan = 513;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overPageBudgetSources, 20000), std::runtime_error);
    auto overObjectBudgetSources = preflightSources;
    overObjectBudgetSources.at(source.role).limits.maxListedObjectsPerScan = 51201;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overObjectBudgetSources, 20000), std::runtime_error);
    auto overListingWireBudgetSources = preflightSources;
    overListingWireBudgetSources.at(source.role).limits.maxListingWireBytes = (64ULL << 20) + 1;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overListingWireBudgetSources, 20000), std::runtime_error);
    auto overPacketCountSources = preflightSources;
    overPacketCountSources.at(source.role).limits.maxPacketCount = 16385;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overPacketCountSources, 20000), std::runtime_error);
    auto overManifestBudgetSources = preflightSources;
    overManifestBudgetSources.at(source.role).limits.maxManifestWireBytes = (16ULL << 20) + 1;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, overManifestBudgetSources, 20000), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);
    const auto beforeRemotePromote = conversations->find(reference.conversationId);
    BOOST_REQUIRE(beforeRemotePromote);
    BOOST_CHECK(!beforeRemotePromote->protectedKvReferences);

    // Exercise valid, positive fetch limits that are below the actual committed
    // packet set. The upper-bound tests above reject malformed configuration;
    // these cases must reach Repo, read its authenticated manifest, and reject
    // the concrete packet-count/aggregate-wire claims without promotion.
    BOOST_REQUIRE_GT(repoPacketManifest.packetNames.size(), 1U);
    auto actualPacketCountLimitSources = preflightSources;
    actualPacketCountLimitSources.at(source.role).limits.maxPacketCount =
      static_cast<std::uint32_t>(repoPacketManifest.packetNames.size() - 1);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, actualPacketCountLimitSources, 20000), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);

    auto actualManifestLimitSources = preflightSources;
    actualManifestLimitSources.at(source.role).limits.maxManifestWireBytes =
      actualRepoManifestBytes - 1;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, actualManifestLimitSources, 20000), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);

    std::uint64_t actualPacketWireBytes = 0;
    for (const auto& packet : persistedPackets) {
      BOOST_REQUIRE_LE(packet.second.size(),
        std::numeric_limits<std::uint64_t>::max() - actualPacketWireBytes);
      actualPacketWireBytes += packet.second.size();
    }
    BOOST_REQUIRE_GT(actualPacketWireBytes, 1U);
    auto actualPacketWireLimitSources = preflightSources;
    actualPacketWireLimitSources.at(source.role).limits.maxPacketWireBytes =
      actualPacketWireBytes - 1;
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, actualPacketWireLimitSources, 20000), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);
    const auto afterBoundedFetches = conversations->find(reference.conversationId);
    BOOST_REQUIRE(afterBoundedFetches);
    BOOST_CHECK(!afterBoundedFetches->protectedKvReferences);
    BOOST_CHECK(afterBoundedFetches->nextTurnProvidersByRole ==
                beforeRemotePromote->nextTurnProvidersByRole);

    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, remoteSources, 20000,
      {{"/unexpected-role", successor}}), std::runtime_error);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, remoteSources, 20000,
      beforeRemotePromote->checkpoint.providersByRole), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, preflightSources, 20000, destinationProviders,
      [] { return true; }), std::runtime_error);
    BOOST_CHECK_EQUAL(budgetVerifierCalls->load(), 0U);
    const auto afterCancelledPromotion = conversations->find(reference.conversationId);
    BOOST_REQUIRE(afterCancelledPromotion);
    BOOST_CHECK(!afterCancelledPromotion->protectedKvReferences);
    BOOST_CHECK(afterCancelledPromotion->nextTurnProvidersByRole ==
                beforeRemotePromote->nextTurnProvidersByRole);
    const std::map<std::string, std::string> provisionalDestinationProviders{
      {source.role, "/provider/spec204-provisional-destination"}};
    BOOST_TEST_MESSAGE("Spec204 diagnostic: starting remote checkpoint promotion");
    const auto firstPromotion = runWithEnvironmentPump([&] {
      return NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
        *requesterUser, *conversations, reference.conversationId,
        reference.checkpointDigest, remoteSources, 20000, provisionalDestinationProviders);
    });
    BOOST_REQUIRE(firstPromotion.protectedKvReferences);
    BOOST_CHECK(firstPromotion.checkpoint.providersByRole == beforeRemotePromote->checkpoint.providersByRole);
    BOOST_CHECK(firstPromotion.nextTurnProvidersByRole == provisionalDestinationProviders);

    // A repeated operation must re-fetch/revalidate the same current Repo set
    // before atomically updating only the pending destination role map.
    const auto promoted = runWithEnvironmentPump([&] {
      return NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
        *requesterUser, *conversations, reference.conversationId,
        reference.checkpointDigest, remoteSources, 20000, destinationProviders);
    });
    BOOST_REQUIRE(promoted.protectedKvReferences);
    BOOST_CHECK(promoted.protectedKvReferences == firstPromotion.protectedKvReferences);
    BOOST_CHECK_EQUAL(promoted.protectedKvReferences->at(source.role).at("manifestName"),
      publication.transport.manifestName.toUri(ndn::name::UriFormat::CANONICAL));
    BOOST_CHECK(promoted.checkpoint.providersByRole == beforeRemotePromote->checkpoint.providersByRole);
    BOOST_CHECK(promoted.nextTurnProvidersByRole == destinationProviders);
    BOOST_CHECK_THROW(NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
      *requesterUser, *conversations, reference.conversationId,
      reference.checkpointDigest, missingRoleSources, 20000, provisionalDestinationProviders),
      std::invalid_argument);
    const auto afterRejectedUpdate = conversations->find(reference.conversationId);
    BOOST_REQUIRE(afterRejectedUpdate);
    BOOST_CHECK(afterRejectedUpdate->nextTurnProvidersByRole == destinationProviders);
    conversations->restore();
    const auto restoredHandoff = conversations->find(reference.conversationId);
    BOOST_REQUIRE(restoredHandoff);
    BOOST_CHECK(restoredHandoff->checkpoint.providersByRole == beforeRemotePromote->checkpoint.providersByRole);
    BOOST_CHECK(restoredHandoff->nextTurnProvidersByRole == destinationProviders);
    BOOST_TEST_MESSAGE("Spec204 diagnostic: remote checkpoint promotion and restore completed");

    NativeConversationContinuation nextContinuation;
    nextContinuation.conversationId = reference.conversationId;
    nextContinuation.parentContextEpoch = restoredHandoff->checkpoint.successorContextEpoch;
    nextContinuation.serviceName = restoredHandoff->serviceName;
    nextContinuation.planRoleMapDigest = restoredHandoff->planRoleMapDigest;
    nextContinuation.parentCheckpointDigest = restoredHandoff->checkpoint.checkpointDigest;
    nextContinuation.requestContractDigest = nativePlanningDigest("spec204-next-map-binding");
    nextContinuation.retentionDeadlineMs = restoredHandoff->retentionDeadlineMs;
    nextContinuation.mode = "APPEND_DELTA";
    nextContinuation.parentCheckpointWire = restoredHandoff->checkpoint.wire;
    nextContinuation.generationId = std::string(32, '2');
    nextContinuation.canonicalTokenIds = restoredHandoff->checkpoint.transcript.at("canonicalTokenIds")
      .get<std::vector<std::int64_t>>();
    nextContinuation.canonicalTokenIds.push_back(6);
    nextContinuation.expectedRoles = restoredHandoff->checkpoint.expectedRoles;
    const auto nextTurn = conversations->beginTurn(nextContinuation, "/request/spec204-next-map");
    BOOST_CHECK(nextTurn.parent.expectedProvidersByRole == destinationProviders);
    const auto recovery = conversations->protectedKvRecoveryForTurn(nextTurn);
    BOOST_REQUIRE(recovery);
    BOOST_CHECK_THROW(conversations->bindRecoveryPlanRoleMap(
      nextTurn, restoredHandoff->checkpoint.providersByRole, *recovery), std::runtime_error);
    const NativeDiError cleanupError("SPEC204_TEST_TURN_ABORT", "test", "placement",
      "abort map binding fixture", "/request/spec204-next-map", 1);
    conversations->abortTurn(nextTurn, cleanupError);
    const auto afterMismatch = conversations->find(reference.conversationId);
    BOOST_REQUIRE(afterMismatch);
    BOOST_CHECK(afterMismatch->nextTurnProvidersByRole == destinationProviders);

    if (fault == CausalRecoveryFault::None) {
      // Keep both source and destination Providers online and deliberately make
      // the final planner proposal select the source. The request must fail at
      // NativeInferenceClient's Owner-map guard before Core commits a plan or
      // either Provider executes the role.
      auto destinationRuntime = makeNativeProviderCollaborationRuntime(makeConfig(1, successorFactory));
      auto destinationRegistration = installHandler(1, destinationRuntime);
      const auto sourceRunnerCountBefore = factory->created().size();
      const auto sourceRunner = factory->created().front().lock();
      BOOST_REQUIRE(sourceRunner);
      const auto beforeWrongPlan = sourceRunner->runtimeMetricsSnapshot();
      BOOST_REQUIRE(beforeWrongPlan);

      auto wrongPlanContinuation = nextContinuation;
      wrongPlanContinuation.requestContractDigest.clear();
      wrongPlanContinuation.generationId = std::string(32, '4');
      const auto wrongPlanInput = causalInput(source.model.descriptor, {6}, tokenizer, 1);
      auto wrongPlanOptions = options;
      wrongPlanOptions.stream->generationId.fill(0x44);
      wrongPlanOptions.generation = nativeGenerationFromOptions(wrongPlanInput.options,
        std::string(32, '4'));
      wrongPlanOptions.conversation = std::move(wrongPlanContinuation);
      wrongPlanOptions.providerNames = {ndn::Name(provider), ndn::Name(successor)};
      const auto wrongPlan = client.request(model, wrongPlanInput, splitter,
        std::make_shared<CausalWrongDestinationPlacement>(provider), wrongPlanOptions);
      const auto wrongPlanDeadline = std::chrono::steady_clock::now() + 15s;
      while (wrongPlan.status() == NativeRequestStatus::Pending &&
             std::chrono::steady_clock::now() < wrongPlanDeadline)
        environment->pumpUntilWithAttributeAuthority([&] {
          return wrongPlan.status() != NativeRequestStatus::Pending;
        });
      BOOST_REQUIRE(wrongPlan.status() == NativeRequestStatus::Failed);
      bool rejectedByOwnerMapGuard = false;
      try {
        (void)wrongPlan.result(0ms);
      }
      catch (const NativeDiError& error) {
        rejectedByOwnerMapGuard = error.code() == "NATIVE_CONVERSATION_DESTINATION_MAP_MISMATCH";
      }
      BOOST_CHECK(rejectedByOwnerMapGuard);
      const auto afterWrongPlan = sourceRunner->runtimeMetricsSnapshot();
      BOOST_REQUIRE(afterWrongPlan);
      BOOST_CHECK_EQUAL(afterWrongPlan->executedPrefillTokens, beforeWrongPlan->executedPrefillTokens);
      BOOST_CHECK_EQUAL(afterWrongPlan->executedDecodeTokens, beforeWrongPlan->executedDecodeTokens);
      BOOST_CHECK_EQUAL(factory->created().size(), sourceRunnerCountBefore);
      BOOST_CHECK(successorFactory->created().empty());
      const auto stillPending = conversations->find(reference.conversationId);
      BOOST_REQUIRE(stillPending);
      BOOST_CHECK(stillPending->nextTurnProvidersByRole == destinationProviders);
      destinationRegistration.close();
      destinationRuntime = {};
    }
  }
  else {
    const auto promoted = runWithEnvironmentPump([&] {
      return NativeRemoteProtectedKvSetPromoter::fetchAndPromote(
        *requesterUser, *conversations, reference.conversationId,
        reference.checkpointDigest, remoteSources, 20000, destinationProviders);
    });
    BOOST_REQUIRE(promoted.protectedKvReferences);
    BOOST_CHECK(promoted.checkpoint.providersByRole == record->checkpoint.providersByRole);
    BOOST_CHECK(promoted.nextTurnProvidersByRole == destinationProviders);
  }
  const auto recoveryReady = conversations->find(reference.conversationId);
  BOOST_REQUIRE(recoveryReady);
  BOOST_REQUIRE(recoveryReady->nextTurnProvidersByRole == destinationProviders);
  publisher.reset();
  closeSourceRegistration(); providerRuntime = {};
  const auto retirementDeadline = std::chrono::steady_clock::now() + 5s;
  while (!oldRunner.expired() && std::chrono::steady_clock::now() < retirementDeadline)
    environment->pumpUntil([&] { return oldRunner.expired(); });
  BOOST_REQUIRE(oldRunner.expired());
  environment->disconnectProviderTransportForTest(0);
  environment->disconnectProviderPeerTransportForTest();
  std::atomic<unsigned> oldProducerPacketsAfterDisconnect{0};
  ndn::signal::ScopedConnection disconnectedProducerObservation = environment->providerFace().onSendData.connect(
    [&](const ndn::Data& data) {
      if (ndn::Name(provider).isPrefixOf(data.getName())) ++oldProducerPacketsAfterDisconnect;
    });
  if (validatesRemoteStorage) {
    BOOST_TEST_MESSAGE("Spec204 diagnostic: fetching original Repo packet set");
    const auto fetched = runWithEnvironmentPump([&] {
      return ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketFromUserStorage(
        *requesterUser, storageTarget, publication.transport.manifestName,
        originalManifest.size(), authorizeProducer, 2000);
    });
    BOOST_CHECK(fetched == originalManifest);
    const auto fetchedPacketSet = runWithEnvironmentPump([&] {
      return ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketSetFromUserStorage(
        *requesterUser, storageTarget,
        publication.transport.manifestName.toUri(ndn::name::UriFormat::ALTERNATE),
        authorizeProducer, 20000, remoteFetchLimits);
    });
    BOOST_CHECK(fetchedPacketSet.target() == storageTarget);
    BOOST_CHECK_EQUAL(fetchedPacketSet.claim().ownerIdentity,
      ndn::Name(requester).toUri(ndn::name::UriFormat::ALTERNATE));
    BOOST_REQUIRE_EQUAL(fetchedPacketSet.packetWires().size(), persistedPackets.size());
    for (std::size_t i = 0; i < persistedPackets.size(); ++i) {
      BOOST_CHECK(ndn::Name(fetchedPacketSet.manifest().packetNames.at(i)) ==
        persistedPackets[i].first);
      BOOST_CHECK(fetchedPacketSet.packetWires()[i] == persistedPackets[i].second);
    }
    BOOST_TEST_MESSAGE("Spec204 diagnostic: original Repo packet set fetched and matched");
    BOOST_CHECK_THROW(runWithEnvironmentPump([&] {
      return ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketFromUserStorage(
        *requesterUser, storageTarget, publication.transport.manifestName,
        originalManifest.size() + 1, authorizeProducer, 2000);
    }), std::runtime_error);
    BOOST_CHECK_THROW(runWithEnvironmentPump([&] {
      return ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketFromUserStorage(
        *requesterUser, storageTarget, publication.transport.manifestName,
        originalManifest.size(),
        [] (const ndn::Data&) { return false; }, 2000);
    }),
      std::runtime_error);
  }
  std::atomic<unsigned> successorRepoManifestRequests{0};
  ndn::signal::ScopedConnection repoPacketObservation = environment->providerFace(1).onSendInterest.connect([&](const ndn::Interest& interest) {
    if (interest.getName() == publication.transport.manifestName) {
      ++successorRepoManifestRequests;
    }
  });

  auto coldConfig = makeConfig(1, successorFactory);
  std::filesystem::create_directory(files.root / "recovery-B");
  BOOST_REQUIRE_EQUAL(::chmod((files.root / "recovery-B").c_str(), 0700), 0);
  coldConfig.kvRecovery = NativeProviderKvRecoveryConfigV1{
    files.root / "recovery-B", {ndn::Name(repoName)}, protectedLimits, portableLimits, 2U << 20};
  auto enteredRecovery = std::make_shared<std::atomic<bool>>(false);
  auto releaseRecovery = std::make_shared<std::atomic<bool>>(false);
  auto recoveryFailures = std::make_shared<std::atomic<unsigned>>(0);
  auto wireBoundRejected = std::make_shared<std::atomic<bool>>(false);
  auto cancelledByRequester = std::make_shared<std::atomic<bool>>(false);
  auto recoveryReturned = std::make_shared<std::atomic<unsigned>>(0);
  auto recoveryFailureReason = std::make_shared<std::string>();
  auto recoveryFailureReasonMutex = std::make_shared<std::mutex>();
  auto recoveryContext = std::make_shared<std::optional<ServiceProvider::CollaborationContext>>();
  coldConfig.nativeFailureObserver = std::make_shared<NativeProviderHandlerConfig::NativeFailureObserver>(
    [recoveryFailures, wireBoundRejected, cancelledByRequester,
      recoveryFailureReason, recoveryFailureReasonMutex](const auto&, const auto& reason) {
      {
        std::lock_guard<std::mutex> lock(*recoveryFailureReasonMutex);
        *recoveryFailureReason = reason;
      }
      if (reason == "KV_RECOVERY_PROTECTED_WIRE_EXCEEDS_POLICY") wireBoundRejected->store(true);
      if (reason == "KV_RECOVERY_CANCELLED") cancelledByRequester->store(true);
      ++*recoveryFailures;
    });
  if (fault == CausalRecoveryFault::CancelBeforeImport ||
      fault == CausalRecoveryFault::ControlBoundary64 ||
      fault == CausalRecoveryFault::ControlBacklog) {
    coldConfig.kvRecovery->protectedLimits.cancelled = [enteredRecovery, releaseRecovery] {
      enteredRecovery->store(true);
      const auto deadline = std::chrono::steady_clock::now() + 5s;
      while (!releaseRecovery->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
      if (!releaseRecovery->load()) throw std::runtime_error("causal recovery gate timed out");
      return false;
    };
  }
  else if (fault == CausalRecoveryFault::CancelAfterRunnerPrepared) {
    // Private-file validation runs before runner creation; the import preflight
    // runs after preparation and before Runtime reserves imported state.
    coldConfig.kvRecovery->portableLimits.portable.cancelled =
      [successorRunnerPrepared, enteredRecovery, releaseRecovery] {
        if (!successorRunnerPrepared->load()) return false;
        enteredRecovery->store(true);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!releaseRecovery->load() && std::chrono::steady_clock::now() < deadline)
          std::this_thread::sleep_for(1ms);
        if (!releaseRecovery->load()) throw std::runtime_error("causal prepared-runner recovery gate timed out");
        return false;
      };
  }
  else if (fault == CausalRecoveryFault::ProtectedWireBound) {
    // Keep the local policy valid so the remote wire bound rejects recovery.
    coldConfig.kvRecovery->protectedLimits = {16, 17, {}};
    coldConfig.kvRecovery->portableLimits = {{1, 1, {}}, 1, 16};
    BOOST_REQUIRE_NO_THROW(coldConfig.kvRecovery->validate());
  }
  auto successorRuntime = makeNativeProviderCollaborationRuntime(std::move(coldConfig));
  BOOST_CHECK_EQUAL(successorRuntime.conversationStateSnapshot().entries, 0U);
  auto successorRegistration = installHandler(1, successorRuntime, recoveryReturned, recoveryContext);
  const auto delta = causalInput(source.model.descriptor, {6}, tokenizer, 1);
  auto continuationOptions = options;
  continuationOptions.stream->generationId.fill(0x22);
  continuationOptions.generation = nativeGenerationFromOptions(delta.options, std::string(32, '2'));
  continuationOptions.providerNames = {ndn::Name(successor)};
  continuationOptions.conversation = NativeConversationContinuation{reference.conversationId,
    reference.contextEpoch, service, record->planRoleMapDigest, reference.checkpointDigest, {},
    record->retentionDeadlineMs, "APPEND_DELTA", record->checkpoint.wire, std::string(32, '2'),
    {1, 3, 4, 5, 6}, {source.role}};
  auto acceptedRecoveryTokens = std::make_shared<std::atomic<unsigned>>(0);
  continuationOptions.onGenerationEvent = [acceptedRecoveryTokens](const auto&) { ++*acceptedRecoveryTokens; };
  const auto repoManifestRequestsBeforeRecovery = successorRepoManifestRequests.load();
  BOOST_TEST_MESSAGE("Spec204 diagnostic: starting cold-provider continuation request");
  auto continued = client.request(model, delta, splitter, std::make_shared<NativePreSplitFirstPlacement>(),
    continuationOptions);
  struct RecoveryGateReleaseGuard
  {
    std::shared_ptr<std::atomic<bool>> release;
    ~RecoveryGateReleaseGuard() { release->store(true); }
  } releaseRecoveryOnExit{releaseRecovery};
  const auto publishRecoveryControls = [&](std::size_t count) {
    const auto sessionId = (*recoveryContext)->sessionId();
    const std::string invalidPayload = "x";
    const ndn::Buffer payload(invalidPayload.begin(), invalidPayload.end());
    const auto topic = ndn::Name("/ndnsf-di/conversation/recovery-cancel");
    for (std::size_t i = 0; i < count; ++i) {
      if (!requesterUser->publishCollaborationData(ndn::Name(successor), sessionId,
            "ndnsf-di-conversation-state-v1", topic, payload)) {
        return false;
      }
    }
    std::size_t queuedControls = 0;
    const auto backlogDeadline = std::chrono::steady_clock::now() + 3s;
    while (queuedControls < count && std::chrono::steady_clock::now() < backlogDeadline) {
      environment->pumpUntilWithAttributeAuthority([&] {
        if (!*recoveryContext) return false;
        queuedControls = (*recoveryContext)->waitFor(
          "ndnsf-di-conversation-state-v1", topic, 1, 0).size();
        return queuedControls >= count;
      });
    }
    return queuedControls >= count;
  };
  if (fault == CausalRecoveryFault::ControlBoundary64) {
    environment->pumpUntilWithAttributeAuthority([&] {
      return enteredRecovery->load() || continued.status() != NativeRequestStatus::Pending;
    });
    BOOST_REQUIRE(enteredRecovery->load());
    const auto pollWindow = std::chrono::steady_clock::now() + 80ms;
    environment->pumpUntil([&] { return std::chrono::steady_clock::now() >= pollWindow; });
    BOOST_REQUIRE(continued.status() == NativeRequestStatus::Pending);
    BOOST_CHECK_EQUAL(acceptedRecoveryTokens->load(), 0U);
    BOOST_CHECK(successorFactory->created().empty());
    BOOST_REQUIRE(publishRecoveryControls(64));
    releaseRecovery->store(true);
  }
  if (fault != CausalRecoveryFault::None && fault != CausalRecoveryFault::ControlBoundary64 &&
      !deletesOwnerStorage) {
    if (fault == CausalRecoveryFault::CancelBeforeImport ||
        fault == CausalRecoveryFault::ControlBacklog ||
        fault == CausalRecoveryFault::CancelAfterRunnerPrepared) {
      environment->pumpUntilWithAttributeAuthority([&] {
        return enteredRecovery->load() || continued.status() != NativeRequestStatus::Pending;
      });
      BOOST_REQUIRE(enteredRecovery->load());
      const auto pollWindow = std::chrono::steady_clock::now() + 80ms;
      environment->pumpUntil([&] { return std::chrono::steady_clock::now() >= pollWindow; });
      BOOST_REQUIRE(continued.status() == NativeRequestStatus::Pending);
      BOOST_CHECK_EQUAL(acceptedRecoveryTokens->load(), 0U);
      if (fault == CausalRecoveryFault::CancelBeforeImport ||
          fault == CausalRecoveryFault::ControlBacklog) {
        BOOST_CHECK(successorFactory->created().empty());
      }
      else {
        BOOST_REQUIRE(successorRunnerPrepared->load());
        BOOST_REQUIRE_EQUAL(successorFactory->created().size(), 1U);
      }
      if (fault == CausalRecoveryFault::ControlBacklog) {
        BOOST_REQUIRE(publishRecoveryControls(65));
      }
      else {
        continued.cancel();
        std::optional<NativeConversationRecoveryCancelV1> observedCancel;
        const auto cancelDeliveryDeadline = std::chrono::steady_clock::now() + 3s;
        while (!observedCancel && std::chrono::steady_clock::now() < cancelDeliveryDeadline) {
          environment->pumpUntilWithAttributeAuthority([&] {
            if (!*recoveryContext) return false;
            const auto controls = (*recoveryContext)->waitFor(
              "ndnsf-di-conversation-state-v1",
              ndn::Name("/ndnsf-di/conversation/recovery-cancel"), 1, 0);
            for (const auto& control : controls) {
              if (control.producer != requester || control.producerRole != "user-control-v1" ||
                  control.sessionId != (*recoveryContext)->sessionId() ||
                  control.keyScope != "ndnsf-di-conversation-state-v1" ||
                  control.topic != ndn::Name("/ndnsf-di/conversation/recovery-cancel"))
                continue;
              try {
                const std::string wire(control.payload.begin(), control.payload.end());
                auto parsed = nativeConversationRecoveryCancelV1FromJson(wire);
                if (parsed.requestId != (*recoveryContext)->sessionId() ||
                    ndn::Name(parsed.providerIdentity) != ndn::Name(successor) ||
                    parsed.parent.conversationId != reference.conversationId ||
                    parsed.parent.contextEpoch != reference.contextEpoch ||
                    parsed.parent.checkpointDigest != reference.checkpointDigest ||
                    parsed.attemptEpoch == 0 || parsed.generationId.empty() || parsed.planDigest.empty() ||
                    parsed.providerBootId != environment->provider(1).getProviderBootEpoch() ||
                    parsed.parent.expiresAtMs <= causalNow())
                  continue;
                observedCancel = std::move(parsed);
                return true;
              }
              catch (const std::exception&) {
              }
            }
            return false;
          });
        }
        BOOST_REQUIRE(observedCancel.has_value());
      }
      releaseRecovery->store(true);
    }
    const bool requesterMayRemainPending = fault == CausalRecoveryFault::ControlBacklog;
    const auto failureDeadline = fault == CausalRecoveryFault::ProtectedWireBound || requesterMayRemainPending
      ? continued.deadline() + 5s : std::chrono::steady_clock::now() + 15s;
    while (((!requesterMayRemainPending && continued.status() == NativeRequestStatus::Pending) ||
            recoveryFailures->load() == 0 || recoveryReturned->load() == 0) &&
           std::chrono::steady_clock::now() < failureDeadline)
      environment->pumpUntil([&] {
        return (requesterMayRemainPending || continued.status() != NativeRequestStatus::Pending) &&
          recoveryFailures->load() > 0 && recoveryReturned->load() > 0;
      });
    BOOST_REQUIRE_GT(recoveryFailures->load(), 0U);
    BOOST_REQUIRE_GT(recoveryReturned->load(), 0U);
    if (fault == CausalRecoveryFault::ProtectedWireBound) BOOST_CHECK(wireBoundRejected->load());
    if (fault == CausalRecoveryFault::ControlBacklog) {
      std::lock_guard<std::mutex> lock(*recoveryFailureReasonMutex);
      BOOST_CHECK_EQUAL(*recoveryFailureReason, "KV_RECOVERY_CANCEL_CONTROL_BACKLOG");
      // Keep the requester live through the original operation deadline and
      // callback-drain allowance. A late streamed error must remain observable
      // here instead of being hidden by test-local cancellation.
      const auto settlementDrainDeadline = continued.deadline() + 5s;
      while (std::chrono::steady_clock::now() < settlementDrainDeadline) {
        environment->pumpUntil([&] {
          return std::chrono::steady_clock::now() >= settlementDrainDeadline;
        });
      }
    }
    if (fault == CausalRecoveryFault::CancelBeforeImport ||
        fault == CausalRecoveryFault::CancelAfterRunnerPrepared) {
      std::lock_guard<std::mutex> lock(*recoveryFailureReasonMutex);
      BOOST_CHECK_MESSAGE(cancelledByRequester->load(),
        "provider recovery failure reason was: " + *recoveryFailureReason);
    }
    const bool requesterCancelled = fault == CausalRecoveryFault::CancelBeforeImport ||
      fault == CausalRecoveryFault::CancelAfterRunnerPrepared;
    const auto observedRequestStatus = continued.status();
    if (requesterMayRemainPending) {
      BOOST_CHECK_MESSAGE(observedRequestStatus == NativeRequestStatus::Failed,
        "observed NativeRequestStatus enum " << static_cast<unsigned>(observedRequestStatus)
        << ", expected the original request deadline to settle the handle");
      std::optional<std::string> observedFailureCode;
      try {
        static_cast<void>(continued.result(0ms));
      }
      catch (const NativeDiError& error) {
        observedFailureCode = error.code();
      }
      BOOST_CHECK(observedFailureCode.has_value());
      if (observedFailureCode)
        BOOST_CHECK_EQUAL(*observedFailureCode, "NATIVE_REQUEST_TIMEOUT");
      // An unexpected still-pending handle must not escape the fixture even
      // when the timeout oracle fails.
      if (continued.status() == NativeRequestStatus::Pending) continued.cancel();
    }
    else {
      const auto expectedRequestStatus = requesterCancelled
        ? NativeRequestStatus::Cancelled : NativeRequestStatus::Failed;
      BOOST_CHECK_MESSAGE(observedRequestStatus == expectedRequestStatus,
        "observed NativeRequestStatus enum " << static_cast<unsigned>(observedRequestStatus)
        << ", expected " << static_cast<unsigned>(expectedRequestStatus));
    }
    BOOST_CHECK_THROW(continued.result(0ms), NativeDiError);
    BOOST_CHECK_EQUAL(acceptedRecoveryTokens->load(), 0U);
    std::weak_ptr<NativeModelRunner> preparedRunner;
    const auto recoveryRunners = successorFactory->created();
    if (fault == CausalRecoveryFault::CancelAfterRunnerPrepared) {
      BOOST_REQUIRE(successorRunnerPrepared->load());
      BOOST_REQUIRE_EQUAL(recoveryRunners.size(), 1U);
      preparedRunner = recoveryRunners.front();
    }
    else {
      BOOST_CHECK(recoveryRunners.empty());
    }
    const auto noState = successorRuntime.conversationStateSnapshot();
    BOOST_CHECK_EQUAL(noState.entries, 0U); BOOST_CHECK_EQUAL(noState.pinnedEntries, 0U);
    BOOST_CHECK_EQUAL(noState.hostBytes, 0U); BOOST_CHECK_EQUAL(noState.gpuBytes, 0U);
    BOOST_CHECK_EQUAL(oldProducerPacketsAfterDisconnect.load(), 0U);
    BOOST_CHECK_EQUAL(conversations->find(reference.conversationId)->checkpoint.wire, record->checkpoint.wire);
    recoveryContext->reset();
    successorRegistration.close(); successorRuntime = {};
    BOOST_REQUIRE(drainClient.close());
    if (fault == CausalRecoveryFault::CancelAfterRunnerPrepared) {
      const auto runnerRetirementDeadline = std::chrono::steady_clock::now() + 5s;
      while (!preparedRunner.expired() && std::chrono::steady_clock::now() < runnerRetirementDeadline)
        environment->pumpUntil([&] { return preparedRunner.expired(); });
      BOOST_CHECK(preparedRunner.expired());
    }
    BOOST_CHECK(std::filesystem::is_empty(files.root / "recovery-B"));
    const auto afterFailedTurn = conversations->find(reference.conversationId);
    BOOST_REQUIRE(afterFailedTurn);
    BOOST_REQUIRE_EQUAL(afterFailedTurn->nextTurnProvidersByRole.size(), 1U);
    BOOST_CHECK_EQUAL(afterFailedTurn->nextTurnProvidersByRole.at(source.role), successor);
    return {};
  }
  BOOST_TEST_MESSAGE("Spec204 diagnostic: continuation request returned; waiting for causal result");
  causalWait(*environment, continued);
  BOOST_TEST_MESSAGE("Spec204 diagnostic: cold-provider continuation completed");
  const auto continuedResult = continued.result(0ms);
  const auto continuedJson = nativeParseJson(std::string(continuedResult.payload.begin(), continuedResult.payload.end()));
  BOOST_CHECK(continuedJson.at("tokenIds") == NativeJson::array({7}));
  BOOST_REQUIRE_EQUAL(successorFactory->created().size(), 1U);
  auto recoveredRunner = successorFactory->created().front().lock(); BOOST_REQUIRE(recoveredRunner);
  const auto recoveredMetrics = recoveredRunner->runtimeMetricsSnapshot(); BOOST_REQUIRE(recoveredMetrics);
  BOOST_CHECK(recoveredMetrics->workCounterCertified);
  BOOST_CHECK_EQUAL(recoveredMetrics->executedPrefillTokens, 1U);
  BOOST_CHECK_EQUAL(recoveredMetrics->executedDecodeTokens, 1U);
  BOOST_CHECK_EQUAL(recoveredMetrics->stateInputMisses, 0U);
  BOOST_CHECK_GE(recoveredMetrics->stateInputHits, 3U);
  BOOST_CHECK_GT(successorRepoManifestRequests.load(), repoManifestRequestsBeforeRecovery);
  BOOST_CHECK_EQUAL(successorRuntime.conversationStateSnapshot().pinnedEntries, 0U);
  const auto successorRecord = conversations->find(reference.conversationId); BOOST_REQUIRE(successorRecord);
  BOOST_CHECK_EQUAL(successorRecord->checkpoint.successorContextEpoch, reference.contextEpoch + 1);
  BOOST_CHECK_EQUAL(successorRecord->checkpoint.providersByRole.at(source.role), successor);
  BOOST_CHECK(successorRecord->nextTurnProvidersByRole.empty());
  BOOST_REQUIRE_EQUAL(successorRecord->checkpoint.transcript.at("providerRoleReceipts").size(), 1U);
  const auto successorReceiptWire = nativeConversationBase64Decode(
    successorRecord->checkpoint.transcript.at("providerRoleReceipts").front().get<std::string>());
  const auto successorReceipt = nativeReadProviderConversationStateReceipt(nativeParseJson(successorReceiptWire));
  ConversationStateReferenceV1 successorReference{successorRecord->checkpoint.conversationId,
    successorRecord->checkpoint.successorContextEpoch, service, successorRecord->planRoleMapDigest,
    successorRecord->checkpoint.checkpointDigest, source.role,
    successorReceipt.computedDigest(), successorReceipt.expiresAtMs};
  const auto successorPortable = successorRuntime.exportCommittedConversationState(
    successorReference, {1U << 20, 2U << 20, {}}, causalNow());
  BOOST_CHECK_EQUAL(successorPortable.originIdentity.providerIdentity, successor);
  BOOST_CHECK_EQUAL(successorPortable.originIdentity.prefixTokenCount, 6U);
  BOOST_CHECK_EQUAL(successorPortable.stateIoDigest, portable.stateIoDigest);
  BOOST_REQUIRE_EQUAL(successorPortable.components.size(), portable.components.size());
  // The pinned graph increments each state scalar once per ORT invocation.
  // B must retain A's actual state through suffix prefill and finalization;
  // token equality alone cannot distinguish equally wrong state families.
  for (const auto& originComponent : portable.components) {
    const auto found = std::find_if(successorPortable.components.begin(), successorPortable.components.end(),
      [&](const auto& component) { return component.inputName == originComponent.inputName; });
    BOOST_REQUIRE(found != successorPortable.components.end());
    BOOST_REQUIRE(originComponent.elementType == TensorElementType::Float32);
    BOOST_CHECK(found->elementType == originComponent.elementType);
    BOOST_CHECK(found->shape == originComponent.shape);
    BOOST_CHECK_EQUAL(found->payloadBytes, originComponent.payloadBytes);
    const auto originTensors = decodeTensorBundle(originComponent.tensorWire);
    const auto successorTensors = decodeTensorBundle(found->tensorWire);
    BOOST_REQUIRE_EQUAL(originTensors.size(), 1U);
    BOOST_REQUIRE_EQUAL(successorTensors.size(), 1U);
    const auto& originTensor = originTensors.front();
    const auto& successorTensor = successorTensors.front();
    BOOST_CHECK_EQUAL(originTensor.name, originComponent.inputName);
    BOOST_CHECK_EQUAL(successorTensor.name, originComponent.inputName);
    BOOST_REQUIRE(originTensor.elementType == TensorElementType::Float32);
    BOOST_REQUIRE(successorTensor.elementType == TensorElementType::Float32);
    BOOST_CHECK(originTensor.shape == originComponent.shape);
    BOOST_CHECK(successorTensor.shape == originComponent.shape);
    BOOST_REQUIRE_EQUAL(originTensor.payload.size(), originComponent.payloadBytes);
    BOOST_REQUIRE_EQUAL(successorTensor.payload.size(), originTensor.payload.size());
    BOOST_REQUIRE_GT(originTensor.payload.size(), 0U);
    BOOST_REQUIRE_EQUAL(originTensor.payload.size() % sizeof(float), 0U);
    for (std::size_t offset = 0; offset < originTensor.payload.size(); offset += sizeof(float)) {
      float originValue = 0, successorValue = 0;
      std::memcpy(&originValue, originTensor.payload.data() + offset, sizeof(float));
      std::memcpy(&successorValue, successorTensor.payload.data() + offset, sizeof(float));
      BOOST_CHECK_EQUAL(originValue, 3.0f); // Original prefill, decode, finalization.
      BOOST_CHECK_EQUAL(successorValue, originValue + 2.0f);
    }
  }
  BOOST_CHECK(originalManifest == repo->core().getDataPacket(publication.transport.manifestName.toUri()));

  // Independent full-context production execution must yield the same token,
  // and its actual runner must account for the whole five-token input.
  const auto fullContext = causalInput(source.model.descriptor, {1, 3, 4, 5, 6}, tokenizer, 1);
  auto baselineOptions = continuationOptions;
  baselineOptions.stream->generationId.fill(0x33);
  baselineOptions.generation = nativeGenerationFromOptions(fullContext.options, std::string(32, '3'));
  baselineOptions.conversation.reset();
  successorFactory->retainNextRunner();
  const auto baseline = client.request(model, fullContext, splitter, std::make_shared<NativePreSplitFirstPlacement>(), baselineOptions);
  causalWait(*environment, baseline);
  const auto baselineResult = baseline.result(0ms);
  const auto baselineJson = nativeParseJson(std::string(baselineResult.payload.begin(), baselineResult.payload.end()));
  BOOST_CHECK(baselineJson.at("tokenIds") == continuedJson.at("tokenIds"));
  BOOST_CHECK_EQUAL(baselineJson.at("text"), continuedJson.at("text"));
  BOOST_REQUIRE_EQUAL(successorFactory->created().size(), 2U);
  auto baselineRunner = successorFactory->takeRetainedRunner(); BOOST_REQUIRE(baselineRunner);
  const auto baselineMetrics = baselineRunner->runtimeMetricsSnapshot(); BOOST_REQUIRE(baselineMetrics);
  BOOST_CHECK(baselineMetrics->workCounterCertified);
  BOOST_CHECK_EQUAL(baselineMetrics->executedPrefillTokens, 5U);
  BOOST_CHECK_EQUAL(oldProducerPacketsAfterDisconnect.load(), 0U);
  recoveredRunner.reset(); baselineRunner.reset();
  successorRegistration.close(); successorRuntime = {};
  BOOST_REQUIRE(drainClient.close());
  const auto successorRetirementDeadline = std::chrono::steady_clock::now() + 5s;
  const auto allSuccessorRunnersGone = [&] {
    const auto runners = successorFactory->created();
    return std::all_of(runners.begin(), runners.end(), [](const auto& runner) { return runner.expired(); });
  };
  while (!allSuccessorRunnersGone() && std::chrono::steady_clock::now() < successorRetirementDeadline)
    environment->pumpUntil(allSuccessorRunnersGone);
  BOOST_CHECK(allSuccessorRunnersGone());
  BOOST_CHECK(std::filesystem::is_empty(files.root / "recovery-B"));
  if (deletesOwnerStorage) {
    BOOST_REQUIRE(resolvedRoleGrant->has_value());
    const auto usageBeforeDelete = journal->userStorageUsage(requester, repoName);
    BOOST_REQUIRE_GT(usageBeforeDelete.committedBytes, 0U);
    BOOST_CHECK_EQUAL(usageBeforeDelete.reservedBytes, 0U);
    const auto ownerStorageObjectName =
      publication.transport.manifestName.toUri(ndn::name::UriFormat::ALTERNATE);
    BOOST_REQUIRE_EQUAL(resolvedRoleGrant->value().objectName, ownerStorageObjectName);
    const ndnsf_distributed_repo::RepoUserStorageDeleteRequest deleteRequest{
      1, "spec204-cold-owner-delete",
      ownerStorageObjectName, resolvedRoleGrant->value().ownerEpoch};
    const auto requestDelete = [&] {
      auto pending = std::make_shared<std::promise<
        ndnsf_distributed_repo::RepoUserStorageDeleteReply>>();
      auto result = pending->get_future();
      const auto requestId = ndnsf_distributed_repo::RepoClient::requestUserStorageDelete(
        *requesterUser, storageTarget, deleteRequest, 2000,
        [pending](const ndn::Name&) {
          pending->set_exception(std::make_exception_ptr(
            std::runtime_error("Spec204 Repo storage delete request timed out")));
        },
        [pending](const ndn_service_framework::ResponseMessage& response) {
          if (!response.getStatus()) {
            pending->set_exception(std::make_exception_ptr(std::runtime_error(
              "Spec204 Repo storage delete request failed: " + response.getErrorInfo())));
            return;
          }
          try {
            const auto payload = response.getPayload();
            pending->set_value(ndnsf_distributed_repo::RepoUserStorageDeleteReply::fromJson(
              std::string(payload.begin(), payload.end())));
          }
          catch (...) { pending->set_exception(std::current_exception()); }
        });
      if (requestId.empty())
        throw std::runtime_error("Spec204 Repo storage delete request was not admitted");
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      environment->pumpUntilWithAttributeAuthority([&] {
        return result.wait_for(0ms) == std::future_status::ready ||
          std::chrono::steady_clock::now() >= deadline;
      });
      if (result.wait_for(0ms) != std::future_status::ready)
        throw std::runtime_error("Spec204 Repo storage delete response was not observed");
      return result.get();
    };
    std::optional<ndnsf_distributed_repo::RepoUserStorageDeleteReply> deleted;
    const auto deleteDeadline = std::chrono::steady_clock::now() + 15s;
    for (unsigned attempt = 0; attempt < 8 &&
         std::chrono::steady_clock::now() < deleteDeadline; ++attempt) {
      auto reply = requestDelete();
      BOOST_REQUIRE(reply.state == "PENDING" || reply.state == "DELETED");
      if (reply.state == "DELETED") {
        deleted = std::move(reply);
        break;
      }
      BOOST_CHECK_EQUAL(reply.releasedBytes, 0U);
      const auto stillCharged = journal->userStorageUsage(requester, repoName);
      BOOST_CHECK_EQUAL(stillCharged.committedBytes, usageBeforeDelete.committedBytes);
      BOOST_CHECK_EQUAL(stillCharged.reservedBytes, 0U);
    }
    BOOST_REQUIRE(deleted.has_value());
    BOOST_CHECK_GT(deleted->releasedBytes, 0U);
    const auto usageAfterDelete = journal->userStorageUsage(requester, repoName);
    BOOST_CHECK_EQUAL(usageAfterDelete.committedBytes, 0U);
    BOOST_CHECK_EQUAL(usageAfterDelete.reservedBytes, 0U);
    return {};
  }
  files.preserveRoot = true;
  return {files.root, repoName, std::move(persistedPackets), environment, repo, repoBackend, rangeStore};
}

CausalRepoReopenEvidence
runActualCausalColdProviderWithMixedUriClaimManifest()
{
  return runActualCausalColdProvider(CausalRecoveryFault::None,
    false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false,
    false, true);
}

BOOST_AUTO_TEST_CASE(ActualRequesterRestoresCommittedCausalStateOnColdProvider)
{
  const auto reopened = runActualCausalColdProvider(CausalRecoveryFault::None);
  verifyCausalRepoReopen(reopened);
}

BOOST_AUTO_TEST_CASE(ActualRepoCanonicalManifestMatchesAlternateOwnerClaimByNdnName)
{
  const auto reopened = runActualCausalColdProviderWithMixedUriClaimManifest();
  verifyCausalRepoReopen(reopened);
}

BOOST_AUTO_TEST_CASE(Spec212TwoRoleCausalSourcePreparesDistinctStateBoundaries)
{
  NativeRequestControl control{"spec212-two-role-source-inspection", 1,
    std::chrono::steady_clock::now() + 30s, [] { return false; }};
  ndnsf::di::test::spec204::NativeCausalRecoveryModel source(
    "/spec212/provider/source", "spec212-two-role-protected-v1", control, true);

  BOOST_REQUIRE_EQUAL(source.roles.size(), 2U);
  BOOST_CHECK_EQUAL(source.roles[0].role, source.role);
  BOOST_CHECK_EQUAL(source.roles[1].role, source.secondRole);
  BOOST_REQUIRE_EQUAL(source.roles[0].expectedInputs.size(), 4U);
  BOOST_REQUIRE_EQUAL(source.roles[0].expectedOutputs.size(), 4U);
  BOOST_REQUIRE_EQUAL(source.roles[1].expectedInputs.size(), 4U);
  BOOST_REQUIRE_EQUAL(source.roles[1].expectedOutputs.size(), 4U);
  BOOST_CHECK(std::any_of(source.roles[0].expectedOutputs.begin(),
    source.roles[0].expectedOutputs.end(), [](const auto& tensor) {
      return tensor.name == "hidden_out";
    }));
  BOOST_CHECK(std::any_of(source.roles[1].expectedInputs.begin(),
    source.roles[1].expectedInputs.end(), [](const auto& tensor) {
      return tensor.name == "hidden_out";
    }));
  BOOST_CHECK(std::any_of(source.roles[1].expectedOutputs.begin(),
    source.roles[1].expectedOutputs.end(), [](const auto& tensor) {
      return tensor.name == "stage1__logits";
    }));
  BOOST_CHECK_EQUAL(source.candidate.roleStateInputsByRole.at(source.role).size(), 3U);
  BOOST_CHECK_EQUAL(source.candidate.roleStateOutputsByRole.at(source.role).size(), 3U);
  BOOST_CHECK_EQUAL(source.candidate.roleStateInputsByRole.at(source.secondRole).size(), 3U);
  BOOST_CHECK_EQUAL(source.candidate.roleStateOutputsByRole.at(source.secondRole).size(), 3U);
  BOOST_CHECK_EQUAL(source.candidate.crossPartitionTensors.size(), 1U);
}

BOOST_AUTO_TEST_CASE(Spec212ProductionOwnerSwitchContinuesOnSuccessorProvider)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true);
}

BOOST_AUTO_TEST_CASE(Spec212NonemptyWithdrawalCancelRestoresSourceBeforeSwitch)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerUnavailableWithdrawalAbortsWithoutUpload)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, true, false, false, false, false, false, false, false,
    false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalCancelAndRetryBeforeCommit)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, true, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212VoluntaryWithdrawalSavesAndClosesAfterOwnerConfirm)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalSavesNonemptyServiceAfterOwnerConfirm)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, true, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalRetriesLostConfirmationReply)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, true, false, false, false, false, false, false,
    false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalBudgetAbortsBeforeExport)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, true, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ServiceWithdrawalWireBudgetRejectsBeforeRepoCommit)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalWireBudgetRejectsBeforeRepoCommit)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, false, false, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212PartialHostWithdrawalRetainsFenceAfterChildFailure)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, false, false, true, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalForceClassifiesBudgetBeforeExport)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, false, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerDepartureWatchSavesOnlyAfterProviderIntent)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerDepartureWatchContinuesOnAutomaticSuccessor)
{
  runActualCausalColdProvider(CausalRecoveryFault::None,
    true, false, false, false, false, true,
    false, false, false, false, true, false, false, false, false,
    false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalSaveRejectsOverlappingUserSwitch)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true, false, false, false, false, false,
    false, false, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalDrainTimeoutRetainsScopeFence)
{
  runActualCausalColdProvider(CausalRecoveryFault::WithdrawalDrainTimeoutAtBoundary,
    true, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalAfterCommittedAppendUsesNewCheckpoint)
{
  runActualCausalColdProvider(CausalRecoveryFault::WithdrawalAfterCommittedAppend,
    true, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalRejectsIncompleteRepoRoleSetBeforeExport)
{
  runActualCausalColdProvider(CausalRecoveryFault::WithdrawalMissingRepoRole,
    true, true, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalPublicationObserverFailureJoinsBeforeResult)
{
  runActualCausalColdProvider(CausalRecoveryFault::WithdrawalPublicationObserverFailure,
    true, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212HostStopWithoutWithdrawalDoesNotUpload)
{
  runActualCausalColdProvider(CausalRecoveryFault::HostStopWithoutWithdrawal,
    true, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212UserSwitchRejectsOverlappingWithdrawal)
{
  runActualCausalColdProvider(CausalRecoveryFault::SwitchFirstWithdrawalOverlap, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerDepartureWatchSavesTwoNonemptyConversations)
{
  runActualCausalColdProvider(CausalRecoveryFault::UnrelatedConversationAfterSwitch, true,
    false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalSavesTwoNonemptyServices)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true, false, false, false, false, true,
    false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalSavesTwoIndependentOwners)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true, false, false, false, false, true,
    false, false, false, false, true, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalRetriesSecondOwnerLostConfirmation)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true, false, false, false, false, true,
    false, false, false, false, true, false, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalRetainsFenceAfterFirstChildCommit)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, true, false, false, false, false, true,
    false, false, false, false, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProviderWithdrawalPostCommitSecondChildTimeoutRetainsFence)
{
  runActualCausalColdProvider(CausalRecoveryFault::PostCommitSecondChildDrainTimeout,
    true, false, false, false, false, true, false, false, false, false, true,
    false, false, false, false, true, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerDepartureWatchWithoutIntentNeverUploads)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WatchExpiryWithoutIntentNeverUploads)
{
  runActualCausalColdProvider(CausalRecoveryFault::NoIntentWatchExpiry, true,
    false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212TransportDisconnectWithoutIntentNeverUploads)
{
  runActualCausalColdProvider(CausalRecoveryFault::NoIntentTransportDisconnect, true,
    false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212WithdrawalTotalByteBudgetRejectsBeforeExport)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ExplicitForceUnsavedDrainsAndClosesOnlyTheService)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ExplicitForceUnsavedClassifiesBudgetFailure)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true,
    false, false, false, false, false, false, false, false, true);
}

BOOST_AUTO_TEST_CASE(Spec212ProductionOwnerSwitchContinuesAcrossTwoRoles)
{
  const auto countWorkerHomes = [] {
    const auto root = std::filesystem::temp_directory_path();
    const auto prefix = "ndnsf-di-onnx-worker-home-" +
      std::to_string(::getpid()) + "-";
    return std::count_if(std::filesystem::directory_iterator(root),
      std::filesystem::directory_iterator{}, [&prefix](const auto& entry) {
        const auto name = entry.path().filename().string();
        return name.compare(0, prefix.size(), prefix) == 0;
      });
  };
  BOOST_REQUIRE_EQUAL(countWorkerHomes(), 0);
  runActualCausalColdProvider(CausalRecoveryFault::None, true, true);
  BOOST_CHECK_EQUAL(countWorkerHomes(), 0);
}

BOOST_AUTO_TEST_CASE(Spec212ProductionOwnerSwitchContinuesAcrossTwoSourceProviders)
{
  runActualCausalColdProvider(CausalRecoveryFault::None, true, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212TwoSourceCancellationBeforeAllReadyCommitsNothing)
{
  runActualCausalColdProvider(
    CausalRecoveryFault::TwoSourceCancelBeforeAllReady, true, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerJournalQuotaFailurePreservesPreSwitchState)
{
  runActualCausalColdProvider(CausalRecoveryFault::OwnerJournalQuotaFailure, true);
}

BOOST_AUTO_TEST_CASE(Spec212SecondRoleRepoCommitFailureDoesNotPromotePartialPacketSet)
{
  runActualCausalColdProvider(CausalRecoveryFault::FailSecondRepoPacketCommit, true, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerCasRejectsStalePublicSwitchAfterConcurrentOwnerTurn)
{
  runActualCausalColdProvider(CausalRecoveryFault::OwnerCasFailure, true);
}

BOOST_AUTO_TEST_CASE(Spec212OwnerPolicyIsFrozenAfterResolution)
{
  runActualCausalColdProvider(CausalRecoveryFault::MutateOwnerPolicyAfterResolution, true);
}

BOOST_AUTO_TEST_CASE(Spec212SuccessfulSwitchLeavesUnrelatedConversationUntouched)
{
  runActualCausalColdProvider(CausalRecoveryFault::UnrelatedConversationAfterSwitch, true);
}

BOOST_AUTO_TEST_CASE(Spec212SynchronousFirstAppendDispatchFailureIsTerminal)
{
  runActualCausalColdProvider(CausalRecoveryFault::SynchronousFirstAppendDispatchError, true);
}

BOOST_AUTO_TEST_CASE(Spec212AsynchronousFirstAppendFailureKeepsOldHandleTerminal)
{
  runActualCausalColdProvider(CausalRecoveryFault::FailFirstDestinationAppend, true);
}

BOOST_AUTO_TEST_CASE(Spec212CloseAfterPromotionBeforeFirstAppendIsTerminal)
{
  runActualCausalColdProvider(CausalRecoveryFault::CloseAfterPromotionBeforeAppend, true);
}

BOOST_AUTO_TEST_CASE(Spec212CloseAfterCommittedAppendPreservesSuccessfulOutcome)
{
  runActualCausalColdProvider(
    CausalRecoveryFault::CloseAfterCommittedAppendBeforeCompletion, true);
}

BOOST_AUTO_TEST_CASE(Spec212PostCommitFailureTerminatesWithoutRollback)
{
  runActualCausalColdProvider(CausalRecoveryFault::FailAfterCommittedAppend, true);
}

BOOST_AUTO_TEST_CASE(Spec212CloseDrainsWhileRepoPacketCommitIsActive)
{
  runActualCausalColdProvider(CausalRecoveryFault::CloseDuringRepoCommit, true);
}

BOOST_AUTO_TEST_CASE(Spec212CloseDrainsWhileOwnerPacketSetBoundaryIsActive)
{
  runActualCausalColdProvider(CausalRecoveryFault::CloseDuringOwnerPacketSetBoundary, true);
}

BOOST_AUTO_TEST_CASE(Spec212DestroyOwnerClientWhilePacketSetBoundaryIsActive)
{
  runActualCausalColdProvider(CausalRecoveryFault::DestroyOwnerClientAtBoundary, true);
}

void runSpec212ProductionAdmissionRejectsConcurrentSwitchAtEventCapacity()
{
  runActualCausalColdProvider(CausalRecoveryFault::EventCapacityCompetition, true);
}

void runSpec212ProductionAdmissionRejectsConcurrentSwitchAtRoleCapacity()
{
  runActualCausalColdProvider(CausalRecoveryFault::RoleCapacityCompetition, true);
}

void runSpec212ControlProgressWhileRoleCallbackIsParked()
{
  runActualCausalColdProvider(CausalRecoveryFault::ControlProgressWhileRoleParked, true);
}

void runSpec212ControlProgressWhileTwoRolesAreParked()
{
  runActualCausalColdProvider(CausalRecoveryFault::ControlProgressWhileRoleParked, true, true);
}

void runSpec212ControlProgressAcrossTwoSourcesWhileParked()
{
  runActualCausalColdProvider(CausalRecoveryFault::ControlProgressWhileRoleParked,
    true, true, true);
}

void runSpec212TwoEventCapacityAcrossTwoSources()
{
  runActualCausalColdProvider(CausalRecoveryFault::TwoEventCapacityCompetition,
    true, true, true);
}

void runSpec212TwoRoleCapacityAcrossTwoSources()
{
  runActualCausalColdProvider(CausalRecoveryFault::TwoRoleCapacityCompetition,
    true, true, true);
}

void runSpec212CloseDrainsWhileOwnerPacketSetBoundaryIsActive()
{
  runActualCausalColdProvider(CausalRecoveryFault::CloseDuringOwnerPacketSetBoundary, true);
}

BOOST_AUTO_TEST_CASE(ActualOwnerStorageDeleteClientConfirmsCleanupAfterColdProviderRecovery)
{
  static_cast<void>(runActualCausalColdProvider(CausalRecoveryFault::DeleteOwnerStorage));
}

BOOST_AUTO_TEST_CASE(ActualRequesterCancellationDuringColdRecoveryExecutesNoTokens)
{
  runActualCausalColdProvider(CausalRecoveryFault::CancelBeforeImport);
}

BOOST_AUTO_TEST_CASE(ActualRequesterCancellationAfterRunnerPreparationExecutesNoTokens)
{
  runActualCausalColdProvider(CausalRecoveryFault::CancelAfterRunnerPrepared);
}

BOOST_AUTO_TEST_CASE(ActualColdRecoveryRejectsOperatorWireBoundBeforeRunnerAllocation)
{
  runActualCausalColdProvider(CausalRecoveryFault::ProtectedWireBound);
}

BOOST_AUTO_TEST_CASE(ActualColdRecoveryRejectsRecoveryControlBacklogBeforeImport)
{
  runActualCausalColdProvider(CausalRecoveryFault::ControlBacklog);
}

BOOST_AUTO_TEST_CASE(ActualColdRecoveryAcceptsSixtyFourRecoveryControls)
{
  runActualCausalColdProvider(CausalRecoveryFault::ControlBoundary64);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationRejectsOwnerGrant)
{
  runActualCausalColdProvider(CausalRecoveryFault::RejectedPublicationGrant);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationCancelsBeforeRepoCommit)
{
  runActualCausalColdProvider(CausalRecoveryFault::CancelledPublicationAtGrant);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationStopsBeforeRepoCommitWhenOwnerClosesGate)
{
  runActualCausalColdProvider(CausalRecoveryFault::RejectedPublicationCommitGate);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationRejectsWrongOwnerGrant)
{
  runActualCausalColdProvider(CausalRecoveryFault::WrongOwnerPublicationGrant);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationRejectsWrongWriterGrant)
{
  runActualCausalColdProvider(CausalRecoveryFault::WrongWriterPublicationGrant);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationRejectsChangedGrantRoot)
{
  runActualCausalColdProvider(CausalRecoveryFault::ChangedRootPublicationGrant);
}

BOOST_AUTO_TEST_CASE(ProtectedCheckpointPublicationRejectsExpiredOwnerGrant)
{
  runActualCausalColdProvider(CausalRecoveryFault::ExpiredPublicationGrant);
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndnsf::di::tests

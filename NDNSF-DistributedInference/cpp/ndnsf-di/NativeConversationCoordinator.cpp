#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <type_traits>

namespace ndnsf::di {
namespace {
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void digestField(const std::string& value)
{
  require(value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
    value.find_first_not_of("0123456789abcdef", 7) == std::string::npos, "conversation digest invalid");
}
bool prefix(const std::vector<std::int64_t>& before, const std::vector<std::int64_t>& after)
{ return after.size() >= before.size() && std::equal(before.begin(), before.end(), after.begin()); }
void validateLocalPlacement(const std::map<std::string, std::string>& placement,
                            const NativeJson& checkpoint)
{
  if (placement.empty()) return; // Legacy records carry no local preference.
  const auto& receipts = checkpoint.at("roleReceiptDigests");
  require(placement.size() == receipts.size(), "conversation local placement role cover mismatch");
  NativeJson roles = NativeJson::array();
  for (const auto& [role, provider] : placement) {
    require(receipts.contains(role) && !provider.empty(), "conversation local placement role mismatch");
    roles.push_back(NativeJson::array({role, provider}));
  }
  require(nativePlanningDigest(nativeCanonicalJson(roles)) == checkpoint.at("planRoleMapDigest"),
          "conversation local placement digest mismatch");
}

void validateNextTurnPlacement(const std::map<std::string, std::string>& placement,
                               const NativeJson& checkpoint)
{
  if (placement.empty()) return;
  const auto& receipts = checkpoint.at("roleReceiptDigests");
  require(placement.size() == receipts.size(), "conversation next-turn placement role cover mismatch");
  std::set<std::string> providers;
  for (const auto& [role, provider] : placement) {
    require(receipts.contains(role) && !provider.empty() && provider.size() <= 4096,
      "conversation next-turn placement role mismatch");
    const ndn::Name identity(provider);
    require(!identity.empty() && identity.wireEncode().size() <= 4096,
      "conversation next-turn Provider identity invalid");
    providers.insert(provider);
  }
  require(providers.size() <= 16, "conversation next-turn Provider roster exceeds bound");
}
std::string ticket()
{
  unsigned char bytes[16];
  require(RAND_bytes(bytes, sizeof(bytes)) == 1, "conversation ticket generation failed");
  std::string result;
  for (const auto byte : bytes) {
    result += "0123456789abcdef"[byte >> 4]; result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
NativeConversationRecord recordFromWire(const NativeJson& cp, std::string wire, NativeJson transcript)
{
  NativeConversationRecord result;
  auto& c = result.checkpoint;
  c.conversationId = cp.at("conversationId").get<std::string>();
  c.parentContextEpoch = cp.at("parentContextEpoch").get<std::uint64_t>();
  c.successorContextEpoch = cp.at("contextEpoch").get<std::uint64_t>();
  c.prefixDigest = cp.at("logicalPrefixDigest").get<std::string>();
  c.modelContractDigest = cp.at("modelContractDigest").get<std::string>();
  c.checkpointDigest = cp.at("checkpointDigest").get<std::string>();
  for (const auto& item : cp.at("roleReceiptDigests").items())
    c.expectedRoles.push_back(item.key());
  c.wire = std::move(wire); c.transcript = std::move(transcript);
  result.serviceName = cp.at("serviceName").get<std::string>();
  result.planRoleMapDigest = cp.at("planRoleMapDigest").get<std::string>();
  result.retentionDeadlineMs = cp.at("expiresAtMs").get<std::uint64_t>();
  return result;
}
}

struct NativeConversationCoordinator::Impl
{
  struct Pending {
    NativeConversationTurn turn;
    std::optional<NativeConversationCheckpoint> prepared;
    std::function<void(const std::string&)> promote;
    std::function<void()> rollback;
    std::function<void(const std::function<void()>&)> commitGate;
    std::function<void()> finalize;
    bool committing = false;
    std::string parentPlanRoleMapDigest;
  };
  explicit Impl(NativeConversationConfig value) : config(std::move(value)) {}
  ~Impl() { for (auto& key : config.authenticationKeys) OPENSSL_cleanse(key.data(), key.size()); }
  NativeConversationConfig config;
  mutable std::mutex mutex;
  std::map<std::string, NativeConversationRecord> records;
  std::map<std::string, Pending> pending;
  // Deliberately not part of records or the journal. A process restart must
  // perform authorization again instead of restoring a cryptographic lease.
  std::map<std::string, NativeGrantLease> grantLeases;
  Pending& find(const NativeConversationTurn& turn)
  {
    const auto it = pending.find(turn.requestId);
    require(it != pending.end() && it->second.turn.ticket == turn.ticket &&
      it->second.turn.attempt == turn.attempt && !turn.ticket.empty(), "DI_NATIVE_CONVERSATION_TURN_UNAVAILABLE");
    return it->second;
  }
  void scope(const NativeJson& cp) const
  {
    require(cp.at("requesterIdentity") == config.requesterIdentity && cp.at("serviceName") == config.serviceName &&
      cp.at("securityDomainDigest") == config.securityDomainDigest, "DI_NATIVE_CONVERSATION_SCOPE_MISMATCH");
  }
  void parent(const NativeConversationTurn& turn,
              const std::string* expectedPlanRoleMapDigest = nullptr) const
  {
    const auto current = records.find(turn.parent.conversationId);
    if (!turn.parent.parentContextEpoch)
      require(current == records.end(), "DI_NATIVE_CONVERSATION_STATE_CHANGED");
    else require(current != records.end() &&
      current->second.checkpoint.successorContextEpoch == turn.parent.parentContextEpoch &&
      current->second.checkpoint.checkpointDigest == turn.parent.parentCheckpointDigest &&
      current->second.planRoleMapDigest ==
        (expectedPlanRoleMapDigest ? *expectedPlanRoleMapDigest : turn.parent.planRoleMapDigest),
      "DI_NATIVE_CONVERSATION_STATE_CHANGED");
  }
  NativeConversationTurn bindSelectedMap(Pending& value,
    const std::map<std::string, std::string>& providersByRole)
  {
    if (!value.turn.parent.expectedProvidersByRole.empty())
      require(providersByRole == value.turn.parent.expectedProvidersByRole,
        "conversation plan map differs from Owner-selected next-turn map");
    std::set<std::string> expected(value.turn.parent.expectedRoles.begin(), value.turn.parent.expectedRoles.end());
    require(expected.size() == providersByRole.size() && !providersByRole.empty(),
      "conversation replacement plan map role cover invalid");
    NativeJson roleMap = NativeJson::array();
    for (const auto& [role, provider] : providersByRole) {
      require(expected.erase(role) == 1 && !provider.empty(),
        "conversation replacement plan map role binding invalid");
      roleMap.push_back(NativeJson::array({role, provider}));
    }
    require(expected.empty(), "conversation replacement plan map role cover invalid");
    const auto digest = nativePlanningDigest(nativeCanonicalJson(roleMap));
    require(digest != value.parentPlanRoleMapDigest, "conversation replacement plan map unchanged");
    auto replacement = value.turn;
    replacement.parent.planRoleMapDigest = digest;
    replacement.providersByRole = providersByRole;
    auto result = replacement;
    // Allocation/validation completes before the only pending-state mutation.
    static_assert(std::is_nothrow_move_assignable<NativeConversationTurn>::value);
    static_assert(std::is_nothrow_move_constructible<NativeConversationTurn>::value);
    value.turn = std::move(replacement);
    return result;
  }
};

NativeConversationCoordinator::NativeConversationCoordinator(NativeConversationConfig config)
  : m_impl(std::make_unique<Impl>(std::move(config)))
{
  auto& value = m_impl->config;
  require(!value.requesterIdentity.empty() && !value.serviceName.empty() && value.serviceName.front() == '/',
    "conversation owner identity required");
  digestField(value.securityDomainDigest);
  if (value.authenticationKeys.empty() && value.journal) value.authenticationKeys = value.journal->authenticationKeys();
  require(!value.authenticationKeys.empty() && value.authenticationKeys.size() <= 16, "conversation owner key required");
  for (const auto& key : value.authenticationKeys) require(key.size() == 32, "conversation key size invalid");
  if (!value.nowMs) value.nowMs = [] {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  };
  if (value.journal) restore();
}
NativeConversationCoordinator::NativeConversationCoordinator(std::filesystem::path)
{ throw std::invalid_argument("conversation journal requires explicit owner/key configuration"); }
NativeConversationCoordinator::~NativeConversationCoordinator() = default;

std::shared_ptr<NativeConversationCoordinator> nativeConversationCoordinatorFromConfig(
  const std::string& configurationJson, const std::filesystem::path& baseDirectory,
  const std::string& expectedRequesterIdentity)
{
  const auto root = nativeParseJson(configurationJson);
  if (root.value("schema", std::string{}) != "ndnsf-di-native-conversation-v1")
    throw std::invalid_argument("unsupported native conversation schema");
  const auto base = std::filesystem::absolute(baseDirectory).lexically_normal();
  const auto resolve = [&base](const std::string& value, const char* field) {
    if (value.empty() || value.find('\0') != std::string::npos)
      throw std::invalid_argument(std::string("native conversation ") + field + " path is invalid");
    const auto candidate = std::filesystem::path(value);
    if (candidate.is_absolute()) return candidate.lexically_normal();
    const auto resolved = (base / candidate).lexically_normal();
    const auto relative = resolved.lexically_relative(base);
    if (relative.empty() || relative == ".." || relative.string().compare(0, 3, "../") == 0)
      throw std::invalid_argument(std::string("native conversation ") + field +
                                  " path escapes configuration directory");
    return resolved;
  };
  const auto& journal = root.at("journal");
  const auto& owner = root.at("owner");
  if (!journal.is_object() || !owner.is_object() || !journal.at("keys").is_array() ||
      journal.at("keys").empty())
    throw std::invalid_argument("native conversation owner configuration is incomplete");
  const auto requester = owner.at("requester_identity").get<std::string>();
  if (!expectedRequesterIdentity.empty() && requester != expectedRequesterIdentity)
    throw std::invalid_argument("native conversation requester identity does not match the ServiceUser");

  std::vector<NativeConversationJournalKey> keys;
  std::set<std::string> keyIds;
  for (const auto& entry : journal.at("keys")) {
    if (!entry.is_object()) throw std::invalid_argument("native conversation key entry is invalid");
    const auto id = entry.at("id").get<std::string>();
    if (!keyIds.insert(id).second) throw std::invalid_argument("native conversation key id is duplicated");
    const auto keyPath = resolve(entry.at("file").get<std::string>(), "key");
    struct stat status{};
    if (::lstat(keyPath.c_str(), &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid() || status.st_nlink != 1 || (status.st_mode & 077) != 0)
      throw std::invalid_argument("native conversation key file must be owner-only");
    std::ifstream input(keyPath, std::ios::binary | std::ios::ate);
    if (!input) throw std::invalid_argument("native conversation key file is unavailable");
    const auto size = input.tellg();
    if (size != 32) throw std::invalid_argument("native conversation key must contain exactly 32 bytes");
    std::vector<std::uint8_t> bytes(32);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
      throw std::invalid_argument("native conversation key file read failed");
    keys.push_back({id, std::move(bytes)});
  }

  NativeConversationJournalConfig journalConfig;
  journalConfig.stateRoot = resolve(journal.at("state_root").get<std::string>(), "state root");
  journalConfig.identity = journal.at("identity").get<std::string>();
  journalConfig.keys = std::move(keys);
  journalConfig.quotaBytes = journal.value("quota_bytes", std::size_t{64 * 1024 * 1024});
  journalConfig.testOnlyAllowEphemeralRoot =
    journal.value("test_only_allow_ephemeral_state_root", false);

  NativeConversationConfig ownerConfig;
  ownerConfig.journal = std::make_shared<NativeConversationJournal>(std::move(journalConfig));
  ownerConfig.requesterIdentity = requester;
  ownerConfig.serviceName = owner.at("service_name").get<std::string>();
  ownerConfig.securityDomainDigest = owner.at("security_domain_digest").get<std::string>();
  return std::make_shared<NativeConversationCoordinator>(std::move(ownerConfig));
}

NativeConversationTurn NativeConversationCoordinator::beginTurn(
  const NativeConversationContinuation& c, std::string requestId, std::uint64_t attempt) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  const auto now = s.config.nowMs();
  require(now > 0 && !requestId.empty() && attempt == 1 && c.conversationId.size() >= 16 &&
    c.conversationId.find_first_of("/\\") == std::string::npos && c.serviceName == s.config.serviceName &&
    c.generationId.size() == 32 && c.generationId.find_first_not_of("0123456789abcdef") == std::string::npos &&
    c.retentionDeadlineMs > now && c.canonicalTokenIds.size() <= 1024 * 1024,
    "DI_NATIVE_CONVERSATION_CONTINUATION_INVALID");
  const bool unboundInitialPlan = c.mode == "FULL_CONTEXT" && c.parentContextEpoch == 0 &&
    c.planRoleMapDigest.empty() && c.expectedRoles.empty();
  if (!unboundInitialPlan) digestField(c.planRoleMapDigest);
  digestField(c.requestContractDigest);
  nativeConversationPrefixDigest(c.canonicalTokenIds);
  std::set<std::string> roles;
  for (const auto& role : c.expectedRoles)
    require(!role.empty() && role.front() == '/' && roles.insert(role).second, "conversation role set invalid");
  require((unboundInitialPlan || !roles.empty()) && s.pending.find(requestId) == s.pending.end(),
    "conversation turn already pending");
  NativeConversationTurn turn;
  turn.parent = c;
  turn.requestId = std::move(requestId);
  turn.attempt = attempt;
  turn.successorContextEpoch = c.parentContextEpoch + 1;
  turn.ticket = ticket();
  turn.executionRequestId = turn.requestId;
  if (c.mode == "FULL_CONTEXT")
    require(c.parentContextEpoch == 0 && c.parentCheckpointWire.empty() && c.parentCheckpointDigest.empty() &&
      c.expectedProvidersByRole.empty(),
      "conversation full context carries parent");
  else {
    require(c.mode == "APPEND_DELTA" && c.parentContextEpoch > 0 &&
      c.parentContextEpoch != std::numeric_limits<std::uint64_t>::max(), "conversation append epoch invalid");
    const auto cp = nativeReadConversationCheckpoint(c.parentCheckpointWire, s.config.authenticationKeys, now);
    s.scope(cp);
    require(cp.at("conversationId") == c.conversationId && cp.at("contextEpoch") == c.parentContextEpoch &&
      cp.at("checkpointDigest") == c.parentCheckpointDigest && cp.at("planRoleMapDigest") == c.planRoleMapDigest,
      "DI_NATIVE_CONVERSATION_PARENT_MISMATCH");
    const auto current = s.records.find(c.conversationId);
    require(current != s.records.end(), "conversation transcript unavailable");
    turn.providersByRole = current->second.checkpoint.providersByRole;
    turn.parent.expectedProvidersByRole = current->second.nextTurnProvidersByRole;
    const auto lease = s.grantLeases.find(c.conversationId);
    if (lease != s.grantLeases.end()) turn.grantLease = lease->second;
    const auto previous = current->second.checkpoint.transcript.at("canonicalTokenIds").get<std::vector<std::int64_t>>();
    require(prefix(previous, c.canonicalTokenIds) && previous.size() < c.canonicalTokenIds.size(),
      "conversation append prefix mismatch");
  }
  s.parent(turn);
  Impl::Pending pending{turn, {}, {}, {}, {}, {}, false, turn.parent.planRoleMapDigest};
  s.pending.emplace(turn.requestId, std::move(pending));
  return turn;
}
void NativeConversationCoordinator::abortTurn(const NativeConversationTurn& turn, const NativeDiError&)
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  const auto it = s.pending.find(turn.requestId);
  if (it != s.pending.end() && it->second.turn.ticket == turn.ticket && it->second.turn.attempt == turn.attempt) {
    s.pending.erase(it);
    // Cancellation/abort fences every lease associated with the conversation;
    // a later turn must perform authorization again rather than reusing a
    // grant that was active when the owner abandoned this turn.
    s.grantLeases.erase(turn.parent.conversationId);
  }
}
void NativeConversationCoordinator::acceptTokenPrefix(const NativeConversationTurn& turn,
  const std::vector<std::int64_t>& tokens)
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(!pending.prepared && !pending.committing && tokens.size() <= 1024 * 1024 &&
    prefix(pending.turn.acceptedTokenIds, tokens), "conversation accepted prefix changed");
  auto hash = nativeConversationPrefixDigest(tokens);
  auto candidate = tokens;
  pending.turn.acceptedTokenIds.swap(candidate); pending.turn.prefixDigest.swap(hash);
}
NativeConversationTurn NativeConversationCoordinator::replaceAttempt(const NativeConversationTurn& turn,
  std::string executionRequestId, std::string requestContractDigest)
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(pending.turn.attempt == 1 && !pending.prepared && !pending.committing &&
    !executionRequestId.empty() && executionRequestId != pending.turn.executionRequestId, "conversation replacement invalid");
  if (!requestContractDigest.empty()) digestField(requestContractDigest);
  pending.turn.attempt = 2;
  pending.turn.executionRequestId = std::move(executionRequestId);
  if (!requestContractDigest.empty())
    pending.turn.parent.requestContractDigest = std::move(requestContractDigest);
  return pending.turn;
}

NativeConversationTurn NativeConversationCoordinator::bindInitialPlanRoleMap(
  const NativeConversationTurn& turn,
  const std::map<std::string, std::string>& providersByRole) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(pending.turn.attempt == 1 && !pending.prepared && !pending.committing &&
    pending.turn.parent.parentContextEpoch == 0 && pending.parentPlanRoleMapDigest.empty() &&
    pending.turn.parent.planRoleMapDigest.empty() && pending.turn.parent.expectedRoles.empty() &&
    !providersByRole.empty(), "conversation initial plan map binding invalid");
  NativeJson roleMap = NativeJson::array();
  std::vector<std::string> roles;
  for (const auto& [role, provider] : providersByRole) {
    require(!role.empty() && role.front() == '/' && !provider.empty(),
      "conversation initial plan map role binding invalid");
    roleMap.push_back(NativeJson::array({role, provider}));
    roles.push_back(role);
  }
  const auto digest = nativePlanningDigest(nativeCanonicalJson(roleMap));
  pending.turn.parent.planRoleMapDigest = digest;
  pending.turn.parent.expectedRoles = std::move(roles);
  pending.turn.providersByRole = providersByRole;
  pending.parentPlanRoleMapDigest = digest;
  return pending.turn;
}

NativeConversationTurn NativeConversationCoordinator::bindAttemptPlanRoleMap(
  const NativeConversationTurn& turn,
  const std::map<std::string, std::string>& providersByRole) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(pending.turn.attempt == 2 && !pending.prepared && !pending.committing &&
    pending.turn.parent.planRoleMapDigest == pending.parentPlanRoleMapDigest &&
    !providersByRole.empty(), "conversation replacement plan map binding invalid");
  return s.bindSelectedMap(pending, providersByRole);
}

NativeConversationTurn NativeConversationCoordinator::bindRecoveryPlanRoleMap(
  const NativeConversationTurn& turn,
  const std::map<std::string, std::string>& providersByRole,
  const NativeConversationKvRecoveryV1& expectedRecovery) const
{
  // This call validates the complete authenticated saved parent without a
  // nested lock. Recheck its immutable record and pending CAS below.
  const auto recovery = protectedKvRecoveryForTurn(turn);
  require(recovery && *recovery == expectedRecovery, "conversation recovery parent snapshot changed");
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  const auto& owned = pending.turn;
  require(owned.attempt == 1 && !owned.aborted && !pending.prepared && !pending.committing &&
    owned.parent.parentContextEpoch > 0 && owned.parent.mode == "APPEND_DELTA" &&
    owned.executionRequestId == turn.executionRequestId &&
    owned.parent.conversationId == turn.parent.conversationId &&
    owned.parent.parentContextEpoch == turn.parent.parentContextEpoch &&
    owned.parent.parentCheckpointDigest == turn.parent.parentCheckpointDigest &&
    owned.parent.requestContractDigest == turn.parent.requestContractDigest &&
    owned.parent.planRoleMapDigest == turn.parent.planRoleMapDigest &&
    owned.parent.planRoleMapDigest == pending.parentPlanRoleMapDigest &&
    owned.parent.expectedRoles == turn.parent.expectedRoles &&
    owned.parent.serviceName == turn.parent.serviceName &&
    owned.parent.retentionDeadlineMs == turn.parent.retentionDeadlineMs &&
    owned.parent.retentionDeadlineMs > s.config.nowMs(), "conversation recovery map binding invalid");
  s.parent(owned, &pending.parentPlanRoleMapDigest);
  const auto& record = s.records.at(owned.parent.conversationId);
  require(record.retentionDeadlineMs > s.config.nowMs() && record.protectedKvReferences &&
    *record.protectedKvReferences == expectedRecovery.references &&
    record.checkpoint.checkpointDigest == expectedRecovery.checkpoint.at("checkpointDigest") &&
    record.checkpoint.successorContextEpoch == expectedRecovery.checkpoint.at("contextEpoch"),
    "conversation recovery parent snapshot changed");
  return s.bindSelectedMap(pending, providersByRole);
}

void NativeConversationCoordinator::bindGrantLease(
  const NativeConversationTurn& turn, const NativeGrantLease& lease) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(!pending.prepared && !pending.committing &&
    lease.scope.conversationId == pending.turn.parent.conversationId &&
    !lease.scope.providerByRole.empty() && !lease.grants.empty(),
    "conversation grant lease binding invalid");
  if (pending.turn.providersByRole.empty()) {
    // A FULL_CONTEXT continuation may carry the authenticated role-map digest
    // without exposing a provider map to the caller. The planner has now
    // produced the owner-side assignment; bind it only after checking that
    // its canonical digest is the continuation's existing commitment.
    NativeJson roleMap = NativeJson::array();
    for (const auto& [role, provider] : lease.scope.providerByRole)
      roleMap.push_back(NativeJson::array({role, provider}));
    const auto roleMapDigest = nativePlanningDigest(nativeCanonicalJson(roleMap));
    require(pending.turn.parent.planRoleMapDigest.empty() ||
      pending.turn.parent.planRoleMapDigest == roleMapDigest,
      "conversation grant lease role-map commitment mismatch");
    pending.turn.providersByRole = lease.scope.providerByRole;
  }
  require(lease.scope.providerByRole == pending.turn.providersByRole,
    "conversation grant lease assignment changed");
  s.grantLeases[lease.scope.conversationId] = lease;
  pending.turn.grantLease = lease;
}

NativeConversationCheckpoint NativeConversationCoordinator::prepareCheckpoint(
  const NativeConversationTurn& turn, const NativeCompletedAttempt& completed) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  const auto& owned = pending.turn;
  const auto now = s.config.nowMs();
  require(!pending.prepared && !pending.committing && completed.complete &&
    completed.requestId == owned.executionRequestId && completed.attempt == owned.attempt &&
    completed.generationId == owned.parent.generationId && completed.commitProviderState &&
    completed.rollbackProviderState && now > 0 && owned.parent.retentionDeadlineMs > now,
    "conversation completion not bound to live turn");
  s.parent(owned, &pending.parentPlanRoleMapDigest);
  auto tokens = owned.parent.canonicalTokenIds;
  tokens.insert(tokens.end(), owned.acceptedTokenIds.begin(), owned.acceptedTokenIds.end());
  require(tokens == completed.tokenIds, "conversation completed prefix mismatch");
  digestField(completed.modelContractDigest); digestField(completed.tokenizerDigest); digestField(completed.chatTemplateDigest);
  // The owner supplies a bounded absolute deadline. Each Provider receipt
  // further limits it to actual state retention; do not silently replace an
  // explicitly configured conversation lifetime with an unrelated default.
  auto expires = owned.parent.retentionDeadlineMs;
  NativeJson digests = NativeJson::object(), receipts = NativeJson::array();
  std::set<std::string> roles(owned.parent.expectedRoles.begin(), owned.parent.expectedRoles.end());
  std::string model;
  for (const auto& receipt : completed.authenticatedReceipts) {
    const auto role = receipt.at("roleName").get<std::string>();
    require(roles.erase(role) == 1 && receipt.at("originRequestId") == owned.executionRequestId &&
      receipt.at("originGenerationId") == owned.parent.generationId, "conversation receipt role or attempt mismatch");
    const auto candidate = receipt.at("modelDigest").get<std::string>();
    if (model.empty()) model = candidate;
    require(candidate == model, "conversation receipt model mismatch");
    expires = std::min(expires, receipt.at("expiresAtMs").get<std::uint64_t>());
    digests[role] = receipt.at("receiptDigest");
    receipts.push_back(nativeConversationBase64Encode(nativeConversationCanonicalJson(receipt)));
  }
  require(roles.empty() && expires > now, "conversation receipt set incomplete or expired");
  const auto logicalPrefix = nativeConversationPrefixDigest(tokens);
  NativeJson cp{{"schema", "ndnsf-di-conversation-checkpoint-v1"}, {"version", 1},
    {"conversationId", owned.parent.conversationId}, {"parentContextEpoch", owned.parent.parentContextEpoch},
    {"contextEpoch", owned.successorContextEpoch}, {"serviceName", s.config.serviceName},
    {"requesterIdentity", s.config.requesterIdentity}, {"securityDomainDigest", s.config.securityDomainDigest},
    {"modelContractDigest", completed.modelContractDigest}, {"planRoleMapDigest", owned.parent.planRoleMapDigest},
    {"logicalPrefixDigest", logicalPrefix}, {"prefixTokenCount", tokens.size()}, {"roleReceiptDigests", digests},
    {"issuedAtMs", now}, {"expiresAtMs", expires}};
  const auto wire = nativeSignConversationCheckpoint(cp, s.config.authenticationKeys.front());
  cp = nativeReadConversationCheckpoint(wire, s.config.authenticationKeys, now);
  NativeJson transcript{{"schema", "ndnsf-di-conversation-transcript-v1"},
    {"conversationId", owned.parent.conversationId}, {"contextEpoch", owned.successorContextEpoch},
    {"requesterIdentity", s.config.requesterIdentity}, {"serviceName", s.config.serviceName},
    {"securityDomainDigest", s.config.securityDomainDigest},
    {"applicationMessages", nativeConversationBase64Encode(completed.applicationMessages)},
    {"tokenizerDigest", completed.tokenizerDigest}, {"chatTemplateDigest", completed.chatTemplateDigest},
    {"canonicalTokenIds", tokens}, {"prefixDigest", logicalPrefix}, {"prefixTokenCount", tokens.size()},
    {"providerRoleReceipts", receipts}, {"checkpointDigest", cp.at("checkpointDigest")},
    {"planRoleMapDigest", owned.parent.planRoleMapDigest}, {"createdAtMs", now}, {"expiresAtMs", expires}};
  const auto nativeInitialPromptTokenCount = owned.parent.parentContextEpoch == 0 ?
    std::optional<std::size_t>{owned.parent.canonicalTokenIds.size()} :
    s.records.at(owned.parent.conversationId).checkpoint.nativeInitialPromptTokenCount;
  nativeValidateConversationTranscript(transcript, cp, nativeInitialPromptTokenCount);
  auto result = recordFromWire(cp, wire, std::move(transcript)).checkpoint;
  result.nativeInitialPromptTokenCount = nativeInitialPromptTokenCount;
  validateLocalPlacement(owned.providersByRole, cp);
  result.providersByRole = owned.providersByRole;
  result.requestId = owned.executionRequestId; result.parentCheckpointDigest = owned.parent.parentCheckpointDigest;
  pending.prepared = result; pending.promote = completed.commitProviderState; pending.rollback = completed.rollbackProviderState;
  pending.commitGate = completed.durableCommitGate; pending.finalize = completed.finalizeProviderState;
  return result;
}

NativeConversationRecord NativeConversationCoordinator::commitTurn(
  const NativeConversationTurn& turn, const NativeConversationCheckpoint& checkpoint)
{
  auto& s = *m_impl;
  std::unique_lock<std::mutex> guard(s.mutex);
  auto& pending = s.find(turn);
  require(pending.prepared && !pending.committing && pending.prepared->wire == checkpoint.wire &&
    pending.prepared->transcript == checkpoint.transcript, "conversation checkpoint not prepared by owner");
  s.parent(pending.turn, &pending.parentPlanRoleMapDigest);
  auto cp = nativeReadConversationCheckpoint(pending.prepared->wire, s.config.authenticationKeys, s.config.nowMs());
  auto record = recordFromWire(cp, pending.prepared->wire, pending.prepared->transcript);
  record.checkpoint.nativeInitialPromptTokenCount = pending.prepared->nativeInitialPromptTokenCount;
  record.checkpoint.providersByRole = pending.prepared->providersByRole;
  record.requestContractDigest = pending.turn.parent.requestContractDigest;
  record.checkpoint.requestId = pending.turn.executionRequestId;
  record.checkpoint.parentCheckpointDigest = pending.turn.parent.parentCheckpointDigest;
  std::map<std::string, NativeConversationRecord> staged;
  staged.emplace(record.checkpoint.conversationId, record);
  auto node = staged.extract(staged.begin());
  const auto promote = pending.promote; const auto rollback = pending.rollback;
  const auto commitGate = pending.commitGate; const auto finalize = pending.finalize;
  const auto runFinalize = [&] {
    if (!finalize) return;
    try {
      finalize();
    }
    catch (const std::exception&) {
      logRuntimeError(
        "NDNSF_DI_CONVERSATION_CONTROL event=finalize_error requestId=" + turn.requestId +
        " conversationId=" + turn.parent.conversationId +
        " checkpointDigest=" + record.checkpoint.checkpointDigest +
        " code=std_exception");
    }
    catch (...) {
      logRuntimeError(
        "NDNSF_DI_CONVERSATION_CONTROL event=finalize_error requestId=" + turn.requestId +
        " conversationId=" + turn.parent.conversationId +
        " checkpointDigest=" + record.checkpoint.checkpointDigest +
        " code=unknown_exception");
    }
  };
  bool published = false;
  pending.committing = true;
  guard.unlock();
  try {
    // Network promotion callbacks must not run under the owner lock. An
    // abort during promotion fences the subsequent durable commit below.
    promote(record.checkpoint.checkpointDigest);
    guard.lock();
    auto& current = s.find(turn);
    s.parent(current.turn, &current.parentPlanRoleMapDigest);
    guard.unlock();
    nativeReadConversationCheckpoint(record.checkpoint.wire, s.config.authenticationKeys, s.config.nowMs());
    const auto publish = [&] {
      require(!published, "conversation commit gate called twice");
      std::lock_guard<std::mutex> publishGuard(s.mutex);
      auto& current = s.find(turn);
      s.parent(current.turn, &current.parentPlanRoleMapDigest);
      if (s.config.journal) s.config.journal->appendConversation(record.checkpoint.wire,
        record.checkpoint.transcript, s.config.nowMs(), record.checkpoint.nativeInitialPromptTokenCount,
        record.checkpoint.providersByRole);
      const auto found = s.records.find(record.checkpoint.conversationId);
      if (found == s.records.end()) s.records.insert(std::move(node));
      else std::swap(found->second, node.mapped());
      s.pending.erase(turn.requestId);
      published = true;
    };
    if (commitGate) commitGate(publish); else publish();
    if (s.config.afterDurableCommit)
      s.config.afterDurableCommit();
    guard.lock();
    require(published, "conversation commit gate did not publish");
    guard.unlock();
    runFinalize();
    return record;
  }
  catch (...) {
    const auto original = std::current_exception();
    if (guard.owns_lock()) guard.unlock();
    // An exception in terminal notification/cleanup cannot undo a durable
    // parent that may already have been observed by another request.
    if (published) {
      runFinalize();
      return record;
    }
    try { rollback(); }
    catch (...) {
      guard.lock();
      const auto it = s.pending.find(turn.requestId);
      if (it != s.pending.end() && it->second.turn.ticket == turn.ticket) {
        s.pending.erase(it);
        s.grantLeases.erase(turn.parent.conversationId);
      }
      throw std::runtime_error("DI_NATIVE_CONVERSATION_PROVIDER_ROLLBACK_FAILED");
    }
    guard.lock();
    const auto it = s.pending.find(turn.requestId);
    if (it != s.pending.end() && it->second.turn.ticket == turn.ticket) {
      s.pending.erase(it);
      s.grantLeases.erase(turn.parent.conversationId);
    }
    std::rethrow_exception(original);
  }
}

std::optional<NativeJson> NativeConversationCoordinator::protectedKvForTurn(
  const NativeConversationTurn& turn) const
{
  const auto recovery = protectedKvRecoveryForTurn(turn);
  if (!recovery) return std::nullopt;
  return recovery->references;
}

std::optional<NativeConversationKvRecoveryV1>
NativeConversationCoordinator::protectedKvRecoveryForTurn(
  const NativeConversationTurn& turn) const
{
  auto& state = *m_impl;
  std::lock_guard<std::mutex> guard(state.mutex);
  const auto& pending = state.find(turn);
  const auto& owned = pending.turn;
  const auto now = state.config.nowMs();
  require(!owned.aborted && !pending.committing && !pending.prepared &&
    owned.parent.retentionDeadlineMs > now &&
    turn.executionRequestId == owned.executionRequestId &&
    turn.parent.conversationId == owned.parent.conversationId &&
    turn.parent.parentContextEpoch == owned.parent.parentContextEpoch &&
    turn.parent.parentCheckpointDigest == owned.parent.parentCheckpointDigest &&
    turn.parent.requestContractDigest == owned.parent.requestContractDigest &&
    turn.parent.planRoleMapDigest == owned.parent.planRoleMapDigest &&
    turn.parent.expectedRoles == owned.parent.expectedRoles &&
    turn.parent.serviceName == owned.parent.serviceName &&
    turn.parent.retentionDeadlineMs == owned.parent.retentionDeadlineMs,
    "conversation KV pending owner changed or expired");
  state.parent(owned, &pending.parentPlanRoleMapDigest);
  if (!owned.parent.parentContextEpoch) return std::nullopt;
  const auto& record = state.records.at(owned.parent.conversationId);
  require(record.retentionDeadlineMs > now, "conversation KV parent expired");
  if (!record.protectedKvReferences) return std::nullopt;
  const auto checkpoint = nativeReadConversationCheckpoint(record.checkpoint.wire,
    state.config.authenticationKeys, now);
  state.scope(checkpoint);
  nativeValidateProtectedKvReferences(*record.protectedKvReferences, checkpoint,
    record.checkpoint.providersByRole);
  for (const auto& reference : record.protectedKvReferences->items())
    require(reference.value().at("version").get<std::uint64_t>() == 2,
      "conversation KV recipient recovery requires v2 metadata");
  NativeConversationKvRecoveryV1 recovery{*record.protectedKvReferences, checkpoint, {}};
  const auto& receipts = record.checkpoint.transcript.at("providerRoleReceipts");
  require(receipts.is_array() && receipts.size() == recovery.references.size(),
    "conversation KV origin role cover mismatch");
  for (const auto& encoded : receipts) {
    require(encoded.is_string() && encoded.get_ref<const std::string&>().size() <= 87384,
      "conversation KV origin receipt exceeds bound");
    auto wire = nativeConversationBase64Decode(encoded.get_ref<const std::string&>());
    require(!wire.empty() && wire.size() <= 65536,
      "conversation KV origin receipt exceeds bound");
    const auto receipt = nativeReadProviderConversationStateReceipt(nativeParseJson(wire));
    const auto producer = record.checkpoint.providersByRole.find(receipt.roleName);
    require(recovery.references.contains(receipt.roleName) &&
      producer != record.checkpoint.providersByRole.end() &&
      ndn::Name(producer->second) == ndn::Name(receipt.providerIdentity) &&
      receipt.computedDigest() == checkpoint.at("roleReceiptDigests").at(receipt.roleName) &&
      receipt.expiresAtMs > now &&
      recovery.originReceipts.emplace(receipt.roleName, std::move(wire)).second,
      "conversation KV origin receipt differs from saved parent");
  }
  require(recovery.originReceipts.size() == recovery.references.size(),
    "conversation KV origin role cover mismatch");
  // Preflight every retained receipt's size before the transcript validator
  // decodes the same original bytes to check its full parent scope.
  nativeValidateConversationTranscript(record.checkpoint.transcript, checkpoint,
    record.checkpoint.nativeInitialPromptTokenCount);
  return recovery;
}

NativeConversationRecord NativeConversationCoordinator::promoteProtectedKvSet(
  const std::string& conversationId, const std::string& expectedCheckpointDigest,
  const std::map<std::string, NativeProtectedKvPublicationV1>& publications)
{
  auto& state = *m_impl;
  std::lock_guard<std::mutex> guard(state.mutex);
  const auto found = state.records.find(conversationId);
  require(state.config.journal && found != state.records.end() && !publications.empty() && publications.size() <= 64,
    "conversation KV committed owner unavailable");
  auto& record = found->second;
  require(record.checkpoint.checkpointDigest == expectedCheckpointDigest &&
    record.retentionDeadlineMs > state.config.nowMs(), "conversation KV checkpoint changed or expired");
  for (const auto& pending : state.pending)
    require(pending.second.turn.parent.conversationId != conversationId, "conversation KV requires idle owner");
  const auto checkpoint = nativeReadConversationCheckpoint(record.checkpoint.wire,
    state.config.authenticationKeys, state.config.nowMs());
  state.scope(checkpoint);
  require(publications.size() == record.checkpoint.expectedRoles.size() &&
    publications.size() == record.checkpoint.providersByRole.size(), "conversation KV role cover mismatch");
  std::vector<std::pair<std::string, NativeJson>> pieces;
  pieces.reserve(publications.size());
  std::size_t textBytes = 0;
  const auto consume = [&](const std::string& value, std::size_t limit = 4096) {
    require(!value.empty() && value.size() <= limit && value.size() <= 128 * 1024 - textBytes,
      "conversation KV reference text exceeds bound"); textBytes += value.size();
  };
  for (const auto& [role, publication] : publications) {
    consume(role);
    require(checkpoint.at("roleReceiptDigests").contains(role) &&
      record.checkpoint.providersByRole.count(role) == 1 &&
      publication.checkpoint.ownerIdentity == ndn::Name(state.config.requesterIdentity) &&
      publication.checkpoint.producerPrefix == ndn::Name(record.checkpoint.providersByRole.at(role)) &&
      publication.checkpoint.checkpointIdentityDigest == nativeProtectedKvRoleBinding(checkpoint, role),
      "conversation KV role commitment mismatch");
    auto reference = nativeProtectedKvPublicationReference(publication);
    for (const auto& field : reference.items())
      if (field.value().is_string())
        consume(field.value().get_ref<const std::string&>(), field.key() == "keyEnvelope" ? 16384 : 4096);
    pieces.emplace_back(role, std::move(reference));
  }
  NativeJson references = NativeJson::object();
  for (auto& [role, reference] : pieces) references[role] = std::move(reference);
  nativeValidateProtectedKvReferences(references, checkpoint, record.checkpoint.providersByRole);
  if (record.protectedKvReferences) {
    require(*record.protectedKvReferences == references, "conversation KV set changed"); return record;
  }
  std::optional<NativeJson> replacement{std::move(references)};
  auto result = record; result.protectedKvReferences = replacement;
  static_assert(std::is_nothrow_swappable_v<std::optional<NativeJson>> &&
                std::is_nothrow_move_constructible_v<NativeConversationRecord>);
  state.config.journal->appendProtectedKvSet(record.checkpoint.wire, *replacement, state.config.nowMs());
  record.protectedKvReferences.swap(replacement);
  return result;
}

NativeConversationRecord NativeConversationCoordinator::promoteProtectedKvSet(
  const std::string& conversationId, const std::string& expectedCheckpointDigest,
  const std::map<std::string, NativeVerifiedRemoteKvPublicationV1>& publications,
  const std::map<std::string, std::string>& nextTurnProvidersByRole,
  const std::function<void()>& requireActive)
{
  auto& state = *m_impl;
  std::lock_guard<std::mutex> guard(state.mutex);
  const auto found = state.records.find(conversationId);
  require(state.config.journal && found != state.records.end() && !publications.empty() && publications.size() <= 64,
    "conversation KV committed owner unavailable");
  auto& record = found->second;
  require(record.checkpoint.checkpointDigest == expectedCheckpointDigest &&
    record.retentionDeadlineMs > state.config.nowMs(), "conversation KV checkpoint changed or expired");
  for (const auto& pending : state.pending)
    require(pending.second.turn.parent.conversationId != conversationId, "conversation KV requires idle owner");
  const auto checkpoint = nativeReadConversationCheckpoint(record.checkpoint.wire,
    state.config.authenticationKeys, state.config.nowMs());
  state.scope(checkpoint);
  if (!nextTurnProvidersByRole.empty()) {
    validateNextTurnPlacement(nextTurnProvidersByRole, checkpoint);
    require(nextTurnProvidersByRole != record.checkpoint.providersByRole,
      "conversation next-turn Provider map is unchanged");
  }
  auto destinationProviders = nextTurnProvidersByRole.empty()
    ? record.nextTurnProvidersByRole : nextTurnProvidersByRole;
  validateNextTurnPlacement(destinationProviders, checkpoint);
  require(publications.size() == record.checkpoint.expectedRoles.size() &&
    publications.size() == record.checkpoint.providersByRole.size(), "conversation KV role cover mismatch");

  std::vector<std::pair<std::string, NativeJson>> pieces;
  pieces.reserve(publications.size());
  std::size_t textBytes = 0;
  const auto consume = [&](const std::string& value, std::size_t limit = 4096) {
    require(!value.empty() && value.size() <= limit && value.size() <= 128 * 1024 - textBytes,
      "conversation KV reference text exceeds bound");
    textBytes += value.size();
  };
  for (const auto& [role, proof] : publications) {
    consume(role);
    require(record.checkpoint.providersByRole.count(role) == 1,
      "conversation remote KV role binding unavailable");
    const auto& ownerReference = proof.ownerReference();
    const auto reference = nativeReadProtectedKvPublicationReference(ownerReference);
    const auto& packetSet = proof.keyGraph().packetSet();
    const auto expectedProducer = ndn::Name(record.checkpoint.providersByRole.at(role));
    const ndn::Name authorizationScope(proof.rootManifest().authorizationScope);
    const auto boundServiceName = authorizationScope.size() > 1 ?
      authorizationScope.getSubName(1) : ndn::Name{};
    require(checkpoint.at("roleReceiptDigests").contains(role) &&
      ndn::Name(state.config.requesterIdentity) == reference.ownerIdentity &&
      reference.producerPrefix == expectedProducer &&
      reference.checkpointIdentityDigest == nativeProtectedKvRoleBinding(checkpoint, role) &&
      boundServiceName == ndn::Name(state.config.serviceName) &&
      ndn::Name(packetSet.claim().ownerIdentity) == reference.ownerIdentity,
      "conversation remote KV role commitment mismatch");
    for (const auto& field : ownerReference.items())
      if (field.value().is_string())
        consume(field.value().get_ref<const std::string&>(), field.key() == "keyEnvelope" ? 16384 : 4096);
    pieces.emplace_back(role, ownerReference);
  }

  NativeJson references = NativeJson::object();
  for (auto& [role, reference] : pieces) references[role] = std::move(reference);
  nativeValidateProtectedKvReferences(references, checkpoint, record.checkpoint.providersByRole);
  // This callback runs while the Coordinator lock is held. It closes the
  // deadline check after lock wait and immediately before the durable boundary.
  if (requireActive)
    requireActive();
  if (record.protectedKvReferences) {
    require(*record.protectedKvReferences == references, "conversation KV set changed");
    if (record.nextTurnProvidersByRole == destinationProviders) return record;
  }
  std::optional<NativeJson> replacement{std::move(references)};
  auto replacementProviders = destinationProviders;
  auto result = record;
  result.protectedKvReferences = replacement;
  result.nextTurnProvidersByRole = replacementProviders;
  static_assert(std::is_nothrow_swappable_v<std::optional<NativeJson>> &&
                std::is_nothrow_move_constructible_v<NativeConversationRecord> &&
                std::is_nothrow_swappable_v<std::map<std::string, std::string>>);
  if (requireActive)
    requireActive();
  state.config.journal->appendProtectedKvSet(record.checkpoint.wire, *replacement, state.config.nowMs(),
    replacementProviders);
  record.protectedKvReferences.swap(replacement);
  record.nextTurnProvidersByRole.swap(replacementProviders);
  return result;
}

void NativeConversationCoordinator::restore(const std::filesystem::path& root)
{
  require(m_impl->config.journal && std::filesystem::absolute(root).lexically_normal() ==
    std::filesystem::absolute(m_impl->config.journal->stateRoot()).lexically_normal(),
    "conversation restore root differs from configured journal");
  restore();
}
void NativeConversationCoordinator::restore()
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  require(s.config.journal && s.pending.empty(), "conversation restore requires idle journal owner");
  s.grantLeases.clear();
  const auto now = s.config.nowMs();
  std::map<std::string, NativeConversationRecord> restored;
  for (const auto& body : s.config.journal->readConversations(now)) {
    const auto wire = body.at("checkpointWire").get<std::string>();
    const auto cp = nativeReadConversationCheckpoint(wire, s.config.authenticationKeys, now);
    const auto nativeInitialPromptTokenCount = body.contains("nativeInitialPromptTokenCount") ?
      std::optional<std::size_t>{body.at("nativeInitialPromptTokenCount").get<std::size_t>()} : std::nullopt;
    s.scope(cp); nativeValidateConversationTranscript(body.at("transcript"), cp, nativeInitialPromptTokenCount);
    auto record = recordFromWire(cp, wire, body.at("transcript"));
    record.checkpoint.nativeInitialPromptTokenCount = nativeInitialPromptTokenCount;
    if (body.contains("providersByRole")) {
      record.checkpoint.providersByRole = body.at("providersByRole").get<std::map<std::string, std::string>>();
      require(!record.checkpoint.providersByRole.empty(), "conversation local placement is empty");
      validateLocalPlacement(record.checkpoint.providersByRole, cp);
    }
    if (body.contains("nextTurnProvidersByRole")) {
      record.nextTurnProvidersByRole = body.at("nextTurnProvidersByRole").get<std::map<std::string, std::string>>();
      require(!record.nextTurnProvidersByRole.empty(), "conversation next-turn placement is empty");
      validateNextTurnPlacement(record.nextTurnProvidersByRole, cp);
      require(record.nextTurnProvidersByRole != record.checkpoint.providersByRole,
        "conversation next-turn placement does not change source map");
    }
    if (body.contains("protectedKvReferences")) {
      nativeValidateProtectedKvReferences(body.at("protectedKvReferences"), cp, record.checkpoint.providersByRole);
      record.protectedKvReferences = body.at("protectedKvReferences");
    }
    const auto found = restored.find(record.checkpoint.conversationId);
    if (found != restored.end()) {
      if (found->second.checkpoint.wire == wire) continue;
      require(found->second.checkpoint.successorContextEpoch == record.checkpoint.parentContextEpoch &&
        prefix(found->second.checkpoint.transcript.at("canonicalTokenIds").get<std::vector<std::int64_t>>(),
          record.checkpoint.transcript.at("canonicalTokenIds").get<std::vector<std::int64_t>>()),
        "conversation restored successor mismatch");
    }
    restored[record.checkpoint.conversationId] = std::move(record);
  }
  s.records.swap(restored);
}
std::optional<NativeConversationRecord> NativeConversationCoordinator::find(const std::string& id) const
{
  auto& s = *m_impl;
  std::lock_guard<std::mutex> guard(s.mutex);
  const auto found = s.records.find(id);
  return found == s.records.end() ? std::nullopt : std::optional<NativeConversationRecord>{found->second};
}

} // namespace ndnsf::di

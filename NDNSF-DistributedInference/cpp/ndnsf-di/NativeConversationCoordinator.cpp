#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <chrono>
#include <fstream>
#include <limits>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

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
  NativeConversationTurn turn{c, std::move(requestId), attempt, c.parentContextEpoch + 1, {}, {}, false, ticket(), {}};
  turn.executionRequestId = turn.requestId;
  if (c.mode == "FULL_CONTEXT")
    require(c.parentContextEpoch == 0 && c.parentCheckpointWire.empty() && c.parentCheckpointDigest.empty(),
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
  if (it != s.pending.end() && it->second.turn.ticket == turn.ticket && it->second.turn.attempt == turn.attempt)
    s.pending.erase(it);
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
  std::set<std::string> expectedRoles(pending.turn.parent.expectedRoles.begin(),
                                      pending.turn.parent.expectedRoles.end());
  require(expectedRoles.size() == providersByRole.size(),
    "conversation replacement plan map role cover invalid");
  NativeJson roleMap = NativeJson::array();
  for (const auto& [role, provider] : providersByRole) {
    require(expectedRoles.erase(role) == 1 && !provider.empty(),
      "conversation replacement plan map role binding invalid");
    roleMap.push_back(NativeJson::array({role, provider}));
  }
  require(expectedRoles.empty(), "conversation replacement plan map role cover invalid");
  const auto digest = nativePlanningDigest(nativeCanonicalJson(roleMap));
  require(digest != pending.parentPlanRoleMapDigest, "conversation replacement plan map unchanged");
  pending.turn.parent.planRoleMapDigest = digest;
  pending.turn.providersByRole = providersByRole;
  return pending.turn;
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
    if (finalize) { try { finalize(); } catch (...) { /* Retention bounds lost FINALIZE. */ } }
    return record;
  }
  catch (...) {
    const auto original = std::current_exception();
    if (guard.owns_lock()) guard.unlock();
    // An exception in terminal notification/cleanup cannot undo a durable
    // parent that may already have been observed by another request.
    if (published) {
      if (finalize) { try { finalize(); } catch (...) {} }
      return record;
    }
    try { rollback(); }
    catch (...) {
      guard.lock();
      const auto it = s.pending.find(turn.requestId);
      if (it != s.pending.end() && it->second.turn.ticket == turn.ticket) s.pending.erase(it);
      throw std::runtime_error("DI_NATIVE_CONVERSATION_PROVIDER_ROLLBACK_FAILED");
    }
    guard.lock();
    const auto it = s.pending.find(turn.requestId);
    if (it != s.pending.end() && it->second.turn.ticket == turn.ticket) s.pending.erase(it);
    std::rethrow_exception(original);
  }
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

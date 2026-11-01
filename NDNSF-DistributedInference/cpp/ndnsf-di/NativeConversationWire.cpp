#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeEpochCoordinator.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedKvPublication.hpp"

#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
#include <openssl/rand.h>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/pib/key.hpp>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>

namespace ndnsf::di {
namespace {
constexpr std::size_t maxCheckpointBytes = 1024 * 1024;
constexpr std::size_t maxTranscriptBytes = 16 * 1024 * 1024;
constexpr std::size_t maxDurableGrantJsonNesting = 32;

void requireDurableGrantJsonNestingBounded(const std::string& wire)
{
  std::array<char, maxDurableGrantJsonNesting> delimiters{};
  std::size_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (const char ch : wire) {
    if (inString) {
      if (escaped) escaped = false;
      else if (ch == '\\') escaped = true;
      else if (ch == '"') inString = false;
      continue;
    }
    if (ch == '"') {
      inString = true;
      continue;
    }
    if (ch == '{' || ch == '[') {
      if (depth == delimiters.size())
        throw std::invalid_argument("durable grant JSON nesting exceeds bound");
      delimiters[depth++] = ch;
    }
    else if (ch == '}' || ch == ']') {
      if (depth == 0)
        throw std::invalid_argument("durable grant JSON has unmatched closing delimiter");
      const auto open = delimiters[depth - 1];
      if (!((open == '{' && ch == '}') || (open == '[' && ch == ']')))
        throw std::invalid_argument("durable grant JSON delimiters do not match");
      --depth;
    }
  }
}

void appendConversationCanonical(std::string& output, const NativeJson& value)
{
  if (value.is_array()) {
    output += '[';
    bool first = true;
    for (const auto& item : value) {
      if (!first) output += ',';
      first = false;
      appendConversationCanonical(output, item);
    }
    output += ']';
  }
  else if (value.is_object()) {
    output += '{';
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!first) output += ',';
      first = false;
      output += NativeJson(it.key()).dump(-1, ' ', false);
      output += ':';
      appendConversationCanonical(output, it.value());
    }
    output += '}';
  }
  else {
    if (value.is_binary() || value.is_discarded())
      throw std::invalid_argument("conversation canonical JSON requires a JSON value");
    output += value.dump(-1, ' ', false);
  }
}

void require(bool condition, const char* message)
{
  if (!condition) throw std::invalid_argument(message);
}

std::string hex(const unsigned char* data, std::size_t size)
{
  std::string result;
  result.reserve(size * 2);
  for (std::size_t i = 0; i != size; ++i) {
    result += "0123456789abcdef"[data[i] >> 4];
    result += "0123456789abcdef"[data[i] & 15];
  }
  return result;
}

std::string digest(const NativeJson& value)
{
  const auto wire = nativeConversationCanonicalJson(value);
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(wire.data()), wire.size(), hash);
  return "sha256:" + hex(hash, sizeof(hash));
}

std::string signature(const std::string& value, const std::vector<std::uint8_t>& key)
{
  require(key.size() == 32, "conversation authentication key must be 32 bytes");
  unsigned char output[EVP_MAX_MD_SIZE];
  unsigned int size = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
            reinterpret_cast<const unsigned char*>(value.data()), value.size(), output, &size))
    throw std::runtime_error("conversation HMAC failed");
  const auto result = hex(output, size);
  OPENSSL_cleanse(output, sizeof(output));
  return result;
}

void fields(const NativeJson& value, std::initializer_list<const char*> names)
{
  require(value.is_object() && value.size() == names.size(), "conversation field set mismatch");
  for (const auto name : names) require(value.contains(name), "conversation field missing");
}

std::string string(const NativeJson& value, const char* key)
{
  require(value.at(key).is_string(), "conversation field must be string");
  return value.at(key).get<std::string>();
}

std::uint64_t integer(const NativeJson& value, const char* key)
{
  const auto& item = value.at(key);
  require(item.is_number_unsigned() ||
          (item.is_number_integer() && item.get<std::int64_t>() >= 0),
          "conversation field must be nonnegative integer");
  return item.get<std::uint64_t>();
}

void requireDigest(const std::string& value)
{
  require(value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
          value.find_first_not_of("0123456789abcdef", 7) == std::string::npos,
          "conversation digest is not canonical");
}

void identity(const NativeJson& value)
{
  const auto id = string(value, "conversationId");
  require(id.size() >= 16 && id.find_first_of("/\\") == std::string::npos,
          "conversation ID must be opaque");
  const auto service = string(value, "serviceName");
  require(!service.empty() && service.front() == '/' &&
          !string(value, "requesterIdentity").empty(), "conversation authorization identity missing");
  requireDigest(string(value, "securityDomainDigest"));
  requireDigest(string(value, "planRoleMapDigest"));
}

void validateUnsigned(const NativeJson& value)
{
  fields(value, {"schema", "version", "conversationId", "parentContextEpoch", "contextEpoch",
    "serviceName", "requesterIdentity", "securityDomainDigest", "modelContractDigest",
    "planRoleMapDigest", "logicalPrefixDigest", "prefixTokenCount", "roleReceiptDigests",
    "issuedAtMs", "expiresAtMs"});
  require(string(value, "schema") == "ndnsf-di-conversation-checkpoint-v1" &&
          integer(value, "version") == 1, "conversation checkpoint version mismatch");
  identity(value);
  const auto parent = integer(value, "parentContextEpoch");
  require(parent != std::numeric_limits<std::uint64_t>::max() &&
          integer(value, "contextEpoch") == parent + 1, "conversation checkpoint epoch mismatch");
  require(integer(value, "issuedAtMs") > 0 &&
          integer(value, "expiresAtMs") > integer(value, "issuedAtMs"),
          "conversation checkpoint lifetime invalid");
  integer(value, "prefixTokenCount");
  requireDigest(string(value, "modelContractDigest"));
  requireDigest(string(value, "logicalPrefixDigest"));
  const auto& receipts = value.at("roleReceiptDigests");
  require(receipts.is_object() && !receipts.empty(), "conversation requires role receipt set");
  for (auto it = receipts.begin(); it != receipts.end(); ++it) {
    require(!it.key().empty() && it.key().front() == '/' && it.value().is_string(),
            "conversation role receipt invalid");
    requireDigest(it.value().get<std::string>());
  }
  require(nativeConversationCanonicalJson(value).size() <= maxCheckpointBytes,
          "conversation checkpoint exceeds bound");
}

std::string base64(const NativeJson& value)
{
  require(value.is_string(), "conversation base64 must be string");
  const auto encoded = value.get<std::string>();
  require(encoded.size() % 4 == 0, "conversation malformed base64");
  const auto padding = encoded.empty() ? 0U : encoded.back() != '=' ? 0U :
    encoded.size() >= 2 && encoded[encoded.size() - 2] == '=' ? 2U : 1U;
  require(encoded.substr(0, encoded.size() - padding).find_first_not_of(
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/") == std::string::npos,
    "conversation malformed base64");
  if (encoded.empty()) return {};
  std::string result(encoded.size() / 4 * 3, '\0');
  const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(result.data()),
    reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
  require(size >= static_cast<int>(padding), "conversation malformed base64");
  result.resize(static_cast<std::size_t>(size) - padding);
  return result;
}
} // namespace

std::string nativeConversationBase64Encode(const std::string& bytes)
{
  require(bytes.size() <= 64 * 1024 * 1024, "conversation base64 exceeds bound");
  if (bytes.empty()) return {};
  std::string encoded(4 * ((bytes.size() + 2) / 3) + 1, '\0');
  const auto size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
    reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
  require(size >= 0, "conversation base64 encoding failed");
  encoded.resize(static_cast<std::size_t>(size));
  return encoded;
}

std::string nativeConversationBase64Decode(const std::string& encoded)
{
  require(encoded.size() <= 64 * 1024 * 1024, "conversation base64 exceeds bound");
  return base64(encoded);
}

std::string nativeConversationCanonicalJson(const NativeJson& value)
{
  std::string result;
  appendConversationCanonical(result, value);
  return result;
}

namespace {
void recoveryString(const std::string& value, std::size_t maximum)
{
  require(!value.empty() && value.size() <= maximum, "recovery string exceeds bounds");
}

void recoveryName(const std::string& value, std::size_t maximum = 2048)
{
  recoveryString(value, maximum);
  const ndn::Name name(value);
  require(value.front() == '/' && name.size() > 0 &&
          (name.toUri(ndn::name::UriFormat::CANONICAL) == value ||
           name.toUri(ndn::name::UriFormat::ALTERNATE) == value),
          "recovery name spelling is invalid");
}

void durableGrantOpaque(const std::string& value, std::size_t maximum, const char* error)
{
  require(!value.empty() && value.size() <= maximum &&
          value.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._~-"
          ) == std::string::npos,
          error);
}

ndn::Name durableGrantName(const std::string& value)
{
  require(!value.empty() && value.size() <= 4096, "durable grant name exceeds bounds");
  const ndn::Name name(value);
  require(name.size() > 0 && name.wireEncode().size() <= 4096 &&
          name.toUri(ndn::name::UriFormat::ALTERNATE) == value,
          "durable grant name is not the canonical ALTERNATE NDN URI");
  return name;
}

void durableGrantRoleChallenge(const NativeDurableStorageGrantRoleChallengeV1& role)
{
  const auto roleName = durableGrantName(role.roleName);
  (void)roleName;
  (void)durableGrantName(role.repoTarget.repoNode);
  (void)durableGrantName(role.repoTarget.providerIdentity);
  (void)durableGrantName(role.repoTarget.servicePrefix);
  const auto producer = durableGrantName(role.producerPrefix);
  (void)durableGrantName(role.writerIdentity);
  require(role.writerIdentity == role.producerPrefix,
          "durable grant writer must equal the complete Producer prefix");
  const auto manifest = durableGrantName(role.manifestName);
  require(producer.size() < manifest.size() && producer.isPrefixOf(manifest),
          "durable grant manifest is outside Producer prefix");
  requireDigest(role.publicationIdentity);
  durableGrantOpaque(role.protectionEpoch, 256, "durable grant protection epoch is invalid");
  const auto dataPrefix = durableGrantName(role.dataPrefix);
  require(producer.size() < dataPrefix.size() && producer.isPrefixOf(dataPrefix),
          "durable grant data prefix is outside Producer prefix");
  require(role.packetWireBytes > 0, "durable grant packet size must be positive");
  require(role.auxiliaryPacketNames.size() <= 256,
          "durable grant auxiliary-name count exceeds bound");
  std::string previous;
  for (const auto& nameText : role.auxiliaryPacketNames) {
    const auto name = durableGrantName(nameText);
    require(producer.size() < name.size() && producer.isPrefixOf(name),
            "durable grant auxiliary name is outside Producer prefix");
    require(previous.empty() || previous < nameText,
            "durable grant auxiliary names are not strictly sorted");
    previous = nameText;
  }
}

void durableGrantRoleSubmission(const NativeDurableStorageGrantRoleSubmissionV1& grant)
{
  (void)durableGrantName(grant.roleName);
  (void)durableGrantName(grant.repoNode);
  (void)durableGrantName(grant.ownerIdentity);
  (void)durableGrantName(grant.writerIdentity);
  (void)durableGrantName(grant.objectName);
  require(!grant.grantId.empty() && grant.grantId.size() <= 4096 &&
          grant.grantId.find('\0') == std::string::npos,
          "durable grant ID is invalid");
  require(grant.ownerEpoch > 0, "durable grant owner epoch must be positive");
}

void recoveryCurrent(const std::string& request, std::uint64_t attempt,
                     const std::string& generation, const std::string& plan)
{
  recoveryString(request, 2048);
  recoveryString(generation, 512);
  require(attempt > 0, "recovery attempt must be positive");
  requireDigest(plan);
}
} // namespace

NativeJson
nativeConversationStateReadyV2ToJson(const NativeConversationStateReadyV2& ready)
{
  recoveryCurrent(ready.requestId, ready.attemptEpoch, ready.generationId, ready.planDigest);
  ready.parent.validate();
  recoveryString(ready.parent.conversationId, 1024);
  recoveryName(ready.parent.serviceName);
  recoveryName(ready.parent.roleName, 512);
  recoveryName(ready.providerIdentity);
  recoveryName(ready.originProviderIdentity);
  recoveryString(ready.providerBootId, 512);
  require(ready.cacheEpoch > 0 &&
          (ready.reason == "state-hit" || ready.reason == "repo-import"),
          "recovery READY state invalid");
  requireDigest(ready.protectedKvWireDigest);
  NativeJson value{
    {"schema", "ndnsf-di-conversation-state-ready-v2"},
    {"requestId", ready.requestId}, {"attemptEpoch", ready.attemptEpoch},
    {"generationId", ready.generationId}, {"planDigest", ready.planDigest},
    {"conversationId", ready.parent.conversationId}, {"contextEpoch", ready.parent.contextEpoch},
    {"serviceName", ready.parent.serviceName}, {"planRoleMapDigest", ready.parent.planRoleMapDigest},
    {"checkpointDigest", ready.parent.checkpointDigest}, {"roleName", ready.parent.roleName},
    {"roleReceiptDigest", ready.parent.roleReceiptDigest}, {"providerIdentity", ready.providerIdentity},
    {"providerBootId", ready.providerBootId}, {"cacheEpoch", ready.cacheEpoch},
    {"ready", true}, {"reason", ready.reason}, {"residency", "GPU_RESIDENT"},
    {"originProviderIdentity", ready.originProviderIdentity},
    {"protectedKvWireDigest", ready.protectedKvWireDigest}, {"expiresAtMs", ready.parent.expiresAtMs}};
  const auto commitment = digest(value);
  require(ready.readyDigest.empty() || ready.readyDigest == commitment,
          "recovery READY digest mismatch");
  value["readyDigest"] = commitment;
  require(nativeConversationCanonicalJson(value).size() <= 32 * 1024,
          "recovery READY exceeds wire bound");
  return value;
}

NativeConversationStateReadyV2
nativeConversationStateReadyV2FromJson(const std::string& wire)
{
  require(wire.size() <= 32 * 1024, "recovery READY exceeds wire bound");
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "requestId", "attemptEpoch", "generationId", "planDigest",
    "conversationId", "contextEpoch", "serviceName", "planRoleMapDigest", "checkpointDigest",
    "roleName", "roleReceiptDigest", "providerIdentity", "providerBootId", "cacheEpoch",
    "ready", "reason", "residency", "originProviderIdentity", "protectedKvWireDigest",
    "expiresAtMs", "readyDigest"});
  require(string(value, "schema") == "ndnsf-di-conversation-state-ready-v2" &&
          value.at("ready").is_boolean() && value.at("ready").get<bool>() &&
          string(value, "residency") == "GPU_RESIDENT", "recovery READY schema/state invalid");
  NativeConversationStateReadyV2 ready;
  ready.requestId = string(value, "requestId"); ready.attemptEpoch = integer(value, "attemptEpoch");
  ready.generationId = string(value, "generationId"); ready.planDigest = string(value, "planDigest");
  auto& parent = ready.parent;
  parent.conversationId = string(value, "conversationId"); parent.contextEpoch = integer(value, "contextEpoch");
  parent.serviceName = string(value, "serviceName"); parent.planRoleMapDigest = string(value, "planRoleMapDigest");
  parent.checkpointDigest = string(value, "checkpointDigest"); parent.roleName = string(value, "roleName");
  parent.roleReceiptDigest = string(value, "roleReceiptDigest"); parent.expiresAtMs = integer(value, "expiresAtMs");
  ready.providerIdentity = string(value, "providerIdentity"); ready.providerBootId = string(value, "providerBootId");
  ready.cacheEpoch = integer(value, "cacheEpoch"); ready.reason = string(value, "reason");
  ready.originProviderIdentity = string(value, "originProviderIdentity");
  ready.protectedKvWireDigest = string(value, "protectedKvWireDigest");
  ready.readyDigest = string(value, "readyDigest"); requireDigest(ready.readyDigest);
  (void)nativeConversationStateReadyV2ToJson(ready);
  return ready;
}

NativeJson
nativeConversationRecoveryCancelV1ToJson(const NativeConversationRecoveryCancelV1& cancel)
{
  recoveryCurrent(cancel.requestId, cancel.attemptEpoch, cancel.generationId, cancel.planDigest);
  cancel.parent.validate();
  recoveryString(cancel.parent.conversationId, 1024);
  recoveryName(cancel.parent.serviceName); recoveryName(cancel.parent.roleName, 512);
  recoveryName(cancel.providerIdentity); recoveryString(cancel.providerBootId, 512);
  NativeJson value{
    {"schema", "ndnsf-di-conversation-recovery-cancel-v1"},
    {"requestId", cancel.requestId}, {"attemptEpoch", cancel.attemptEpoch},
    {"generationId", cancel.generationId}, {"planDigest", cancel.planDigest},
    {"conversationId", cancel.parent.conversationId}, {"contextEpoch", cancel.parent.contextEpoch},
    {"serviceName", cancel.parent.serviceName}, {"planRoleMapDigest", cancel.parent.planRoleMapDigest},
    {"checkpointDigest", cancel.parent.checkpointDigest}, {"roleName", cancel.parent.roleName},
    {"roleReceiptDigest", cancel.parent.roleReceiptDigest}, {"expiresAtMs", cancel.parent.expiresAtMs},
    {"providerIdentity", cancel.providerIdentity}, {"providerBootId", cancel.providerBootId}};
  require(nativeConversationCanonicalJson(value).size() <= 16 * 1024,
          "recovery cancel exceeds control transport bound");
  return value;
}

NativeConversationRecoveryCancelV1
nativeConversationRecoveryCancelV1FromJson(const std::string& wire)
{
  require(wire.size() <= 16 * 1024, "recovery cancel exceeds control transport bound");
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "requestId", "attemptEpoch", "generationId", "planDigest",
    "conversationId", "contextEpoch", "serviceName", "planRoleMapDigest", "checkpointDigest",
    "roleName", "roleReceiptDigest", "expiresAtMs", "providerIdentity", "providerBootId"});
  require(string(value, "schema") == "ndnsf-di-conversation-recovery-cancel-v1",
          "recovery cancel schema invalid");
  NativeConversationRecoveryCancelV1 cancel;
  cancel.requestId = string(value, "requestId"); cancel.attemptEpoch = integer(value, "attemptEpoch");
  cancel.generationId = string(value, "generationId"); cancel.planDigest = string(value, "planDigest");
  cancel.parent.conversationId = string(value, "conversationId");
  cancel.parent.contextEpoch = integer(value, "contextEpoch");
  cancel.parent.serviceName = string(value, "serviceName");
  cancel.parent.planRoleMapDigest = string(value, "planRoleMapDigest");
  cancel.parent.checkpointDigest = string(value, "checkpointDigest");
  cancel.parent.roleName = string(value, "roleName");
  cancel.parent.roleReceiptDigest = string(value, "roleReceiptDigest");
  cancel.parent.expiresAtMs = integer(value, "expiresAtMs");
  cancel.providerIdentity = string(value, "providerIdentity");
  cancel.providerBootId = string(value, "providerBootId");
  (void)nativeConversationRecoveryCancelV1ToJson(cancel);
  return cancel;
}

void
nativeValidateConversationRecoveryCancelV1(
  const NativeConversationRecoveryCancelV1& cancel, const NativeSelectionProjectionV3& projection,
  const std::string& localBootId, std::uint64_t nowMs)
{
  (void)nativeConversationRecoveryCancelV1ToJson(cancel);
  require(projection.protectedKvRestore && projection.conversationStateReference &&
          projection.conversationTurnBinding, "recovery cancel lacks admitted parent");
  const auto& expected = *projection.conversationStateReference;
  const auto& parent = cancel.parent;
  require(cancel.requestId == projection.requestId && cancel.attemptEpoch == projection.attempt &&
          cancel.generationId == projection.generationContract.generationId &&
          cancel.planDigest == projection.planDigest &&
          parent.conversationId == expected.conversationId && parent.contextEpoch == expected.contextEpoch &&
          parent.serviceName == expected.serviceName && parent.planRoleMapDigest == expected.planRoleMapDigest &&
          parent.checkpointDigest == expected.checkpointDigest && parent.roleName == expected.roleName &&
          parent.roleReceiptDigest == expected.roleReceiptDigest &&
          ndn::Name(cancel.providerIdentity) == ndn::Name(projection.provider) &&
          !localBootId.empty() && cancel.providerBootId == localBootId,
          "recovery cancel differs from admitted selection");
  require(parent.expiresAtMs > nowMs && parent.expiresAtMs <= expected.expiresAtMs &&
          parent.expiresAtMs <= projection.deadlineMs &&
          parent.expiresAtMs <= projection.protectedKvRestore->recipient.expiresAtMs &&
          parent.expiresAtMs <= projection.conversationTurnBinding->retentionDeadlineMs,
          "recovery cancel expiry exceeds current scope");
}

NativeJson
nativeConversationRecoveryActivationV1ToJson(const NativeConversationRecoveryActivationV1& activation)
{
  recoveryCurrent(activation.requestId, activation.attemptEpoch, activation.generationId, activation.planDigest);
  recoveryString(activation.conversationId, 1024);
  recoveryName(activation.serviceName); recoveryName(activation.roleName, 512);
  require(activation.contextEpoch > 0 && activation.expiresAtMs > 0,
          "recovery activation epoch/expiry invalid");
  requireDigest(activation.planRoleMapDigest); requireDigest(activation.checkpointDigest);
  requireDigest(activation.readyDigest); requireDigest(activation.readySetDigest);
  NativeJson value{
    {"schema", "ndnsf-di-conversation-recovery-activate-v1"},
    {"requestId", activation.requestId}, {"attemptEpoch", activation.attemptEpoch},
    {"generationId", activation.generationId}, {"planDigest", activation.planDigest},
    {"conversationId", activation.conversationId}, {"contextEpoch", activation.contextEpoch},
    {"serviceName", activation.serviceName}, {"planRoleMapDigest", activation.planRoleMapDigest},
    {"checkpointDigest", activation.checkpointDigest}, {"roleName", activation.roleName},
    {"readyDigest", activation.readyDigest}, {"readySetDigest", activation.readySetDigest},
    {"expiresAtMs", activation.expiresAtMs}};
  require(nativeConversationCanonicalJson(value).size() <= 16 * 1024,
          "recovery activation exceeds control transport bound");
  return value;
}

NativeConversationRecoveryActivationV1
nativeConversationRecoveryActivationV1FromJson(const std::string& wire)
{
  require(wire.size() <= 16 * 1024, "recovery activation exceeds control transport bound");
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "requestId", "attemptEpoch", "generationId", "planDigest",
    "conversationId", "contextEpoch", "serviceName", "planRoleMapDigest", "checkpointDigest",
    "roleName", "readyDigest", "readySetDigest", "expiresAtMs"});
  require(string(value, "schema") == "ndnsf-di-conversation-recovery-activate-v1",
          "recovery activation schema invalid");
  NativeConversationRecoveryActivationV1 activation;
  activation.requestId = string(value, "requestId"); activation.attemptEpoch = integer(value, "attemptEpoch");
  activation.generationId = string(value, "generationId"); activation.planDigest = string(value, "planDigest");
  activation.conversationId = string(value, "conversationId"); activation.contextEpoch = integer(value, "contextEpoch");
  activation.serviceName = string(value, "serviceName"); activation.planRoleMapDigest = string(value, "planRoleMapDigest");
  activation.checkpointDigest = string(value, "checkpointDigest"); activation.roleName = string(value, "roleName");
  activation.readyDigest = string(value, "readyDigest"); activation.readySetDigest = string(value, "readySetDigest");
  activation.expiresAtMs = integer(value, "expiresAtMs");
  (void)nativeConversationRecoveryActivationV1ToJson(activation);
  return activation;
}

NativeJson
nativeDurableStorageGrantChallengeV1ToJson(
  const NativeDurableStorageGrantChallengeV1& challenge)
{
  durableGrantOpaque(challenge.operationId, 256, "durable grant operation ID is invalid");
  requireDigest(challenge.checkpointDigest);
  requireDigest(challenge.snapshotDigest);
  requireDigest(challenge.roleMapDigest);
  require(!challenge.roles.empty() && challenge.roles.size() <= 64,
          "durable grant role count is outside bounds");
  NativeJson roles = NativeJson::array();
  std::string previousRole;
  for (const auto& role : challenge.roles) {
    durableGrantRoleChallenge(role);
    require(previousRole.empty() || previousRole < role.roleName,
            "durable grant roles are not strictly sorted");
    previousRole = role.roleName;
    NativeJson auxiliaryNames = NativeJson::array();
    for (const auto& name : role.auxiliaryPacketNames) auxiliaryNames.push_back(name);
    roles.push_back({
      {"roleName", role.roleName},
      {"repoTarget", {{"repoNode", role.repoTarget.repoNode},
                       {"providerIdentity", role.repoTarget.providerIdentity},
                       {"servicePrefix", role.repoTarget.servicePrefix}}},
      {"producerPrefix", role.producerPrefix},
      {"writerIdentity", role.writerIdentity},
      {"manifestName", role.manifestName},
      {"publicationIdentity", role.publicationIdentity},
      {"protectionEpoch", role.protectionEpoch},
      {"dataPrefix", role.dataPrefix},
      {"packetWireBytes", role.packetWireBytes},
      {"auxiliaryPacketNames", std::move(auxiliaryNames)}});
  }
  NativeJson value{
    {"schema", "ndnsf-di-kv-storage-grant-challenge-v1"},
    {"operationId", challenge.operationId},
    {"checkpointDigest", challenge.checkpointDigest},
    {"snapshotDigest", challenge.snapshotDigest},
    {"roleMapDigest", challenge.roleMapDigest},
    {"roles", std::move(roles)}};
  require(nativeConversationCanonicalJson(value).size() <= 64 * 1024,
          "durable grant challenge exceeds control transport bound");
  return value;
}

NativeDurableStorageGrantChallengeV1
nativeDurableStorageGrantChallengeV1FromJson(const std::string& wire)
{
  require(wire.size() <= 64 * 1024,
          "durable grant challenge exceeds control transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "operationId", "checkpointDigest", "snapshotDigest",
    "roleMapDigest", "roles"});
  require(string(value, "schema") == "ndnsf-di-kv-storage-grant-challenge-v1" &&
          value.at("roles").is_array() && !value.at("roles").empty() &&
          value.at("roles").size() <= 64,
          "durable grant challenge schema or role set is invalid");
  NativeDurableStorageGrantChallengeV1 challenge;
  challenge.operationId = string(value, "operationId");
  challenge.checkpointDigest = string(value, "checkpointDigest");
  challenge.snapshotDigest = string(value, "snapshotDigest");
  challenge.roleMapDigest = string(value, "roleMapDigest");
  for (const auto& item : value.at("roles")) {
    fields(item, {"roleName", "repoTarget", "producerPrefix", "writerIdentity",
      "manifestName", "publicationIdentity", "protectionEpoch", "dataPrefix",
      "packetWireBytes", "auxiliaryPacketNames"});
    fields(item.at("repoTarget"), {"repoNode", "providerIdentity", "servicePrefix"});
    require(item.at("auxiliaryPacketNames").is_array(),
            "durable grant auxiliary names must be an array");
    NativeDurableStorageGrantRoleChallengeV1 role;
    role.roleName = string(item, "roleName");
    role.repoTarget.repoNode = string(item.at("repoTarget"), "repoNode");
    role.repoTarget.providerIdentity = string(item.at("repoTarget"), "providerIdentity");
    role.repoTarget.servicePrefix = string(item.at("repoTarget"), "servicePrefix");
    role.producerPrefix = string(item, "producerPrefix");
    role.writerIdentity = string(item, "writerIdentity");
    role.manifestName = string(item, "manifestName");
    role.publicationIdentity = string(item, "publicationIdentity");
    role.protectionEpoch = string(item, "protectionEpoch");
    role.dataPrefix = string(item, "dataPrefix");
    role.packetWireBytes = integer(item, "packetWireBytes");
    for (const auto& auxiliaryName : item.at("auxiliaryPacketNames")) {
      require(auxiliaryName.is_string(), "durable grant auxiliary name must be a string");
      role.auxiliaryPacketNames.push_back(auxiliaryName.get<std::string>());
    }
    challenge.roles.push_back(std::move(role));
  }
  require(nativeConversationCanonicalJson(nativeDurableStorageGrantChallengeV1ToJson(challenge)) == wire,
          "durable grant challenge wire is not canonical");
  return challenge;
}

NativeJson
nativeDurableStorageGrantSubmissionV1ToJson(
  const NativeDurableStorageGrantSubmissionV1& submission)
{
  durableGrantOpaque(submission.operationId, 256, "durable grant operation ID is invalid");
  requireDigest(submission.checkpointDigest);
  requireDigest(submission.snapshotDigest);
  requireDigest(submission.roleMapDigest);
  require(!submission.grants.empty() && submission.grants.size() <= 64,
          "durable grant submission count is outside bounds");
  NativeJson grants = NativeJson::array();
  std::string previousRole;
  for (const auto& grant : submission.grants) {
    durableGrantRoleSubmission(grant);
    require(previousRole.empty() || previousRole < grant.roleName,
            "durable grant submissions are not strictly sorted");
    previousRole = grant.roleName;
    grants.push_back({
      {"roleName", grant.roleName}, {"repoNode", grant.repoNode},
      {"ownerIdentity", grant.ownerIdentity}, {"writerIdentity", grant.writerIdentity},
      {"objectName", grant.objectName}, {"grantId", grant.grantId},
      {"ownerEpoch", grant.ownerEpoch}});
  }
  NativeJson value{
    {"schema", "ndnsf-di-kv-storage-grant-submission-v1"},
    {"operationId", submission.operationId},
    {"checkpointDigest", submission.checkpointDigest},
    {"snapshotDigest", submission.snapshotDigest},
    {"roleMapDigest", submission.roleMapDigest},
    {"grants", std::move(grants)}};
  require(nativeConversationCanonicalJson(value).size() <= 64 * 1024,
          "durable grant submission exceeds control transport bound");
  return value;
}

NativeDurableStorageGrantSubmissionV1
nativeDurableStorageGrantSubmissionV1FromJson(const std::string& wire)
{
  require(wire.size() <= 64 * 1024,
          "durable grant submission exceeds control transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "operationId", "checkpointDigest", "snapshotDigest",
    "roleMapDigest", "grants"});
  require(string(value, "schema") == "ndnsf-di-kv-storage-grant-submission-v1" &&
          value.at("grants").is_array() && !value.at("grants").empty() &&
          value.at("grants").size() <= 64,
          "durable grant submission schema or role set is invalid");
  NativeDurableStorageGrantSubmissionV1 submission;
  submission.operationId = string(value, "operationId");
  submission.checkpointDigest = string(value, "checkpointDigest");
  submission.snapshotDigest = string(value, "snapshotDigest");
  submission.roleMapDigest = string(value, "roleMapDigest");
  for (const auto& item : value.at("grants")) {
    fields(item, {"roleName", "repoNode", "ownerIdentity", "writerIdentity",
      "objectName", "grantId", "ownerEpoch"});
    NativeDurableStorageGrantRoleSubmissionV1 grant;
    grant.roleName = string(item, "roleName");
    grant.repoNode = string(item, "repoNode");
    grant.ownerIdentity = string(item, "ownerIdentity");
    grant.writerIdentity = string(item, "writerIdentity");
    grant.objectName = string(item, "objectName");
    grant.grantId = string(item, "grantId");
    grant.ownerEpoch = integer(item, "ownerEpoch");
    submission.grants.push_back(std::move(grant));
  }
  require(nativeConversationCanonicalJson(nativeDurableStorageGrantSubmissionV1ToJson(submission)) == wire,
          "durable grant submission wire is not canonical");
  return submission;
}

void
nativeValidateDurableStorageGrantSubmissionV1(
  const NativeDurableStorageGrantChallengeV1& challenge,
  const NativeDurableStorageGrantSubmissionV1& submission,
  const std::string& authenticatedOwnerIdentity)
{
  (void)nativeDurableStorageGrantChallengeV1ToJson(challenge);
  (void)nativeDurableStorageGrantSubmissionV1ToJson(submission);
  (void)durableGrantName(authenticatedOwnerIdentity);
  require(submission.operationId == challenge.operationId &&
          submission.checkpointDigest == challenge.checkpointDigest &&
          submission.snapshotDigest == challenge.snapshotDigest &&
          submission.roleMapDigest == challenge.roleMapDigest,
          "durable grant submission does not match its challenge");
  require(submission.grants.size() == challenge.roles.size(),
          "durable grant submission does not cover the challenged role set");

  for (std::size_t index = 0; index < challenge.roles.size(); ++index) {
    const auto& role = challenge.roles[index];
    const auto& grant = submission.grants[index];
    require(grant.roleName == role.roleName &&
            grant.repoNode == role.repoTarget.repoNode &&
            grant.ownerIdentity == authenticatedOwnerIdentity &&
            grant.writerIdentity == role.writerIdentity &&
            grant.objectName == role.manifestName,
            "durable grant submission binding does not match its challenge");
  }
}

NativeJson
nativeConversationHandoffControlV1ToJson(
  const NativeConversationHandoffControlV1& control)
{
  const char* action = nullptr;
  switch (control.action) {
    case NativeConversationHandoffActionV1::Begin: action = "BEGIN"; break;
    case NativeConversationHandoffActionV1::Poll: action = "POLL"; break;
    case NativeConversationHandoffActionV1::Grant: action = "GRANT"; break;
    case NativeConversationHandoffActionV1::Commit: action = "COMMIT"; break;
    case NativeConversationHandoffActionV1::Cancel: action = "CANCEL"; break;
    default: throw std::invalid_argument("conversation handoff action is invalid");
  }

  require(control.conversationId.size() >= 16 && control.conversationId.size() <= 256 &&
          control.conversationId.find_first_of("/\\") == std::string::npos,
          "conversation handoff ID is invalid");
  durableGrantOpaque(control.operationId, 256,
                     "conversation handoff operation ID is invalid");
  requireDigest(control.checkpointDigest);
  requireDigest(control.sourceRoleMapDigest);
  requireDigest(control.destinationRoleMapDigest);
  require(control.deadlineMs != 0,
          "conversation handoff deadline is invalid");

  NativeJson grant = nullptr;
  if (control.action == NativeConversationHandoffActionV1::Grant) {
    require(control.grantSubmission.has_value(),
            "conversation handoff grant action requires a submission");
    const auto& submission = *control.grantSubmission;
    require(submission.operationId == control.operationId &&
            submission.checkpointDigest == control.checkpointDigest &&
            submission.roleMapDigest == control.sourceRoleMapDigest,
            "conversation handoff grant binding mismatch");
    grant = nativeDurableStorageGrantSubmissionV1ToJson(submission);
  }
  else {
    require(!control.grantSubmission.has_value(),
            "conversation handoff action forbids a grant submission");
  }

  NativeJson value{
    {"schema", "ndnsf-di-conversation-handoff-control-v1"},
    {"action", action},
    {"operationId", control.operationId},
    {"conversationId", control.conversationId},
    {"checkpointDigest", control.checkpointDigest},
    {"sourceRoleMapDigest", control.sourceRoleMapDigest},
    {"destinationRoleMapDigest", control.destinationRoleMapDigest},
    {"deadlineMs", control.deadlineMs},
    {"grantSubmission", std::move(grant)}};
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "conversation handoff control exceeds transport bound");
  return value;
}

NativeConversationHandoffControlV1
nativeConversationHandoffControlV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "conversation handoff control exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "action", "operationId", "conversationId",
    "checkpointDigest", "sourceRoleMapDigest", "destinationRoleMapDigest",
    "deadlineMs", "grantSubmission"});
  require(string(value, "schema") ==
            "ndnsf-di-conversation-handoff-control-v1",
          "conversation handoff control schema mismatch");

  NativeConversationHandoffControlV1 control;
  const auto action = string(value, "action");
  if (action == "BEGIN") control.action = NativeConversationHandoffActionV1::Begin;
  else if (action == "POLL") control.action = NativeConversationHandoffActionV1::Poll;
  else if (action == "GRANT") control.action = NativeConversationHandoffActionV1::Grant;
  else if (action == "COMMIT") control.action = NativeConversationHandoffActionV1::Commit;
  else if (action == "CANCEL") control.action = NativeConversationHandoffActionV1::Cancel;
  else throw std::invalid_argument("conversation handoff action is invalid");
  control.operationId = string(value, "operationId");
  control.conversationId = string(value, "conversationId");
  control.checkpointDigest = string(value, "checkpointDigest");
  control.sourceRoleMapDigest = string(value, "sourceRoleMapDigest");
  control.destinationRoleMapDigest = string(value, "destinationRoleMapDigest");
  require(value.at("deadlineMs").is_number_unsigned(),
          "conversation handoff deadline must be an unsigned integer");
  control.deadlineMs = value.at("deadlineMs").get<std::uint64_t>();

  const auto& grant = value.at("grantSubmission");
  if (!grant.is_null()) {
    require(grant.is_object(),
            "conversation handoff grant submission must be an object");
    control.grantSubmission = nativeDurableStorageGrantSubmissionV1FromJson(
      nativeConversationCanonicalJson(grant));
  }
  require(nativeConversationCanonicalJson(
            nativeConversationHandoffControlV1ToJson(control)) == wire,
          "conversation handoff control wire is not canonical");
  return control;
}

NativeJson
nativeConversationHandoffReplyV1ToJson(
  const NativeConversationHandoffReplyV1& reply)
{
  const char* state = nullptr;
  switch (reply.state) {
    case NativeConversationHandoffStateV1::Pending: state = "PENDING"; break;
    case NativeConversationHandoffStateV1::GrantRequired: state = "GRANT_REQUIRED"; break;
    case NativeConversationHandoffStateV1::BoundaryReady: state = "BOUNDARY_READY"; break;
    case NativeConversationHandoffStateV1::Saved: state = "SAVED"; break;
    case NativeConversationHandoffStateV1::Cancelled: state = "CANCELLED"; break;
    case NativeConversationHandoffStateV1::CancelClosed: state = "CANCEL_CLOSED"; break;
    default: throw std::invalid_argument("conversation handoff reply state is invalid");
  }

  require(reply.conversationId.size() >= 16 && reply.conversationId.size() <= 256 &&
          reply.conversationId.find_first_of("/\\") == std::string::npos,
          "conversation handoff reply ID is invalid");
  durableGrantOpaque(reply.operationId, 256,
                     "conversation handoff reply operation ID is invalid");
  requireDigest(reply.checkpointDigest);
  requireDigest(reply.sourceRoleMapDigest);
  requireDigest(reply.destinationRoleMapDigest);
  require(reply.deadlineMs != 0,
          "conversation handoff reply deadline is invalid");
  require(reply.ownerReferencesByRole.size() <= 64,
          "conversation handoff reply role count exceeds bound");

  NativeJson challenge = nullptr;
  if (reply.state == NativeConversationHandoffStateV1::GrantRequired) {
    require(reply.grantChallenge.has_value(),
            "grant-required handoff reply needs a challenge");
    const auto& value = *reply.grantChallenge;
    require(value.operationId == reply.operationId &&
            value.checkpointDigest == reply.checkpointDigest &&
            value.roleMapDigest == reply.sourceRoleMapDigest,
            "conversation handoff challenge binding mismatch");
    challenge = nativeDurableStorageGrantChallengeV1ToJson(value);
    require(reply.ownerReferencesByRole.empty(),
            "grant-required handoff reply cannot include publications");
  }
  else {
    require(!reply.grantChallenge.has_value(),
            "conversation handoff reply state forbids a grant challenge");
  }

  NativeJson references = NativeJson::object();
  std::size_t referenceBytes = 0;
  for (const auto& [role, reference] : reply.ownerReferencesByRole) {
    require(!role.empty() && role.size() <= 256 && reference.is_object(),
            "conversation handoff publication reference is invalid");
    const auto wire = nativeConversationCanonicalJson(reference);
    require(wire.size() <= 128 * 1024 - referenceBytes,
            "conversation handoff publication references exceed bound");
    referenceBytes += wire.size();
    references[role] = reference;
  }
  if (reply.state == NativeConversationHandoffStateV1::Saved) {
    require(!reply.ownerReferencesByRole.empty(),
            "saved handoff reply needs publication references");
  }
  else {
    require(reply.ownerReferencesByRole.empty(),
            "conversation handoff reply state forbids publication references");
  }

  NativeJson value{
    {"schema", "ndnsf-di-conversation-handoff-reply-v1"},
    {"state", state},
    {"operationId", reply.operationId},
    {"conversationId", reply.conversationId},
    {"checkpointDigest", reply.checkpointDigest},
    {"sourceRoleMapDigest", reply.sourceRoleMapDigest},
    {"destinationRoleMapDigest", reply.destinationRoleMapDigest},
    {"deadlineMs", reply.deadlineMs},
    {"grantChallenge", std::move(challenge)},
    {"ownerReferencesByRole", std::move(references)}};
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "conversation handoff reply exceeds transport bound");
  return value;
}

NativeConversationHandoffReplyV1
nativeConversationHandoffReplyV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "conversation handoff reply exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "state", "operationId", "conversationId",
    "checkpointDigest", "sourceRoleMapDigest", "destinationRoleMapDigest",
    "deadlineMs", "grantChallenge", "ownerReferencesByRole"});
  require(string(value, "schema") ==
            "ndnsf-di-conversation-handoff-reply-v1",
          "conversation handoff reply schema mismatch");

  NativeConversationHandoffReplyV1 reply;
  const auto state = string(value, "state");
  if (state == "PENDING") reply.state = NativeConversationHandoffStateV1::Pending;
  else if (state == "GRANT_REQUIRED") reply.state = NativeConversationHandoffStateV1::GrantRequired;
  else if (state == "BOUNDARY_READY") reply.state = NativeConversationHandoffStateV1::BoundaryReady;
  else if (state == "SAVED") reply.state = NativeConversationHandoffStateV1::Saved;
  else if (state == "CANCELLED") reply.state = NativeConversationHandoffStateV1::Cancelled;
  else if (state == "CANCEL_CLOSED") reply.state = NativeConversationHandoffStateV1::CancelClosed;
  else throw std::invalid_argument("conversation handoff reply state is invalid");
  reply.operationId = string(value, "operationId");
  reply.conversationId = string(value, "conversationId");
  reply.checkpointDigest = string(value, "checkpointDigest");
  reply.sourceRoleMapDigest = string(value, "sourceRoleMapDigest");
  reply.destinationRoleMapDigest = string(value, "destinationRoleMapDigest");
  require(value.at("deadlineMs").is_number_unsigned(),
          "conversation handoff reply deadline must be an unsigned integer");
  reply.deadlineMs = value.at("deadlineMs").get<std::uint64_t>();

  const auto& challenge = value.at("grantChallenge");
  if (!challenge.is_null()) {
    require(challenge.is_object(),
            "conversation handoff reply challenge must be an object");
    reply.grantChallenge = nativeDurableStorageGrantChallengeV1FromJson(
      nativeConversationCanonicalJson(challenge));
  }
  const auto& references = value.at("ownerReferencesByRole");
  require(references.is_object() && references.size() <= 64,
          "conversation handoff reply references must be a bounded object");
  for (const auto& [role, reference] : references.items())
    require(reply.ownerReferencesByRole.emplace(role, reference).second,
            "conversation handoff reply contains duplicate roles");

  require(nativeConversationCanonicalJson(
            nativeConversationHandoffReplyV1ToJson(reply)) == wire,
          "conversation handoff reply wire is not canonical");
  return reply;
}

void
nativeValidateConversationHandoffReplyBindingV1(
  const NativeConversationHandoffControlV1& control,
  const NativeConversationHandoffReplyV1& reply)
{
  // Validate both objects before comparing their event identity. A caller must
  // not accidentally treat a malformed locally constructed request or reply
  // as correlated merely because their string fields happen to match.
  (void)nativeConversationHandoffControlV1ToJson(control);
  (void)nativeConversationHandoffReplyV1ToJson(reply);
  require(control.operationId == reply.operationId &&
          control.conversationId == reply.conversationId &&
          control.checkpointDigest == reply.checkpointDigest &&
          control.sourceRoleMapDigest == reply.sourceRoleMapDigest &&
          control.destinationRoleMapDigest == reply.destinationRoleMapDigest &&
          control.deadlineMs == reply.deadlineMs,
          "conversation handoff reply does not match its control request");
  require(reply.state != NativeConversationHandoffStateV1::BoundaryReady ||
            control.action == NativeConversationHandoffActionV1::Poll,
          "boundary-ready handoff reply requires a poll request");
}

NativeJson
nativeWithdrawalQueryV1ToJson(const NativeWithdrawalQueryV1& query)
{
  require(query.conversationId.size() >= 16 && query.conversationId.size() <= 256 &&
          query.conversationId.find_first_of("/\\") == std::string::npos,
          "withdrawal query conversation ID is invalid");
  requireDigest(query.checkpointDigest);
  require(query.deadlineMs != 0, "withdrawal query deadline is invalid");
  NativeJson value{{"schema", "ndnsf-di-withdrawal-query-v1"},
                   {"conversationId", query.conversationId},
                   {"checkpointDigest", query.checkpointDigest},
                   {"deadlineMs", query.deadlineMs}};
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "withdrawal query exceeds transport bound");
  return value;
}

NativeWithdrawalQueryV1
nativeWithdrawalQueryV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024, "withdrawal query exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "conversationId", "checkpointDigest", "deadlineMs"});
  require(string(value, "schema") == "ndnsf-di-withdrawal-query-v1",
          "withdrawal query schema mismatch");
  NativeWithdrawalQueryV1 query;
  query.conversationId = string(value, "conversationId");
  query.checkpointDigest = string(value, "checkpointDigest");
  query.deadlineMs = integer(value, "deadlineMs");
  require(nativeConversationCanonicalJson(nativeWithdrawalQueryV1ToJson(query)) == wire,
          "withdrawal query wire is not canonical");
  return query;
}

NativeJson
nativeWithdrawalConfirmV1ToJson(const NativeWithdrawalConfirmV1& confirm)
{
  durableGrantOpaque(confirm.withdrawalId, 256,
                     "withdrawal confirmation ID is invalid");
  durableGrantOpaque(confirm.operationId, 256,
                     "withdrawal confirmation operation ID is invalid");
  require(confirm.conversationId.size() >= 16 &&
          confirm.conversationId.size() <= 256 &&
          confirm.conversationId.find_first_of("/\\") == std::string::npos,
          "withdrawal confirmation conversation ID is invalid");
  require(confirm.registrationGeneration != 0,
          "withdrawal confirmation registration generation is invalid");
  requireDigest(confirm.checkpointDigest);
  requireDigest(confirm.completeRoleSetDigest);
  requireDigest(confirm.journalDigest);
  require(confirm.deadlineMs != 0,
          "withdrawal confirmation deadline is invalid");
  NativeJson value{{"schema", "ndnsf-di-withdrawal-confirm-v1"},
                   {"withdrawalId", confirm.withdrawalId},
                   {"registrationGeneration", confirm.registrationGeneration},
                   {"conversationId", confirm.conversationId},
                   {"checkpointDigest", confirm.checkpointDigest},
                   {"operationId", confirm.operationId},
                   {"completeRoleSetDigest", confirm.completeRoleSetDigest},
                   {"journalDigest", confirm.journalDigest},
                   {"deadlineMs", confirm.deadlineMs}};
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "withdrawal confirmation exceeds transport bound");
  return value;
}

NativeWithdrawalConfirmV1
nativeWithdrawalConfirmV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "withdrawal confirmation exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "withdrawalId", "registrationGeneration",
                 "conversationId", "checkpointDigest", "operationId",
                 "completeRoleSetDigest", "journalDigest", "deadlineMs"});
  require(string(value, "schema") == "ndnsf-di-withdrawal-confirm-v1",
          "withdrawal confirmation schema mismatch");
  NativeWithdrawalConfirmV1 confirm;
  confirm.withdrawalId = string(value, "withdrawalId");
  confirm.registrationGeneration = integer(value, "registrationGeneration");
  confirm.conversationId = string(value, "conversationId");
  confirm.checkpointDigest = string(value, "checkpointDigest");
  confirm.operationId = string(value, "operationId");
  confirm.completeRoleSetDigest = string(value, "completeRoleSetDigest");
  confirm.journalDigest = string(value, "journalDigest");
  confirm.deadlineMs = integer(value, "deadlineMs");
  require(nativeConversationCanonicalJson(nativeWithdrawalConfirmV1ToJson(confirm)) == wire,
          "withdrawal confirmation wire is not canonical");
  return confirm;
}

NativeJson
nativeWithdrawalConfirmReplyV1ToJson(const NativeWithdrawalConfirmReplyV1& reply)
{
  auto value = nativeWithdrawalConfirmV1ToJson(reply.confirm);
  value["schema"] = "ndnsf-di-withdrawal-confirm-reply-v1";
  value["status"] = "CONFIRMED";
  return value;
}

NativeWithdrawalConfirmReplyV1
nativeWithdrawalConfirmReplyV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "withdrawal confirmation reply exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  auto value = nativeParseJson(wire);
  fields(value, {"schema", "status", "withdrawalId", "registrationGeneration",
                 "conversationId", "checkpointDigest", "operationId",
                 "completeRoleSetDigest", "journalDigest", "deadlineMs"});
  require(string(value, "schema") == "ndnsf-di-withdrawal-confirm-reply-v1" &&
          string(value, "status") == "CONFIRMED",
          "withdrawal confirmation reply status is invalid");
  value.erase("status");
  value["schema"] = "ndnsf-di-withdrawal-confirm-v1";
  NativeWithdrawalConfirmReplyV1 reply;
  reply.confirm = nativeWithdrawalConfirmV1FromJson(nativeConversationCanonicalJson(value));
  require(nativeConversationCanonicalJson(nativeWithdrawalConfirmReplyV1ToJson(reply)) == wire,
          "withdrawal confirmation reply wire is not canonical");
  return reply;
}

NativeJson
nativeWithdrawalIntentV1ToJson(const NativeWithdrawalIntentV1& intent)
{
  durableGrantOpaque(intent.withdrawalId, 256, "withdrawal intent ID is invalid");
  durableGrantOpaque(intent.providerBootId, 256,
                     "withdrawal intent Provider boot ID is invalid");
  (void)durableGrantName(intent.ownerIdentity);
  (void)durableGrantName(intent.serviceName);
  require(intent.registrationGeneration != 0,
          "withdrawal intent registration generation is invalid");
  const char* scope = nullptr;
  switch (intent.scope) {
    case NativeWithdrawalScopeV1::Service: scope = "service"; break;
    case NativeWithdrawalScopeV1::Provider: scope = "provider"; break;
    default: throw std::invalid_argument("withdrawal intent scope is invalid");
  }
  require(intent.deadlineMs != 0, "withdrawal intent deadline is invalid");
  require(intent.conversationId.size() >= 16 && intent.conversationId.size() <= 256 &&
          intent.conversationId.find_first_of("/\\") == std::string::npos,
          "withdrawal intent conversation ID is invalid");
  requireDigest(intent.checkpointDigest);
  requireDigest(intent.sourceRoleMapDigest);
  requireDigest(intent.policyDigest);
  require(!intent.localRoles.empty() && intent.localRoles.size() <= 64,
          "withdrawal intent local role count is invalid");
  for (std::size_t index = 0; index < intent.localRoles.size(); ++index) {
    require(intent.localRoles[index].size() <= 256,
            "withdrawal intent role exceeds bound");
    (void)durableGrantName(intent.localRoles[index]);
    require(index == 0 || intent.localRoles[index - 1] < intent.localRoles[index],
            "withdrawal intent roles are not strictly ordered");
  }
  NativeJson value{{"schema", "ndnsf-di-withdrawal-intent-v1"},
                   {"withdrawalId", intent.withdrawalId},
                   {"ownerIdentity", intent.ownerIdentity},
                   {"serviceName", intent.serviceName},
                   {"providerBootId", intent.providerBootId},
                   {"registrationGeneration", intent.registrationGeneration},
                   {"scope", scope},
                   {"deadlineMs", intent.deadlineMs},
                   {"conversationId", intent.conversationId},
                   {"checkpointDigest", intent.checkpointDigest},
                   {"sourceRoleMapDigest", intent.sourceRoleMapDigest},
                   {"localRoles", intent.localRoles},
                   {"policyDigest", intent.policyDigest}};
  require(nativeConversationCanonicalJson(value).size() <= ndn::MAX_NDN_PACKET_SIZE,
          "withdrawal intent exceeds signed packet bound");
  return value;
}

NativeWithdrawalIntentV1
nativeWithdrawalIntentV1FromJson(const std::string& wire)
{
  require(wire.size() <= ndn::MAX_NDN_PACKET_SIZE,
          "withdrawal intent exceeds signed packet bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "withdrawalId", "ownerIdentity", "serviceName",
                 "providerBootId", "registrationGeneration", "scope", "deadlineMs",
                 "conversationId", "checkpointDigest", "sourceRoleMapDigest",
                 "localRoles", "policyDigest"});
  require(string(value, "schema") == "ndnsf-di-withdrawal-intent-v1",
          "withdrawal intent schema mismatch");
  NativeWithdrawalIntentV1 intent;
  intent.withdrawalId = string(value, "withdrawalId");
  intent.ownerIdentity = string(value, "ownerIdentity");
  intent.serviceName = string(value, "serviceName");
  intent.providerBootId = string(value, "providerBootId");
  intent.registrationGeneration = integer(value, "registrationGeneration");
  const auto scope = string(value, "scope");
  if (scope == "service") intent.scope = NativeWithdrawalScopeV1::Service;
  else if (scope == "provider") intent.scope = NativeWithdrawalScopeV1::Provider;
  else throw std::invalid_argument("withdrawal intent scope is invalid");
  intent.deadlineMs = integer(value, "deadlineMs");
  intent.conversationId = string(value, "conversationId");
  intent.checkpointDigest = string(value, "checkpointDigest");
  intent.sourceRoleMapDigest = string(value, "sourceRoleMapDigest");
  require(value.at("localRoles").is_array() && value.at("localRoles").size() <= 64,
          "withdrawal intent roles must be a bounded array");
  for (const auto& role : value.at("localRoles")) {
    require(role.is_string(), "withdrawal intent role must be a string");
    intent.localRoles.push_back(role.get<std::string>());
  }
  intent.policyDigest = string(value, "policyDigest");
  require(nativeConversationCanonicalJson(nativeWithdrawalIntentV1ToJson(intent)) == wire,
          "withdrawal intent wire is not canonical");
  return intent;
}

NativeJson
nativeWithdrawalQueryReplyV1ToJson(const NativeWithdrawalQueryReplyV1& reply)
{
  (void)nativeWithdrawalQueryV1ToJson(reply.query);
  const char* status = nullptr;
  switch (reply.status) {
    case NativeWithdrawalQueryStatusV1::NoIntent: status = "NO_INTENT"; break;
    case NativeWithdrawalQueryStatusV1::Busy: status = "BUSY"; break;
    case NativeWithdrawalQueryStatusV1::Intent: status = "INTENT"; break;
    default: throw std::invalid_argument("withdrawal query reply status is invalid");
  }
  NativeJson intentValue = nullptr;
  NativeJson proofValue = nullptr;
  if (reply.status == NativeWithdrawalQueryStatusV1::Intent) {
    require(reply.intent.has_value() && !reply.originalProofWire.empty() &&
            reply.originalProofWire.size() <= ndn::MAX_NDN_PACKET_SIZE,
            "withdrawal query reply intent proof is missing or oversized");
    require(reply.intent->conversationId == reply.query.conversationId &&
            reply.intent->checkpointDigest == reply.query.checkpointDigest &&
            reply.query.deadlineMs <= reply.intent->deadlineMs,
            "withdrawal query reply intent does not match query");
    intentValue = nativeWithdrawalIntentV1ToJson(*reply.intent);
    const std::string proofBytes(
      reinterpret_cast<const char*>(reply.originalProofWire.data()),
      reply.originalProofWire.size());
    proofValue = nativeConversationBase64Encode(proofBytes);
  }
  else {
    require(!reply.intent && reply.originalProofWire.empty(),
            "withdrawal query reply without intent carries proof");
  }
  NativeJson value{{"schema", "ndnsf-di-withdrawal-query-reply-v1"},
                   {"status", status},
                   {"conversationId", reply.query.conversationId},
                   {"checkpointDigest", reply.query.checkpointDigest},
                   {"deadlineMs", reply.query.deadlineMs},
                   {"intent", std::move(intentValue)},
                   {"proofWire", std::move(proofValue)}};
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "withdrawal query reply exceeds transport bound");
  return value;
}

NativeWithdrawalQueryReplyV1
nativeWithdrawalQueryReplyV1FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "withdrawal query reply exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "status", "conversationId", "checkpointDigest",
                 "deadlineMs", "intent", "proofWire"});
  require(string(value, "schema") == "ndnsf-di-withdrawal-query-reply-v1",
          "withdrawal query reply schema mismatch");
  NativeWithdrawalQueryReplyV1 reply;
  reply.query.conversationId = string(value, "conversationId");
  reply.query.checkpointDigest = string(value, "checkpointDigest");
  reply.query.deadlineMs = integer(value, "deadlineMs");
  const auto status = string(value, "status");
  if (status == "NO_INTENT") reply.status = NativeWithdrawalQueryStatusV1::NoIntent;
  else if (status == "BUSY") reply.status = NativeWithdrawalQueryStatusV1::Busy;
  else if (status == "INTENT") reply.status = NativeWithdrawalQueryStatusV1::Intent;
  else throw std::invalid_argument("withdrawal query reply status is invalid");
  if (reply.status == NativeWithdrawalQueryStatusV1::Intent) {
    require(value.at("intent").is_object() && value.at("proofWire").is_string(),
            "withdrawal query reply intent proof is invalid");
    reply.intent = nativeWithdrawalIntentV1FromJson(
      nativeConversationCanonicalJson(value.at("intent")));
    const auto proofBytes = nativeConversationBase64Decode(
      string(value, "proofWire"));
    reply.originalProofWire.assign(proofBytes.begin(), proofBytes.end());
  }
  else {
    require(value.at("intent").is_null() && value.at("proofWire").is_null(),
            "withdrawal query reply without intent carries proof");
  }
  require(nativeConversationCanonicalJson(
            nativeWithdrawalQueryReplyV1ToJson(reply)) == wire,
          "withdrawal query reply wire is not canonical");
  return reply;
}

namespace {
NativeJson
withdrawalIntentProofPayloadV1(const NativeWithdrawalIntentV1& intent)
{
  (void)nativeWithdrawalIntentV1ToJson(intent);
  const char* scope = intent.scope == NativeWithdrawalScopeV1::Service
    ? "service" : "provider";
  return NativeJson{{"schema", "ndnsf-di-withdrawal-intent-proof-v1"},
                    {"withdrawalId", intent.withdrawalId},
                    {"ownerIdentityDigest", digest(NativeJson(intent.ownerIdentity))},
                    {"serviceName", intent.serviceName},
                    {"providerBootId", intent.providerBootId},
                    {"registrationGeneration", intent.registrationGeneration},
                    {"scope", scope},
                    {"deadlineMs", intent.deadlineMs},
                    {"conversationId", intent.conversationId},
                    {"checkpointDigest", intent.checkpointDigest},
                    {"sourceRoleMapDigest", intent.sourceRoleMapDigest},
                    {"localRoleSetDigest", digest(NativeJson(intent.localRoles))},
                    {"policyDigest", intent.policyDigest}};
}
} // namespace

ndn::Name
nativeWithdrawalIntentProofNameV1(
  const ndn::Name& providerIdentity, const NativeWithdrawalIntentV1& intent)
{
  require(!providerIdentity.empty(), "withdrawal proof Provider identity is empty");
  (void)durableGrantName(providerIdentity.toUri(ndn::name::UriFormat::ALTERNATE));
  const auto payload = withdrawalIntentProofPayloadV1(intent);
  const auto payloadDigest = digest(payload);
  ndn::Name name = providerIdentity;
  name.append("withdrawal-intent").append("v1")
      .append(intent.withdrawalId).append(payloadDigest.substr(7));
  return name;
}

ndn::Data
nativeSignWithdrawalIntentProofV1(
  const ndn::Name& providerIdentity, const NativeWithdrawalIntentV1& intent,
  const std::function<void(ndn::Data&)>& signProviderData)
{
  require(static_cast<bool>(signProviderData),
          "withdrawal proof Provider signer is unavailable");
  const auto content = nativeConversationCanonicalJson(withdrawalIntentProofPayloadV1(intent));
  ndn::Data proof(nativeWithdrawalIntentProofNameV1(providerIdentity, intent));
  proof.setContent(ndn::span<const std::uint8_t>(
    reinterpret_cast<const std::uint8_t*>(content.data()), content.size()));
  signProviderData(proof);
  const auto& signedContent = proof.getContent();
  require(proof.getName() == nativeWithdrawalIntentProofNameV1(providerIdentity, intent) &&
          signedContent.value_size() == content.size() &&
          std::equal(content.begin(), content.end(),
                     reinterpret_cast<const char*>(signedContent.value())) &&
          proof.wireEncode().size() <= ndn::MAX_NDN_PACKET_SIZE,
          "withdrawal proof signer changed its binding or exceeded packet bound");
  return proof;
}

void
nativeValidateWithdrawalIntentProofV1(
  const ndn::Buffer& originalWire, const ndn::Name& expectedProvider,
  const NativeWithdrawalIntentV1& expectedIntent, std::uint64_t nowMs,
  const std::function<bool(const ndn::Data&, const ndn::Name&)>& verifyProducer)
{
  require(static_cast<bool>(verifyProducer),
          "withdrawal proof Producer verifier is unavailable");
  require(!originalWire.empty() && originalWire.size() <= ndn::MAX_NDN_PACKET_SIZE,
          "withdrawal proof wire size is invalid");
  require(nowMs < expectedIntent.deadlineMs, "withdrawal proof has expired");
  ndn::Data proof{ndn::Block(ndn::span<const std::uint8_t>(
    originalWire.data(), originalWire.size()))};
  const auto& encoded = proof.wireEncode();
  require(encoded.size() == originalWire.size() &&
          std::equal(originalWire.begin(), originalWire.end(), encoded.data()) &&
          proof.getName() == nativeWithdrawalIntentProofNameV1(
            expectedProvider, expectedIntent),
          "withdrawal proof original packet identity is invalid");
  const auto& content = proof.getContent();
  const std::string payload(reinterpret_cast<const char*>(content.value()),
                            content.value_size());
  require(payload == nativeConversationCanonicalJson(
            withdrawalIntentProofPayloadV1(expectedIntent)),
          "withdrawal proof content binding is invalid");
  ndn::Name signerIdentity;
  try {
    const auto& signature = proof.getSignatureInfo();
    if (!signature.hasKeyLocator() ||
        signature.getKeyLocator().getType() != ndn::tlv::Name)
      throw std::invalid_argument("withdrawal proof signer is not named");
    const auto& keyName = signature.getKeyLocator().getName();
    signerIdentity = ndn::security::isValidKeyName(keyName)
      ? ndn::security::extractIdentityFromKeyName(keyName)
      : ndn::security::extractIdentityFromCertName(keyName);
  }
  catch (const std::exception&) {
    throw std::invalid_argument("withdrawal proof signer identity is invalid");
  }
  require(signerIdentity == expectedProvider,
          "withdrawal proof signer does not own the Producer namespace");
  require(verifyProducer(proof, expectedProvider),
          "withdrawal proof Producer signature is not authorized");
}

NativeJson
nativeConversationHandoffControlV2ToJson(
  const NativeConversationHandoffControlV2& event)
{
  auto value = nativeConversationHandoffControlV1ToJson(event.control);
  value["schema"] = "ndnsf-di-handoff-control-v2";
  require(!event.sourceProvidersByRole.empty() &&
          event.sourceProvidersByRole.size() <= 64,
          "conversation handoff source role map exceeds bounds");
  NativeJson sourceProviders = NativeJson::array();
  std::set<std::string> distinctProviders;
  for (const auto& [role, provider] : event.sourceProvidersByRole) {
    (void)durableGrantName(role);
    (void)durableGrantName(provider);
    sourceProviders.push_back(NativeJson::array({role, provider}));
    distinctProviders.insert(provider);
  }
  require(distinctProviders.size() <= 16 &&
          digest(sourceProviders) == event.control.sourceRoleMapDigest,
          "conversation handoff source role map binding is invalid");
  NativeJson withdrawalBinding = nullptr;
  const char* trigger = nullptr;
  switch (event.trigger) {
    case NativeConversationSaveTriggerV2::UserSwitch:
      trigger = "USER_SWITCH";
      require(event.withdrawingProviderIdentity.empty() &&
              !event.withdrawalIntent && event.withdrawalProofWire.empty(),
              "user switch forbids withdrawal binding");
      break;
    case NativeConversationSaveTriggerV2::WithdrawalSave: {
      trigger = "WITHDRAWAL_SAVE";
      const auto provider = durableGrantName(event.withdrawingProviderIdentity);
      require(event.withdrawalIntent.has_value() &&
              !event.withdrawalProofWire.empty() &&
              event.withdrawalProofWire.size() <= ndn::MAX_NDN_PACKET_SIZE,
              "withdrawal control proof is missing or oversized");
      const auto& intent = *event.withdrawalIntent;
      const auto intentValue = nativeWithdrawalIntentV1ToJson(intent);
      require(intent.conversationId == event.control.conversationId &&
              intent.checkpointDigest == event.control.checkpointDigest &&
              intent.sourceRoleMapDigest == event.control.sourceRoleMapDigest &&
              event.control.deadlineMs <= intent.deadlineMs &&
              event.control.destinationRoleMapDigest == digest(NativeJson::array()),
              "withdrawal control intent binding is invalid");
      std::vector<std::string> localRoles;
      for (const auto& [role, source] : event.sourceProvidersByRole) {
        if (source == event.withdrawingProviderIdentity)
          localRoles.push_back(role);
      }
      require(!localRoles.empty() && localRoles == intent.localRoles,
              "withdrawal control source membership is invalid");
      (void)provider;
      const std::string proofBytes(
        reinterpret_cast<const char*>(event.withdrawalProofWire.data()),
        event.withdrawalProofWire.size());
      withdrawalBinding = NativeJson{
        {"providerIdentity", event.withdrawingProviderIdentity},
        {"intent", intentValue},
        {"proofWire", nativeConversationBase64Encode(proofBytes)}};
      break;
    }
    default:
      throw std::invalid_argument("conversation handoff trigger is invalid");
  }
  value["trigger"] = trigger;
  value["sourceProvidersByRole"] = sourceProviders;
  value["withdrawalBinding"] = withdrawalBinding;
  value["contextDigest"] = digest(NativeJson{
    {"operationId", event.control.operationId},
    {"conversationId", event.control.conversationId},
    {"checkpointDigest", event.control.checkpointDigest},
    {"sourceRoleMapDigest", event.control.sourceRoleMapDigest},
    {"sourceProvidersByRole", sourceProviders},
    {"destinationRoleMapDigest", event.control.destinationRoleMapDigest},
    {"deadlineMs", event.control.deadlineMs},
    {"trigger", trigger},
    {"withdrawalBinding", withdrawalBinding}});
  require(nativeConversationCanonicalJson(value).size() <= 128 * 1024,
          "conversation handoff control exceeds transport bound");
  return value;
}

NativeConversationHandoffControlV2
nativeConversationHandoffControlV2FromJson(const std::string& wire)
{
  require(wire.size() <= 128 * 1024,
          "conversation handoff control exceeds transport bound");
  requireDurableGrantJsonNestingBounded(wire);
  const auto value = nativeParseJson(wire);
  fields(value, {"schema", "action", "operationId", "conversationId",
    "checkpointDigest", "sourceRoleMapDigest", "destinationRoleMapDigest",
    "deadlineMs", "grantSubmission", "trigger", "sourceProvidersByRole",
    "withdrawalBinding",
    "contextDigest"});
  require(string(value, "schema") ==
            "ndnsf-di-handoff-control-v2",
          "conversation handoff control schema mismatch");
  requireDigest(string(value, "contextDigest"));

  auto base = value;
  base.erase("trigger");
  base.erase("sourceProvidersByRole");
  base.erase("withdrawalBinding");
  base.erase("contextDigest");
  base["schema"] = "ndnsf-di-conversation-handoff-control-v1";
  NativeConversationHandoffControlV2 event;
  event.control = nativeConversationHandoffControlV1FromJson(
    nativeConversationCanonicalJson(base));
  const auto& sourceProviders = value.at("sourceProvidersByRole");
  require(sourceProviders.is_array() && !sourceProviders.empty() &&
          sourceProviders.size() <= 64,
          "conversation handoff source role map exceeds bounds");
  for (const auto& pair : sourceProviders) {
    require(pair.is_array() && pair.size() == 2 &&
            pair.at(0).is_string() && pair.at(1).is_string(),
            "conversation handoff source role pair is invalid");
    const auto inserted = event.sourceProvidersByRole.emplace(
      pair.at(0).get<std::string>(), pair.at(1).get<std::string>());
    require(inserted.second, "conversation handoff source role is duplicated");
  }
  const auto trigger = string(value, "trigger");
  const auto& binding = value.at("withdrawalBinding");
  if (trigger == "USER_SWITCH") {
    require(binding.is_null(), "user switch forbids withdrawal binding");
  }
  else if (trigger == "WITHDRAWAL_SAVE") {
    event.trigger = NativeConversationSaveTriggerV2::WithdrawalSave;
    fields(binding, {"providerIdentity", "intent", "proofWire"});
    event.withdrawingProviderIdentity = string(binding, "providerIdentity");
    require(binding.at("intent").is_object(),
            "withdrawal control intent must be an object");
    event.withdrawalIntent = nativeWithdrawalIntentV1FromJson(
      nativeConversationCanonicalJson(binding.at("intent")));
    const auto proofBytes = nativeConversationBase64Decode(
      string(binding, "proofWire"));
    event.withdrawalProofWire.assign(proofBytes.begin(), proofBytes.end());
  }
  else {
    throw std::invalid_argument("conversation handoff trigger is invalid");
  }
  require(nativeConversationCanonicalJson(
            nativeConversationHandoffControlV2ToJson(event)) == wire,
          "conversation handoff control wire is not canonical");
  return event;
}

std::string
nativeConversationRecoveryReadySetDigest(
  const std::map<std::string, NativeConversationStateReadyV2>& readyByRole)
{
  require(!readyByRole.empty(), "recovery READY set is empty");
  NativeJson set = NativeJson::array();
  for (const auto& [role, ready] : readyByRole) {
    require(role == ready.parent.roleName, "recovery READY role set mismatch");
    requireDigest(ready.readyDigest);
    (void)nativeConversationStateReadyV2ToJson(ready);
    set.push_back({{"roleName", role}, {"readyDigest", ready.readyDigest}});
  }
  return digest(set);
}

void
nativeValidateConversationStateReadyV2(
  const NativeConversationStateReadyV2& ready, const NativeSelectionProjectionV3& projection,
  const std::string& admittedBootEpoch, const std::string& actualProducer,
  const std::string& actualProducerRole, std::uint64_t nowMs)
{
  requireDigest(ready.readyDigest);
  (void)nativeConversationStateReadyV2ToJson(ready);
  require(projection.conversationStateReference && projection.conversationTurnBinding &&
          projection.protectedKvRestore && projection.protectedKvRestore->originReceipt,
          "recovery READY lacks planned origin expectation");
  const auto& restore = *projection.protectedKvRestore;
  nativeValidateProtectedKvRestoreProjection(restore, projection);
  const auto reference = nativeReadProtectedKvPublicationReference(restore.reference);
  const auto origin = nativeReadProviderConversationStateReceipt(nativeParseJson(*restore.originReceipt));
  const auto& expected = *projection.conversationStateReference;
  const auto& parent = ready.parent;
  require(ready.requestId == projection.requestId && ready.attemptEpoch == projection.attempt &&
          ready.generationId == projection.generationContract.generationId &&
          ready.planDigest == projection.planDigest && parent.conversationId == expected.conversationId &&
          parent.contextEpoch == expected.contextEpoch && parent.serviceName == expected.serviceName &&
          parent.planRoleMapDigest == expected.planRoleMapDigest && parent.checkpointDigest == expected.checkpointDigest &&
          parent.roleName == expected.roleName && parent.roleReceiptDigest == expected.roleReceiptDigest &&
          ndn::Name(ready.providerIdentity) == ndn::Name(projection.provider) &&
          ndn::Name(actualProducer) == ndn::Name(projection.provider) &&
          actualProducerRole == expected.roleName && !admittedBootEpoch.empty() &&
          ready.providerBootId == admittedBootEpoch &&
          ndn::Name(ready.originProviderIdentity) == ndn::Name(origin.providerIdentity) &&
          ready.protectedKvWireDigest == reference.wireDigest,
          "recovery READY differs from admitted selection");
  require(parent.expiresAtMs > nowMs && parent.expiresAtMs <= projection.deadlineMs &&
          parent.expiresAtMs <= expected.expiresAtMs && parent.expiresAtMs <= restore.recipient.expiresAtMs &&
          parent.expiresAtMs <= origin.expiresAtMs &&
          parent.expiresAtMs <= projection.conversationTurnBinding->retentionDeadlineMs,
          "recovery READY expiry exceeds current scope");
}

void
nativeValidateConversationRecoveryActivationV1(
  const NativeConversationRecoveryActivationV1& activation,
  const NativeConversationStateReadyV2& ownReady, std::uint64_t nowMs)
{
  (void)nativeConversationRecoveryActivationV1ToJson(activation);
  requireDigest(ownReady.readyDigest);
  (void)nativeConversationStateReadyV2ToJson(ownReady);
  require(activation.requestId == ownReady.requestId && activation.attemptEpoch == ownReady.attemptEpoch &&
          activation.generationId == ownReady.generationId && activation.planDigest == ownReady.planDigest &&
          activation.conversationId == ownReady.parent.conversationId &&
          activation.contextEpoch == ownReady.parent.contextEpoch && activation.serviceName == ownReady.parent.serviceName &&
          activation.planRoleMapDigest == ownReady.parent.planRoleMapDigest &&
          activation.checkpointDigest == ownReady.parent.checkpointDigest && activation.roleName == ownReady.parent.roleName &&
          activation.readyDigest == ownReady.readyDigest && activation.expiresAtMs > nowMs &&
          activation.expiresAtMs <= ownReady.parent.expiresAtMs,
          "recovery activation differs from executable READY");
}

std::string nativeSealConversationEnvelope(
  const std::string& plaintext, const std::string& envelopeId,
  std::uint64_t expiresAtMs, const NativeConversationJournalKey& key)
{
  require(plaintext.size() <= maxTranscriptBytes && !envelopeId.empty() &&
          envelopeId.find_first_of("/\\") == std::string::npos &&
          envelopeId != "." && envelopeId != ".." && expiresAtMs > 0 &&
          key.bytes.size() == 32 && !key.id.empty() &&
          key.id.find_first_of("/\\") == std::string::npos, "conversation envelope input invalid");
  NativeJson metadata{{"schema", "ndnsf-di-protected-request-v3"}, {"requestId", envelopeId},
                      {"expiresAtMs", expiresAtMs}, {"keyId", key.id}};
  const auto aad = metadata.dump(-1, ' ', true);
  std::string nonce(12, '\0');
  if (RAND_bytes(reinterpret_cast<unsigned char*>(nonce.data()), nonce.size()) != 1)
    throw std::runtime_error("conversation nonce generation failed");
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
    EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!cipher) throw std::runtime_error("conversation cipher allocation failed");
  std::string ciphertext(plaintext.size() + 16, '\0');
  int count = 0, written = 0;
  const bool ok =
    EVP_EncryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
    EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
    EVP_EncryptInit_ex(cipher.get(), nullptr, nullptr, key.bytes.data(),
      reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
    EVP_EncryptUpdate(cipher.get(), nullptr, &count,
      reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
    EVP_EncryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(ciphertext.data()), &written,
      reinterpret_cast<const unsigned char*>(plaintext.data()), static_cast<int>(plaintext.size())) == 1 &&
    EVP_EncryptFinal_ex(cipher.get(), reinterpret_cast<unsigned char*>(ciphertext.data()) + written, &count) == 1;
  if (!ok) throw std::runtime_error("conversation envelope encryption failed");
  ciphertext.resize(static_cast<std::size_t>(written + count) + 16);
  if (EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_GET_TAG, 16,
      ciphertext.data() + written + count) != 1)
    throw std::runtime_error("conversation envelope tag failed");
  metadata["nonce"] = nativeConversationBase64Encode(nonce);
  metadata["ciphertext"] = nativeConversationBase64Encode(ciphertext);
  return metadata.dump(-1, ' ', true);
}

std::vector<std::vector<std::uint8_t>> nativeConversationAuthenticationKeys(
  const std::string& identity, const std::vector<NativeConversationJournalKey>& keys)
{
  require(!identity.empty() && identity.find_first_of("/\\") == std::string::npos &&
          !keys.empty() && keys.size() <= 16, "conversation journal identity/key ring invalid");
  std::string context = "ndnsf-di-runtime-authentication-subkey-v1";
  context.push_back('\0');
  context += "conversation-checkpoint-v1";
  context.push_back('\0');
  context += identity;
  std::set<std::string> ids;
  std::vector<std::vector<std::uint8_t>> result;
  for (const auto& key : keys) {
    require(!key.id.empty() && key.id.find_first_of("/\\") == std::string::npos &&
            key.bytes.size() == 32 && ids.insert(key.id).second,
            "conversation journal key invalid");
    unsigned int size = 32;
    std::vector<std::uint8_t> derived(size);
    if (!HMAC(EVP_sha256(), key.bytes.data(), static_cast<int>(key.bytes.size()),
              reinterpret_cast<const unsigned char*>(context.data()), context.size(),
              derived.data(), &size))
      throw std::runtime_error("conversation subkey derivation failed");
    result.push_back(std::move(derived));
  }
  return result;
}

std::string nativeReadConversationEnvelope(
  const std::string& encoded, const std::string& envelopeId,
  const std::vector<NativeConversationJournalKey>& keys, std::uint64_t nowMs)
{
  require(!keys.empty() && keys.size() <= 16 && nowMs > 0 && !envelopeId.empty() &&
          encoded.size() <= 64 * 1024 * 1024, "conversation envelope input invalid");
  std::set<std::string> ids;
  for (const auto& key : keys)
    require(key.bytes.size() == 32 && !key.id.empty() && ids.insert(key.id).second,
            "conversation journal key invalid");
  auto body = nativeParseJson(encoded);
  require(string(body, "requestId") == envelopeId && integer(body, "expiresAtMs") > nowMs,
          "conversation envelope identity or expiry mismatch");
  const auto schema = string(body, "schema");
  NativeJson metadata{{"schema", schema}, {"requestId", envelopeId},
                      {"expiresAtMs", integer(body, "expiresAtMs")}};
  if (schema == "ndnsf-di-protected-request-v1") {
    fields(body, {"schema", "requestId", "expiresAtMs", "payload", "mac"});
    const auto mac = string(body, "mac");
    body.erase("mac");
    // RuntimeJournal uses Python's ASCII JSON for envelope authentication.
    const auto unsignedWire = body.dump(-1, ' ', true);
    bool verified = false;
    for (const auto& key : keys) {
      const auto expected = signature(unsignedWire, key.bytes);
      verified |= mac.size() == expected.size() &&
                  CRYPTO_memcmp(mac.data(), expected.data(), expected.size()) == 0;
    }
    require(verified, "conversation envelope authentication failed");
    return base64(body.at("payload"));
  }
  require(schema == "ndnsf-di-protected-request-v2" || schema == "ndnsf-di-protected-request-v3",
          "conversation envelope schema mismatch");
  const bool keyed = schema == "ndnsf-di-protected-request-v3";
  if (keyed) {
    fields(body, {"schema", "requestId", "expiresAtMs", "keyId", "nonce", "ciphertext"});
    metadata["keyId"] = string(body, "keyId");
  }
  else fields(body, {"schema", "requestId", "expiresAtMs", "nonce", "ciphertext"});
  const auto aad = metadata.dump(-1, ' ', true);
  const auto nonce = base64(body.at("nonce"));
  const auto ciphertext = base64(body.at("ciphertext"));
  require(nonce.size() == 12 && ciphertext.size() >= 16,
          "conversation envelope nonce or tag invalid");
  for (const auto& key : keys) {
    if (keyed && key.id != string(body, "keyId")) continue;
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> cipher(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!cipher) throw std::runtime_error("conversation cipher allocation failed");
    std::string plaintext(ciphertext.size(), '\0');
    int count = 0, written = 0;
    const auto payloadSize = ciphertext.size() - 16;
    const bool initialized =
      EVP_DecryptInit_ex(cipher.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_DecryptInit_ex(cipher.get(), nullptr, nullptr, key.bytes.data(),
        reinterpret_cast<const unsigned char*>(nonce.data())) == 1 &&
      EVP_DecryptUpdate(cipher.get(), nullptr, &count,
        reinterpret_cast<const unsigned char*>(aad.data()), static_cast<int>(aad.size())) == 1 &&
      EVP_DecryptUpdate(cipher.get(), reinterpret_cast<unsigned char*>(plaintext.data()), &written,
        reinterpret_cast<const unsigned char*>(ciphertext.data()), static_cast<int>(payloadSize)) == 1 &&
      EVP_CIPHER_CTX_ctrl(cipher.get(), EVP_CTRL_GCM_SET_TAG, 16,
        const_cast<char*>(ciphertext.data() + payloadSize)) == 1;
    if (initialized && EVP_DecryptFinal_ex(cipher.get(),
          reinterpret_cast<unsigned char*>(plaintext.data()) + written, &count) == 1) {
      plaintext.resize(static_cast<std::size_t>(written + count));
      return plaintext;
    }
    OPENSSL_cleanse(plaintext.data(), plaintext.size());
  }
  throw std::invalid_argument("conversation envelope authentication failed");
}

ProviderConversationStateReceiptV1
nativeReadProviderConversationStateReceipt(const NativeJson& value)
{
  static const std::set<std::string> fields = {
    "adapterDigest", "cacheEpoch", "conversationId", "expiresAtMs",
    "graphSemanticDigest", "layoutDigest", "modelDigest",
    "originGenerationId", "originRequestId", "parentContextEpoch",
    "planRoleMapDigest", "positionDigest", "prefixDigest", "prefixTokenCount",
    "providerBootId", "providerIdentity", "receiptDigest", "requesterIdentity",
    "roleName", "roleSplitDigest", "schema", "securityDomainDigest",
    "serviceName", "signature", "stateComponentDigests", "stateSchemaDigest",
    "successorContextEpoch",
  };
  if (!value.is_object() || value.value("schema", std::string{}) !=
        "ndnsf-di-provider-conversation-receipt-v1")
    throw std::invalid_argument("conversation receipt schema mismatch");
  std::set<std::string> seen;
  for (const auto& item : value.items()) seen.insert(item.key());
  if (seen != fields) throw std::invalid_argument("conversation receipt field set mismatch");
  ProviderConversationStateReceiptV1 result;
  result.conversationId = value.at("conversationId").get<std::string>();
  result.parentContextEpoch = integer(value, "parentContextEpoch");
  result.successorContextEpoch = integer(value, "successorContextEpoch");
  result.originRequestId = value.at("originRequestId").get<std::string>();
  result.originGenerationId = value.at("originGenerationId").get<std::string>();
  result.serviceName = value.at("serviceName").get<std::string>();
  result.requesterIdentity = value.at("requesterIdentity").get<std::string>();
  result.securityDomainDigest = value.at("securityDomainDigest").get<std::string>();
  result.modelDigest = value.at("modelDigest").get<std::string>();
  result.graphSemanticDigest = value.at("graphSemanticDigest").get<std::string>();
  result.adapterDigest = value.at("adapterDigest").get<std::string>();
  result.roleName = value.at("roleName").get<std::string>();
  result.roleSplitDigest = value.at("roleSplitDigest").get<std::string>();
  result.layoutDigest = value.at("layoutDigest").get<std::string>();
  result.planRoleMapDigest = value.at("planRoleMapDigest").get<std::string>();
  result.providerIdentity = value.at("providerIdentity").get<std::string>();
  result.providerBootId = value.at("providerBootId").get<std::string>();
  result.cacheEpoch = integer(value, "cacheEpoch");
  result.prefixDigest = value.at("prefixDigest").get<std::string>();
  const auto prefixTokenCount = integer(value, "prefixTokenCount");
  require(prefixTokenCount <= std::numeric_limits<std::uint32_t>::max(),
          "conversation receipt prefix count overflows");
  result.prefixTokenCount = static_cast<std::uint32_t>(prefixTokenCount);
  result.positionDigest = value.at("positionDigest").get<std::string>();
  result.stateSchemaDigest = value.at("stateSchemaDigest").get<std::string>();
  result.stateComponentDigests = value.at("stateComponentDigests").get<std::vector<std::string>>();
  result.expiresAtMs = integer(value, "expiresAtMs");
  require(result.parentContextEpoch != std::numeric_limits<std::uint64_t>::max(),
          "conversation receipt epoch overflows");
  result.validate();
  if (value.at("receiptDigest") != result.computedDigest() ||
      !value.at("signature").is_string() || !value.at("signature").get<std::string>().empty())
    throw std::invalid_argument("conversation receipt digest or signature field mismatch");
  return result;
}

std::string nativeConversationPrefixDigest(const std::vector<std::int64_t>& tokens)
{
  for (const auto token : tokens) require(token >= 0, "conversation token must be nonnegative");
  return digest({{"schema", "ndnsf-di-prefix-v1"}, {"tokenIds", tokens}});
}

std::string nativeSignConversationCheckpoint(
  NativeJson value, const std::vector<std::uint8_t>& key)
{
  validateUnsigned(value);
  const auto hash = digest(value);
  value["checkpointDigest"] = hash;
  value["signature"] = signature(hash, key);
  const auto wire = nativeConversationCanonicalJson(value);
  require(wire.size() <= maxCheckpointBytes, "conversation checkpoint exceeds bound");
  return wire;
}

NativeJson nativeReadConversationCheckpoint(
  const std::string& wire, const std::vector<std::vector<std::uint8_t>>& keys,
  std::uint64_t nowMs)
{
  require(!wire.empty() && wire.size() <= maxCheckpointBytes, "conversation checkpoint exceeds bound");
  require(!keys.empty() && keys.size() <= 16 && nowMs > 0,
          "conversation verification configuration invalid");
  auto value = nativeParseJson(wire);
  require(nativeConversationCanonicalJson(value) == wire, "conversation checkpoint is not canonical");
  const auto claimed = string(value, "checkpointDigest");
  const auto signedHash = string(value, "signature");
  auto unsignedValue = value;
  unsignedValue.erase("checkpointDigest");
  unsignedValue.erase("signature");
  validateUnsigned(unsignedValue);
  requireDigest(claimed);
  require(claimed == digest(unsignedValue), "conversation checkpoint digest mismatch");
  require(signedHash.size() == 64 && signedHash.find_first_not_of("0123456789abcdef") ==
          std::string::npos, "conversation checkpoint signature malformed");
  bool verified = false;
  for (const auto& key : keys) {
    const auto expected = signature(claimed, key);
    verified |= CRYPTO_memcmp(expected.data(), signedHash.data(), expected.size()) == 0;
  }
  require(verified, "conversation checkpoint authentication failed");
  require(integer(value, "expiresAtMs") > nowMs, "conversation checkpoint expired");
  return value;
}

void nativeValidateConversationTranscript(const NativeJson& value, const NativeJson& checkpoint,
  std::optional<std::size_t> nativeInitialPromptTokenCount)
{
  require(nativeConversationCanonicalJson(value).size() <= maxTranscriptBytes, "conversation transcript exceeds bound");
  fields(value, {"schema", "conversationId", "contextEpoch", "requesterIdentity", "serviceName",
    "securityDomainDigest", "applicationMessages", "tokenizerDigest", "chatTemplateDigest",
    "canonicalTokenIds", "prefixDigest", "prefixTokenCount", "providerRoleReceipts",
    "checkpointDigest", "planRoleMapDigest", "createdAtMs", "expiresAtMs"});
  require(string(value, "schema") == "ndnsf-di-conversation-transcript-v1", "conversation transcript schema mismatch");
  identity(value);
  requireDigest(string(value, "tokenizerDigest"));
  requireDigest(string(value, "chatTemplateDigest"));
  requireDigest(string(value, "prefixDigest"));
  requireDigest(string(value, "checkpointDigest"));
  const auto& tokens = value.at("canonicalTokenIds");
  require(tokens.is_array() && !tokens.empty(), "conversation transcript token list missing");
  std::vector<std::int64_t> ids;
  for (const auto& token : tokens) {
    require(token.is_number_integer() &&
      (!token.is_number_unsigned() || token.get<std::uint64_t>() <=
       static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())),
      "conversation token type invalid");
    ids.push_back(token.get<std::int64_t>());
  }
  require(integer(value, "prefixTokenCount") == ids.size() &&
          string(value, "prefixDigest") == nativeConversationPrefixDigest(ids),
          "conversation transcript prefix mismatch");
  require(integer(value, "contextEpoch") > 0 && integer(value, "createdAtMs") > 0 &&
          integer(value, "expiresAtMs") > integer(value, "createdAtMs"),
          "conversation transcript lifetime invalid");
  base64(value.at("applicationMessages"));
  require(value.at("providerRoleReceipts").is_array(), "conversation transcript receipts invalid");
  std::set<std::string> roles;
  const auto runtimePrefix = nativeInitialPromptTokenCount ? nativeGenerationStatePrefixDigest(
    ids, *nativeInitialPromptTokenCount, string(value, "tokenizerDigest")) : std::string{};
  std::string receiptPrefix;
  for (const auto& encoded : value.at("providerRoleReceipts")) {
    auto receipt = nativeParseJson(base64(encoded));
    const auto role = string(receipt, "roleName");
    const auto receiptDigest = string(receipt, "receiptDigest");
    require(roles.insert(role).second && checkpoint.at("roleReceiptDigests").contains(role) &&
            checkpoint.at("roleReceiptDigests").at(role) == receiptDigest,
            "conversation transcript role receipt mismatch");
    for (const auto field : {"conversationId", "requesterIdentity", "serviceName",
          "securityDomainDigest", "planRoleMapDigest", "prefixTokenCount"}) {
      if (receipt.at(field) != checkpoint.at(field))
        throw std::invalid_argument(std::string("conversation receipt scope mismatch: ") + field);
    }
    const auto currentPrefix = string(receipt, "prefixDigest");
    if (receiptPrefix.empty()) receiptPrefix = currentPrefix;
    require(currentPrefix == receiptPrefix &&
            (currentPrefix == string(checkpoint, "logicalPrefixDigest") ||
             (!runtimePrefix.empty() && currentPrefix == runtimePrefix)) &&
            receipt.at("successorContextEpoch") == checkpoint.at("contextEpoch") &&
            receipt.at("parentContextEpoch") == checkpoint.at("parentContextEpoch") &&
            integer(receipt, "expiresAtMs") >= integer(checkpoint, "expiresAtMs"),
            "conversation receipt lineage mismatch");
    receipt.erase("receiptDigest");
    receipt.erase("signature");
    require(receiptDigest == digest(receipt), "conversation transcript receipt digest mismatch");
  }
  require(roles.size() == checkpoint.at("roleReceiptDigests").size(),
          "conversation transcript role receipts incomplete");
  for (const auto field : {"conversationId", "contextEpoch", "requesterIdentity", "serviceName",
        "securityDomainDigest", "planRoleMapDigest", "checkpointDigest", "prefixTokenCount", "expiresAtMs"})
    require(value.at(field) == checkpoint.at(field), "conversation transcript checkpoint binding mismatch");
  require(value.at("prefixDigest") == checkpoint.at("logicalPrefixDigest"),
          "conversation transcript logical prefix mismatch");
}

NativeConversationHandoffBoundaryGateV1::NativeConversationHandoffBoundaryGateV1(
  const std::vector<std::string>& sourceProviders,
  std::chrono::steady_clock::time_point deadline)
  : m_deadline(deadline)
{
  if (sourceProviders.empty() || sourceProviders.size() > 64)
    throw std::invalid_argument("handoff boundary gate requires 1 to 64 source Providers");
  if (deadline <= std::chrono::steady_clock::now())
    throw std::invalid_argument("handoff boundary deadline has already expired");
  for (const auto& provider : sourceProviders) {
    if (provider.empty() || provider.size() > 4096 ||
        !m_expectedProviders.insert(provider).second)
      throw std::invalid_argument("handoff boundary gate has an invalid or duplicate Provider");
  }
}

bool NativeConversationHandoffBoundaryGateV1::reportBoundaryReady(
  const std::string& providerIdentity)
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_expectedProviders.find(providerIdentity) == m_expectedProviders.end())
    throw std::invalid_argument("handoff boundary report is from an unknown Provider");
  if (m_state == State::Cancelled)
    return false;
  if (m_state == State::CommitReleased)
    return true;
  if (std::chrono::steady_clock::now() >= m_deadline) {
    m_state = State::Cancelled;
    return false;
  }
  m_boundaryReadyProviders.insert(providerIdentity);
  return true;
}

bool NativeConversationHandoffBoundaryGateV1::cancel()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == State::Cancelled)
    return true;
  if (m_state == State::CommitReleased)
    return false;
  m_state = State::Cancelled;
  return true;
}

bool NativeConversationHandoffBoundaryGateV1::releaseCommit()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == State::CommitReleased)
    return true;
  if (m_state == State::Cancelled)
    return false;
  if (std::chrono::steady_clock::now() >= m_deadline) {
    m_state = State::Cancelled;
    return false;
  }
  if (m_boundaryReadyProviders.size() != m_expectedProviders.size())
    return false;
  m_state = State::CommitReleased;
  return true;
}

NativeConversationHandoffBoundaryGateV1::State
NativeConversationHandoffBoundaryGateV1::state() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state == State::Collecting &&
      std::chrono::steady_clock::now() >= m_deadline ? State::Cancelled : m_state;
}

NativeConversationHandoffRoleBoundaryGateV1::NativeConversationHandoffRoleBoundaryGateV1(
  const std::vector<std::string>& roles,
  std::chrono::steady_clock::time_point deadline)
  : m_deadline(deadline)
{
  if (roles.empty() || roles.size() > 64)
    throw std::invalid_argument("handoff role boundary gate requires 1 to 64 roles");
  if (deadline <= std::chrono::steady_clock::now())
    throw std::invalid_argument("handoff role boundary deadline has already expired");
  for (const auto& role : roles) {
    if (role.empty() || role.size() > 256 || role.find('\0') != std::string::npos ||
        !m_expectedRoles.insert(role).second)
      throw std::invalid_argument("handoff role boundary gate has an invalid or duplicate role");
  }
}

bool
NativeConversationHandoffRoleBoundaryGateV1::parkRoleAndWait(
  const std::string& role)
{
  std::unique_lock<std::mutex> lock(m_mutex);
  if (m_expectedRoles.find(role) == m_expectedRoles.end())
    throw std::invalid_argument("handoff role boundary is from an unknown role");
  if (m_state == State::Cancelled)
    return false;
  if (m_readyRoles.find(role) != m_readyRoles.end()) {
    const bool collecting = m_state == State::Collecting;
    if (collecting)
      m_state = State::Cancelled;
    lock.unlock();
    if (collecting)
      m_changed.notify_all();
    throw std::logic_error("handoff role reached its packet boundary more than once");
  }
  if (m_state == State::CommitReleased)
    return false;
  if (std::chrono::steady_clock::now() >= m_deadline) {
    m_state = State::Cancelled;
    lock.unlock();
    m_changed.notify_all();
    return false;
  }
  m_readyRoles.insert(role);
  if (!m_changed.wait_until(lock, m_deadline, [this] {
        return m_state != State::Collecting;
      })) {
    m_state = State::Cancelled;
    lock.unlock();
    m_changed.notify_all();
    return false;
  }
  return m_state == State::CommitReleased;
}

bool
NativeConversationHandoffRoleBoundaryGateV1::boundaryReady() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state == State::Collecting &&
    m_readyRoles.size() == m_expectedRoles.size() &&
    std::chrono::steady_clock::now() < m_deadline;
}

std::size_t
NativeConversationHandoffRoleBoundaryGateV1::readyRoleCount() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_readyRoles.size();
}

bool
NativeConversationHandoffRoleBoundaryGateV1::cancel()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == State::Cancelled)
    return true;
  if (m_state == State::CommitReleased)
    return false;
  m_state = State::Cancelled;
  m_changed.notify_all();
  return true;
}

bool
NativeConversationHandoffRoleBoundaryGateV1::releaseCommit()
{
  std::lock_guard<std::mutex> lock(m_mutex);
  if (m_state == State::CommitReleased)
    return true;
  if (m_state == State::Cancelled)
    return false;
  if (std::chrono::steady_clock::now() >= m_deadline) {
    m_state = State::Cancelled;
    m_changed.notify_all();
    return false;
  }
  if (m_readyRoles.size() != m_expectedRoles.size()) return false;
  m_state = State::CommitReleased;
  m_changed.notify_all();
  return true;
}

NativeConversationHandoffRoleBoundaryGateV1::State
NativeConversationHandoffRoleBoundaryGateV1::state() const
{
  std::lock_guard<std::mutex> lock(m_mutex);
  return m_state == State::Collecting &&
      std::chrono::steady_clock::now() >= m_deadline ? State::Cancelled : m_state;
}

} // namespace ndnsf::di

#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace ndnsf::di {

struct NativeConversationJournalKey
{
  std::string id;
  std::vector<std::uint8_t> bytes;
};

std::vector<std::vector<std::uint8_t>> nativeConversationAuthenticationKeys(
  const std::string& identity, const std::vector<NativeConversationJournalKey>& keys);
std::string nativeReadConversationEnvelope(
  const std::string& encoded, const std::string& envelopeId,
  const std::vector<NativeConversationJournalKey>& keys, std::uint64_t nowMs);
std::string nativeSealConversationEnvelope(
  const std::string& plaintext, const std::string& envelopeId,
  std::uint64_t expiresAtMs, const NativeConversationJournalKey& key);
std::string nativeConversationBase64Encode(const std::string& bytes);
std::string nativeConversationBase64Decode(const std::string& encoded);
// Conversation V1 follows the Python reference's ensure_ascii=False wire;
// this is intentionally separate from the framework canonical JSON contract.
std::string nativeConversationCanonicalJson(const NativeJson& value);

// Existing conversation.py V1 wires. These helpers own validation and
// authentication only; the coordinator owns turns and durable promotion.
std::string nativeConversationPrefixDigest(const std::vector<std::int64_t>& tokens);
std::string nativeSignConversationCheckpoint(
  NativeJson unsignedCheckpoint, const std::vector<std::uint8_t>& authenticationKey);
NativeJson nativeReadConversationCheckpoint(
  const std::string& wire,
  const std::vector<std::vector<std::uint8_t>>& verificationKeys,
  std::uint64_t nowMs);

// Call only for decrypted journal payloads and an authenticated checkpoint.
// Never expose transcript contents to observers or network edges.
void nativeValidateConversationTranscript(
  const NativeJson& transcript, const NativeJson& checkpoint,
  std::optional<std::size_t> nativeInitialPromptTokenCount = std::nullopt);

} // namespace ndnsf::di

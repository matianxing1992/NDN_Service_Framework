#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"
#include "ndn-service-framework/NDNSFMessages.hpp"
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/span.hpp>
#include <cstdint>

namespace ndnsf::di {

struct NativeProtectedKvCheckpointLimitsV1
{
  std::uint64_t maxPlaintextBytes = 0;
  std::uint64_t maxWireBytes = 0;
  std::function<bool()> cancelled;
};

// A byte commitment, not a storage grant, producer authentication or execution permission.
struct NativeProtectedKvCheckpointReferenceV1
{
  ndn::Name ownerIdentity;
  ndn::Name producerPrefix;
  ndn::Name ownerCertificateName;
  std::string ownerCertificateDigest;
  std::string checkpointIdentityDigest;
  std::string wireDigest;
  std::uint64_t wireBytes = 0;
  // Exact canonical ciphertext-key metadata from NDKVE001, never a plaintext key.
  // Empty identifies a legacy byte reference without owner-visible key metadata.
  std::string keyEnvelope;

  ndn::Name objectName() const;
};

// Expected authenticated invocation context, never an authority or persisted lease.
struct NativeProtectedKvRecipientBindingV1
{
  ndn::Name requesterIdentity, recipientIdentity, serviceName, requestId;
  ndn::Name recipientCertificateName;
  std::string authorizationBinding, planDigest, roleName, recipientPublicKeyDigest;
  std::uint64_t attemptEpoch = 1;
  std::uint64_t expiresAtMs = 0;
};

// Representation/binding validation only; grants and execution remain external.
void nativeValidateProtectedKvKeyEnvelope(const NativeProtectedKvCheckpointReferenceV1& reference);
// Representation/binding checks only, shared by native restore wire and crypto.
void nativeValidateProtectedKvRecipientCapsule(
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& binding);
// Check the visible AAD commitment as well; successful TPM/GCM is still required.
void nativeValidateProtectedKvRecipientCapsule(
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const NativeProtectedKvRecipientBindingV1& binding);

NativeProtectedKvCheckpointReferenceV1 nativeProtectPortableKvCheckpoint(
  const std::filesystem::path& privateInput,
  const std::filesystem::path& protectedOutput,
  const ndn::security::Certificate& ownerEncryptionCertificate,
  const ndn::Name& producerPrefix,
  const std::string& checkpointIdentityDigest,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options = {});

// Bounded ciphertext input for Core's existing vector publication API; no key is unwrapped.
ndn::Buffer nativeLoadProtectedPortableKvCheckpoint(
  const std::filesystem::path& protectedInput,
  const NativeProtectedKvCheckpointReferenceV1& expectedReference,
  const NativeProtectedKvCheckpointLimitsV1& limits);

// Caller validates the recovered portable representation and fresh authorization before import.
void nativeRecoverProtectedPortableKvCheckpoint(
  const std::filesystem::path& protectedInput,
  const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& expectedReference,
  const ndn::security::Certificate& ownerEncryptionCertificate,
  const ndn::security::KeyChain& ownerKeyChain,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeCheckpointExportOptions& options = {});

// Crypto only. Caller supplies current ACK/Selection bindings and serializes
// the borrowed KeyChain on its owner thread. No plaintext KV key is returned.
ndn_service_framework::RecipientEncryptedAssignment nativeWrapProtectedKvKeyForRecipient(
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn::security::Certificate& ownerCertificate,
  const ndn::security::KeyChain& ownerKeyChain,
  ndn::span<const std::uint8_t> recipientPublicKey,
  const NativeProtectedKvRecipientBindingV1& binding,
  const std::function<void()>& requireCurrent);

// Atomically recover unchanged ciphertext with only the recipient TPM. The
// mandatory guard checks real caller deadlines/leases; this API grants no execution.
using NativeProtectedKvRecipientConsumerV1 = std::function<bool(
  const ndn_service_framework::RecipientEncryptedAssignment&,
  const ndn::Name&, const std::string&, const ndn::Buffer&,
  const std::function<void(const ndn::Buffer&)>&)>;

// Trusted synchronous worker adapter, normally the current Core Context. No
// borrowed buffer may outlive consume; key ownership stays inside this helper.
void nativeRecoverProtectedPortableKvCheckpointForRecipient(
  const std::filesystem::path& protectedInput,
  const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const NativeProtectedKvRecipientBindingV1& binding,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const NativeProtectedKvRecipientConsumerV1& recipientConsumer,
  const std::function<void()>& requireCurrent,
  const NativeCheckpointExportOptions& options = {});

void nativeRecoverProtectedPortableKvCheckpointForRecipient(
  const std::filesystem::path& protectedInput,
  const std::filesystem::path& privateOutput,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn_service_framework::RecipientEncryptedAssignment& capsule,
  const ndn::security::Certificate& recipientCertificate,
  const ndn::security::KeyChain& recipientKeyChain,
  const NativeProtectedKvRecipientBindingV1& binding,
  const NativeProtectedKvCheckpointLimitsV1& limits,
  const std::function<void()>& requireCurrent,
  const NativeCheckpointExportOptions& options = {});

} // namespace ndnsf::di

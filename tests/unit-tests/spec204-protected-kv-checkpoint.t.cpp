#include "tests/unit-tests/spec204-protected-kv-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalArtifactPublisher.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/PrivateBufferWiper.hpp"
#include "ndn-service-framework/HybridMessageCrypto.hpp"
#include <ndn-cxx/security/signing-helpers.hpp>
#include <algorithm>
#include <atomic>
#include <future>
#include <thread>

using namespace ndnsf::di;
using namespace ndnsf::di::test::protectedKv;

namespace {
using Capsule = ndn_service_framework::RecipientEncryptedAssignment;

struct RecipientFixture : Fixture
{
  ndn::security::KeyChain recipientKeys{"pib-memory:kv-recipient", "tpm-memory:kv-recipient"};
  ndn::security::Certificate recipient;
  NativeProtectedKvCheckpointReferenceV1 reference;
  NativeProtectedKvRecipientBindingV1 binding;
  bool active = true;
  std::uint64_t now = 10;
  unsigned probes = 0;

  RecipientFixture()
  {
    recipient = recipientKeys.createIdentity(ndn::Name("/fresh/provider/b"), ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
    reference = protect();
    binding.requesterIdentity = certificate.getIdentity();
    binding.recipientIdentity = recipient.getIdentity();
    binding.recipientCertificateName = recipient.getName();
    binding.serviceName = ndn::Name("/SERVICE/infer");
    binding.requestId = ndn::Name("/opaque-capsule-request/42");
    binding.authorizationBinding = "lease42"; binding.planDigest = hash("sealed plan");
    binding.roleName = "decoder"; binding.attemptEpoch = 1; binding.expiresAtMs = 100;
    const auto key = recipient.getPublicKey();
    binding.recipientPublicKeyDigest = hash(std::string(reinterpret_cast<const char*>(key.data()), key.size()));
  }
  // This trusted fixture guard exercises enforcement, not Selection admission.
  void current() { ++probes; if (!active || now >= binding.expiresAtMs) throw std::runtime_error("stale recipient context"); }
  Capsule wrap()
  {
    return nativeWrapProtectedKvKeyForRecipient(reference, certificate, ownerKeys,
      recipient.getPublicKey(), binding, [this] { current(); });
  }
  void recoverRecipient(const Capsule& capsule, const NativeProtectedKvRecipientBindingV1& expected,
                        const NativeCheckpointExportOptions& options = {})
  {
    nativeRecoverProtectedPortableKvCheckpointForRecipient(cipher, restored, reference, capsule,
      recipient, recipientKeys, expected, limits, [this] { current(); }, options);
  }
  NativeProtectedKvRecipientConsumerV1 consumer()
  {
    return [this](const auto& capsule, const ndn::Name& certificateName,
      const std::string& publicDigest, const ndn::Buffer& associatedData, const auto& consume) {
      if (certificateName != recipient.getName() || publicDigest != binding.recipientPublicKeyDigest)
        throw std::invalid_argument("fixture recipient key binding");
      ndn::Buffer plaintext; detail::PrivateBufferWiper wipe(plaintext);
      if (!ndn_service_framework::decryptRecipientAssignment(capsule, recipient.getIdentity(),
            certificateName, recipientKeys, associatedData, plaintext)) return false;
      consume(plaintext); return true;
    };
  }
};
}

BOOST_AUTO_TEST_SUITE(Spec204ProtectedKvCheckpoint)

BOOST_AUTO_TEST_CASE(CiphertextLoaderChecksHeaderBoundsAndActualLoadedDigest)
{
  Fixture fixture; const auto reference = fixture.protect(); const auto wire = read(fixture.cipher);
  const auto loaded = nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, reference, fixture.limits);
  BOOST_CHECK_EQUAL(std::string(loaded.begin(), loaded.end()), wire);
  auto bounded = fixture.limits; bounded.maxWireBytes = reference.wireBytes - 1;
  BOOST_CHECK_THROW(nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, reference, bounded), std::invalid_argument);
  auto wrong = reference; wrong.ownerIdentity.append("other");
  BOOST_CHECK_THROW(nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, wrong, fixture.limits), std::invalid_argument);
  auto changed = wire; changed.back() ^= 1; write(fixture.cipher, changed);
  BOOST_CHECK_THROW(nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, reference, fixture.limits), std::invalid_argument);
  write(fixture.cipher, wire); bounded = fixture.limits; bounded.cancelled = [] { return true; };
  BOOST_CHECK_THROW(nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, reference, bounded), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(OwnerKeyEnvelopeMatchesHeaderAndRejectsSubstitution)
{
  Fixture fixture; const auto reference = fixture.protect(); const auto wire = read(fixture.cipher);
  BOOST_REQUIRE(!reference.keyEnvelope.empty());
  BOOST_CHECK_EQUAL(reference.keyEnvelope, wire.substr(16, metadataSize(wire)));
  BOOST_CHECK_NO_THROW(nativeValidateProtectedKvKeyEnvelope(reference));
  auto legacy = reference; legacy.keyEnvelope.clear();
  BOOST_CHECK_THROW(nativeValidateProtectedKvKeyEnvelope(legacy), std::invalid_argument);
  const auto loaded = nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, legacy, fixture.limits);
  BOOST_CHECK_EQUAL(std::string(loaded.begin(), loaded.end()), wire);
  fixture.recover(legacy); BOOST_CHECK_EQUAL(read(fixture.restored), read(fixture.plain));

  const auto other = fixture.protect(); write(fixture.cipher, wire); write(fixture.restored, "old");
  auto substituted = reference; substituted.keyEnvelope = other.keyEnvelope;
  // Representation validity alone cannot prove correspondence to this wire digest.
  BOOST_CHECK_NO_THROW(nativeValidateProtectedKvKeyEnvelope(substituted));
  BOOST_CHECK_THROW(nativeLoadProtectedPortableKvCheckpoint(fixture.cipher, substituted, fixture.limits), std::invalid_argument);
  BOOST_CHECK_THROW(fixture.recover(substituted), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  std::vector<std::string> malformed{std::string(16385, 'x'), " " + reference.keyEnvelope};
  for (const auto& edit : std::vector<std::function<void(NativeJson&)>>{
    [](auto& j){ j["owner"] = "/different-owner"; },
    [](auto& j){ j["producer"] = "/different-producer"; },
    [](auto& j){ j["ownerCertificateName"] = "/different-owner/KEY/key/issuer/v=1"; },
    [](auto& j){ j["ownerCertificateDigest"] = hash("different certificate"); },
    [](auto& j){ j["checkpointIdentityDigest"] = hash("different role"); },
    [](auto& j){ j["plaintextBytes"] = std::uint64_t(1); },
    [](auto& j){ j["wrappedKeyDigest"] = hash("different wrapped key"); },
    [](auto& j){ j["nonce"] = std::string(24, 'Z'); },
    [](auto& j){ j["extra"] = "unknown"; }, [](auto& j){ j["version"] = true; }}) {
    auto fields = nativeParseJson(reference.keyEnvelope); edit(fields);
    malformed.push_back(nativeCanonicalJson(fields));
  }
  for (const auto& metadata : malformed) {
    auto invalid = reference; invalid.keyEnvelope = metadata;
    BOOST_CHECK_THROW(nativeValidateProtectedKvKeyEnvelope(invalid), std::invalid_argument);
    BOOST_CHECK_THROW(fixture.recover(invalid), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  }
}

BOOST_AUTO_TEST_CASE(OwnerTpmRecoversCompletePortableStateWithoutOriginalProvider)
{
  Fixture fixture; const auto reference = fixture.protect();
  const auto plaintext = read(fixture.plain), wire = read(fixture.cipher);
  BOOST_REQUIRE(plaintext.size() > 2 * 65536); BOOST_CHECK_EQUAL(wire.substr(0, 8), "NDKVE001");
  BOOST_CHECK(wire.find(plaintext) == std::string::npos);
  BOOST_CHECK(wire.find(fixture.state.originIdentity.providerBootId) == std::string::npos);
  BOOST_CHECK_EQUAL(reference.wireBytes, wire.size()); BOOST_CHECK_EQUAL(reference.wireDigest, hash(wire));
  BOOST_CHECK_EQUAL(reference.objectName(), ndn::Name("/actual/producer/complete/a/kvcache/v1").append(reference.wireDigest.substr(7)));
  const auto fields = nativeParseJson(wire.substr(16, metadataSize(wire)));
  BOOST_CHECK_EQUAL(fields.size(), 12U); BOOST_CHECK_EQUAL(fields.at("algorithm").get<std::string>(), "AES-256-GCM");
  BOOST_CHECK_EQUAL(fields.at("plaintextBytes").get<std::uint64_t>(), plaintext.size());
  BOOST_CHECK_EQUAL(fields.at("plaintextDigest").get<std::string>(), hash(plaintext));
  // No provider instance, Face or request-scope key exists in this recovery.
  fixture.recover(reference); BOOST_CHECK_EQUAL(read(fixture.restored), plaintext);
  const auto recovered = nativeReadPrivatePortableKvCheckpoint(fixture.restored, fixture.portableLimits);
  BOOST_CHECK(recovered.originIdentity == fixture.state.originIdentity);
  BOOST_CHECK(recovered.components.front().tensorWire == fixture.state.components.front().tensorWire);
  for (const auto& path : {fixture.cipher, fixture.restored}) {
    struct stat info{}; BOOST_REQUIRE_EQUAL(::stat(path.c_str(), &info), 0); BOOST_CHECK_EQUAL(info.st_mode & 0777, 0600);
  }
}

BOOST_AUTO_TEST_CASE(FreshProtectionAndTrustedBindingsRejectBeforeOutputMutation)
{
  Fixture fixture; const auto first = fixture.protect(), second = fixture.protect();
  BOOST_CHECK(first.wireDigest != second.wireDigest); BOOST_CHECK(first.objectName() != second.objectName());
  write(fixture.restored, "prior private output");
  std::vector<NativeProtectedKvCheckpointReferenceV1> wrong;
  auto value = second; value.ownerIdentity.append("other"); wrong.push_back(value);
  value = second; value.producerPrefix.append("other"); wrong.push_back(value);
  value = second; value.checkpointIdentityDigest = hash("other checkpoint"); wrong.push_back(value);
  value = second; value.ownerCertificateDigest = hash("other certificate"); wrong.push_back(value);
  value = second; value.ownerCertificateName.append("other"); wrong.push_back(value);
  value = second; ++value.wireBytes; wrong.push_back(value);
  value = second; value.wireDigest = hash("other wire"); wrong.push_back(value);
  for (const auto& reference : wrong) {
    BOOST_CHECK_THROW(fixture.recover(reference), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior private output");
  }
  ndn::security::KeyChain missingKey{"pib-memory:protected-kv-missing", "tpm-memory:protected-kv-missing"};
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpoint(fixture.cipher, fixture.restored,
    second, fixture.certificate, missingKey, fixture.limits), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior private output");
}

BOOST_AUTO_TEST_CASE(RecommittedTamperedHeaderCiphertextAndTagFailAuthentication)
{
  Fixture fixture; const auto original = fixture.protect(); const auto wire = read(fixture.cipher);
  write(fixture.restored, "old");
  std::vector<std::string> corrupted;
  auto changed = wire; changed.at(16 + metadataSize(wire) + 2) ^= 1; corrupted.push_back(changed);
  changed = wire; changed.back() ^= 1; corrupted.push_back(changed);
  auto fields = nativeParseJson(wire.substr(16, metadataSize(wire)));
  fields["plaintextDigest"] = hash("changed plaintext"); corrupted.push_back(replaceMetadata(wire, nativeCanonicalJson(fields)));
  fields = nativeParseJson(wire.substr(16, metadataSize(wire))); fields["nonce"] = std::string(24, '0');
  corrupted.push_back(replaceMetadata(wire, nativeCanonicalJson(fields)));
  for (const auto& bytes : corrupted) {
    auto reference = original; commitMutatedWire(fixture, reference, bytes);
    // Legacy byte-only references still exercise GCM authentication, independently
    // of the new owner-visible metadata pin checked by the envelope case above.
    reference.keyEnvelope.clear();
    BOOST_CHECK_THROW(fixture.recover(reference), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  }
}

BOOST_AUTO_TEST_CASE(MalformedBoundedFramingAndMetadataReject)
{
  Fixture fixture; const auto original = fixture.protect(); const auto wire = read(fixture.cipher);
  write(fixture.restored, "old"); std::vector<std::string> malformed;
  auto changed = wire; changed[0] = 'X'; malformed.push_back(changed);
  malformed.push_back(wire.substr(0, wire.size() - 1)); malformed.push_back(wire + "x");
  changed = wire; for (unsigned i = 8; i < 16; ++i) changed[i] = static_cast<char>(255); malformed.push_back(changed);
  const auto metadata = wire.substr(16, metadataSize(wire));
  malformed.push_back(replaceMetadata(wire, "{\"version\":1," + metadata.substr(1)));
  malformed.push_back(replaceMetadata(wire, " " + metadata));
  for (const auto& edit : std::vector<std::function<void(NativeJson&)>>{
    [](auto& j){ j["unknown"] = 1; }, [](auto& j){ j.erase("nonce"); },
    [](auto& j){ j["version"] = true; }, [](auto& j){ j["plaintextBytes"] = -1; },
    [](auto& j){ j["plaintextBytes"] = 1.5; }, [](auto& j){ j["plaintextBytes"] = std::uint64_t(1000000001); },
    [](auto& j){ j["wrappedKey"] = "aa"; }, [](auto& j){ j["nonce"] = std::string(24, 'Z'); },
    [](auto& j){ j["nonce"] = NativeJson::array({NativeJson::array({NativeJson::array({1})})}); }}) {
    auto fields = nativeParseJson(metadata); edit(fields); malformed.push_back(replaceMetadata(wire, nativeCanonicalJson(fields)));
  }
  for (const auto& bytes : malformed) {
    auto reference = original; commitMutatedWire(fixture, reference, bytes);
    reference.keyEnvelope.clear();
    BOOST_CHECK_THROW(fixture.recover(reference), std::invalid_argument); BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  }
}

BOOST_AUTO_TEST_CASE(PrivateFdAndResourceBoundsReject)
{
  Fixture fixture; const auto reference = fixture.protect(); const auto original = read(fixture.cipher);
  auto bad = fixture.limits; bad.maxPlaintextBytes = 0;
  BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(fixture.plain, fixture.cipher, fixture.certificate,
    reference.producerPrefix, reference.checkpointIdentityDigest, bad), std::invalid_argument);
  bad = fixture.limits; bad.maxWireBytes = 16;
  BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(fixture.plain, fixture.cipher, fixture.certificate,
    reference.producerPrefix, reference.checkpointIdentityDigest, bad), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.cipher), original);
  BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(fixture.plain, fixture.cipher, fixture.certificate,
    ndn::Name(), reference.checkpointIdentityDigest, fixture.limits), std::invalid_argument);
  BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(fixture.plain, fixture.cipher, fixture.certificate,
    ndn::Name("/actual/producer").append(std::string(4000, 'a')), reference.checkpointIdentityDigest,
    fixture.limits), std::invalid_argument);
  BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(fixture.plain, fixture.plain, fixture.certificate,
    reference.producerPrefix, reference.checkpointIdentityDigest, fixture.limits), std::invalid_argument);
  const auto fifo = fixture.directory / "fifo", link = fixture.directory / "symlink", hard = fixture.directory / "hardlink";
  BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0); std::filesystem::create_symlink(fixture.plain, link);
  for (const auto& path : {fifo, link}) BOOST_CHECK_THROW(nativeProtectPortableKvCheckpoint(path,
    fixture.cipher, fixture.certificate, reference.producerPrefix, reference.checkpointIdentityDigest, fixture.limits), std::invalid_argument);
  std::filesystem::create_hard_link(fixture.plain, hard); BOOST_CHECK_THROW(fixture.protect(), std::invalid_argument);
  std::filesystem::remove(hard); BOOST_REQUIRE_EQUAL(::chmod(fixture.plain.c_str(), 0644), 0);
  BOOST_CHECK_THROW(fixture.protect(), std::invalid_argument); BOOST_REQUIRE_EQUAL(::chmod(fixture.plain.c_str(), 0600), 0);
  BOOST_CHECK_EQUAL(read(fixture.cipher), original);
}

BOOST_AUTO_TEST_CASE(AtomicFaultsAndMidStreamCancellationKeepPriorOutputs)
{
  Fixture fixture; const auto reference = fixture.protect(); const auto wire = read(fixture.cipher);
  write(fixture.restored, "old");
  for (const auto stage : {NativeCheckpointExportStage::BeforeWrite, NativeCheckpointExportStage::BeforeFileSync,
      NativeCheckpointExportStage::BeforeRename, NativeCheckpointExportStage::BeforeDirectorySync}) {
    NativeCheckpointExportOptions options; options.beforeStage = [stage](auto current) {
      if (current == stage) throw std::runtime_error("controlled atomic fault");
    };
    BOOST_CHECK_THROW(fixture.protect(options), std::runtime_error); BOOST_CHECK_EQUAL(read(fixture.cipher), wire);
    BOOST_CHECK_THROW(fixture.recover(reference, options), std::runtime_error); BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  }
  bool entered = false; unsigned probes = 0;
  NativeCheckpointExportOptions options; options.beforeStage = [&](auto stage) { if (stage == NativeCheckpointExportStage::BeforeWrite) entered = true; };
  fixture.limits.cancelled = [&] { return entered && ++probes >= 3; };
  BOOST_CHECK_THROW(fixture.protect(options), std::runtime_error); BOOST_CHECK_EQUAL(read(fixture.cipher), wire);
  entered = false; probes = 0;
  BOOST_CHECK_THROW(fixture.recover(reference, options), std::runtime_error); BOOST_CHECK_EQUAL(read(fixture.restored), "old");
  BOOST_CHECK_EQUAL(std::distance(std::filesystem::directory_iterator(fixture.directory), {}), 3);
}

BOOST_AUTO_TEST_CASE(PinnedInputChangesBetweenHashAndCryptoReject)
{
  Fixture fixture; const auto reference = fixture.protect(); const auto plain = read(fixture.plain), wire = read(fixture.cipher);
  write(fixture.restored, "old");
  for (const auto change : {0, 1, 2}) {
    write(fixture.plain, plain);
    NativeCheckpointExportOptions options; options.beforeStage = [&](auto stage) {
      if (stage != NativeCheckpointExportStage::BeforeWrite) return;
      auto bytes = plain; if (change == 0) bytes.back() ^= 1; else if (change == 1) bytes.pop_back(); else bytes += 'x';
      write(fixture.plain, bytes);
    };
    BOOST_CHECK_THROW(fixture.protect(options), std::invalid_argument); BOOST_CHECK_EQUAL(read(fixture.cipher), wire);
  }
  NativeCheckpointExportOptions options; options.beforeStage = [&](auto stage) {
    if (stage == NativeCheckpointExportStage::BeforeWrite) { auto bytes = wire; bytes.back() ^= 1; write(fixture.cipher, bytes); }
  };
  BOOST_CHECK_THROW(fixture.recover(reference, options), std::invalid_argument); BOOST_CHECK_EQUAL(read(fixture.restored), "old");
}

BOOST_AUTO_TEST_CASE(FreshRecipientTpmRecoversOriginalCompletePortableState)
{
  RecipientFixture fixture; const auto wire = read(fixture.cipher);
  const auto capsule = fixture.wrap(); Capsule decoded;
  BOOST_REQUIRE(decoded.WireDecode(capsule.WireEncode()));
  write(fixture.restored, "prior");
  // B's TPM has no owner private key, even when given the owner's public certificate.
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpoint(fixture.cipher, fixture.restored,
    fixture.reference, fixture.certificate, fixture.recipientKeys, fixture.limits), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  fixture.recoverRecipient(decoded, fixture.binding);
  BOOST_CHECK_EQUAL(read(fixture.restored), read(fixture.plain));
  const auto state = nativeReadPrivatePortableKvCheckpoint(fixture.restored, fixture.portableLimits);
  BOOST_CHECK(state.originIdentity == fixture.state.originIdentity);
  BOOST_REQUIRE_EQUAL(state.components.size(), fixture.state.components.size());
  BOOST_CHECK(state.components.front().tensorWire == fixture.state.components.front().tensorWire);
  BOOST_CHECK_EQUAL(read(fixture.cipher), wire);
  BOOST_CHECK_EQUAL(fixture.reference.wireDigest, hash(wire));
  BOOST_CHECK_EQUAL(fixture.reference.producerPrefix, ndn::Name("/actual/producer/complete/a"));
  BOOST_CHECK_GT(fixture.probes, 3U);
}

BOOST_AUTO_TEST_CASE(RecipientConsumerRecoversCompletePortableState)
{
  RecipientFixture fixture; const auto capsule = fixture.wrap();
  const auto wire = read(fixture.cipher);
  nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher, fixture.restored,
    fixture.reference, capsule, fixture.binding, fixture.limits, fixture.consumer(),
    [&] { fixture.current(); });
  BOOST_CHECK_EQUAL(read(fixture.restored), read(fixture.plain));
  const auto recovered = nativeReadPrivatePortableKvCheckpoint(fixture.restored, fixture.portableLimits);
  BOOST_CHECK(recovered.originIdentity == fixture.state.originIdentity);
  BOOST_CHECK_EQUAL(recovered.stateIoDigest, fixture.state.stateIoDigest);
  BOOST_REQUIRE_EQUAL(recovered.components.size(), fixture.state.components.size());
  for (std::size_t i = 0; i < recovered.components.size(); ++i)
    BOOST_CHECK(recovered.components[i].tensorWire == fixture.state.components[i].tensorWire);
  BOOST_CHECK_EQUAL(read(fixture.cipher), wire);
}

BOOST_AUTO_TEST_CASE(RecipientConsumerRejectsBrokenAdaptersBeforePrivateWriter)
{
  RecipientFixture fixture; const auto capsule = fixture.wrap();
  write(fixture.restored, "prior");
  std::size_t writerStages = 0;
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](NativeCheckpointExportStage) { ++writerStages; };
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher,
    fixture.restored, fixture.reference, capsule, fixture.binding, fixture.limits,
    NativeProtectedKvRecipientConsumerV1{}, [&] { fixture.current(); }, options), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  BOOST_CHECK_EQUAL(writerStages, 0U);
  for (unsigned mode = 0; mode != 7; ++mode) {
    fixture.active = true;
    writerStages = 0;
    std::atomic<bool> wrongThreadRejected{false};
    NativeProtectedKvRecipientConsumerV1 broken = [&](const auto&, const auto&,
      const auto&, const auto&, const auto& consume) {
      const ndn::Buffer bytes(mode == 1 ? 31 : 32, 0x55);
      if (mode == 0) return true;
      if (mode == 5) throw std::runtime_error("adapter failure");
      if (mode == 4) {
        std::thread worker([&] {
          try { consume(bytes); }
          catch (const std::invalid_argument&) { wrongThreadRejected.store(true); }
        });
        worker.join(); return true;
      }
      consume(bytes);
      if (mode == 3) {
        try { consume(bytes); } catch (const std::invalid_argument&) {}
      }
      if (mode == 6) fixture.active = false;
      return mode != 2;
    };
    BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher,
      fixture.restored, fixture.reference, capsule, fixture.binding, fixture.limits, broken,
      [&] { fixture.current(); }, options), std::exception);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
    BOOST_CHECK_EQUAL(writerStages, 0U);
    if (mode == 4) BOOST_CHECK(wrongThreadRejected.load());
  }
}

BOOST_AUTO_TEST_CASE(RecipientCapsuleBindsEveryExpectedContextField)
{
  RecipientFixture fixture; const auto capsule = fixture.wrap(); write(fixture.restored, "prior");
  for (const auto& edit : std::vector<std::function<void(NativeProtectedKvRecipientBindingV1&)>>{
    [](auto& b) { b.requesterIdentity.append("other"); },
    [](auto& b) { b.recipientIdentity.append("other"); },
    [](auto& b) { b.serviceName.append("other"); },
    [](auto& b) { b.requestId.append("other"); },
    [](auto& b) { b.authorizationBinding += "other"; },
    [](auto& b) { b.planDigest = hash("other plan"); },
    [](auto& b) { b.roleName = "other"; },
    [](auto& b) { b.attemptEpoch = 2; },
    [](auto& b) { ++b.expiresAtMs; },
    [](auto& b) { b.recipientCertificateName.append("other"); },
    [](auto& b) { b.recipientPublicKeyDigest = hash("other key"); }}) {
    auto expected = fixture.binding; edit(expected);
    BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, expected), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  }
  // Another valid certificate of the very same RSA key must not substitute.
  auto alternate = fixture.recipient;
  alternate.setName(ndn::Name(fixture.recipient.getKeyName()).append("another-issuer").appendVersion(2));
  fixture.recipientKeys.sign(alternate, ndn::security::signingByIdentity(fixture.recipient.getIdentity()));
  auto expected = fixture.binding; expected.recipientCertificateName = alternate.getName();
  auto changed = capsule;
  changed.setField("recipientCertName", alternate.getName().toUri(ndn::name::UriFormat::CANONICAL));
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher, fixture.restored,
    fixture.reference, changed, alternate, fixture.recipientKeys, expected, fixture.limits,
    [&] { fixture.current(); }), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
}

BOOST_AUTO_TEST_CASE(RecipientCapsuleRejectsMalformedOrReboundMaterial)
{
  RecipientFixture fixture; const auto capsule = fixture.wrap(); write(fixture.restored, "prior");
  for (const auto& edit : std::vector<std::function<void(Capsule&)>>{
    [](auto& c) { c.setVersion(2); },
    [](auto& c) { c.setField("schemaVersion", "2"); },
    [](auto& c) { c.setField("algorithm", "other"); },
    [](auto& c) { c.setField("unknown", "field"); },
    [](auto& c) { c.setField("wrappedAssignmentKey", std::string(2050, '0')); },
    [](auto& c) { c.setField("nonce", std::string(24, 'Z')); },
    [](auto& c) { c.setField("ciphertext", "00"); },
    [](auto& c) { auto s = c.getField("authTag"); s[0] = s[0] == '0' ? '1' : '0'; c.setField("authTag", s); },
    [](auto& c) { c.setField("assignmentDigest", std::string(64, '0')); }}) {
    auto changed = capsule; edit(changed);
    BOOST_CHECK_THROW(fixture.recoverRecipient(changed, fixture.binding), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  }
  Capsule missing;
  for (const auto& [key, value] : capsule.getFields()) if (key != "authTag") missing.setField(key, value);
  BOOST_CHECK_THROW(fixture.recoverRecipient(missing, fixture.binding), std::invalid_argument);
  ndn::security::KeyChain wrongKeys{"pib-memory:kv-wrong", "tpm-memory:kv-wrong"};
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher, fixture.restored,
    fixture.reference, capsule, fixture.recipient, wrongKeys, fixture.binding, fixture.limits,
    [&] { fixture.current(); }), std::invalid_argument);
  const auto original = fixture.reference;
  for (int variant = 0; variant < 3; ++variant) {
    fixture.reference = original;
    if (variant == 0) fixture.reference.keyEnvelope.clear();
    if (variant == 1) fixture.reference.wireDigest = hash("different wire");
    if (variant == 2) fixture.reference.keyEnvelope = fixture.protect().keyEnvelope;
    BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, fixture.binding), std::invalid_argument);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  }
  BOOST_CHECK_THROW(nativeWrapProtectedKvKeyForRecipient(original, fixture.certificate, fixture.ownerKeys,
    fixture.recipient.getPublicKey(), fixture.binding, {}), std::invalid_argument);
  BOOST_CHECK_THROW(nativeRecoverProtectedPortableKvCheckpointForRecipient(fixture.cipher, fixture.restored,
    original, capsule, fixture.recipient, fixture.recipientKeys, fixture.binding, fixture.limits, {}), std::invalid_argument);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
}

BOOST_AUTO_TEST_CASE(RecipientCurrentGuardAndAtomicCancellationRetainPriorOutput)
{
  RecipientFixture fixture; const auto capsule = fixture.wrap(); write(fixture.restored, "prior");
  fixture.active = false;
  BOOST_CHECK_THROW(fixture.wrap(), std::runtime_error);
  BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, fixture.binding), std::runtime_error);
  fixture.active = true; fixture.now = fixture.binding.expiresAtMs;
  BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, fixture.binding), std::runtime_error);
  fixture.now = 10;
  for (const auto target : {NativeCheckpointExportStage::BeforeWrite, NativeCheckpointExportStage::BeforeFileSync,
       NativeCheckpointExportStage::BeforeRename, NativeCheckpointExportStage::BeforeDirectorySync}) {
    NativeCheckpointExportOptions options;
    options.beforeStage = [&](auto stage) { if (stage == target) fixture.active = false; };
    BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, fixture.binding, options), std::runtime_error);
    BOOST_CHECK_EQUAL(read(fixture.restored), "prior"); fixture.active = true;
  }
  bool entered = false; unsigned chunks = 0;
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](auto stage) { if (stage == NativeCheckpointExportStage::BeforeWrite) entered = true; };
  fixture.limits.cancelled = [&] { return entered && ++chunks >= 3; };
  BOOST_CHECK_THROW(fixture.recoverRecipient(capsule, fixture.binding, options), std::runtime_error);
  BOOST_CHECK_EQUAL(read(fixture.restored), "prior");
  BOOST_CHECK_EQUAL(std::distance(std::filesystem::directory_iterator(fixture.directory), {}), 3);
}

BOOST_AUTO_TEST_CASE(PrivateKeyBufferWipesSuccessfulClearedAndDigestRejectedOutputs)
{
  RecipientFixture fixture; const ndn::Buffer secret(32, 0x5a), aad{1, 2, 3};
  const auto capsule = ndn_service_framework::encryptRecipientAssignment(secret, fixture.recipient.getPublicKey(),
    fixture.recipient.getIdentity(), fixture.recipient.getName(), aad);
  for (int variant = 0; variant < 3; ++variant) {
    auto changed = capsule;
    if (variant == 1) { auto s = changed.getField("authTag"); s[0] = s[0] == '0' ? '1' : '0'; changed.setField("authTag", s); }
    if (variant == 2) changed.setField("assignmentDigest", std::string(64, '0'));
    ndn::Buffer output;
    {
      detail::PrivateBufferWiper wipe(output);
      const bool accepted = ndn_service_framework::decryptRecipientAssignment(changed,
        fixture.recipient.getIdentity(), fixture.recipient.getName(), fixture.recipientKeys, aad, output);
      BOOST_CHECK_EQUAL(accepted, variant == 0);
      BOOST_CHECK_EQUAL(output.size(), variant == 1 ? 0U : 32U);
      BOOST_REQUIRE_GE(output.capacity(), 32U);
      if (variant != 1) BOOST_CHECK(output == secret);
    }
    BOOST_CHECK_EQUAL(output.size(), output.capacity());
    BOOST_CHECK(std::all_of(output.begin(), output.end(), [](auto byte) { return byte == 0; }));
  }
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierReleasesAllRolesTogether)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto deadline = Barrier::Clock::now() + std::chrono::seconds(5);
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}}, deadline);

  auto encoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });
  auto decoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });

  const auto challenges = barrier->waitForAllChallenges();
  BOOST_REQUIRE_EQUAL(challenges.size(), 2U);
  BOOST_CHECK_EQUAL(challenges[0].roleName, "decoder");
  BOOST_CHECK_EQUAL(challenges[0].manifestName.toUri(), "/provider/b/kv/manifest/1");
  BOOST_CHECK_EQUAL(challenges[1].roleName, "encoder");
  BOOST_CHECK_EQUAL(challenges[1].manifestName.toUri(), "/provider/a/kv/manifest/1");
  BOOST_CHECK(encoder.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
  BOOST_CHECK(decoder.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);

  Barrier::GrantSet grants;
  for (const auto& challenge : challenges) {
    const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
    grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
      "/repo", "/owner", grantId, challenge.writerIdentity,
      challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
  }
  barrier->releaseAll(std::move(grants));

  BOOST_REQUIRE(encoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_REQUIRE(decoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_EQUAL(encoder.get().grantId, "grant-encoder");
  BOOST_CHECK_EQUAL(decoder.get().grantId, "grant-decoder");
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsPartialAndReboundGrantSets)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto makeBarrier = [] {
    return std::make_shared<Barrier>("/owner",
      Barrier::ProducerPrefixesByRole{
        {"encoder", ndn::Name("/provider/a")},
        {"decoder", ndn::Name("/provider/b")}},
      Barrier::Clock::now() + std::chrono::seconds(5));
  };
  const auto startPublishers = [](const std::shared_ptr<Barrier>& barrier) {
    auto encoder = std::async(std::launch::async, [barrier] {
      return barrier->resolverFor("encoder")(
        ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
    });
    auto decoder = std::async(std::launch::async, [barrier] {
      return barrier->resolverFor("decoder")(
        ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
    });
    return std::make_pair(std::move(encoder), std::move(decoder));
  };
  const auto completeGrants = [](const std::vector<NativeDurableStorageRoleGrantChallengeV1>& challenges) {
    Barrier::GrantSet grants;
    for (const auto& challenge : challenges) {
      const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
      grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
        "/repo", "/owner", grantId, challenge.writerIdentity,
        challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
    }
    return grants;
  };

  for (int mode = 0; mode < 2; ++mode) {
    auto barrier = makeBarrier();
    auto publishers = startPublishers(barrier);
    const auto challenges = barrier->waitForAllChallenges();
    auto grants = completeGrants(challenges);
    if (mode == 0) {
      grants.erase("decoder");
    }
    else {
      grants.at("encoder").objectName = "/provider/a/kv/manifest/rebound";
    }
    BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
    BOOST_REQUIRE(publishers.first.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    BOOST_REQUIRE(publishers.second.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    BOOST_CHECK_THROW(publishers.first.get(), std::runtime_error);
    BOOST_CHECK_THROW(publishers.second.get(), std::runtime_error);
  }
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsWrongProducerAndAbortsOtherRoles)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::seconds(5));
  auto wrong = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  BOOST_CHECK_THROW(wrong.get(), std::runtime_error);
  auto other = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  BOOST_CHECK_THROW(other.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierDeadlineAbortsIncompleteRoleSet)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::milliseconds(100));
  auto publisher = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });

  BOOST_CHECK_THROW(barrier->waitForAllChallenges(), std::runtime_error);
  BOOST_REQUIRE(publisher.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(publisher.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsSwappedRoleGrants)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{
      {"encoder", ndn::Name("/provider/a")},
      {"decoder", ndn::Name("/provider/b")}},
    Barrier::Clock::now() + std::chrono::seconds(5));
  auto encoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", [] {});
  });
  auto decoder = std::async(std::launch::async, [barrier] {
    return barrier->resolverFor("decoder")(
      ndn::Name("/provider/b/kv/manifest/1"), "/provider/b", [] {});
  });
  const auto challenges = barrier->waitForAllChallenges();
  Barrier::GrantSet grants;
  for (const auto& challenge : challenges) {
    const auto grantId = challenge.roleName == "encoder" ? "grant-encoder" : "grant-decoder";
    grants.emplace(challenge.roleName, NativeDurableStorageRoleGrantV1{
      "/repo", "/owner", grantId, challenge.writerIdentity,
      challenge.manifestName.toUri(ndn::name::UriFormat::CANONICAL), 1});
  }
  std::swap(grants.at("encoder"), grants.at("decoder"));
  BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
  BOOST_REQUIRE(encoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_REQUIRE(decoder.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(encoder.get(), std::runtime_error);
  BOOST_CHECK_THROW(decoder.get(), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(DurableGrantBarrierRejectsGrantSetReleasedAfterDeadline)
{
  using Barrier = NativeDurableStorageGrantBarrierV1;
  const auto deadline = Barrier::Clock::now() + std::chrono::seconds(1);
  auto barrier = std::make_shared<Barrier>("/owner",
    Barrier::ProducerPrefixesByRole{{"encoder", ndn::Name("/provider/a")}}, deadline);
  std::promise<void> callbackEntered;
  std::promise<void> resumeCallback;
  auto resume = resumeCallback.get_future().share();
  auto callbackCount = std::make_shared<unsigned>(0);
  auto current = [callbackCount, &callbackEntered, resume] {
    if ((*callbackCount)++ == 1) {
      callbackEntered.set_value();
      resume.wait();
    }
  };
  auto publisher = std::async(std::launch::async, [barrier, current] {
    return barrier->resolverFor("encoder")(
      ndn::Name("/provider/a/kv/manifest/1"), "/provider/a", current);
  });
  const auto challenges = barrier->waitForAllChallenges();
  BOOST_REQUIRE_EQUAL(challenges.size(), 1U);
  auto callbackEnteredFuture = callbackEntered.get_future();
  BOOST_REQUIRE(callbackEnteredFuture.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  std::this_thread::sleep_until(deadline + std::chrono::milliseconds(5));
  Barrier::GrantSet grants{{"encoder", NativeDurableStorageRoleGrantV1{
    "/repo", "/owner", "grant-encoder", "/provider/a",
    "/provider/a/kv/manifest/1", 1}}};
  BOOST_CHECK_THROW(barrier->releaseAll(std::move(grants)), std::runtime_error);
  resumeCallback.set_value();
  BOOST_REQUIRE(publisher.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
  BOOST_CHECK_THROW(publisher.get(), std::runtime_error);
}

BOOST_AUTO_TEST_SUITE_END()

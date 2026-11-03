#include "tests/unit-tests/native-protected-kv-fixture.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>

namespace ndnsf::di::test {
namespace {

struct PacketFixture : protectedKv::Fixture
{
  NativeProtectedKvCheckpointReferenceV1 reference = protect();
  ndn::security::Certificate source = ownerKeys.createIdentity(reference.producerPrefix,
    ndn::RsaKeyParams(2048)).getDefaultKey().getDefaultCertificate();
  NativeProtectedKvPacketLimits packetLimits{limits, 1000000, 128};
  bool cancelled = false;

  void current() const
  {
    if (cancelled) throw std::runtime_error("packet fixture cancelled");
  }

  NativeProtectedKvPacketSet prepare()
  {
    return nativePrepareProtectedKvPackets(cipher, reference, source, packetLimits,
      [&](ndn::Data& data) { ownerKeys.sign(data, ndn::security::signingByCertificate(source)); },
      [&] { current(); });
  }

  bool verify(const ndn::Buffer& wire, const ndn::Name& producer)
  {
    current();
    const ndn::Data data{ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size()))};
    if (ndn::security::Certificate::isValidName(data.getName())) {
      const auto trusted = source.wireEncode();
      return producer == source.getIdentity() &&
        wire == ndn::Buffer(trusted.begin(), trusted.end());
    }
    return producer == source.getIdentity() && producer.isPrefixOf(data.getName()) &&
      ndn::security::verifySignature(data, source);
  }

  void materialize(const NativeProtectedKvPacketSet& packets)
  {
    nativeMaterializeProtectedKvPackets(packets.packetWires, reference, packets.rootName,
      cipher, packetLimits, [&](const auto& wire, const auto& producer) {
        return verify(wire, producer);
      }, [&] { current(); });
  }
};

} // namespace

BOOST_AUTO_TEST_SUITE(Spec221ProtectedKvPackets)

BOOST_AUTO_TEST_CASE(SignedCiphertextPacketsRecoverWithoutOriginalFiles)
{
  PacketFixture fixture;
  const auto expectedPlain = protectedKv::read(fixture.plain);
  const auto expectedCipher = protectedKv::read(fixture.cipher);
  const auto packets = fixture.prepare();
  BOOST_REQUIRE(packets.packetWires.size() > 3);
  BOOST_CHECK(packets.rootName == nativeProtectedKvPacketRootName(fixture.reference, fixture.source));
  BOOST_CHECK(packets.packetNames.front() == packets.rootName);
  BOOST_CHECK(packets.packetNames.back() == fixture.source.getName());
  ndn::Buffer aggregate;
  for (size_t i = 0; i < packets.packetWires.size(); ++i) {
    BOOST_CHECK(fixture.reference.producerPrefix.isPrefixOf(packets.packetNames[i]));
    aggregate.insert(aggregate.end(), packets.packetWires[i].begin(), packets.packetWires[i].end());
  }
  BOOST_CHECK_EQUAL(aggregate.size(), packets.wireBytes);
  BOOST_CHECK_EQUAL(protectedKv::hash({aggregate.begin(), aggregate.end()}), packets.wireDigest);
  std::filesystem::remove(fixture.plain);
  std::filesystem::remove(fixture.cipher);
  fixture.materialize(packets);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), expectedCipher);
  fixture.recover(fixture.reference);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.restored), expectedPlain);
  struct stat status{};
  BOOST_REQUIRE_EQUAL(::stat(fixture.cipher.c_str(), &status), 0);
  BOOST_CHECK_EQUAL(status.st_mode & 0777, 0600);
}

BOOST_AUTO_TEST_CASE(TamperOrderMissingAndTrustFailuresPreservePriorOutput)
{
  PacketFixture fixture;
  const auto packets = fixture.prepare();
  const auto prior = protectedKv::read(fixture.cipher);
  auto modified = packets;
  modified.packetWires[0].back() ^= 1;
  BOOST_CHECK_THROW(fixture.materialize(modified), std::invalid_argument);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
  modified = packets;
  std::swap(modified.packetWires[1], modified.packetWires[2]);
  BOOST_CHECK_THROW(fixture.materialize(modified), std::invalid_argument);
  modified = packets;
  modified.packetWires.pop_back();
  BOOST_CHECK_THROW(fixture.materialize(modified), std::invalid_argument);
  modified = packets;
  modified.packetWires[1].push_back(0);
  BOOST_CHECK_THROW(fixture.materialize(modified), std::exception);
  BOOST_CHECK_THROW(nativeMaterializeProtectedKvPackets(packets.packetWires, fixture.reference,
    packets.rootName, fixture.cipher, fixture.packetLimits,
    [](const auto&, const auto&) { return false; }, [] {}), std::invalid_argument);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
}

BOOST_AUTO_TEST_CASE(CertificateRotationChangesPublicationNames)
{
  PacketFixture fixture;
  const auto first = nativeProtectedKvPacketRootName(fixture.reference, fixture.source);
  ndn::security::MakeCertificateOptions options;
  options.version = 1234;
  const auto rotated = fixture.ownerKeys.makeCertificate(fixture.source,
    ndn::security::signingByCertificate(fixture.source), options);
  BOOST_CHECK(first != nativeProtectedKvPacketRootName(fixture.reference, rotated));
  BOOST_CHECK_THROW(nativeProtectedKvPacketRootName(fixture.reference, fixture.certificate),
    std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(AnotherTrustedKeyCannotReplaceTheCommittedSigningCertificate)
{
  PacketFixture fixture;
  const auto packets = fixture.prepare();
  const auto prior = protectedKv::read(fixture.cipher);
  const auto identity = fixture.ownerKeys.getPib().getIdentity(fixture.source.getIdentity());
  const auto other = fixture.ownerKeys.createKey(identity, ndn::RsaKeyParams(2048))
    .getDefaultCertificate();
  for (const size_t index : {size_t{0}, size_t{1}}) {
    auto modified = packets;
    ndn::Data replacement{ndn::Block(ndn::span<const uint8_t>(
      modified.packetWires[index].data(), modified.packetWires[index].size()))};
    fixture.ownerKeys.sign(replacement, ndn::security::signingByCertificate(other));
    const auto wire = replacement.wireEncode();
    modified.packetWires[index].assign(wire.begin(), wire.end());
    // Both keys may be trusted for this Producer. The descriptor still commits
    // one certificate, which must sign every application packet in this set.
    BOOST_CHECK_THROW(nativeMaterializeProtectedKvPackets(modified.packetWires,
      fixture.reference, packets.rootName, fixture.cipher, fixture.packetLimits,
      [&](const ndn::Buffer& candidate, const ndn::Name& producer) {
        if (fixture.verify(candidate, producer)) return true;
        const ndn::Data data{ndn::Block(ndn::span<const uint8_t>(
          candidate.data(), candidate.size()))};
        return producer == other.getIdentity() && producer.isPrefixOf(data.getName()) &&
          ndn::security::verifySignature(data, other);
      }, [] {}), std::invalid_argument);
    BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
  }
}

BOOST_AUTO_TEST_CASE(BudgetsCancellationAndSignerMutationFailBeforePublication)
{
  PacketFixture fixture;
  fixture.packetLimits.maxPackets = 3;
  BOOST_CHECK_THROW(fixture.prepare(), std::invalid_argument);
  fixture.packetLimits.maxPackets = 128;
  fixture.packetLimits.maxPacketWireBytes = 1;
  BOOST_CHECK_THROW(fixture.prepare(), std::invalid_argument);
  fixture.packetLimits.maxPacketWireBytes = 1000000;
  fixture.cancelled = true;
  BOOST_CHECK_THROW(fixture.prepare(), std::runtime_error);
  fixture.cancelled = false;
  BOOST_CHECK_THROW(nativePrepareProtectedKvPackets(fixture.cipher, fixture.reference,
    fixture.source, fixture.packetLimits, [&](ndn::Data& data) {
      data.setName(ndn::Name("/wrong/name"));
      fixture.ownerKeys.sign(data, ndn::security::signingByCertificate(fixture.source));
    }, [] {}), std::invalid_argument);
  const auto packets = fixture.prepare();
  const auto prior = protectedKv::read(fixture.cipher);
  fixture.cancelled = true;
  BOOST_CHECK_THROW(fixture.materialize(packets), std::runtime_error);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
}

BOOST_AUTO_TEST_CASE(ResignedInvalidMetadataAndLateCancellationPreservePriorOutput)
{
  PacketFixture fixture;
  const auto packets = fixture.prepare();
  const auto prior = protectedKv::read(fixture.cipher);
  for (const size_t index : {size_t{0}, size_t{1}}) {
    auto modified = packets;
    ndn::Data replacement{ndn::Block(ndn::span<const uint8_t>(
      modified.packetWires[index].data(), modified.packetWires[index].size()))};
    replacement.setFreshnessPeriod(ndn::time::milliseconds(1));
    fixture.ownerKeys.sign(replacement, ndn::security::signingByCertificate(fixture.source));
    const auto wire = replacement.wireEncode();
    modified.packetWires[index].assign(wire.begin(), wire.end());
    BOOST_CHECK_THROW(fixture.materialize(modified), std::invalid_argument);
    BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
  }
  NativeCheckpointExportOptions options;
  options.beforeStage = [&](NativeCheckpointExportStage stage) {
    if (stage == NativeCheckpointExportStage::BeforeRename) fixture.cancelled = true;
  };
  BOOST_CHECK_THROW(nativeMaterializeProtectedKvPackets(packets.packetWires,
    fixture.reference, packets.rootName, fixture.cipher, fixture.packetLimits,
    [&](const auto& wire, const auto& producer) { return fixture.verify(wire, producer); },
    [&] { fixture.current(); }, options), std::runtime_error);
  BOOST_CHECK_EQUAL(protectedKv::read(fixture.cipher), prior);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::test

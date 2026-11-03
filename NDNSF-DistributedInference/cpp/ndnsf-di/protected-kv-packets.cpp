#include "NDNSF-DistributedInference/cpp/ndnsf-di/protected-kv-packets.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-private-checkpoint-file.hpp"

#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <unistd.h>

namespace ndnsf::di {
namespace {

constexpr std::uint64_t SEGMENT_BYTES = 6000;
constexpr std::uint64_t FRESHNESS_MS = 60000;
constexpr std::uint64_t MAXIMUM_WIRE_BYTES = 1000000000;

void
require(bool accepted)
{
  if (!accepted) throw std::invalid_argument("protected KV packet set invalid");
}

std::string
hash(ndn::span<const std::uint8_t> bytes)
{
  ndn::util::Sha256 digest;
  digest.update(bytes);
  auto value = digest.toString();
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return "sha256:" + value;
}

ndn::Buffer
wireOf(const ndn::Data& data)
{
  const auto& wire = data.wireEncode();
  require(wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
  return {wire.begin(), wire.end()};
}

void
checkLimits(const NativeProtectedKvPacketLimits& limits,
            const NativeProtectedKvCheckpointReferenceV1& reference)
{
  nativeValidateProtectedKvKeyEnvelope(reference);
  require(limits.checkpoint.maxPlaintextBytes > 0 && limits.checkpoint.maxWireBytes > 0 &&
    reference.wireBytes <= limits.checkpoint.maxWireBytes &&
    limits.checkpoint.maxWireBytes <= MAXIMUM_WIRE_BYTES &&
    limits.maxPacketWireBytes > 0 && limits.maxPacketWireBytes <= MAXIMUM_WIRE_BYTES &&
    reference.wireBytes <= limits.maxPacketWireBytes &&
    limits.maxPackets >= 3 && limits.maxPackets <= 65536 && reference.wireBytes > 0);
  require(1 + (reference.wireBytes - 1) / SEGMENT_BYTES <= limits.maxPackets - 2);
}

std::string
descriptor(const NativeProtectedKvCheckpointReferenceV1& reference,
           const ndn::security::Certificate& certificate)
{
  nativeValidateProtectedKvKeyEnvelope(reference);
  require(certificate.getIdentity() == reference.producerPrefix &&
    certificate.getValidityPeriod().isValid());
  const auto certificateWire = wireOf(certificate);
  const auto& envelope = reference.keyEnvelope;
  const auto body = nativeCanonicalJson(NativeJson{
    {"format", "NDKVE001-SEGMENTS-1"}, {"wireDigest", reference.wireDigest},
    {"wireBytes", reference.wireBytes},
    {"keyEnvelopeDigest", hash({reinterpret_cast<const std::uint8_t*>(envelope.data()),
                               envelope.size()})},
    {"segmentBytes", SEGMENT_BYTES},
    {"segmentCount", 1 + (reference.wireBytes - 1) / SEGMENT_BYTES},
    {"freshnessMs", FRESHNESS_MS},
    {"certificateName", certificate.getName().toUri(ndn::name::UriFormat::CANONICAL)},
    {"certificateWireDigest", hash(certificateWire)}});
  require(body.size() <= 6000);
  return body;
}

ndn::Name
rootName(const NativeProtectedKvCheckpointReferenceV1& reference, const std::string& body)
{
  const auto digest = hash({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()});
  return reference.objectName().append("packets").append(digest.substr(7)).append("manifest");
}

ndn::Data
decode(const ndn::Buffer& wire)
{
  require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE);
  ndn::Data data{ndn::Block(ndn::span<const std::uint8_t>(wire.data(), wire.size()))};
  require(data.wireEncode().size() == wire.size());
  return data;
}

void
checkPacket(const ndn::Data& data, const ndn::Name& name,
            const std::optional<ndn::name::Component>& finalBlock)
{
  require(data.getName() == name && data.getContentType() == ndn::tlv::ContentType_Blob &&
    data.getFreshnessPeriod() == ndn::time::milliseconds(FRESHNESS_MS) &&
    data.getFinalBlock() == finalBlock);
}

void
checkSigningCertificate(const ndn::Data& data, const ndn::security::Certificate& certificate)
{
  const auto& info = data.getSignatureInfo();
  require(info.hasKeyLocator() && info.getKeyLocator().getType() == ndn::tlv::Name);
  const auto& locator = info.getKeyLocator().getName();
  require((locator == certificate.getKeyName() || locator == certificate.getName()) &&
    ndn::security::verifySignature(data, certificate));
}

} // namespace

ndn::Name
nativeProtectedKvPacketRootName(const NativeProtectedKvCheckpointReferenceV1& reference,
                               const ndn::security::Certificate& certificate)
{
  return rootName(reference, descriptor(reference, certificate));
}

NativeProtectedKvPacketSet
nativePrepareProtectedKvPackets(const std::filesystem::path& protectedFile,
  const NativeProtectedKvCheckpointReferenceV1& reference,
  const ndn::security::Certificate& certificate,
  const NativeProtectedKvPacketLimits& limits, const NativeKvPacketSigner& signer,
  const std::function<void()>& requireCurrent)
{
  require(signer && requireCurrent);
  const auto current = [&] {
    requireCurrent();
    if (limits.checkpoint.cancelled && limits.checkpoint.cancelled())
      throw std::runtime_error("protected KV packet operation cancelled");
  };
  current();
  checkLimits(limits, reference);
  const auto body = descriptor(reference, certificate);
  auto checkpointLimits = limits.checkpoint;
  checkpointLimits.cancelled = [&] {
    current();
    return false;
  };
  const auto ciphertext = nativeLoadProtectedPortableKvCheckpoint(
    protectedFile, reference, checkpointLimits);
  NativeProtectedKvPacketSet result;
  result.rootName = rootName(reference, body);
  const auto addWire = [&](const ndn::Data& data) {
    current();
    auto wire = wireOf(data);
    require(wire.size() <= limits.maxPacketWireBytes - result.wireBytes &&
      result.packetWires.size() < limits.maxPackets);
    result.wireBytes += wire.size();
    result.packetNames.push_back(data.getName());
    result.packetWires.push_back(std::move(wire));
  };
  const auto sign = [&](ndn::Data& data) {
    current();
    const auto name = data.getName();
    const auto meta = data.getMetaInfo().wireEncode();
    const auto content = data.getContent();
    signer(data);
    current();
    require(data.getName() == name && data.getMetaInfo().wireEncode() == meta &&
      data.getContent() == content);
    checkSigningCertificate(data, certificate);
    addWire(data);
  };
  ndn::Data root(result.rootName);
  root.setFreshnessPeriod(ndn::time::milliseconds(FRESHNESS_MS));
  root.setContent({reinterpret_cast<const std::uint8_t*>(body.data()), body.size()});
  sign(root);
  const auto count = 1 + (reference.wireBytes - 1) / SEGMENT_BYTES;
  const auto finalBlock = ndn::name::Component::fromSegment(count - 1);
  for (std::uint64_t i = 0; i < count; ++i) {
    const auto offset = i * SEGMENT_BYTES;
    const auto length = std::min<std::uint64_t>(SEGMENT_BYTES, reference.wireBytes - offset);
    ndn::Data segment(ndn::Name(result.rootName.getPrefix(-1)).appendSegment(i));
    segment.setFreshnessPeriod(ndn::time::milliseconds(FRESHNESS_MS));
    segment.setFinalBlock(finalBlock);
    segment.setContent({ciphertext.data() + offset, static_cast<size_t>(length)});
    sign(segment);
  }
  addWire(certificate);
  ndn::util::Sha256 aggregate;
  for (const auto& wire : result.packetWires) aggregate.update(wire);
  auto digest = aggregate.toString();
  std::transform(digest.begin(), digest.end(), digest.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  result.wireDigest = "sha256:" + digest;
  current();
  return result;
}

void
nativeMaterializeProtectedKvPackets(const std::vector<ndn::Buffer>& wires,
  const NativeProtectedKvCheckpointReferenceV1& reference, const ndn::Name& expectedRoot,
  const std::filesystem::path& protectedOutput, const NativeProtectedKvPacketLimits& limits,
  const NativeKvObjectVerifier& verifier, const std::function<void()>& requireCurrent,
  const NativeCheckpointExportOptions& options)
{
  require(verifier && requireCurrent);
  const auto current = [&] {
    requireCurrent();
    if (limits.checkpoint.cancelled && limits.checkpoint.cancelled())
      throw std::runtime_error("protected KV packet operation cancelled");
  };
  current();
  checkLimits(limits, reference);
  const auto count = 1 + (reference.wireBytes - 1) / SEGMENT_BYTES;
  require(wires.size() == count + 2);
  std::uint64_t total = 0;
  for (const auto& wire : wires) {
    require(!wire.empty() && wire.size() <= ndn::MAX_NDN_PACKET_SIZE &&
      wire.size() <= limits.maxPacketWireBytes - total);
    total += wire.size();
  }
  const ndn::security::Certificate certificate(decode(wires.back()));
  const auto body = descriptor(reference, certificate);
  require(rootName(reference, body) == expectedRoot);
  require(verifier(wires.back(), reference.producerPrefix));
  current();
  const auto root = decode(wires.front());
  checkPacket(root, expectedRoot, std::nullopt);
  const auto& content = root.getContent();
  require(content.value_size() == body.size() &&
    std::equal(body.begin(), body.end(), content.value()));
  require(verifier(wires.front(), reference.producerPrefix));
  checkSigningCertificate(root, certificate);
  current();
  ndn::Buffer ciphertext;
  ciphertext.reserve(static_cast<size_t>(reference.wireBytes));
  for (std::uint64_t i = 0; i < count; ++i) {
    current();
    const auto segment = decode(wires[i + 1]);
    checkPacket(segment, ndn::Name(expectedRoot.getPrefix(-1)).appendSegment(i),
      ndn::name::Component::fromSegment(count - 1));
    const auto& bytes = segment.getContent();
    require(bytes.value_size() == std::min<std::uint64_t>(SEGMENT_BYTES,
      reference.wireBytes - ciphertext.size()));
    require(verifier(wires[i + 1], reference.producerPrefix));
    checkSigningCertificate(segment, certificate);
    current();
    ciphertext.insert(ciphertext.end(), bytes.value(), bytes.value() + bytes.value_size());
  }
  nativeValidateProtectedKvCheckpointWire(ciphertext, reference, limits.checkpoint);
  current();
  detail::writePrivateCheckpointFile(protectedOutput, options, [&](int fd) {
    size_t offset = 0;
    while (offset < ciphertext.size()) {
      current();
      const auto length = std::min<size_t>(65536, ciphertext.size() - offset);
      const auto written = ::write(fd, ciphertext.data() + offset, length);
      if (written < 0 && errno == EINTR) continue;
      if (written <= 0) throw std::runtime_error("protected KV packet output failed");
      offset += static_cast<size_t>(written);
    }
  }, [&] { current(); return false; });
}

} // namespace ndnsf::di

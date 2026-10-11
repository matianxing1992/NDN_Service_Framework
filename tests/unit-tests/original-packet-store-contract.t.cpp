#include "tests/boost-test.hpp"
#include "ndn-service-framework/EncryptedLargeDataRangeStore.hpp"
#include "ndn-service-framework/detail/OriginalPacketFile.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

namespace ndn_service_framework::test {
namespace {
class RangeOnlySource : public EncryptedLargeDataRangeSource {
public:
  std::uint64_t size() const noexcept override { return 1; }
  std::vector<std::uint8_t> read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > 1 || length > 1 - offset) throw std::out_of_range("range");
    return std::vector<std::uint8_t>(length, 42);
  }
};

class RangeOnlyStore : public EncryptedLargeDataRangeStore {
public:
  std::shared_ptr<const EncryptedLargeDataRangeSource> commitFile(
    const std::string&, const std::filesystem::path&, std::uint64_t,
    const std::function<void()>&) override
  {
    return std::make_shared<RangeOnlySource>();
  }
};
} // namespace

BOOST_AUTO_TEST_SUITE(OriginalPacketStoreContract)
BOOST_AUTO_TEST_CASE(RangeBackendCannotPretendToStoreOriginalPackets)
{
  RangeOnlyStore legacy;
  EncryptedLargeDataRangeStore& store = legacy;
  BOOST_CHECK(!store.supportsOriginalDataPackets());
  bool suppliedPacket = false;
  const auto next = [&]() -> std::optional<std::pair<ndn::Name, std::vector<std::uint8_t>>> {
    suppliedPacket = true;
    return std::nullopt;
  };
  EncryptedLargeDataCommitOptions options;
  options.packetStorageFormat = "original-packet-set";
  BOOST_CHECK_THROW(store.commitOriginalDataPackets(ndn::Name("/producer/manifest"), options, next),
                    std::runtime_error);
  BOOST_CHECK(!suppliedPacket);
  BOOST_CHECK_THROW(store.commitFile("/producer/object", {}, 1, options), std::runtime_error);
  const RangeOnlySource source;
  BOOST_CHECK_THROW(source.readOriginalDataPacket(ndn::Name("/producer/segment")), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(HistoricalAggregateLookupPreservesSourceSlot)
{
  const auto source = std::make_shared<RangeOnlySource>();
  // Match the existing Repo adapter's positional aggregate initialization.
  const EncryptedLargeDataLookupResult result{
    "/object", "identity", "epoch", "key", "key-version", "manifest-digest",
    "/object", "content-digest", 1, source};
  BOOST_CHECK(result.source == source);
  BOOST_CHECK_EQUAL(result.packetStorageFormat, "encrypted-envelope-bytes");
  BOOST_CHECK(result.manifestName.empty());
  BOOST_CHECK(result.dataPrefix.empty());
  BOOST_CHECK_EQUAL(result.packetWireBytes, 0);
}

BOOST_AUTO_TEST_CASE(LocalPacketOwnerPreservesSignedWireAndRejectsIncompleteSets)
{
  ndn::KeyChain keys("pib-memory:", "tpm-memory:");
  const ndn::Name prefix("/org/NDNSF/actor/NDNSF/TENSOR/ROUND/0/key/DATA");
  unsigned generated = 0;
  const auto next = [&](std::uint64_t segment) {
    ++generated;
    ndn::Data data(ndn::Name(prefix).appendSegment(segment));
    data.setContent(std::vector<std::uint8_t>(31, static_cast<std::uint8_t>(segment)));
    data.setFinalBlock(ndn::name::Component::fromSegment(12));
    keys.sign(data, ndn::security::signingWithSha256());
    const auto& wire = data.wireEncode();
    return std::vector<std::uint8_t>(wire.begin(), wire.end());
  };
  const auto owner = detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), prefix, 13, 100000, next);
  BOOST_CHECK_EQUAL(generated, 13);
  const auto first = owner->read(0);
  for (unsigned i = 1; i < 13; ++i) BOOST_CHECK(!owner->read(i).empty());
  const auto again = owner->read(0);
  BOOST_CHECK_EQUAL_COLLECTIONS(first.begin(), first.end(), again.begin(), again.end());
  BOOST_CHECK_EQUAL(generated, 13); // reads never call generation/signing
  BOOST_CHECK_GT(owner->storageBytes(), first.size() * 13);
  BOOST_CHECK_THROW(owner->read(13), std::out_of_range);
  BOOST_CHECK_THROW(detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), prefix, 13, 0, next), std::runtime_error);
  BOOST_CHECK_THROW(detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), ndn::Name("/wrong"), 13, 100000, next),
    std::runtime_error);
  BOOST_CHECK_THROW(detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), prefix, 1, 100000, next), std::runtime_error);
  BOOST_CHECK_THROW(detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), prefix, 13, 100000,
    [&](std::uint64_t segment) {
      auto wire = next(segment);
      wire.push_back(0);
      return wire;
    }), std::runtime_error);
  unsigned checks = 0;
  BOOST_CHECK_THROW(detail::OriginalPacketFile::create(
    std::filesystem::temp_directory_path(), prefix, 13, 100000, next,
    [&] { if (++checks == 4) throw std::runtime_error("cancelled"); }), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(ProtectedManifestRoundTripBindsExactProducerAndPacketGeometry)
{
  ProtectedBlobManifest manifest;
  manifest.producer = ndn::Name("/org/NDNSF/BLOB/model-publisher");
  manifest.plaintextDigest.assign(32, 0x2a);
  manifest.plaintextBytes = 8000;
  manifest.envelopeBytes = 9000;
  manifest.segmentBytes = 7000;
  manifest.segmentCount = 2;
  manifest.representationSpec = "{\"encoding\":\"test\",\"segmentBytes\":7000}";
  manifest.protectionSpec = "{\"epoch\":\"1\",\"keyReference\":\"test\"}";
  manifest.authorizationScope = "/SERVICE/test";
  manifest.protectionEpoch = "1";
  manifest.keyReferenceId = "test";
  manifest.keyIdentifier = "test-key";
  const auto protection = computeNamingDigest("ndnsf-naming/protection",
    {{1, 5, std::vector<std::uint8_t>(manifest.representationSpec.begin(), manifest.representationSpec.end())},
     {2, 5, std::vector<std::uint8_t>(manifest.protectionSpec.begin(), manifest.protectionSpec.end())}});
  manifest.contentName = PublishedContentName{};
  manifest.contentName->label = "model-payload";
  std::string plaintextHex;
  for (size_t i = 0; i < 32; ++i) plaintextHex += "2a";
  manifest.dataPrefix = manifest.contentName->contentPrefix(manifest.producer, plaintextHex)
    .append("PROTECTED").append(makeNamingDigestComponent(protection)).append("DATA");
  const auto payload = manifest.encode();
  const auto name = manifest.getName();
  const auto parsed = ProtectedBlobManifest::decode(payload, manifest.producer, name);
  BOOST_CHECK(parsed.dataPrefix == manifest.dataPrefix);
  BOOST_CHECK_EQUAL(parsed.segmentCount, 2);
  BOOST_CHECK_EQUAL(parsed.plaintextBytes, 8000);
  BOOST_CHECK_EQUAL(parsed.envelopeBytes, 9000);
  BOOST_CHECK_EQUAL(parsed.representationSpec, manifest.representationSpec);
  BOOST_CHECK_EQUAL(parsed.protectionSpec, manifest.protectionSpec);
  BOOST_CHECK_EQUAL(parsed.keyReferenceId, manifest.keyReferenceId);
  BOOST_CHECK_EQUAL(parsed.keyIdentifier, manifest.keyIdentifier);
  BOOST_CHECK_EQUAL(parsed.protectionEpoch, manifest.protectionEpoch);
  BOOST_CHECK_EQUAL(parsed.authorizationScope, manifest.authorizationScope);
  BOOST_CHECK(parsed.encode() == payload);
  BOOST_REQUIRE(ProtectedBlobManifest::inferProducer(name));
  BOOST_CHECK(*ProtectedBlobManifest::inferProducer(name) == manifest.producer);
  const std::vector<ndn::Name> suffixes{
    ndn::Name("/NDNSF-DI/model").append(makeNamingLabelComponent("qwen"))
      .append(makeNamingDigestComponent(std::string(64, 'b'))).append("MANIFEST"),
    ndn::Name("/NDNSF-DI/material").append(makeNamingLabelComponent("material")).append("MANIFEST"),
    ndn::Name("/NDNSF-DI/receipt/assembly").append(makeNamingLabelComponent("receipt"))};
  for (const auto& invalidSuffix : {
         ndn::Name("/NDNSF-DI/receipt/assembly").append(ndn::name::Component("name=")),
         ndn::Name("/NDNSF-DI/receipt/assembly1").append(ndn::name::Component("name=receipt")),
         ndn::Name("/NDNSF-DI/unrelated").append(ndn::name::Component("name=receipt"))}) {
    BOOST_CHECK_THROW(ProtectedBlobManifest::validateSuffix(invalidSuffix), std::invalid_argument);
    const auto unrelated = ndn::Name(manifest.producer).append(invalidSuffix)
      .append(makeNamingDigestComponent(std::string(64, 'c')));
    BOOST_CHECK(!ProtectedBlobManifest::inferProducer(unrelated));
    BOOST_CHECK(!ProtectedBlobManifest::inferProducer(unrelated, DataNameReadMode::AllowLegacyRead));
  }
  for (const auto& suffix : suffixes) {
    auto semantic = manifest;
    semantic.manifestSuffix = suffix;
    if (suffix[1] == ndn::name::Component("model")) {
      semantic.contentName->kind = ContentDataKind::ModelFile;
      semantic.contentName->modelName = "qwen";
      semantic.contentName->modelIdentity = std::string(64, 'b');
    }
    else {
      semantic.contentName->kind = ContentDataKind::Material;
      semantic.contentName->label = suffix[1] == ndn::name::Component("material") ? "material" : "receipt";
    }
    semantic.dataPrefix = semantic.contentName->contentPrefix(manifest.producer, plaintextHex)
      .append("PROTECTED").append(makeNamingDigestComponent(protection)).append("DATA");
    const auto semanticName = semantic.getName();
    BOOST_CHECK(semanticName.getPrefix(-1) ==
      ndn::Name(manifest.producer).append(suffix));
    const auto owner = ProtectedBlobManifest::inferProducer(semanticName);
    BOOST_REQUIRE(owner);
    BOOST_CHECK(*owner == manifest.producer);
    const auto decoded = ProtectedBlobManifest::decode(semantic.encode(), *owner, semanticName);
    BOOST_CHECK(decoded.manifestSuffix == suffix);
    BOOST_CHECK(decoded.dataPrefix == semantic.dataPrefix);
    BOOST_CHECK_THROW(ProtectedBlobManifest::decode(payload, manifest.producer, semanticName),
                      std::invalid_argument);
  }
  auto legacy = manifest;
  legacy.contentName.reset();
  legacy.dataPrefix = makeProducerDataPrefix(manifest.producer, "BLOB")
    .append(makeNamingDigestComponent(plaintextHex)).append("PROTECTED")
    .append(makeNamingDigestComponent(protection)).append("DATA");
  const auto legacyPayload = legacy.encode(DataNameReadMode::AllowLegacyRead);
  const auto legacyName = legacy.getName(DataNameReadMode::AllowLegacyRead);
  BOOST_CHECK_THROW(legacy.encode(), std::invalid_argument);
  BOOST_CHECK_THROW(ProtectedBlobManifest::decode(legacyPayload, manifest.producer, legacyName),
                    std::invalid_argument);
  const auto legacyDecoded = ProtectedBlobManifest::decode(legacyPayload, manifest.producer, legacyName,
                                                          DataNameReadMode::AllowLegacyRead);
  BOOST_CHECK(legacyDecoded.encode(DataNameReadMode::AllowLegacyRead) == legacyPayload);
  BOOST_CHECK(!ProtectedBlobManifest::inferProducer(legacyName));
  BOOST_REQUIRE(ProtectedBlobManifest::inferProducer(legacyName, DataNameReadMode::AllowLegacyRead));
  BOOST_CHECK_THROW(ProtectedBlobManifest::validateSuffix(ndn::Name("/MODEL/not-a-digest/MANIFEST")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(ProtectedBlobManifest::validateSuffix(ndn::Name("/other/RECEIPT")),
                    std::invalid_argument);
  BOOST_CHECK_THROW(ProtectedBlobManifest::decode(payload, ndn::Name("/org"), name), std::invalid_argument);
  BOOST_CHECK_THROW(ProtectedBlobManifest::decode(payload, manifest.producer,
    ndn::Name(name).append("wrong")), std::invalid_argument);
  auto trailing = payload;
  trailing.push_back(0);
  BOOST_CHECK_THROW(ProtectedBlobManifest::decode(trailing, manifest.producer, name), std::invalid_argument);
  auto invalid = manifest;
  invalid.segmentCount = 1;
  BOOST_CHECK_THROW(invalid.encode(), std::invalid_argument);
  invalid = manifest;
  invalid.segmentBytes = 0;
  BOOST_CHECK_THROW(invalid.encode(), std::invalid_argument);
  invalid = manifest;
  invalid.protectionSpec = "{}";
  BOOST_CHECK_THROW(invalid.encode(), std::invalid_argument);
}
BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test

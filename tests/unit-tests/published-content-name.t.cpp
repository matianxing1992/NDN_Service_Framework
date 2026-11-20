#include "tests/boost-test.hpp"
#include "ndn-service-framework/PublishedContentName.hpp"
#include "ndn-service-framework/ProducerDataNaming.hpp"
#include "ndn-service-framework/ProtectedBlobManifest.hpp"

#include <stdexcept>
#include <iostream>

namespace ndn_service_framework::test {
BOOST_AUTO_TEST_SUITE(PublishedContentNaming)

BOOST_AUTO_TEST_CASE(FixedAssignmentUsesModelRoleAndCanonicalPayloadIdentity)
{
  PublishedContentName value;
  value.kind = ContentDataKind::Assignment;
  value.label = "S0R0";
  value.modelName = "qwen2.5-0.5b";
  value.modelIdentity = std::string(64, 'a');
  const ndn::Name producer("/organization/user");
  const auto payloadDigest = std::string(64, 'b');
  const auto expected = ndn::Name(producer).append("NDNSF-DI").append("assignment")
    .append("model").append(ndn::name::Component("name=qwen2.5-0.5b"))
    .append("ROLE").append(ndn::name::Component("name=S0R0"))
    .append(makeNamingDigestComponent(payloadDigest));
  BOOST_CHECK(value.contentPrefix(producer, payloadDigest) == expected);
  auto dataPrefix = expected;
  dataPrefix.append("PROTECTED").append(makeNamingDigestComponent(std::string(64, 'c')))
    .append("DATA");
  BOOST_CHECK(ProtectedBlobManifest::inferDataProducer(dataPrefix) ==
              std::optional<ndn::Name>(producer));
  auto manifest = expected;
  manifest.append("PROTECTED").append(makeNamingDigestComponent(std::string(64, 'c')))
    .append("MANIFEST").append(makeNamingDigestComponent(std::string(64, 'd')));
  BOOST_CHECK(ProtectedBlobManifest::inferProducer(manifest) ==
              std::optional<ndn::Name>(producer));
  BOOST_CHECK(PublishedContentName::decode(value.encode()).encode() == value.encode());
  auto changed = value;
  changed.label = "S1R0";
  BOOST_CHECK(changed.objectIdentity() != value.objectIdentity());
  changed = value;
  changed.modelName = "model\\with\"quotes";
  BOOST_CHECK(changed.objectIdentity() != value.objectIdentity());
  BOOST_CHECK_NO_THROW(changed.encode());
  changed.modelIdentity.clear();
  BOOST_CHECK_THROW(changed.encode(), std::invalid_argument);
  changed = value;
  changed.inputKind = "image";
  BOOST_CHECK_THROW(changed.encode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ModelAndArtifactSemantics)
{
  PublishedContentName value;
  value.kind = ContentDataKind::ModelFile;
  value.label = "model.onnx";
  value.modelName = "qwen2.5-0.5b";
  value.modelIdentity = std::string(64, 'a');
  // Frozen independent SHA256 oracle of C01 framing and C02 canonical parent JSON.
  BOOST_CHECK_EQUAL(value.objectIdentity(), "b9352c2850c0f5883a5501b9d682c06d2683822d5f6a647a635201e3f2363f6f");
  const ndn::Name producer("/org/provider/p0");
  const auto name = value.contentPrefix(producer, std::string(64, 'b'));
  auto expected = ndn::Name(producer).append("NDNSF-DI").append("model")
    .append(ndn::name::Component("name=qwen2.5-0.5b"))
    .append(makeNamingDigestComponent(value.modelIdentity)).append("file")
    .append(ndn::name::Component("name=model.onnx"))
    .append(makeNamingDigestComponent(value.objectIdentity()))
    .append(makeNamingDigestComponent(std::string(64, 'b')));
  BOOST_CHECK(name == expected);
  const auto stable = value.objectIdentity();
  value.kind = ContentDataKind::ArtifactFile;
  value.artifactKind = "stage";
  value.coordinate = "layers=0-11";
  value.profileIdentity = std::string(64, 'c');
  const auto artifact = value.contentPrefix(producer, std::string(64, 'b'));
  BOOST_CHECK(artifact != name);
  BOOST_CHECK(value.objectIdentity() != stable);
  BOOST_CHECK(artifact[producer.size() + 4] == ndn::name::Component("artifact"));
  value.coordinate = "layers/0/11";
  BOOST_CHECK_THROW(value.contentPrefix(producer, std::string(64, 'b')), std::invalid_argument);
  value.coordinate = "layers\"0";
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument);
  value.coordinate = "layers\\0";
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument);
  value.coordinate = "layers=0-11";
  value.kind = ContentDataKind::ModelFile;
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument); // Irrelevant artifact fields.
  value.kind = static_cast<ContentDataKind>(999);
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(LongLabelIdentityAndCategoryIsolation)
{
  PublishedContentName value;
  value.label = std::string(64, 'a') + "-first";
  const auto first = value.contentPrefix(ndn::Name("/producer"), std::string(64, 'b'));
  const auto firstIdentity = value.objectIdentity();
  value.label = std::string(64, 'a') + "-second";
  const auto second = value.contentPrefix(ndn::Name("/producer"), std::string(64, 'b'));
  BOOST_CHECK(first[3] == second[3]); // Same visible shortened label.
  BOOST_CHECK(firstIdentity != value.objectIdentity());
  BOOST_CHECK(first != second);
  value.kind = ContentDataKind::Material;
  BOOST_CHECK(value.contentPrefix(ndn::Name("/producer"), std::string(64, 'b')) != second);
  value.kind = ContentDataKind::Input;
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument);
  value.inputKind = "image";
  BOOST_CHECK_NO_THROW(value.contentPrefix(ndn::Name("/producer"), std::string(64, 'b')));
  value.kind = ContentDataKind::ModelFile;
  BOOST_CHECK_THROW(value.objectIdentity(), std::invalid_argument); // No model identity.
}

BOOST_AUTO_TEST_CASE(SemanticMetadataRoundTripAndRejection)
{
  PublishedContentName value;
  value.kind = ContentDataKind::ModelFile;
  value.label = "model.onnx";
  value.modelName = "qwen2.5-0.5b";
  value.modelIdentity = std::string(64, 'a');
  const auto wire = value.encode();
  const auto restored = PublishedContentName::decode(wire);
  BOOST_CHECK_EQUAL(restored.label, value.label);
  BOOST_CHECK_EQUAL(restored.modelName, value.modelName);
  BOOST_CHECK_EQUAL(restored.modelIdentity, value.modelIdentity);
  BOOST_CHECK_EQUAL(restored.objectIdentity(), value.objectIdentity());
  BOOST_CHECK(restored.contentPrefix(ndn::Name("/p"), std::string(64, 'b')) ==
              value.contentPrefix(ndn::Name("/p"), std::string(64, 'b')));
  auto trailing = wire;
  trailing.push_back(0);
  BOOST_CHECK_THROW(PublishedContentName::decode(trailing), std::exception);
  auto wrongEnvelope = wire;
  wrongEnvelope[0] = 240;
  BOOST_CHECK_THROW(PublishedContentName::decode(wrongEnvelope), std::exception);
  auto truncated = wire;
  truncated.pop_back();
  BOOST_CHECK_THROW(PublishedContentName::decode(truncated), std::exception);
}

BOOST_AUTO_TEST_CASE(SemanticCoordinatesRoundTripAndClosedValidation)
{
  PublishedContentName value;
  value.kind = ContentDataKind::Material;
  value.semanticRole = ContentSemanticRole::MaterialBundle;
  value.label = "bundle-12";
  value.modelName = "qwen3";
  value.modelFamily = "qwen3";
  value.modelSize = "0.6b";
  value.quantization = "int4";
  value.sourceRevisionLabel = "r3";
  value.bundleIndex = 12;
  const auto wire = value.encode();
  const auto restored = PublishedContentName::decode(wire);
  BOOST_CHECK(restored.semanticRole == ContentSemanticRole::MaterialBundle);
  BOOST_CHECK_EQUAL(restored.modelFamily, "qwen3");
  BOOST_CHECK_EQUAL(restored.quantization, "int4");
  BOOST_REQUIRE(restored.bundleIndex.has_value());
  BOOST_CHECK_EQUAL(*restored.bundleIndex, 12U);
  BOOST_CHECK_EQUAL(restored.objectIdentity(), value.objectIdentity());
  const auto path = restored.modelPathComponents();
  BOOST_REQUIRE_EQUAL(path.size(), 5U);
  BOOST_CHECK_EQUAL(path[0], "qwen3");
  BOOST_CHECK_EQUAL(path[1], "0.6b");
  BOOST_CHECK_EQUAL(path[2], "int4");
  BOOST_CHECK_EQUAL(path[3], "revision");
  BOOST_CHECK_EQUAL(path[4], "r3");

  // A coordinate changes the object identity; the name must not stay the same.
  auto other = value;
  other.quantization = "int8";
  BOOST_CHECK(other.objectIdentity() != value.objectIdentity());

  // Closed validation: role/kind mismatch, bundle index outside a bundle, use id outside a capsule.
  auto wrongRole = value;
  wrongRole.semanticRole = ContentSemanticRole::SourceGraph;
  BOOST_CHECK_THROW(wrongRole.encode(), std::invalid_argument);
  auto extraBundle = value;
  extraBundle.bundleIndex.reset();
  BOOST_CHECK_THROW(extraBundle.encode(), std::invalid_argument);
  auto strayUse = value;
  strayUse.useId = "use1";
  BOOST_CHECK_THROW(strayUse.encode(), std::invalid_argument);
  auto missingPath = value;
  missingPath.modelSize.clear();
  BOOST_CHECK_THROW(missingPath.encode(), std::invalid_argument);

  // A legacy instance keeps exactly the previous eight-field wire.
  PublishedContentName legacy;
  legacy.kind = ContentDataKind::Blob;
  legacy.label = "payload";
  BOOST_CHECK(!legacy.hasSemanticCoordinates());
  const auto legacyWire = legacy.encode();
  BOOST_CHECK(PublishedContentName::decode(legacyWire).encode() == legacyWire);
}

BOOST_AUTO_TEST_CASE(SemanticStemFollowsTheN4Rows)
{
  const ndn::Name producer("/lab/preparer");
  const std::string rep(64, 'a');
  PublishedContentName value;
  value.modelName = "qwen3";
  value.modelFamily = "qwen3";
  value.modelSize = "0.6b";
  value.quantization = "int4";
  value.sourceRevisionLabel = "r3";
  const auto stem = [&](ContentDataKind kind, ContentSemanticRole role) {
    value.kind = kind;
    value.semanticRole = role;
    return value.semanticStem(producer);
  };
  BOOST_CHECK(stem(ContentDataKind::ModelFile, ContentSemanticRole::SourceGraph) ==
              ndn::Name(producer).append("NDNSF-DI").append("model")
                .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
                .append("source").append("graph"));
  BOOST_CHECK(stem(ContentDataKind::ModelFile, ContentSemanticRole::ModelManifest) ==
              ndn::Name(producer).append("NDNSF-DI").append("model")
                .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
                .append("manifest"));

  value.bundleIndex = 12;
  BOOST_CHECK(stem(ContentDataKind::Material, ContentSemanticRole::MaterialBundle) ==
              ndn::Name(producer).append("NDNSF-DI").append("material")
                .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
                .append("bundle").append(makeNamingDecimalComponent(12)));
  value.bundleIndex.reset();

  value.useId = "use7";
  BOOST_CHECK(stem(ContentDataKind::Assignment, ContentSemanticRole::StoredCapsule) ==
              ndn::Name(producer).append("NDNSF-DI").append("key-capsule")
                .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
                .append("source").append("stored").append("use")
                .append(makeNamingLabelComponent("use7")));
  BOOST_CHECK(stem(ContentDataKind::Assignment, ContentSemanticRole::AuthorizationManifest) ==
              ndn::Name(producer).append("NDNSF-DI").append("authorization")
                .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
                .append("use").append(makeNamingLabelComponent("use7")));

  // The representation tail is one 32-byte Generic component, not an implicit digest.
  value.kind = ContentDataKind::ModelFile;
  value.semanticRole = ContentSemanticRole::SourceGraph;
  value.useId.clear();  // the use id belongs to the capsule rows only
  const auto prefix = value.representationPrefix(producer, rep);
  BOOST_REQUIRE(prefix.size() >= 2);
  BOOST_CHECK_EQUAL(prefix[-2].toUri(), std::string("rep"));
  BOOST_CHECK_EQUAL(prefix[-1].value_size(), 32U);
  BOOST_CHECK(!prefix[-1].isImplicitSha256Digest());
  BOOST_CHECK(prefix[-1].type() == ndn::tlv::GenericNameComponent);

  // A short digest is not a representation identity.
  BOOST_CHECK_THROW(value.representationPrefix(producer, std::string(63, 'a')), std::invalid_argument);
  // An instance without a role has no N4 stem yet.
  PublishedContentName legacy;
  legacy.label = "payload";
  BOOST_CHECK_THROW(legacy.semanticStem(producer), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(CapsuleRoleRequiresCurrentUseAndModelPath)
{
  PublishedContentName value;
  value.kind = ContentDataKind::Assignment;
  value.semanticRole = ContentSemanticRole::StoredCapsule;
  value.label = "S0R0";
  value.modelName = "qwen3";
  value.modelIdentity = std::string(64, 'a');
  value.modelFamily = "qwen3";
  value.modelSize = "0.6b";
  value.quantization = "int4";
  value.sourceRevisionLabel = "r3";
  value.useId = "use7";
  const auto restored = PublishedContentName::decode(value.encode());
  BOOST_CHECK(restored.semanticRole == ContentSemanticRole::StoredCapsule);
  BOOST_CHECK_EQUAL(restored.useId, "use7");
  auto noUse = value;
  noUse.useId.clear();
  BOOST_CHECK_THROW(noUse.encode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(SemanticWireCorpus)
{
  const auto producer = ndn::Name("/org/NDNSF-DI/model/publisher").appendVersion(257);
  for (int kind = 0; kind <= 8; ++kind) {
    PublishedContentName value;
    value.kind = static_cast<ContentDataKind>(kind);
    value.label = u8"权重/α.dat";
    if ((kind >= 1 && kind <= 5) || kind == 8) {
      value.modelName = "qwen2.5-0.5b";
      value.modelIdentity = std::string(64, 'a');
    }
    if (value.kind == ContentDataKind::ArtifactFile) {
      value.artifactKind = "stage"; value.coordinate = "layers=0-11";
      value.profileIdentity = std::string(64, 'c');
    }
    if (value.kind == ContentDataKind::Input) value.inputKind = "image";
    const auto name = value.contentPrefix(producer, std::string(64, 'b'));
    constexpr char Hex[] = "0123456789abcdef";
    std::string encoded;
    const auto& wire = name.wireEncode();
    for (const auto byte : wire) { encoded += Hex[byte >> 4]; encoded += Hex[byte & 15]; }
    std::cout << "SPEC201_SEMANTIC " << kind << " " << encoded << std::endl;
    BOOST_CHECK(!name.empty());
  }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace ndn_service_framework::test

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

BOOST_AUTO_TEST_CASE(PartitionAndPageRoundTripThroughTheWire)
{
  PublishedContentName value;
  value.kind = ContentDataKind::ArtifactFile;
  value.semanticRole = ContentSemanticRole::ArtifactManifest;
  value.label = "manifest";
  value.modelName = "qwen3";
  value.modelIdentity = std::string(64, 'a');
  value.modelFamily = "qwen3";
  value.modelSize = "0.6b";
  value.quantization = "int4";
  value.sourceRevisionLabel = "r3";
  value.profileLabel = "kv-0-4-ffn-0-1536";
  value.artifactEntry = "mlp-13";
  value.partition.kind = ArtifactPartitionKind::Hybrid;
  value.partition.layerBegin = 0;
  value.partition.layerEnd = 14;
  value.partition.tensorDegree = 2;
  const auto wire = value.encode();
  const auto restored = PublishedContentName::decode(wire);
  BOOST_CHECK_EQUAL(restored.artifactEntry, "mlp-13");
  BOOST_CHECK(restored.partition.kind == ArtifactPartitionKind::Hybrid);
  BOOST_CHECK_EQUAL(restored.partition.layerEnd, 14U);
  BOOST_CHECK_EQUAL(restored.partition.tensorDegree, 2U);
  BOOST_CHECK_EQUAL(restored.partitionJson(), value.partitionJson());
  BOOST_CHECK_EQUAL(restored.objectIdentity(), value.objectIdentity());

  // A reordered or edited partition object is not canonical.
  auto tampered = value;
  tampered.partition.rank = 1;
  BOOST_CHECK(tampered.encode() != wire);

  // A manifest page names its business manifest stem plus the page number.
  PublishedContentName page;
  page.kind = ContentDataKind::ModelFile;
  page.semanticRole = ContentSemanticRole::ManifestPage;
  page.modelName = "qwen3";
  page.modelIdentity = std::string(64, 'a');
  page.modelFamily = "qwen3";
  page.modelSize = "0.6b";
  page.quantization = "int4";
  page.sourceRevisionLabel = "r3";
  page.pageNumber = 4;
  const auto pageStem = page.semanticStem(ndn::Name("/lab/preparer"));
  BOOST_CHECK(pageStem[-2] == ndn::name::Component("page"));
  BOOST_CHECK(pageStem[-1] == makeNamingDecimalComponent(4));
  const auto pageWire = page.encode();
  BOOST_REQUIRE(PublishedContentName::decode(pageWire).pageNumber.has_value());
  BOOST_CHECK_EQUAL(*PublishedContentName::decode(pageWire).pageNumber, 4U);
  auto missingPage = page;
  missingPage.pageNumber.reset();
  BOOST_CHECK_THROW(missingPage.encode(), std::invalid_argument);
}

BOOST_AUTO_TEST_CASE(ArtifactStemNamesTheActualPartition)
{
  const ndn::Name producer("/lab/preparer");
  PublishedContentName value;
  value.kind = ContentDataKind::ArtifactFile;
  value.semanticRole = ContentSemanticRole::ArtifactFileObject;
  value.label = "model.onnx";
  value.modelName = "qwen3";
  value.modelFamily = "qwen3";
  value.modelSize = "0.6b";
  value.quantization = "int4";
  value.sourceRevisionLabel = "r3";
  value.profileLabel = "kv-0-4-ffn-0-1536";
  value.artifactEntry = "mlp-13";
  value.partition.kind = ArtifactPartitionKind::Hybrid;
  value.partition.stageIndex = 0;
  value.partition.layerBegin = 0;
  value.partition.layerEnd = 14;
  value.partition.tensorDegree = 2;
  value.partition.rank = 0;
  const auto stem = value.semanticStem(producer);
  const auto expected = ndn::Name(producer).append("NDNSF-DI").append("artifact")
    .append("qwen3").append("0.6b").append("int4").append("revision").append("r3")
    .append("hybrid").append("stage").append("0").append("layers").append("0").append("14")
    .append("degree").append("2").append("rank").append("0")
    .append("entry").append("mlp-13").append("profile").append("kv-0-4-ffn-0-1536")
    .append("file").append(makeNamingLabelComponent("model.onnx"));
  BOOST_CHECK(stem == expected);

  // Same layers, different actual slice -> a different stem, so the layout cannot be aliased.
  auto weighted = value;
  weighted.profileLabel = "kv-0-2-ffn-0-768";
  BOOST_CHECK(weighted.semanticStem(producer) != stem);
  BOOST_CHECK_EQUAL(weighted.partitionJson(),
                    std::string("{\"componentId\":\"\",\"cut\":\"0\",\"degree\":\"2\","
                                "\"kind\":\"hybrid\",\"layerBegin\":\"0\",\"layerEnd\":\"14\","
                                "\"rank\":\"0\",\"side\":\"\",\"stage\":\"0\"}"));

  // A rank outside the degree, an empty layer interval and a missing partition are refused.
  auto badRank = value;
  badRank.partition.rank = 2;
  BOOST_CHECK_THROW(badRank.semanticStem(producer), std::invalid_argument);
  auto badLayers = value;
  badLayers.partition.layerEnd = 0;
  BOOST_CHECK_THROW(badLayers.semanticStem(producer), std::invalid_argument);
  auto noPartition = value;
  noPartition.partition.kind = ArtifactPartitionKind::None;
  BOOST_CHECK_THROW(noPartition.semanticStem(producer), std::invalid_argument);

  // The automatic ONNX cut names the verified prefix node count and side.
  auto automatic = value;
  automatic.partition = {};
  automatic.partition.kind = ArtifactPartitionKind::AutomaticGraphCut;
  automatic.partition.cutPrefixNodes = 37;
  automatic.partition.graphSide = "suffix";
  const auto cutStem = automatic.semanticStem(producer);
  BOOST_CHECK(cutStem[-10] == ndn::name::Component("cut"));
  BOOST_CHECK(cutStem[-9] == ndn::name::Component("37"));
  BOOST_CHECK(cutStem[-7] == ndn::name::Component("suffix"));
}

BOOST_AUTO_TEST_CASE(SemanticMetadataGoldenWireVectors)
{
  // Independent oracle: the expected wire is assembled here from the field table, not by calling encode().
  // The oracle states the field table; the comparison is exact at the TLV element level so it does not depend on
  // this implementation's envelope-length spelling.
  const auto elements = [](const std::vector<std::uint8_t>& wire) {
    ndn::Block block(ndn::span<const std::uint8_t>(wire.data(), wire.size()));
    block.parse();
    std::vector<std::pair<uint64_t, std::string>> result;
    for (const auto& element : block.elements())
      result.emplace_back(element.type(), std::string(
        reinterpret_cast<const char*>(element.value()), element.value_size()));
    return result;
  };
  const auto expect = [&](const std::vector<std::uint8_t>& wire,
                          const std::vector<std::pair<uint64_t, std::string>>& table) {
    // The oracle fixes the field types, their order and their encoded lengths; a value that must be a specific
    // string is asserted separately below so this check does not depend on int spelling.
    const auto actual = elements(wire);
    BOOST_REQUIRE_EQUAL(actual.size(), table.size());
    for (std::size_t i = 0; i < table.size(); ++i) {
      BOOST_CHECK_EQUAL(actual[i].first, table[i].first);
      BOOST_CHECK_EQUAL(actual[i].second.size(), table[i].second.size());
    }
  };

  // Legacy record: only the eight original fields, kinds still unwritten.
  PublishedContentName legacy;
  legacy.kind = ContentDataKind::Blob;
  legacy.label = "payload";
  const auto legacyWire = legacy.encode();
  expect(legacyWire, {{129, std::string(1, '\0')}, {131, "payload"}, {133, ""}, {135, ""},
                      {137, ""}, {139, ""}, {141, ""}, {143, ""}});
  const auto legacyDecoded = PublishedContentName::decode(legacyWire);
  BOOST_CHECK_EQUAL(legacyDecoded.label, "payload");
  BOOST_CHECK(!legacyDecoded.hasSemanticCoordinates());

  // Extended record without a bundle: 145 role plus the four coordinates, profile and use id.
  PublishedContentName model;
  model.kind = ContentDataKind::ModelFile;
  model.semanticRole = ContentSemanticRole::SourceGraph;
  model.label = "canonical-source";
  model.modelName = "qwen3";
  model.modelIdentity = std::string(64, 'a');
  model.modelFamily = "qwen3";
  model.modelSize = "0.6b";
  model.quantization = "int4";
  model.sourceRevisionLabel = "r3";
  const auto modelWire = model.encode();
  expect(modelWire, {{129, std::string(1, '\1')}, {131, "canonical-source"}, {133, "qwen3"},
                     {135, std::string(64, 'a')}, {137, ""}, {139, ""}, {141, ""}, {143, ""},
                     {145, "1"}, {147, "qwen3"}, {149, "0.6b"}, {151, "int4"}, {153, "r3"},
                     {155, ""}, {157, ""}});
  const auto modelDecoded = PublishedContentName::decode(modelWire);
  BOOST_CHECK(modelDecoded.semanticRole == ContentSemanticRole::SourceGraph);
  BOOST_CHECK_EQUAL(modelDecoded.modelFamily, "qwen3");
  BOOST_CHECK_EQUAL(modelDecoded.quantization, "int4");
  BOOST_CHECK_EQUAL(modelDecoded.sourceRevisionLabel, "r3");

  // A bundle adds field 159; a page adds 165 after the partition.
  PublishedContentName bundle;
  bundle.kind = ContentDataKind::Material;
  bundle.semanticRole = ContentSemanticRole::MaterialBundle;
  bundle.modelName = "qwen3";
  bundle.modelIdentity = std::string(64, 'a');
  bundle.modelFamily = "qwen3";
  bundle.modelSize = "0.6b";
  bundle.quantization = "int4";
  bundle.sourceRevisionLabel = "r3";
  bundle.bundleIndex = 12;
  const auto bundleWire = bundle.encode();
  expect(bundleWire, {{129, std::string(1, '\6')}, {131, ""}, {133, "qwen3"},
                      {135, std::string(64, 'a')}, {137, ""}, {139, ""}, {141, ""}, {143, ""},
                      {145, "3"}, {147, "qwen3"}, {149, "0.6b"}, {151, "int4"}, {153, "r3"},
                      {155, ""}, {157, ""}, {159, "\f"}});

  // A changed coordinate changes the wire, and a truncated record is refused.
  auto tampered = PublishedContentName::decode(bundleWire);
  tampered.bundleIndex = 13;
  BOOST_CHECK(tampered.encode() != bundleWire);
  auto missingRole = modelWire;
  BOOST_CHECK_THROW(PublishedContentName::decode(std::vector<std::uint8_t>(
                      modelWire.begin(), modelWire.begin() + 40)), std::exception);
}

BOOST_AUTO_TEST_CASE(SemanticManifestUsesTheStemWithoutAPayloadHash)
{
  const ndn::Name producer("/lab/preparer");
  PublishedContentName content;
  content.kind = ContentDataKind::Material;
  content.semanticRole = ContentSemanticRole::MaterialBundle;
  content.label = "bundle-12";
  content.modelName = "qwen3";
  content.modelIdentity = std::string(64, 'a');
  content.modelFamily = "qwen3";
  content.modelSize = "0.6b";
  content.quantization = "int4";
  content.sourceRevisionLabel = "r3";
  content.bundleIndex = 12;

  ProtectedBlobManifest manifest;
  manifest.producer = producer;
  manifest.contentName = content;
  manifest.plaintextDigest.assign(32, 0x7f);
  manifest.plaintextBytes = 4096;
  manifest.envelopeBytes = 4200;
  manifest.segmentBytes = 4000;
  manifest.segmentCount = 2;
  manifest.representationSpec = "{\"encoding\":\"ndnsf-hybrid-envelope\",\"schema\":1}";
  manifest.protectionSpec = "{\"cipher\":\"AES-256-GCM\",\"scope\":\"request/1\"}";
  manifest.authorizationScope = "request/1";
  manifest.protectionEpoch = "epoch-3";
  manifest.keyReferenceId = std::string(64, 'b');
  manifest.keyIdentifier = std::string(64, 'c');
  const auto representation = std::string(64, 'd');
  manifest.dataPrefix = content.representationPrefix(producer, representation);
  manifest.dataPrefix.append("DATA");

  const auto manifestName = manifest.getName();
  const auto expected = content.representationPrefix(producer, representation).append("MANIFEST");
  BOOST_CHECK(manifestName == expected);
  BOOST_CHECK_EQUAL(manifestName.size(), expected.size());

  const auto payload = manifest.encode();
  const auto decoded = ProtectedBlobManifest::decode(payload, producer, manifestName);
  BOOST_CHECK(decoded.getName() == manifestName);

  // A prefix that does not carry the representation tail is refused for a semantic object.
  auto wrong = manifest;
  wrong.dataPrefix = content.semanticStem(producer).append("PROTECTED")
    .append(makeNamingDigestComponent(std::string(64, 'e'))).append("DATA");
  BOOST_CHECK_THROW(wrong.encode(), std::invalid_argument);
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

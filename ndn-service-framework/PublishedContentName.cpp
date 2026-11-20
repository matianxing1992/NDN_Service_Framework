#include "PublishedContentName.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <array>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <vector>

namespace ndn_service_framework {
namespace {

bool atom(const std::string& value)
{
  return !value.empty() && std::all_of(value.begin(), value.end(), [](unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '-' || c == '=' || c == '.';
  });
}

std::string quotedLabel(const std::string& value)
{
  makeNamingLabelComponent(value); // Valid UTF-8; control characters are rejected.
  std::string result = "\"";
  for (const char c : value) {
    if (c == '"' || c == '\\') result += '\\';
    result += c;
  }
  return result + "\"";
}

bool isModelCoordinateKind(ContentDataKind kind)
{
  return kind == ContentDataKind::ModelFile || kind == ContentDataKind::Weights ||
    kind == ContentDataKind::Initializer || kind == ContentDataKind::Tokenizer ||
    kind == ContentDataKind::ArtifactFile || kind == ContentDataKind::Material ||
    kind == ContentDataKind::Assignment;
}

/** N4 rows that name a `<ModelPath>`; a fixed assignment and a plain input do not carry one. */
bool roleNeedsModelPath(ContentSemanticRole role)
{
  return role != ContentSemanticRole::None && role != ContentSemanticRole::FixedAssignment &&
    role != ContentSemanticRole::SemanticInput && role != ContentSemanticRole::BlobObject;
}

bool roleAllowed(ContentDataKind kind, ContentSemanticRole role)
{
  switch (role) {
    case ContentSemanticRole::None: return true;
    case ContentSemanticRole::SourceGraph:
    case ContentSemanticRole::Initializers:
    case ContentSemanticRole::PreparedMetadata:
    case ContentSemanticRole::ModelManifest:
    case ContentSemanticRole::AssemblyReceipt:
      return kind == ContentDataKind::ModelFile;
    case ContentSemanticRole::WeightsObject: return kind == ContentDataKind::Weights;
    case ContentSemanticRole::TokenizerObject: return kind == ContentDataKind::Tokenizer;
    case ContentSemanticRole::MaterialBundle:
    case ContentSemanticRole::MaterialManifest:
      return kind == ContentDataKind::Material;
    case ContentSemanticRole::ManifestPage:
      // A page belongs to whichever business manifest owns the stem: model or material.
      return kind == ContentDataKind::Material || kind == ContentDataKind::ModelFile;
    case ContentSemanticRole::ArtifactFileObject:
    case ContentSemanticRole::ArtifactManifest:
      return kind == ContentDataKind::ArtifactFile;
    case ContentSemanticRole::AuthorizationManifest:
    case ContentSemanticRole::FixedAssignment:
    case ContentSemanticRole::CanonicalCapsule:
    case ContentSemanticRole::StoredCapsule:
      return kind == ContentDataKind::Assignment;
    case ContentSemanticRole::SemanticInput: return kind == ContentDataKind::Input;
    case ContentSemanticRole::BlobObject: return kind == ContentDataKind::Blob;
  }
  return false;
}

/** Every N4 coordinate fact is an atom or the explicit word that stands for a missing fact. */
void requireCoordinate(const std::string& value, const char* field)
{
  if (!atom(value))
    throw std::invalid_argument(std::string("invalid semantic coordinate: ") + field);
}

bool roleNeedsLabel(ContentSemanticRole role)
{
  switch (role) {
    case ContentSemanticRole::FixedAssignment:
    case ContentSemanticRole::WeightsObject:
    case ContentSemanticRole::TokenizerObject:
    case ContentSemanticRole::ArtifactFileObject:
    case ContentSemanticRole::SemanticInput:
    case ContentSemanticRole::BlobObject:
      return true;
    default:
      return false;
  }
}

void validateSemanticCoordinates(const PublishedContentName& value)
{
  if (roleNeedsLabel(value.semanticRole) && value.label.empty())
    throw std::invalid_argument("semantic role requires a label");
  if (!roleAllowed(value.kind, value.semanticRole))
    throw std::invalid_argument("semantic role does not belong to this content kind");
  const bool model = isModelCoordinateKind(value.kind);
  const bool hasPath = !value.modelFamily.empty() || !value.modelSize.empty() ||
    !value.quantization.empty() || !value.sourceRevisionLabel.empty();
  if (!model && hasPath)
    throw std::invalid_argument("model path on a non-model object");
  if (roleNeedsModelPath(value.semanticRole)) {
    // A semantic role promises the N4 row, so it must carry the coordinates that row names.
    requireCoordinate(value.modelFamily, "modelFamily");
    requireCoordinate(value.modelSize, "modelSize");
    requireCoordinate(value.quantization, "quantization");
    requireCoordinate(value.sourceRevisionLabel, "sourceRevisionLabel");
  }
  if (value.bundleIndex.has_value() != (value.semanticRole == ContentSemanticRole::MaterialBundle))
    throw std::invalid_argument("bundle index belongs only to a physical material bundle");
  if (value.pageNumber.has_value() != (value.semanticRole == ContentSemanticRole::ManifestPage))
    throw std::invalid_argument("page number belongs only to a manifest page");
  const bool artifact = value.semanticRole == ContentSemanticRole::ArtifactFileObject ||
    value.semanticRole == ContentSemanticRole::ArtifactManifest;
  if (!artifact && !value.profileLabel.empty())
    throw std::invalid_argument("profile label on a non-artifact object");
  if (artifact)
    requireCoordinate(value.profileLabel, "profileLabel");
  const bool capsule = value.semanticRole == ContentSemanticRole::AuthorizationManifest ||
    value.semanticRole == ContentSemanticRole::CanonicalCapsule ||
    value.semanticRole == ContentSemanticRole::StoredCapsule;
  if (!capsule && !value.useId.empty())
    throw std::invalid_argument("use id on an object that has no current use");
  if (capsule)
    requireCoordinate(value.useId, "useId");
}


struct IdentityParts {
  std::string nameSpace;
  std::string category;
  std::string leaf;
  std::string parent;
};

IdentityParts parts(const PublishedContentName& value)
{
  // A label is validated when present; the roles that name an object with it require one below.
  if (!value.label.empty())
    makeNamingLabelComponent(value.label);
  IdentityParts result{"NDNSF-DI", "model", "", "{}"};
  bool model = true;
  switch (value.kind) {
    case ContentDataKind::Blob: result.nameSpace = "NDNSF"; result.category = "blob"; model = false; break;
    case ContentDataKind::Material: result.category = "material"; model = false; break;
    case ContentDataKind::Input: result.category = "input"; model = false; break;
    case ContentDataKind::Assignment: result.category = "assignment"; model = false; break;
    case ContentDataKind::ModelFile: result.leaf = "file"; break;
    case ContentDataKind::Weights: result.leaf = "weights"; break;
    case ContentDataKind::Initializer: result.leaf = "initializer"; break;
    case ContentDataKind::Tokenizer: result.leaf = "tokenizer"; break;
    case ContentDataKind::ArtifactFile: result.leaf = "file"; break;
    default: throw std::invalid_argument("unknown content data kind");
  }
  if (value.kind == ContentDataKind::Assignment) {
    makeNamingLabelComponent(value.modelName);
    makeNamingDigestComponent(value.modelIdentity);
    if (!value.artifactKind.empty() || !value.coordinate.empty() ||
        !value.profileIdentity.empty() || !value.inputKind.empty())
      throw std::invalid_argument("unexpected assignment metadata");
    result.parent = "{\"modelIdentity\":\"" + value.modelIdentity +
      "\",\"modelName\":" + quotedLabel(value.modelName) + "}";
    return result;
  }
  if (model) {
    makeNamingLabelComponent(value.modelName);
    makeNamingDigestComponent(value.modelIdentity);
    if (!value.inputKind.empty()) throw std::invalid_argument("model has input kind");
    const bool structured = value.hasSemanticCoordinates() &&
      (value.semanticRole == ContentSemanticRole::ArtifactFileObject ||
       value.semanticRole == ContentSemanticRole::ArtifactManifest);
    if (value.kind == ContentDataKind::ArtifactFile && !structured) {
      if (!atom(value.artifactKind) || !atom(value.coordinate))
        throw std::invalid_argument("invalid artifact kind/coordinate");
      makeNamingDigestComponent(value.profileIdentity);
    }
    else if (value.kind == ContentDataKind::ArtifactFile) {
      // The partition coordinates replace the legacy single-component kind/coordinate pair.
      if (!value.artifactKind.empty() || !value.coordinate.empty() || !value.profileIdentity.empty())
        throw std::invalid_argument("legacy artifact fields on a partitioned artifact");
    }
    else if (!value.artifactKind.empty() || !value.coordinate.empty() || !value.profileIdentity.empty()) {
      throw std::invalid_argument("unexpected artifact identity");
    }
    // Values below are validated ASCII atoms or full lowerhex digests; no escaping is needed.
    result.parent = "{\"coordinate\":\"" + value.coordinate + "\",\"kind\":\"" +
      value.artifactKind + "\",\"modelIdentity\":\"" + value.modelIdentity +
      "\",\"profile\":\"" + value.profileIdentity + "\"}";
    // Preserve the model subcategory in the semantic-object transcript.
    result.category += "/" + (value.kind == ContentDataKind::ArtifactFile ? "artifact/file" : result.leaf);
  }
  else {
    // A material or assignment row names a ModelPath, so the model facts are legal there; they are not
    // part of the legacy name shape, which contentPrefix still builds for these kinds.
    const bool coordinates = value.hasSemanticCoordinates();
    if ((!coordinates && (!value.modelName.empty() || !value.modelIdentity.empty())) ||
        !value.artifactKind.empty() || !value.coordinate.empty() || !value.profileIdentity.empty())
      throw std::invalid_argument("unexpected model identity");
    if (value.kind == ContentDataKind::Input) {
      if (!atom(value.inputKind)) throw std::invalid_argument("invalid input kind");
      result.parent = "{\"kind\":\"" + value.inputKind + "\"}";
    }
    else if (!value.inputKind.empty()) throw std::invalid_argument("unexpected input kind");
  }
  return result;
}

std::vector<uint8_t> raw(const std::string& value)
{
  return {value.begin(), value.end()};
}

} // namespace

namespace {

const char* partitionName(ArtifactPartitionKind kind)
{
  switch (kind) {
    case ArtifactPartitionKind::None: return "";
    case ArtifactPartitionKind::FullGraph: return "full-graph";
    case ArtifactPartitionKind::PipelineStage: return "pipeline-stage";
    case ArtifactPartitionKind::TensorParallel: return "tensor-parallel";
    case ArtifactPartitionKind::Hybrid: return "hybrid";
    case ArtifactPartitionKind::AutomaticGraphCut: return "automatic-graph-cut";
    case ArtifactPartitionKind::SemanticComponent: return "semantic-component";
  }
  throw std::invalid_argument("unknown artifact partition kind");
}

ArtifactPartitionKind parsePartitionName(const std::string& value)
{
  for (const auto kind : {ArtifactPartitionKind::FullGraph, ArtifactPartitionKind::PipelineStage,
                          ArtifactPartitionKind::TensorParallel, ArtifactPartitionKind::Hybrid,
                          ArtifactPartitionKind::AutomaticGraphCut,
                          ArtifactPartitionKind::SemanticComponent}) {
    if (value == partitionName(kind))
      return kind;
  }
  throw std::invalid_argument("unknown artifact partition name: " + value);
}

void validatePartition(const ArtifactPartition& partition)
{
  const auto layers = [&] {
    if (partition.layerBegin >= partition.layerEnd)
      throw std::invalid_argument("artifact layer interval must be non-empty and half-open");
  };
  const auto degree = [&] {
    if (partition.tensorDegree < 1 || partition.rank >= partition.tensorDegree)
      throw std::invalid_argument("artifact rank must be inside the tensor degree");
  };
  switch (partition.kind) {
    case ArtifactPartitionKind::None:
      if (partition.stageIndex != 0 || partition.layerBegin != 0 || partition.layerEnd != 0 ||
          partition.tensorDegree != 0 || partition.rank != 0 || partition.cutPrefixNodes != 0 ||
          !partition.graphSide.empty() || !partition.componentId.empty())
        throw std::invalid_argument("partition fields on an object without a partition");
      return;
    case ArtifactPartitionKind::FullGraph:
      if (partition.layerEnd != 0 || partition.tensorDegree != 0)
        throw std::invalid_argument("full graph partition carries no stage or degree");
      return;
    case ArtifactPartitionKind::PipelineStage:
      layers();
      if (partition.tensorDegree != 0)
        throw std::invalid_argument("pipeline stage partition carries no tensor degree");
      return;
    case ArtifactPartitionKind::TensorParallel:
      layers(); degree();
      return;
    case ArtifactPartitionKind::Hybrid:
      layers(); degree();
      return;
    case ArtifactPartitionKind::AutomaticGraphCut:
      if (partition.cutPrefixNodes == 0 || (partition.graphSide != "prefix" && partition.graphSide != "suffix"))
        throw std::invalid_argument("automatic graph cut needs a positive prefix count and a side");
      return;
    case ArtifactPartitionKind::SemanticComponent:
      if (!atom(partition.componentId))
        throw std::invalid_argument("semantic component needs a component id");
      return;
  }
}

} // namespace

ArtifactPartition
parsePartitionJson(const std::string& json)
{
  // The partition JSON is a closed, sorted-key object produced by partitionJson(); this reader accepts only
  // that shape and rebuilds the fields, so a hand-written or reordered object is rejected by the canonical
  // re-encode check afterwards.
  ArtifactPartition partition;
  const auto value = [&json](const std::string& key) -> std::string {
    const std::string prefix = "\"" + key + "\":\"";
    const auto at = json.find(prefix);
    if (at == std::string::npos)
      throw std::invalid_argument("partition json is missing " + key);
    const auto begin = at + prefix.size();
    const auto end = json.find('"', begin);
    if (end == std::string::npos)
      throw std::invalid_argument("partition json has an unterminated " + key);
    return json.substr(begin, end - begin);
  };
  if (json.empty() || json.front() != '{' || json.back() != '}')
    throw std::invalid_argument("partition json is not an object");
  partition.kind = parsePartitionName(value("kind"));
  partition.componentId = value("componentId");
  partition.graphSide = value("side");
  const auto number = [&](const std::string& key) -> std::uint64_t {
    const auto text = value(key);
    std::size_t consumed = 0;
    const auto parsed = std::stoull(text, &consumed);
    if (consumed != text.size() || (!text.empty() && text[0] == '+'))
      throw std::invalid_argument("partition json has a non-canonical number");
    return parsed;
  };
  partition.cutPrefixNodes = number("cut");
  partition.tensorDegree = number("degree");
  partition.layerBegin = number("layerBegin");
  partition.layerEnd = number("layerEnd");
  partition.rank = number("rank");
  partition.stageIndex = number("stage");
  return partition;
}

std::string
PublishedContentName::partitionJson() const
{
  validatePartition(partition);
  if (partition.kind == ArtifactPartitionKind::None)
    return {};
  // Sorted keys, decimal integers, no escaping beyond the validated atoms.
  std::string json = "{\"componentId\":\"" + partition.componentId +
    "\",\"cut\":\"" + std::to_string(partition.cutPrefixNodes) +
    "\",\"degree\":\"" + std::to_string(partition.tensorDegree) +
    "\",\"kind\":\"" + partitionName(partition.kind) +
    "\",\"layerBegin\":\"" + std::to_string(partition.layerBegin) +
    "\",\"layerEnd\":\"" + std::to_string(partition.layerEnd) +
    "\",\"rank\":\"" + std::to_string(partition.rank) +
    "\",\"side\":\"" + partition.graphSide +
    "\",\"stage\":\"" + std::to_string(partition.stageIndex) + "\"}";
  return json;
}

std::string
PublishedContentName::objectIdentity() const
{
  const auto identity = parts(*this);
  validateSemanticCoordinates(*this);
  std::vector<NamingDigestField> fields{
    {1, 2, raw(identity.nameSpace)}, {2, 2, raw(identity.category)},
    {3, 2, raw(label)}, {4, 5, raw(identity.parent)}};
  if (hasSemanticCoordinates()) {
    // Coordinate facts are part of the object identity, not decoration on the name.
    fields.push_back({5, 2, raw(std::to_string(static_cast<std::uint8_t>(semanticRole)))});
    fields.push_back({6, 2, raw(modelFamily)});
    fields.push_back({7, 2, raw(modelSize)});
    fields.push_back({8, 2, raw(quantization)});
    fields.push_back({9, 2, raw(sourceRevisionLabel)});
    fields.push_back({10, 2, raw(bundleIndex ? std::to_string(*bundleIndex) : std::string())});
    fields.push_back({11, 2, raw(profileLabel)});
    fields.push_back({12, 2, raw(useId)});
  }
  return computeNamingDigest("ndnsf-naming/semantic-object", fields);
}

ndn::Name
PublishedContentName::semanticStem(const ndn::Name& producer) const
{
  if (semanticRole == ContentSemanticRole::None)
    throw std::invalid_argument("semantic stem requires an explicit semantic role");
  validateSemanticCoordinates(*this);
  const auto require = [this](bool condition, const char* field) {
    if (!condition)
      throw std::invalid_argument(std::string("semantic role requires ") + field);
  };
  ndn::Name name(producer);
  const auto push = [&name](const std::string& value) {
    name.append(ndn::name::Component(value));
  };
  const auto pushComponent = [&name](const ndn::name::Component& component) {
    name.append(component);
  };
  const auto pushPath = [&] {
    for (const auto& component : modelPathComponents())
      push(component);
  };
  switch (semanticRole) {
    case ContentSemanticRole::SourceGraph:
      push("NDNSF-DI"); push("model"); pushPath(); push("source"); push("graph");
      break;
    case ContentSemanticRole::Initializers:
      push("NDNSF-DI"); push("model"); pushPath(); push("source"); push("initializers");
      break;
    case ContentSemanticRole::PreparedMetadata:
      push("NDNSF-DI"); push("model"); pushPath(); push("metadata");
      break;
    case ContentSemanticRole::ModelManifest:
      push("NDNSF-DI"); push("model"); pushPath(); push("manifest");
      break;
    case ContentSemanticRole::WeightsObject:
      push("NDNSF-DI"); push("model"); pushPath(); push("source"); push("weights");
      pushComponent(makeNamingLabelComponent(label));
      break;
    case ContentSemanticRole::TokenizerObject:
      push("NDNSF-DI"); push("model"); pushPath(); push("tokenizer");
      pushComponent(makeNamingLabelComponent(label));
      break;
    case ContentSemanticRole::AssemblyReceipt:
      push("NDNSF-DI"); push("receipt"); pushPath(); push("assembly");
      break;
    case ContentSemanticRole::MaterialBundle:
      require(bundleIndex.has_value(), "a bundle number");
      push("NDNSF-DI"); push("material"); pushPath(); push("bundle");
      pushComponent(makeNamingDecimalComponent(*bundleIndex));
      break;
    case ContentSemanticRole::MaterialManifest:
      push("NDNSF-DI"); push("material"); pushPath(); push("manifest");
      break;
    case ContentSemanticRole::AuthorizationManifest:
      push("NDNSF-DI"); push("authorization"); pushPath(); push("use");
      pushComponent(makeNamingLabelComponent(useId));
      break;
    case ContentSemanticRole::FixedAssignment:
      push("NDNSF-DI"); push("assignment"); pushPath(); push("role");
      pushComponent(makeNamingLabelComponent(label));
      break;
    case ContentSemanticRole::CanonicalCapsule:
      push("NDNSF-DI"); push("key-capsule"); pushPath(); push("source"); push("canonical");
      push("use"); pushComponent(makeNamingLabelComponent(useId));
      break;
    case ContentSemanticRole::StoredCapsule:
      push("NDNSF-DI"); push("key-capsule"); pushPath(); push("source"); push("stored");
      push("use"); pushComponent(makeNamingLabelComponent(useId));
      break;
    case ContentSemanticRole::SemanticInput:
      require(!inputKind.empty(), "an input kind");
      push("NDNSF-DI"); push("input"); push(inputKind); pushComponent(makeNamingLabelComponent(label));
      break;
    case ContentSemanticRole::BlobObject:
      push("NDNSF"); push("blob"); pushComponent(makeNamingLabelComponent(label));
      break;
    case ContentSemanticRole::ArtifactFileObject:
    case ContentSemanticRole::ArtifactManifest: {
      // <ArtifactStem> = <Producer>/NDNSF-DI/artifact/<ModelPath>/<PartitionCoordinate>/entry/<Entry>
      //                 /profile/<ProfileLabel>
      if (partition.kind == ArtifactPartitionKind::None)
        throw std::invalid_argument("artifact role requires a partition coordinate");
      if (artifactEntry.empty() || !atom(artifactEntry))
        throw std::invalid_argument("artifact role requires an entry label");
      validatePartition(partition);
      push("NDNSF-DI"); push("artifact"); pushPath();
      switch (partition.kind) {
        case ArtifactPartitionKind::FullGraph:
          push("graph"); push("full");
          break;
        case ArtifactPartitionKind::PipelineStage:
          push("pp"); push("stage"); push(std::to_string(partition.stageIndex));
          push("layers"); push(std::to_string(partition.layerBegin));
          push(std::to_string(partition.layerEnd));
          break;
        case ArtifactPartitionKind::TensorParallel:
          push("tp"); push("layers"); push(std::to_string(partition.layerBegin));
          push(std::to_string(partition.layerEnd)); push("degree");
          push(std::to_string(partition.tensorDegree)); push("rank");
          push(std::to_string(partition.rank));
          break;
        case ArtifactPartitionKind::Hybrid:
          push("hybrid"); push("stage"); push(std::to_string(partition.stageIndex));
          push("layers"); push(std::to_string(partition.layerBegin));
          push(std::to_string(partition.layerEnd)); push("degree");
          push(std::to_string(partition.tensorDegree)); push("rank");
          push(std::to_string(partition.rank));
          break;
        case ArtifactPartitionKind::AutomaticGraphCut:
          push("graph"); push("automatic"); push("cut");
          push(std::to_string(partition.cutPrefixNodes)); push("side");
          push(partition.graphSide);
          break;
        case ArtifactPartitionKind::SemanticComponent:
          push("graph"); push("semantic"); push("component"); push(partition.componentId);
          break;
        case ArtifactPartitionKind::None:
          throw std::invalid_argument("artifact role requires a partition coordinate");
      }
      push("entry"); push(artifactEntry); push("profile"); push(profileLabel);
      if (semanticRole == ContentSemanticRole::ArtifactManifest)
        push("manifest");
      else
        push("file"); pushComponent(makeNamingLabelComponent(label));
      break;
    }
    case ContentSemanticRole::ManifestPage:
      // <ManifestStem>/page/<PageNumber>; the stem is the business manifest's prefix without `/rep/<R>`.
      require(pageNumber.has_value(), "a page number");
      if (kind == ContentDataKind::Material) {
        push("NDNSF-DI"); push("material"); pushPath(); push("manifest");
      }
      else {
        push("NDNSF-DI"); push("model"); pushPath(); push("manifest");
      }
      push("page"); pushComponent(makeNamingDecimalComponent(*pageNumber));
      break;
    case ContentSemanticRole::None:
      throw std::invalid_argument("semantic stem requires an explicit semantic role");
  }
  checkNamingNameSize(name);
  return name;
}

ndn::Name
PublishedContentName::representationPrefix(const ndn::Name& producer,
                                          const std::string& representationId) const
{
  auto name = semanticStem(producer);
  // One 32-byte representation component; not an ImplicitSha256DigestComponent.
  name.append(ndn::name::Component("rep"));
  auto component = makeNamingDigestComponent(representationId);
  if (component.value_size() != 32)
    throw std::invalid_argument("representation identity must be one sha256 digest");
  name.append(component);
  checkNamingNameSize(ndn::Name(name).append("DATA").appendSegment(
    std::numeric_limits<std::uint64_t>::max()));
  return name;
}

std::vector<std::string>
PublishedContentName::modelPathComponents() const
{
  validateSemanticCoordinates(*this);
  if (!isModelCoordinateKind(kind))
    throw std::invalid_argument("model path is not defined for this content kind");
  const auto component = [](const std::string& value) {
    return value.empty() ? std::string("unspecified") : value;
  };
  return {component(modelFamily), component(modelSize), component(quantization),
          "revision", component(sourceRevisionLabel)};
}

bool
PublishedContentName::hasSemanticCoordinates() const
{
  return semanticRole != ContentSemanticRole::None || !modelFamily.empty() || !modelSize.empty() ||
    !quantization.empty() || !sourceRevisionLabel.empty() || bundleIndex.has_value() ||
    !profileLabel.empty() || !useId.empty() || !artifactEntry.empty() ||
    partition.kind != ArtifactPartitionKind::None || pageNumber.has_value();
}

ndn::Name PublishedContentName::contentPrefix(const ndn::Name& producer,
                                             const std::string& plaintextDigest) const
{
  const auto identity = parts(*this);
  if (hasSemanticCoordinates()) {
    // C1: the semantic stem ends before `/rep/<RepresentationId>`; the publisher adds the tail once.
    return semanticStem(producer);
  }
  if (kind == ContentDataKind::Assignment) {
    auto name = makeProducerDataPrefix(producer, "NDNSF-DI", "assignment");
    name.append("model").append(makeNamingLabelComponent(modelName))
      .append("ROLE").append(makeNamingLabelComponent(label))
      .append(makeNamingDigestComponent(plaintextDigest));
    checkNamingNameSize(name);
    return name;
  }
  const bool model = !identity.leaf.empty();
  auto name = makeProducerDataPrefix(producer, identity.nameSpace,
                                     model ? "model" : identity.category);
  if (model) {
    name.append(makeNamingLabelComponent(modelName)).append(makeNamingDigestComponent(modelIdentity));
    if (kind == ContentDataKind::ArtifactFile) {
      name.append("artifact").append(ndn::name::Component(artifactKind))
        .append(ndn::name::Component(coordinate)).append(makeNamingDigestComponent(profileIdentity));
    }
    name.append(ndn::name::Component(identity.leaf));
  }
  else if (kind == ContentDataKind::Input) name.append(ndn::name::Component(inputKind));
  name.append(makeNamingLabelComponent(label)).append(makeNamingDigestComponent(objectIdentity()))
    .append(makeNamingDigestComponent(plaintextDigest));
  checkNamingNameSize(name);
  return name;
}

std::vector<uint8_t>
PublishedContentName::encode() const
{
  parts(*this);
  validateSemanticCoordinates(*this);
  ndn::Block body(244);
  body.push_back(ndn::makeNonNegativeIntegerBlock(129, static_cast<uint64_t>(kind)));
  const std::array<const std::string*, 7> fields{
    &label, &modelName, &modelIdentity, &artifactKind, &coordinate, &profileIdentity, &inputKind};
  for (size_t i = 0; i < fields.size(); ++i)
    body.push_back(ndn::makeStringBlock(131 + 2 * i, *fields[i]));
  if (hasSemanticCoordinates()) {
    // Appended after the legacy block so an instance without coordinates keeps exactly today's wire.
    body.push_back(ndn::makeNonNegativeIntegerBlock(
      145, static_cast<uint64_t>(semanticRole)));
    body.push_back(ndn::makeStringBlock(147, modelFamily));
    body.push_back(ndn::makeStringBlock(149, modelSize));
    body.push_back(ndn::makeStringBlock(151, quantization));
    body.push_back(ndn::makeStringBlock(153, sourceRevisionLabel));
    body.push_back(ndn::makeStringBlock(155, profileLabel));
    body.push_back(ndn::makeStringBlock(157, useId));
    if (bundleIndex)
      body.push_back(ndn::makeNonNegativeIntegerBlock(159, *bundleIndex));
    if (!artifactEntry.empty())
      body.push_back(ndn::makeStringBlock(161, artifactEntry));
    const auto partition = partitionJson();
    if (!partition.empty())
      body.push_back(ndn::makeStringBlock(163, partition));
    if (pageNumber)
      body.push_back(ndn::makeNonNegativeIntegerBlock(165, *pageNumber));
  }
  body.encode();
  if (body.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::length_error("semantic content metadata exceeds packet size");
  return {body.begin(), body.end()};
}

PublishedContentName PublishedContentName::decode(const std::vector<uint8_t>& wire)
{
  if (wire.empty() || wire.size() > ndn::MAX_NDN_PACKET_SIZE)
    throw std::invalid_argument("invalid semantic metadata size");
  ndn::Block body(ndn::span<const uint8_t>(wire.data(), wire.size()));
  if (body.type() != 244 || body.size() != wire.size())
    throw std::invalid_argument("invalid semantic metadata envelope");
  body.parse();
  const auto& fields = body.elements();
  const bool extended = fields.size() > 8;
  if (!extended && fields.size() != 8)
    throw std::invalid_argument("invalid semantic metadata field count");
  // Eight base fields plus seven coordinates; a material bundle adds the bundle index.
  if (extended && fields.size() < 15)
    throw std::invalid_argument("invalid semantic metadata field count");
  for (size_t i = 0; i < 8; ++i)
    if (fields[i].type() != 129 + 2 * i)
      throw std::invalid_argument("invalid semantic metadata field order");
  if (extended) {
    static const std::array<uint64_t, 7> extendedTypes{145, 147, 149, 151, 153, 155, 157};
    for (size_t i = 0; i < extendedTypes.size(); ++i)
      if (fields[8 + i].type() != extendedTypes[i])
        throw std::invalid_argument("invalid semantic coordinate field order");
    // 145,147,149,151,153,155,157 then optional 159 bundle, 161 entry, 163 partition, 165 page,
    // each in that order; anything else is a malformed record.
    std::size_t index = 15;
    for (const uint64_t type : {159, 161, 163, 165}) {
      if (index < fields.size() && fields[index].type() == type)
        ++index;
    }
    if (index != fields.size())
      throw std::invalid_argument("invalid semantic coordinate tail");
  }
  const auto kindValue = ndn::readNonNegativeInteger(fields[0]);
  if (kindValue > 8) throw std::invalid_argument("unknown semantic metadata kind");
  PublishedContentName result;
  result.kind = static_cast<ContentDataKind>(kindValue);
  const std::array<std::string*, 7> outputs{
    &result.label, &result.modelName, &result.modelIdentity, &result.artifactKind,
    &result.coordinate, &result.profileIdentity, &result.inputKind};
  for (size_t i = 0; i < outputs.size(); ++i) {
    const auto& field = fields[i + 1];
    outputs[i]->assign(reinterpret_cast<const char*>(field.value()), field.value_size());
  }
  if (extended) {
    const auto roleValue = ndn::readNonNegativeInteger(fields[8]);
    if (roleValue > 18) throw std::invalid_argument("unknown semantic role");
    result.semanticRole = static_cast<ContentSemanticRole>(roleValue);
    std::array<std::string*, 4> coordinates{
      &result.modelFamily, &result.modelSize, &result.quantization, &result.sourceRevisionLabel};
    for (size_t i = 0; i < coordinates.size(); ++i) {
      const auto& field = fields[9 + i];
      coordinates[i]->assign(reinterpret_cast<const char*>(field.value()), field.value_size());
    }
    const auto& profile = fields[13];
    result.profileLabel.assign(reinterpret_cast<const char*>(profile.value()), profile.value_size());
    const auto& use = fields[14];
    result.useId.assign(reinterpret_cast<const char*>(use.value()), use.value_size());
    std::size_t tail = 15;
    if (tail < fields.size() && fields[tail].type() == 159) {
      result.bundleIndex = ndn::readNonNegativeInteger(fields[tail]);
      ++tail;
    }
    if (tail < fields.size() && fields[tail].type() == 161) {
      result.artifactEntry.assign(reinterpret_cast<const char*>(fields[tail].value()),
                                  fields[tail].value_size());
      ++tail;
    }
    if (tail < fields.size() && fields[tail].type() == 163) {
      result.partition = parsePartitionJson(
        std::string(reinterpret_cast<const char*>(fields[tail].value()), fields[tail].value_size()));
      ++tail;
    }
    if (tail < fields.size() && fields[tail].type() == 165) {
      result.pageNumber = ndn::readNonNegativeInteger(fields[tail]);
      ++tail;
    }
  }
  if (result.encode() != wire) throw std::invalid_argument("noncanonical semantic metadata");
  return result;
}

} // namespace ndn_service_framework

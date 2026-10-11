#include "PublishedContentName.hpp"
#include "ProducerDataNaming.hpp"

#include <ndn-cxx/encoding/block-helpers.hpp>
#include <array>

#include <algorithm>
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

struct IdentityParts {
  std::string nameSpace;
  std::string category;
  std::string leaf;
  std::string parent;
};

IdentityParts parts(const PublishedContentName& value)
{
  makeNamingLabelComponent(value.label); // Reject invalid full labels, not just their prefix.
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
    if (value.kind == ContentDataKind::ArtifactFile) {
      if (!atom(value.artifactKind) || !atom(value.coordinate))
        throw std::invalid_argument("invalid artifact kind/coordinate");
      makeNamingDigestComponent(value.profileIdentity);
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
    if (!value.modelName.empty() || !value.modelIdentity.empty() || !value.artifactKind.empty() ||
        !value.coordinate.empty() || !value.profileIdentity.empty())
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

std::string PublishedContentName::objectIdentity() const
{
  const auto identity = parts(*this);
  return computeNamingDigest("ndnsf-naming/semantic-object",
    {{1, 2, raw(identity.nameSpace)}, {2, 2, raw(identity.category)},
     {3, 2, raw(label)}, {4, 5, raw(identity.parent)}});
}

ndn::Name PublishedContentName::contentPrefix(const ndn::Name& producer,
                                             const std::string& plaintextDigest) const
{
  const auto identity = parts(*this);
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

std::vector<uint8_t> PublishedContentName::encode() const
{
  parts(*this);
  ndn::Block body(244);
  body.push_back(ndn::makeNonNegativeIntegerBlock(129, static_cast<uint64_t>(kind)));
  const std::array<const std::string*, 7> fields{
    &label, &modelName, &modelIdentity, &artifactKind, &coordinate, &profileIdentity, &inputKind};
  for (size_t i = 0; i < fields.size(); ++i)
    body.push_back(ndn::makeStringBlock(131 + 2 * i, *fields[i]));
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
  if (fields.size() != 8) throw std::invalid_argument("invalid semantic metadata field count");
  for (size_t i = 0; i < fields.size(); ++i)
    if (fields[i].type() != 129 + 2 * i)
      throw std::invalid_argument("invalid semantic metadata field order");
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
  if (result.encode() != wire) throw std::invalid_argument("noncanonical semantic metadata");
  return result;
}

} // namespace ndn_service_framework

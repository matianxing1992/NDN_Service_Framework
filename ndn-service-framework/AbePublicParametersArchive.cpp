#include "AbePublicParametersArchive.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <string_view>

#include <ndn-cxx/util/sha256.hpp>

namespace ndn_service_framework {
namespace {

constexpr char ENVELOPE_SCHEMA[] = "NDNSF-HIST-PUBPARAMS-v1";

bool
isDigestHex(const std::string& value)
{
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

std::string
digestOf(std::string_view bytes)
{
  ndn::util::Sha256 digest;
  if (!bytes.empty())
    digest << std::string(bytes);
  auto hex = digest.toString();
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return hex;
}

} // namespace

ndn::Name
historicalPublicParametersName(const ndn::Name& identity, uint64_t generation, uint64_t epoch,
                               const std::string& digestHex, uint64_t writerGeneration)
{
  if (generation == 0)
    throw std::invalid_argument("historical public parameters need a generation");
  if (!isDigestHex(digestHex))
    throw std::invalid_argument("historical public parameters need a content digest");
  ndn::Name name(identity);
  name.append("NDNSF").append("HISTORY").append("PUBPARAMS");
  name.append(std::to_string(generation));
  name.append(std::to_string(epoch));
  std::string canonicalDigest = digestHex;
  std::transform(canonicalDigest.begin(), canonicalDigest.end(), canonicalDigest.begin(),
                 [] (unsigned char value) { return static_cast<char>(std::tolower(value)); });
  name.append(canonicalDigest);
  if (writerGeneration != 0)
    name.append("WRITER").appendVersion(writerGeneration);
  return name;
}

ndn::Name
historicalPublicParametersIndexName(const ndn::Name& identity, uint64_t writerGeneration,
                                    const ControllerVersion& currentMaterial)
{
  if (writerGeneration == 0 || !currentMaterial.isValid())
    throw std::invalid_argument("historical public parameters index needs a writer and material");
  return ndn::Name(identity).append("NDNSF").append("HISTORY").append("PUBPARAMS")
    .append("INDEX").appendVersion(writerGeneration)
    .appendVersion(currentMaterial.controllerGenerationTimestamp)
    .appendVersion(currentMaterial.controllerEpoch);
}

bool
buildHistoricalPublicParametersData(const ControllerGenerationStore::AbeHistoryEntry& entry,
                                    const ndn::Name& identity,
                                    const std::function<void(ndn::Data&)>& sign,
                                    ndn::Data& result, uint64_t writerGeneration)
{
  if (entry.generationTimestamp == 0 || entry.publicParameters.empty() ||
      entry.publicParametersName.empty())
    return false;
  const auto stored = entry.publicParametersWireDigest;
  if (stored.size() != 71 || stored.compare(0, 7, "sha256:") != 0 || !isDigestHex(stored.substr(7)))
    return false;
  std::string hex = stored.substr(7);
  std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  // Bind what we publish to what we archived: a mismatch means the record is damaged, and a proof
  // that does not match its own digest is worse than no proof.  The stored digest may be upper case,
  // because it was produced by a different helper than the one used here.
  if (digestOf(entry.publicParameters) != hex)
    return false;

  std::string envelope(ENVELOPE_SCHEMA);
  envelope += "\noriginal ";
  envelope += entry.publicParametersName;
  envelope += "\ndigest ";
  envelope += stored;
  envelope += "\nlength ";
  envelope += std::to_string(entry.publicParameters.size());
  envelope += "\n";
  envelope += entry.publicParameters;

  result.setName(historicalPublicParametersName(identity, entry.generationTimestamp, entry.epoch,
                                                hex, writerGeneration));
  result.setContent(ndn::Buffer(reinterpret_cast<const uint8_t*>(envelope.data()),
                                envelope.size()));
  result.setFreshnessPeriod(ndn::time::milliseconds(0));
  if (sign)
    sign(result);
  return true;
}

std::vector<std::shared_ptr<ndn::Data>>
collectHistoricalPublicParameterAnswers(
  const std::vector<ControllerGenerationStore::AbeHistoryEntry>& entries,
  const ndn::Name& identity,
  const std::function<void(ndn::Data&)>& sign)
{
  std::vector<std::shared_ptr<ndn::Data>> answers;
  answers.reserve(entries.size());
  for (const auto& entry : entries) {
    try {
      auto answer = std::make_shared<ndn::Data>();
      if (!buildHistoricalPublicParametersData(entry, identity, sign, *answer))
        continue;
      answers.push_back(std::move(answer));
    }
    catch (const std::exception&) {
      // One generation that cannot be bound must not stop the others or take the Controller down.
      continue;
    }
  }
  return answers;
}

} // namespace ndn_service_framework

namespace ndn_service_framework {

namespace {
constexpr char INDEX_SCHEMA[] = "NDNSF-HIST-PUBPARAMS-INDEX-v1";
}

bool
buildHistoricalPublicParametersIndex(
  const std::vector<ControllerGenerationStore::AbeHistoryEntry>& entries,
  const ndn::Name& identity, const std::function<void(ndn::Data&)>& sign, ndn::Data& result)
{
  if (identity.empty())
    return false;
  std::string content(INDEX_SCHEMA);
  content += "\n";
  std::size_t listed = 0;
  for (const auto& entry : entries) {
    if (entry.generationTimestamp == 0 || entry.publicParameters.empty())
      continue;
    if (entry.publicParametersWireDigest.rfind("sha256:", 0) != 0 ||
        entry.publicParametersWireDigest.size() != 71)
      continue;
    // Publish the digest exactly as a proof name carries it: the proof lower-cases the stored value, so
    // an index written with the stored case sends readers to a name nothing answers.
    auto digestHex = entry.publicParametersWireDigest.substr(7);
    std::transform(digestHex.begin(), digestHex.end(), digestHex.begin(), [] (unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    content += std::to_string(entry.generationTimestamp) + " " + std::to_string(entry.epoch) + " " +
               digestHex + " " + entry.publicParametersName + "\n";
    ++listed;
  }
  if (listed == 0)
    return false;
  ndn::Name name(identity);
  name.append("NDNSF").append("HISTORY").append("PUBPARAMS");
  ndn::Data data(name);
  data.setContent(ndn::Buffer(reinterpret_cast<const uint8_t*>(content.data()), content.size()));
  data.setFreshnessPeriod(ndn::time::milliseconds(0));
  if (sign)
    sign(data);
  result = std::move(data);
  return true;
}

bool
parseHistoricalPublicParametersIndex(const std::string& content,
                                     std::vector<std::tuple<uint64_t, uint64_t, std::string,
                                                            std::string>>& records)
{
  records.clear();
  std::size_t begin = 0;
  bool first = true;
  while (begin <= content.size()) {
    const auto end = content.find('\n', begin);
    const std::string line = content.substr(begin, end == std::string::npos ? std::string::npos
                                                                           : end - begin);
    begin = end == std::string::npos ? content.size() + 1 : end + 1;
    if (line.empty())
      continue;
    if (first) {
      if (line != INDEX_SCHEMA)
        return false;
      first = false;
      continue;
    }
    std::istringstream fields(line);
    std::string generation, epoch, digest, original;
    if (!(fields >> generation >> epoch >> digest >> original))
      return false;
    if (digest.size() != 64 || original.empty() || original.front() != '/')
      return false;
    char* stop = nullptr;
    const auto gen = std::strtoull(generation.c_str(), &stop, 10);
    if (stop == generation.c_str() || *stop != '\0')
      return false;
    const auto ep = std::strtoull(epoch.c_str(), &stop, 10);
    if (stop == epoch.c_str() || *stop != '\0')
      return false;
    records.emplace_back(gen, ep, digest, original);
  }
  return !records.empty();
}

bool
parseHistoricalPublicParameters(const std::string& content, HistoricalPublicParameters& proof)
{
  std::size_t begin = 0;
  std::vector<std::string> lines;
  while (begin <= content.size() && lines.size() < 4) {
    const auto end = content.find('\n', begin);
    if (end == std::string::npos)
      return false;
    lines.push_back(content.substr(begin, end - begin));
    begin = end + 1;
  }
  if (lines.size() != 4 || lines[0] != "NDNSF-HIST-PUBPARAMS-v1")
    return false;
  const auto field = [] (const std::string& line, const char* prefix) -> std::string {
    const std::string head(prefix);
    if (line.compare(0, head.size(), head) != 0)
      throw std::runtime_error("missing field");
    return line.substr(head.size());
  };
  try {
    HistoricalPublicParameters parsed;
    parsed.originalName = field(lines[1], "original ");
    parsed.digest = field(lines[2], "digest ");
    const auto lengthText = field(lines[3], "length ");
    if (parsed.digest.size() != 71 || parsed.digest.rfind("sha256:", 0) != 0 ||
        parsed.originalName.empty() || parsed.originalName.front() != '/')
      return false;
    char* stop = nullptr;
    parsed.length = std::strtoull(lengthText.c_str(), &stop, 10);
    if (stop == lengthText.c_str() || *stop != '\0')
      return false;
    parsed.publicParameters = content.substr(begin, parsed.length);
    if (parsed.publicParameters.size() != parsed.length)
      return false;
    // The digest binds the bytes; a mismatch means the proof and its content disagree.
    ndn::util::Sha256 hash;
    hash << parsed.publicParameters;
    auto hex = hash.toString();
    std::transform(hex.begin(), hex.end(), hex.begin(), [] (unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    // The envelope carries whatever case the store used; compare canonically.
    auto declared = parsed.digest.substr(7);
    std::transform(declared.begin(), declared.end(), declared.begin(), [] (unsigned char value) {
      return static_cast<char>(std::tolower(value));
    });
    if (declared != hex)
      return false;
    proof = std::move(parsed);
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

} // namespace ndn_service_framework

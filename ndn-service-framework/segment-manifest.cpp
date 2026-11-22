#include "segment-manifest.hpp"

#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>

#include <algorithm>
#include <stdexcept>

namespace ndn_service_framework {
namespace {

constexpr uint32_t PAGE_SIZE = SegmentManifestVerifier::SOURCE_PACKETS_PER_PAGE;
constexpr uint8_t ROOT_MAGIC[] = {'N', 'D', 'S', 'M', 1};
constexpr uint8_t PAGE_MAGIC[] = {'N', 'D', 'S', 'I', 1};

void
require(bool condition, const char* reason)
{
  if (!condition) {
    throw std::invalid_argument(reason);
  }
}

void
checkScope(const SegmentManifestScope& scope)
{
  require(!scope.producer.empty() && scope.producer.size() < scope.objectPrefix.size() &&
          scope.producer.isPrefixOf(scope.objectPrefix), "segment-manifest-scope");
}

void
checkCount(size_t count, const SegmentManifestLimits& limits)
{
  require(count >= 2 && count <= SegmentManifestVerifier::MAX_SOURCE_PACKETS && count <= limits.maxSourceSegments &&
          (!limits.expectedSourceSegments || count == *limits.expectedSourceSegments),
          "segment-manifest-source-count");
}

void
addBytes(uint64_t& total, uint64_t bytes, uint64_t limit)
{
  require(total <= limit && bytes <= limit - total, "segment-manifest-wire-budget");
  total += bytes;
}

class Reader
{
public:
  explicit Reader(const ndn::Block& content)
    : m_input(content.value_bytes())
  {
  }

  ndn::span<const uint8_t>
  take(size_t size)
  {
    require(size <= m_input.size(), "segment-manifest-truncated");
    const auto part = m_input.first(size);
    m_input = m_input.subspan(size);
    return part;
  }

  template<typename T>
  T
  integer()
  {
    T value = 0;
    for (const auto byte : take(sizeof(T))) {
      value = static_cast<T>((value << 8) | byte);
    }
    return value;
  }

  void
  magic(ndn::span<const uint8_t> expected)
  {
    const auto actual = take(expected.size());
    require(std::equal(actual.begin(), actual.end(), expected.begin()), "segment-manifest-format");
  }

  ndn::Name
  name()
  {
    const auto encoded = take(integer<uint16_t>());
    const ndn::Block block(encoded);
    require(block.size() == encoded.size(), "segment-manifest-name-frame");
    return ndn::Name(block);
  }

  std::array<uint8_t, 32>
  digest()
  {
    std::array<uint8_t, 32> result{};
    const auto value = take(result.size());
    std::copy(value.begin(), value.end(), result.begin());
    return result;
  }

  void
  finish() const
  {
    require(m_input.empty(), "segment-manifest-trailing-bytes");
  }

private:
  ndn::span<const uint8_t> m_input;
};

template<typename T>
void
appendInteger(std::vector<uint8_t>& output, T value)
{
  for (size_t shift = sizeof(T); shift > 0; --shift) {
    output.push_back(static_cast<uint8_t>(value >> ((shift - 1) * 8)));
  }
}

void
appendName(std::vector<uint8_t>& output, const ndn::Name& name)
{
  const auto& encoded = name.wireEncode();
  require(encoded.size() <= ndn::MAX_NDN_PACKET_SIZE, "segment-manifest-name-size");
  appendInteger(output, static_cast<uint16_t>(encoded.size()));
  output.insert(output.end(), encoded.begin(), encoded.end());
}

void
checkWireSize(uint32_t size)
{
  require(size != 0 && size <= ndn::MAX_NDN_PACKET_SIZE, "segment-manifest-packet-size");
}

bool
hasCertificateSignature(const ndn::Data& data)
{
  return (data.getSignatureType() == ndn::tlv::SignatureSha256WithRsa ||
          data.getSignatureType() == ndn::tlv::SignatureSha256WithEcdsa) &&
         data.getSignatureInfo().hasKeyLocator();
}

} // namespace

ndn::Name
SegmentManifestScope::rootName() const
{
  return ndn::Name(objectPrefix).append("MANIFEST");
}

ndn::Name
SegmentManifestScope::pageName(uint32_t page) const
{
  return rootName().append("PAGE").append(std::to_string(page));
}

ndn::Name
SegmentManifestScope::sourceName(uint32_t segment) const
{
  return ndn::Name(objectPrefix).append("DATA").appendSegment(segment);
}

ManifestPublication::ManifestPublication(SegmentManifestScope scope)
  : m_scope(std::move(scope))
{
}

ManifestPublication
ManifestPublication::prepare(ndn::security::KeyChain& keyChain, const ndn::security::SigningInfo& signer,
                             const SegmentManifestScope& scope, const SegmentManifestLimits& limits,
                             ndn::span<const ndn::ConstBufferPtr> sources,
                             ndn::span<const uint8_t> applicationManifest,
                             ndn::time::milliseconds freshness)
{
  checkScope(scope);
  checkCount(sources.size(), limits);
  require(applicationManifest.size() <= ndn::MAX_NDN_PACKET_SIZE, "segment-manifest-app-size");
  const auto count = static_cast<uint32_t>(sources.size());
  const uint32_t pageCount = SegmentManifestVerifier::pageCountFor(count);
  ManifestPublication result(scope);
  result.m_packets.reserve(1 + pageCount + count);
  result.m_packets.emplace_back(scope.rootName());
  for (uint32_t page = 0; page < pageCount; ++page) {
    result.m_packets.emplace_back(scope.pageName(page));
  }

  const auto appendRecord = [] (auto& encoded, uint32_t index, const ndn::Data& data) {
    const auto entry = SegmentManifestVerifier::record(data);
    appendInteger(encoded, index);
    appendInteger(encoded, entry.wireSize);
    encoded.insert(encoded.end(), entry.digest.begin(), entry.digest.end());
  };

  uint64_t sourceWireBytes = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const auto& payload = sources[i];
    require(payload && payload->size() <= ndn::MAX_NDN_PACKET_SIZE,
            "segment-manifest-source-content-size");
    auto& data = result.m_packets.emplace_back(scope.sourceName(i));
    data.setFinalBlock(ndn::name::Component::fromSegment(count - 1));
    data.setFreshnessPeriod(freshness).setContent(payload);
    keyChain.sign(data, ndn::security::signingWithSha256());
    const auto size = data.wireEncode().size();
    checkWireSize(static_cast<uint32_t>(size));
    addBytes(sourceWireBytes, size, limits.maxWireBytes);
  }

  auto totalWireBytes = sourceWireBytes;
  for (uint32_t page = 0; page < pageCount; ++page) {
    const auto start = page * PAGE_SIZE;
    const auto size = std::min(PAGE_SIZE, count - start);
    std::vector<uint8_t> content(std::begin(PAGE_MAGIC), std::end(PAGE_MAGIC));
    appendInteger(content, page);
    appendInteger(content, start);
    appendInteger(content, static_cast<uint16_t>(size));
    for (uint32_t i = start; i < start + size; ++i) {
      appendRecord(content, i, result.m_packets[1 + pageCount + i]);
    }
    auto& data = result.m_packets[1 + page];
    data.setFreshnessPeriod(freshness).setContent(content);
    keyChain.sign(data, ndn::security::signingWithSha256());
    const auto wireSize = data.wireEncode().size();
    checkWireSize(static_cast<uint32_t>(wireSize));
    addBytes(totalWireBytes, wireSize, limits.maxWireBytes);
  }

  std::vector<uint8_t> content(std::begin(ROOT_MAGIC), std::end(ROOT_MAGIC));
  content.push_back(pageCount == 0 ? 0 : 1);
  appendName(content, scope.producer);
  appendName(content, scope.objectPrefix);
  content.insert(content.end(), scope.bindingDigest.begin(), scope.bindingDigest.end());
  appendInteger(content, count);
  appendInteger(content, sourceWireBytes);
  appendInteger(content, static_cast<uint32_t>(applicationManifest.size()));
  content.insert(content.end(), applicationManifest.begin(), applicationManifest.end());
  const auto records = pageCount == 0 ? count : pageCount;
  appendInteger(content, static_cast<uint16_t>(records));
  for (uint32_t i = 0; i < records; ++i) {
    appendRecord(content, i, result.m_packets[i + 1]);
  }

  auto& root = result.m_packets.front();
  root.setFreshnessPeriod(freshness).setContent(content);
  keyChain.sign(root, signer);
  require(hasCertificateSignature(root), "segment-manifest-root-signature");
  const auto wireSize = root.wireEncode().size();
  checkWireSize(static_cast<uint32_t>(wireSize));
  addBytes(totalWireBytes, wireSize, limits.maxWireBytes);
  return result;
}

SegmentManifestVerifier::SegmentManifestVerifier(ValidatedManifestRoot root,
                                                 const SegmentManifestScope& scope,
                                                 const SegmentManifestLimits& limits)
  : m_root(std::move(root))
  , m_scope(scope)
{
  checkScope(scope);
  requireCurrent();
  const auto& data = m_root.data();
  require(m_root.producer() == scope.producer && data.getName() == scope.rootName() &&
          !data.getFinalBlock() && hasCertificateSignature(data), "segment-manifest-root");
  const auto rootSize = data.wireEncode().size();
  checkWireSize(static_cast<uint32_t>(rootSize));
  Reader reader(data.getContent());
  reader.magic(ROOT_MAGIC);
  const auto layout = reader.integer<uint8_t>();
  require(reader.name() == scope.producer && reader.name() == scope.objectPrefix &&
          reader.digest() == scope.bindingDigest, "segment-manifest-binding");
  const auto count = reader.integer<uint32_t>();
  checkCount(count, limits);
  const bool paged = count > PAGE_SIZE;
  require(layout == (paged ? 1 : 0), "segment-manifest-layout");
  m_sourceWireBytes = reader.integer<uint64_t>();
  require(m_sourceWireBytes >= count &&
          m_sourceWireBytes <= uint64_t{count} * ndn::MAX_NDN_PACKET_SIZE,
          "segment-manifest-source-wire-size");
  auto totalWireBytes = m_sourceWireBytes;
  addBytes(totalWireBytes, rootSize, limits.maxWireBytes);
  m_applicationManifest = reader.take(reader.integer<uint32_t>());
  const auto records = reader.integer<uint16_t>();
  require(records == (paged ? pageCountFor(count) : count),
          "segment-manifest-record-count");

  m_sources.resize(count);
  for (uint32_t i = 0; i < records; ++i) {
    require(reader.integer<uint32_t>() == i, "segment-manifest-record-index");
    const Record entry{reader.integer<uint32_t>(), reader.digest()};
    checkWireSize(entry.wireSize);
    if (paged) {
      addBytes(totalWireBytes, entry.wireSize, limits.maxWireBytes);
      m_pages.push_back(entry);
    }
    else {
      addBytes(m_indexedSourceWireBytes, entry.wireSize, m_sourceWireBytes);
      m_sources[i] = entry;
    }
  }
  reader.finish();
  require(paged || m_indexedSourceWireBytes == m_sourceWireBytes,
          "segment-manifest-source-wire-size");
}

SegmentManifestVerifier::Record
SegmentManifestVerifier::record(const ndn::Data& data)
{
  const auto size = static_cast<uint32_t>(data.wireEncode().size());
  checkWireSize(size);
  Record result{size, {}};
  const auto digest = data.getFullName().get(-1).value_bytes();
  std::copy(digest.begin(), digest.end(), result.digest.begin());
  return result;
}

ndn::Name
SegmentManifestVerifier::fullName(ndn::Name name, const Record& entry)
{
  return name.appendImplicitSha256Digest(entry.digest);
}

bool
SegmentManifestVerifier::matches(const ndn::Data& data, const ndn::Name& name, const Record& entry)
{
  return data.getSignatureType() == ndn::tlv::DigestSha256 &&
         data.wireEncode().size() == entry.wireSize && data.getFullName() == name;
}

void
SegmentManifestVerifier::requireCurrent() const
{
  require(isCurrent(), "segment-manifest-root-expired");
}

ndn::span<const uint8_t>
SegmentManifestVerifier::applicationManifest() const
{
  requireCurrent();
  return m_applicationManifest;
}

ndn::Name
SegmentManifestVerifier::pageFullName(uint32_t page) const
{
  requireCurrent();
  return fullName(m_scope.pageName(page), m_pages.at(page));
}

ndn::Name
SegmentManifestVerifier::sourceFullName(uint32_t segment) const
{
  requireCurrent();
  const auto& source = m_sources.at(segment);
  require(source.has_value(), "segment-manifest-source-index-unavailable");
  return fullName(m_scope.sourceName(segment), *source);
}

void
SegmentManifestVerifier::acceptPage(uint32_t page, const ndn::Data& data)
{
  const auto name = pageFullName(page);
  require(!data.getFinalBlock() && matches(data, name, m_pages.at(page)),
          "segment-manifest-page-mismatch");
  const auto start = page * PAGE_SIZE;
  require(!m_sources[start], "segment-manifest-duplicate-page");
  const auto count = std::min(PAGE_SIZE, sourceCount() - start);
  Reader reader(data.getContent());
  reader.magic(PAGE_MAGIC);
  require(reader.integer<uint32_t>() == page && reader.integer<uint32_t>() == start &&
          reader.integer<uint16_t>() == count, "segment-manifest-page-range");

  std::vector<Record> entries;
  entries.reserve(count);
  auto indexedBytes = m_indexedSourceWireBytes;
  for (uint32_t i = start; i < start + count; ++i) {
    require(reader.integer<uint32_t>() == i, "segment-manifest-record-index");
    const Record entry{reader.integer<uint32_t>(), reader.digest()};
    checkWireSize(entry.wireSize);
    addBytes(indexedBytes, entry.wireSize, m_sourceWireBytes);
    entries.push_back(entry);
  }
  reader.finish();
  if (m_indexedPages + 1 == pageCount()) {
    require(indexedBytes == m_sourceWireBytes, "segment-manifest-source-wire-size");
  }
  requireCurrent();
  std::copy(entries.begin(), entries.end(), m_sources.begin() + start);
  m_indexedSourceWireBytes = indexedBytes;
  ++m_indexedPages;
}

bool
SegmentManifestVerifier::matchesSource(uint32_t segment, const ndn::Data& data) const
{
  if (!isCurrent() || segment >= sourceCount() || !m_sources[segment]) {
    return false;
  }
  static const ndn::name::Component sourceComponent("DATA");
  const auto& name = data.getName();
  const auto& entry = *m_sources[segment];
  // Match the exact typed name and then compare the same full-wire digest.
  // The already-encoded Data caches its implicit digest after Interest matching.
  if (name.size() != m_scope.objectPrefix.size() + 2 ||
      !m_scope.objectPrefix.isPrefixOf(name) || name[-2] != sourceComponent ||
      name[-1] != ndn::name::Component::fromSegment(segment) ||
      data.getFinalBlock() != ndn::name::Component::fromSegment(sourceCount() - 1) ||
      data.getSignatureType() != ndn::tlv::DigestSha256 ||
      data.wireEncode().size() != entry.wireSize) {
    return false;
  }
  const auto digest = data.getFullName()[-1].value_bytes();
  return std::equal(digest.begin(), digest.end(), entry.digest.begin(), entry.digest.end()) &&
         isCurrent();
}

} // namespace ndn_service_framework

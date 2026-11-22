#ifndef NDNSF_SEGMENT_MANIFEST_HPP
#define NDNSF_SEGMENT_MANIFEST_HPP

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/security/signing-info.hpp>

#include <array>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace ndn::security {
class KeyChain;
}

namespace ndn_service_framework {

class MessageValidator;

struct SegmentManifestScope
{
  ndn::Name producer;
  ndn::Name objectPrefix;
  std::array<uint8_t, 32> bindingDigest{};

  ndn::Name rootName() const;
  ndn::Name pageName(uint32_t page) const;
  ndn::Name sourceName(uint32_t segment) const;
};

struct SegmentManifestLimits
{
  uint32_t maxSourceSegments = 4096;
  uint64_t maxWireBytes = 0;
  std::optional<uint32_t> expectedSourceSegments;
};

class ManifestPublication
{
public:
  /** Prepare a complete original packet set in root, index, source order. */
  static ManifestPublication
  prepare(ndn::security::KeyChain& keyChain, const ndn::security::SigningInfo& signer,
          const SegmentManifestScope& scope, const SegmentManifestLimits& limits,
          ndn::span<const ndn::ConstBufferPtr> sources, ndn::span<const uint8_t> applicationManifest,
          ndn::time::milliseconds freshness);

  const SegmentManifestScope& scope() const { return m_scope; }
  const std::vector<ndn::Data>& packets() const { return m_packets; }

private:
  explicit ManifestPublication(SegmentManifestScope scope);

  SegmentManifestScope m_scope;
  std::vector<ndn::Data> m_packets;
};

/** A successful checked root and a fence tied to its validator's current trust state. */
class ValidatedManifestRoot
{
public:
  const ndn::Data& data() const { return m_data; }
  const ndn::Name& producer() const { return m_producer; }
  bool isCurrent() const { return m_isCurrent(); }

private:
  friend class MessageValidator;

  ValidatedManifestRoot(ndn::Data data, ndn::Name producer, std::function<bool()> isCurrent)
    : m_data(std::move(data))
    , m_producer(std::move(producer))
    , m_isCurrent(std::move(isCurrent))
  {
  }

  ndn::Data m_data;
  ndn::Name m_producer;
  std::function<bool()> m_isCurrent;
};

/** Per-operation index. All members must be checked before the caller completes the object. */
class SegmentManifestVerifier
{
public:
  static constexpr uint32_t MAX_SOURCE_PACKETS = 4096;
  static constexpr uint32_t SOURCE_PACKETS_PER_PAGE = 128;
  static constexpr uint32_t pageCountFor(uint32_t sourceCount)
  {
    return sourceCount <= SOURCE_PACKETS_PER_PAGE ? 0 :
      1 + (sourceCount - 1) / SOURCE_PACKETS_PER_PAGE;
  }

  SegmentManifestVerifier(ValidatedManifestRoot root, const SegmentManifestScope& scope,
                          const SegmentManifestLimits& limits);

  ndn::span<const uint8_t> applicationManifest() const;
  uint32_t sourceCount() const { return static_cast<uint32_t>(m_sources.size()); }
  uint32_t pageCount() const { return static_cast<uint32_t>(m_pages.size()); }
  bool isCurrent() const { return m_root.isCurrent(); }

  ndn::Name pageFullName(uint32_t page) const;
  ndn::Name sourceFullName(uint32_t segment) const;
  void acceptPage(uint32_t page, const ndn::Data& data);
  bool matchesSource(uint32_t segment, const ndn::Data& data) const;

private:
  struct Record
  {
    uint32_t wireSize;
    std::array<uint8_t, 32> digest;
  };

  static Record record(const ndn::Data& data);
  static ndn::Name fullName(ndn::Name name, const Record& record);
  static bool matches(const ndn::Data& data, const ndn::Name& fullName, const Record& record);
  void requireCurrent() const;

  ValidatedManifestRoot m_root;
  SegmentManifestScope m_scope;
  ndn::span<const uint8_t> m_applicationManifest;
  std::vector<Record> m_pages;
  std::vector<std::optional<Record>> m_sources;
  uint64_t m_sourceWireBytes = 0;
  uint64_t m_indexedSourceWireBytes = 0;
  uint32_t m_indexedPages = 0;
  friend class ManifestPublication;
};

} // namespace ndn_service_framework

#endif // NDNSF_SEGMENT_MANIFEST_HPP

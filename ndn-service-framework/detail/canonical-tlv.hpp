#ifndef NDN_SERVICE_FRAMEWORK_DETAIL_CANONICAL_TLV_HPP
#define NDN_SERVICE_FRAMEWORK_DETAIL_CANONICAL_TLV_HPP

#include <ndn-cxx/encoding/block.hpp>
#include <stdexcept>
#include <vector>

namespace ndn_service_framework::detail {

inline void requireCanonicalTlvHeader(const ndn::Block& block)
{
  if (!block.hasWire()) throw std::invalid_argument("missing TLV wire");
  auto cursor = block.begin();
  const auto end = block.end();
  const auto typeBegin = cursor;
  const auto type = ndn::tlv::readType(cursor, end);
  const auto lengthBegin = cursor;
  const auto length = ndn::tlv::readVarNumber(cursor, end);
  if (type != block.type() || static_cast<size_t>(cursor - lengthBegin) != ndn::tlv::sizeOfVarNumber(length) ||
      static_cast<size_t>(lengthBegin - typeBegin) != ndn::tlv::sizeOfVarNumber(type) ||
      length != static_cast<uint64_t>(end - cursor))
    throw std::invalid_argument("noncanonical TLV header");
}

/** Scan before materializing children. Block equality compares type/value,
 * so re-encoding equality alone cannot reject overlong outer TLV headers. */
inline std::vector<ndn::Block> canonicalTlvChildren(const ndn::Block& block, size_t maximum)
{
  requireCanonicalTlvHeader(block);
  std::vector<ndn::Block> result;
  auto cursor = block.value_begin();
  const auto end = block.value_end();
  while (cursor != end) {
    if (result.size() >= maximum) throw std::invalid_argument("too many TLV children");
    const auto begin = cursor;
    ndn::tlv::readType(cursor, end);
    const auto length = ndn::tlv::readVarNumber(cursor, end);
    if (length > static_cast<uint64_t>(end - cursor)) throw std::invalid_argument("truncated TLV child");
    cursor += length;
    ndn::Block child(block, begin, cursor);
    requireCanonicalTlvHeader(child);
    result.push_back(std::move(child));
  }
  return result;
}
} // namespace ndn_service_framework::detail
#endif

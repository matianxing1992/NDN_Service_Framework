#ifndef NDNSF_TEST_NONCANONICAL_TLV_FIXTURE_HPP
#define NDNSF_TEST_NONCANONICAL_TLV_FIXTURE_HPP
#include <ndn-cxx/encoding/block.hpp>
#include <ndn-cxx/encoding/block-helpers.hpp>
#include <ndn-cxx/name.hpp>

namespace ndn_service_framework::test {
inline ndn::Block withExactTlvChildren(uint32_t type, const std::vector<ndn::Block>& children)
{
  ndn::Buffer value;
  for (const auto& child : children) value.insert(value.end(), child.begin(), child.end());
  // Block::push_back/encode normalizes child headers. Copy their original wire
  // into the parent's value so malformed nested fixtures stay malformed.
  return ndn::makeBinaryBlock(type, ndn::span<const uint8_t>(value.data(), value.size()));
}

inline ndn::Block withOverlongTlvHeader(const ndn::Block& block, bool type)
{
  ndn::Buffer wire;
  const auto typeWidth = ndn::tlv::sizeOfVarNumber(block.type());
  if (!type) wire.insert(wire.end(), block.begin(), block.begin() + typeWidth);
  wire.push_back(255); // Nine-byte representation of a small type or length.
  const uint64_t value = type ? block.type() : block.value_size();
  for (int shift = 56; shift >= 0; shift -= 8) wire.push_back(static_cast<uint8_t>(value >> shift));
  if (type) wire.insert(wire.end(), block.begin() + typeWidth, block.end());
  else wire.insert(wire.end(), block.value_begin(), block.value_end());
  return ndn::Block(ndn::span<const uint8_t>(wire.data(), wire.size()));
}

inline ndn::Block nameWithOverlongComponent(const ndn::Name& name)
{
  const auto& encoded = name.wireEncode();
  encoded.parse();
  std::vector<ndn::Block> malformed;
  bool first = true;
  for (const auto& component : encoded.elements()) {
    malformed.push_back(first ? withOverlongTlvHeader(component, true) : component);
    first = false;
  }
  return withExactTlvChildren(ndn::tlv::Name, malformed);
}
} // namespace ndn_service_framework::test
#endif

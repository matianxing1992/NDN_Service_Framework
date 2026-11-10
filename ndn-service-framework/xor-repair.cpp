#include "xor-repair.hpp"
#include <stdexcept>

namespace ndn_service_framework::detail {
void xorInto(ndn::span<uint8_t> target, ndn::span<const uint8_t> source)
{
  if (source.size() > target.size()) throw std::invalid_argument("XOR source exceeds vector width");
  for (std::size_t i = 0; i < source.size(); ++i) target[i] ^= source[i];
}
}

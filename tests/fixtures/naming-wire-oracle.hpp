#ifndef NDNSF_TESTS_FIXTURES_NAMING_WIRE_ORACLE_HPP
#define NDNSF_TESTS_FIXTURES_NAMING_WIRE_ORACLE_HPP

#include "ndn-service-framework/ProducerDataNaming.hpp"

#include <openssl/sha.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ndn_service_framework::test::naming_oracle {

using Bytes = std::vector<uint8_t>;

inline Bytes
wire(const ndn::Name& name)
{
  const auto& value = name.wireEncode();
  return Bytes(value.data(), value.data() + value.size());
}

inline Bytes
utf8(const std::string& value)
{
  return Bytes(value.begin(), value.end());
}

inline Bytes
uint64(uint64_t value)
{
  Bytes output;
  for (int shift = 56; shift >= 0; shift -= 8) output.push_back(value >> shift);
  return output;
}

inline ndn::name::Component
rawHash(const Bytes& value)
{
  std::array<uint8_t, SHA256_DIGEST_LENGTH> digest{};
  SHA256(value.data(), value.size(), digest.data());
  return ndn::name::Component(ndn::span<const uint8_t>(digest.data(), digest.size()));
}

// Independent C01 transcript: expected Names do not use the production naming codec.
inline ndn::name::Component
commitment(const std::string& domain, const std::vector<NamingDigestField>& fields)
{
  Bytes value;
  const auto integer = [&](uint64_t number, size_t width) {
    for (size_t i = width; i > 0; --i) value.push_back(number >> (8 * (i - 1)));
  };
  integer(domain.size(), 2); value.insert(value.end(), domain.begin(), domain.end());
  integer(2, 2); integer(fields.size(), 2);
  for (const auto& field : fields) {
    integer(field.tag, 2); value.push_back(field.kind); integer(field.value.size(), 8);
    value.insert(value.end(), field.value.begin(), field.value.end());
  }
  return rawHash(value);
}

inline ndn::name::Component
repeatedDigest(uint8_t byte)
{
  const Bytes value(32, byte);
  return ndn::name::Component(ndn::span<const uint8_t>(value.data(), value.size()));
}

inline std::string
digestText(char character)
{
  return "sha256:" + std::string(64, character);
}

} // namespace ndn_service_framework::test::naming_oracle

#endif // NDNSF_TESTS_FIXTURES_NAMING_WIRE_ORACLE_HPP

#ifndef NDNSF_EXAMPLES_UAV_DIGEST_HPP
#define NDNSF_EXAMPLES_UAV_DIGEST_HPP

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace ndnsf::examples::uav {

/** Return canonical lowercase hexadecimal for a raw SHA-256 digest. */
inline std::string
canonicalSha256Hex(std::string value)
{
  if (value.size() != 64 ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') ||
               (ch >= 'A' && ch <= 'F');
      })) {
    throw std::invalid_argument("SHA-256 digest must be 64 hexadecimal characters");
  }
  std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
    return static_cast<char>(std::tolower(ch));
  });
  return value;
}

/** Return the wire-contract spelling for an SHA-256 digest. */
inline std::string
canonicalSha256Digest(std::string value)
{
  return "sha256:" + canonicalSha256Hex(std::move(value));
}

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_DIGEST_HPP

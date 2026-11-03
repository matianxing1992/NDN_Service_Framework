#ifndef NDNSF_DI_DETAIL_PRIVATE_BUFFER_WIPER_HPP
#define NDNSF_DI_DETAIL_PRIVATE_BUFFER_WIPER_HPP

#include <openssl/crypto.h>
#include <cstdint>
#include <vector>

namespace ndnsf::di::detail {

// The borrowed vector must outlive this guard. Core decryption may clear its
// size while retaining plaintext allocation. Resize within existing capacity
// constructs live byte elements without allocation, then wipe that full range.
class PrivateBufferWiper
{
public:
  explicit PrivateBufferWiper(std::vector<std::uint8_t>& bytes) noexcept : m_bytes(bytes) {}
  PrivateBufferWiper(const PrivateBufferWiper&) = delete;
  PrivateBufferWiper& operator=(const PrivateBufferWiper&) = delete;
  ~PrivateBufferWiper() noexcept
  {
    m_bytes.resize(m_bytes.capacity());
    if (!m_bytes.empty()) OPENSSL_cleanse(m_bytes.data(), m_bytes.size());
  }
private:
  std::vector<std::uint8_t>& m_bytes;
};

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_PRIVATE_BUFFER_WIPER_HPP

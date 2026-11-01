#pragma once

#include <ndn-cxx/encoding/buffer.hpp>
#include <openssl/crypto.h>

namespace ndnsf::di::detail {

// The borrowed vector must outlive this guard. Core decryption may clear its
// size while retaining plaintext allocation. Resize within existing capacity
// constructs live byte elements without allocation, then wipe that full range.
class PrivateBufferWiper
{
public:
  explicit PrivateBufferWiper(ndn::Buffer& bytes) noexcept : m_bytes(bytes) {}
  PrivateBufferWiper(const PrivateBufferWiper&) = delete;
  PrivateBufferWiper& operator=(const PrivateBufferWiper&) = delete;
  ~PrivateBufferWiper() noexcept
  {
    m_bytes.resize(m_bytes.capacity());
    if (!m_bytes.empty()) OPENSSL_cleanse(m_bytes.data(), m_bytes.size());
  }
private:
  ndn::Buffer& m_bytes;
};

} // namespace ndnsf::di::detail

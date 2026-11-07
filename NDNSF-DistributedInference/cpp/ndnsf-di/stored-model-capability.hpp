#ifndef NDNSF_DI_STORED_MODEL_CAPABILITY_HPP
#define NDNSF_DI_STORED_MODEL_CAPABILITY_HPP

#include <ndn-cxx/data.hpp>
#include <ndn-cxx/name.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace ndnsf::di {

/** Public commitment passed in authenticated request controls; contains no model key. */
struct NativeStoredModelReference
{
  ndn::Name producer;
  ndn::Name repository;
  ndn::Name rootName;
  std::string rootWireDigest;
  void validate() const;
};

/** Private capability transferred through a trusted channel, never public Repo metadata.
 * The root wire digest is the trust anchor; DigestSha256 alone does not prove origin.
 */
struct NativeStoredModelHandle : NativeStoredModelReference
{
  std::vector<std::uint8_t> masterKey;

  NativeStoredModelHandle() = default;
  NativeStoredModelHandle(const NativeStoredModelHandle&) = default;
  NativeStoredModelHandle(NativeStoredModelHandle&&) noexcept = default;
  NativeStoredModelHandle& operator=(const NativeStoredModelHandle&);
  NativeStoredModelHandle& operator=(NativeStoredModelHandle&&) noexcept;
  ~NativeStoredModelHandle();
  void validate() const;
};

/** Atomic private-file export. The parent directory must already exist. */
void
nativeExportStoredModelHandle(const NativeStoredModelHandle& handle,
                              const std::filesystem::path& destination);

/** Load a bounded, current-user-owned regular 0600 file without following a final symlink. */
NativeStoredModelHandle
nativeLoadStoredModelHandle(const std::filesystem::path& source);

/** Public root points to one segmented logical-object index, not all segment Names. */
struct NativeStoredModelRoot
{
  std::string modelDigest;
  ndn::Name indexName;
  std::string indexDigest;
  std::uint64_t indexBytes = 0;
  std::uint32_t segmentBytes = 6144;
  std::uint32_t segmentCount = 0;
};

/** Pin exact root wire and Producer before admitting any public object references. */
NativeStoredModelRoot
nativeAdmitStoredModelRoot(const NativeStoredModelReference& reference, const ndn::Data& root);

/** One logical object, with segment Names inferred from its original prefix. */
struct NativeStoredModelObject
{
  ndn::Name name;
  std::string plaintextDigest;
  std::string ciphertextDigest;
  std::uint64_t plaintextBytes = 0;
  std::uint64_t ciphertextBytes = 0;
};

/** Encrypt one immutable logical object. Wire = 12-byte nonce || ciphertext || 16-byte tag.
 * Publication must allocate a new immutable object Name for changed content/protection.
 */
std::vector<std::uint8_t>
nativeSealStoredModelObject(const std::vector<std::uint8_t>& masterKey,
                            const ndn::Name& producer, const ndn::Name& objectName,
                            const std::vector<std::uint8_t>& plaintext,
                            NativeStoredModelObject& reference);

/** Object reference must come from the root-authenticated index, never an untrusted peer. */
std::vector<std::uint8_t>
nativeOpenStoredModelObject(const NativeStoredModelHandle& handle,
                            const NativeStoredModelObject& reference,
                            const std::vector<std::uint8_t>& wire);

/** Provider-side opening after authenticated assignment admission of this object's key.
 * No master capability is needed or accepted at this boundary.
 */
std::vector<std::uint8_t>
nativeOpenStoredModelObjectWithKey(const NativeStoredModelReference& publication,
                                  const NativeStoredModelObject& reference,
                                  const std::vector<std::uint8_t>& objectKey,
                                  const std::vector<std::uint8_t>& wire);

/** Derive only an assigned object's key for the existing protected Provider control channel.
 * Callers must enforce assignment membership before delivering this key.
 */
std::vector<std::uint8_t>
nativeStoredModelObjectKey(const std::vector<std::uint8_t>& masterKey,
                           const ndn::Name& producer, const ndn::Name& objectName);

} // namespace ndnsf::di

#endif // NDNSF_DI_STORED_MODEL_CAPABILITY_HPP

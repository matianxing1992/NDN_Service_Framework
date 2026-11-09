#ifndef NDNSF_DI_MODEL_OBJECT_KEYS_HPP
#define NDNSF_DI_MODEL_OBJECT_KEYS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGroupKeyAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPreparation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/stored-model-repository.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <map>
#include <set>

namespace ndn_service_framework { class ServiceUser; }

namespace ndnsf::di {

struct NativePreparedCanonicalPublication;

struct NativeModelObjectKeyIndexV1
{
  std::optional<NativeStoredModelReference> storedPublication;
  std::map<std::string, NativeStoredModelIndexedObject> storedObjectsByName;
  std::set<std::string> providerAssemblyMetadataNames;
  std::set<std::string> nonProviderObjectNames;
  std::set<std::string> templatePayloadIds;
  std::map<std::uint64_t, std::set<std::string>> payloadIdsByNode;
  std::map<std::string, std::set<std::string>> payloadIdsByDataName;
};

/** Request-scoped Provider keys for one admitted stored-model assignment.
 * Raw keys never leave this move-only lease and are wiped when it drains.
 */
class NativeStoredModelObjectKeyLease
{
public:
  NativeStoredModelObjectKeyLease(NativeStoredModelReference publication,
                                  ndn::Name forwardingHint);
  ~NativeStoredModelObjectKeyLease() noexcept;
  NativeStoredModelObjectKeyLease(const NativeStoredModelObjectKeyLease&) = delete;
  NativeStoredModelObjectKeyLease& operator=(const NativeStoredModelObjectKeyLease&) = delete;

  void add(NativeStoredModelIndexedObject object, std::vector<std::uint8_t> key);
  void finalize();
  const NativeStoredModelReference& publication() const noexcept { return m_publication; }
  const ndn::Name& forwardingHint() const noexcept { return m_forwardingHint; }
  const std::string& cacheIdentity() const noexcept { return m_cacheIdentity; }
  bool contains(const ndn::Name& name) const;
  const NativeStoredModelObject& object(const ndn::Name& name) const;
  std::uint32_t segmentBytes(const ndn::Name& name) const;
  std::vector<std::uint8_t> open(const ndn::Name& name,
                                 const std::vector<std::uint8_t>& ciphertext) const;

private:
  struct Entry
  {
    Entry(NativeStoredModelObject object, std::uint32_t segmentBytes,
          std::vector<std::uint8_t> key);
    Entry(Entry&&) = default;
    Entry(const Entry&) = delete;
    Entry& operator=(Entry&&) = delete;
    Entry& operator=(const Entry&) = delete;
    ~Entry() noexcept;

    NativeStoredModelObject object;
    std::uint32_t segmentBytes = 0;
    std::vector<std::uint8_t> key;
  };

  const Entry& find(const ndn::Name& name) const;
  NativeStoredModelReference m_publication;
  ndn::Name m_forwardingHint;
  std::map<std::string, Entry> m_entries;
  std::string m_cacheIdentity;
  bool m_finalized = false;
};

std::set<std::string>
nativeSelectModelObjectKeyDataNames(const NativeModelObjectKeyIndexV1& index,
                                    const std::vector<std::uint64_t>& nodeIndices);

std::shared_ptr<const NativeModelObjectKeyIndexV1>
nativeBuildStoredModelObjectKeyIndex(const NativeStoredModelReference& publication,
  const NativeStoredModelCatalog& objects,
  const NativePreparedCanonicalPublication& preparedPublication);

NativeJson
nativeModelObjectKeyContext(const NativeSelectionProjectionV3& projection,
  const ndn::Name& requester, const ndn::Name& service, const std::string& providerBootId,
  const ndn::Name& certificate, const std::string& publicKeyDigest,
  std::uint64_t expiresAtMs, std::uint64_t nowMs);

ndn_service_framework::LargeDataPublishResult
nativePublishModelObjectKeys(const std::shared_ptr<ndn_service_framework::ServiceUser>& user,
  const std::string& service, const NativeArtifactBinding& artifacts,
  const NativeSelectionProjectionV3& projection, const NativeFixedRoleAssignmentV1& fixed,
  const NativeRecipientKeyMaterialV1& recipient, const std::string& providerBootId,
  std::uint64_t expiresAtMs, const NativeRequestControl& control);

ndn_service_framework::LargeDataPublishResult
nativePublishStoredModelObjectKeys(const std::shared_ptr<ndn_service_framework::ServiceUser>& user,
  const std::string& service, const NativeArtifactBinding& artifacts,
  const NativeStoredModelHandle& handle, const ndn::Name& forwardingHint,
  const NativeSelectionProjectionV3& projection,
  const NativeFixedRoleAssignmentV1& fixed, const NativeRecipientKeyMaterialV1& recipient,
  const std::string& providerBootId, std::uint64_t expiresAtMs,
  const NativeRequestControl& control);

std::shared_ptr<const NativeStoredModelObjectKeyLease>
nativeInstallModelObjectKeys(ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeSelectionProjectionV3& projection, const std::string& providerBootId,
  std::chrono::steady_clock::time_point deadline,
  const std::shared_ptr<NativeWorkStatistics>& workStatistics = {});

std::optional<ndn::Buffer>
nativeFetchAndOpenStoredModelObjectCurrent(
  ndn_service_framework::ServiceProvider::CollaborationContext& context,
  const NativeStoredModelObjectKeyLease& lease, const ndn::Name& name,
  std::chrono::steady_clock::time_point deadline);

} // namespace ndnsf::di

#endif // NDNSF_DI_MODEL_OBJECT_KEYS_HPP

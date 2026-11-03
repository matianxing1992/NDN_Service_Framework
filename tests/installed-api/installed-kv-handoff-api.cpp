#include <ndnsf-di/api.hpp>
#include <NDNSF-DistributedInference/cpp/ndnsf-di/NativeInferenceProvider.hpp>
#include <ndnsf-distributed-repo/RepoClient.hpp>

#include <chrono>
#include <map>
#include <string>

using namespace ndnsf::di;

using SwitchMethod = ConversationHandoffHandle (Conversation::*)(
  const std::map<std::string, std::string>&,
  const std::map<std::string, NativeRemoteProtectedKvRoleTrustPolicyV1>&,
  std::chrono::steady_clock::time_point) const;

// External linkage retains symbol references: this checks the installed SDK,
// not an in-tree header or a reimplementation of the handoff transaction.
SwitchMethod switchMethod = static_cast<SwitchMethod>(&Conversation::switchProviders);
auto departureWatchMethod = &Conversation::watchProviderDeparture;
auto providerWithdrawalMethod = &NativeInferenceProvider::withdraw;
auto serviceWithdrawalMethod = &NativeServiceRegistration::withdraw;
auto ownerContextResolver = &resolveNativeConversationHandoffOwnerContextV1;
auto originalPacketFetch =
  &ndnsf_distributed_repo::RepoClient::fetchOriginalDataPacketSetFromUserStorage;
auto originalPacketCommit = &ndnsf_distributed_repo::RepoClient::commitUserStorageOriginalPackets;

int
main()
{
  return 0;
}

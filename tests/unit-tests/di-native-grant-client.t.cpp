#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGrantClient.hpp"

#include <boost/test/unit_test.hpp>
#include <openssl/sha.h>

namespace ndnsf::di {
namespace {
std::string digest(const std::string& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
} // namespace

BOOST_AUTO_TEST_CASE(NativeGrantClientBindsAndPublishesExactName)
{
  const auto d = digest("grant");
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [&] (const NativeGrantRequest& request) {
      BOOST_CHECK_EQUAL(request.providerIdentity, "/provider/a");
      return NativeKeyGrant{"unused", d, "/recipient/a", "{\"grant\":1}", request.expiresAtMs};
    });
  std::string publishedName;
  std::string publishedWire;
  NativeGrantClient client(
    "/user/a", authority,
    [&] (const std::string& name, const std::string& wire) {
      publishedName = name;
      publishedWire = wire;
      return name;
    });
  NativeProviderGrantView view;
  view.provider = "/provider/a";
  view.role = "/role/0";
  view.planCoreDigest = d;
  view.policyDigest = d;
  view.modelDigest = d;
  view.graphDigest = d;
  view.artifactDigest = d;
  view.requesterIdentity = "/user/a";
  view.requestId = "/request/1";
  view.attempt = 1;
  view.modelManifestDigest = d;
  view.protectionEpoch = "epoch-1";
  view.expiresAtMs = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count()) + 60'000;
  const auto binding = client.acquire(
    view, std::chrono::system_clock::now() + std::chrono::seconds(2));
  BOOST_CHECK_EQUAL(binding.provider, view.provider);
  BOOST_CHECK_EQUAL(binding.grantDigest, d);
  BOOST_CHECK_EQUAL(binding.grantName, publishedName);
  BOOST_CHECK_EQUAL(binding.wireJson, publishedWire);
}

BOOST_AUTO_TEST_CASE(NativeGrantClientRejectsWrongRequester)
{
  auto authority = std::make_shared<NativeArtifactPolicyAuthority>(
    [] (const NativeGrantRequest&) { return NativeKeyGrant{}; });
  NativeGrantClient client("/user/a", authority,
    [] (const std::string& name, const std::string&) { return name; });
  NativeProviderGrantView view;
  view.requesterIdentity = "/user/b";
  BOOST_CHECK_THROW(client.acquire(view, std::chrono::system_clock::now() +
                                   std::chrono::seconds(1)), std::invalid_argument);
}

} // namespace ndnsf::di

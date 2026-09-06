// Native side of test_spec181_provider_grant_integration.py. The normal mode
// consumes exact Data from the real requester process through the same
// credential loader, transport and ProtectedRuntime used by the factory.
// Explicit wire-fixture mode exists only for separate sanitizer diagnostics.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include <boost/property_tree/json_parser.hpp>
#include <openssl/sha.h>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
std::uint64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
}
std::string digest(const std::vector<std::uint8_t>& value)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(value.data(), value.size(), hash);
  std::string result = "sha256:";
  for (auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 15];
  }
  return result;
}
}

int main(int argc, char** argv)
{
  using namespace ndnsf::di;
  boost::property_tree::ptree result;
  result.put("boundary", "BEFORE_ASSEMBLY");
  result.put("transport", argc == 3 ? "wire-fixture" : "ndn");
  try {
    if (argc != 2 && argc != 3) throw std::runtime_error("expected metadata [wire-fixture]");
    boost::property_tree::ptree input;
    boost::property_tree::read_json(argv[1], input);
    const auto& grant = input.get_child("binding");
    ProtectedRuntimeBindingV1 binding;
    binding.provider = grant.get<std::string>("provider");
    binding.role = input.get<std::string>("role.role");
    binding.requestId = grant.get<std::string>("request_id");
    binding.attempt = grant.get<std::uint64_t>("attempt");
    binding.planCoreDigest = grant.get<std::string>("plan_core_digest");
    binding.planDigest = input.get<std::string>("planDigest");
    binding.securityPolicySnapshotDigest = grant.get<std::string>("security_policy_snapshot_digest");
    binding.protectionEpoch = grant.get<std::string>("protection_epoch");
    binding.grantName = grant.get<std::string>("grant_name");
    binding.grantDigest = grant.get<std::string>("grant_digest");
    binding.providerBootId = "spec181-integration-native";
    binding.fencingToken = "spec181-integration-selection";
    binding.expiresAtMs = input.get<std::uint64_t>("deadlineMs");
    const auto repeats = input.get<unsigned>("repeat", 1);
    if (repeats == 0 || repeats > 1000) throw std::runtime_error("invalid repeat count");
    for (unsigned index = 0; index < repeats; ++index) {
      auto config = loadNativeProtectedGrantConfig(binding.provider, "YOLO26n", binding.protectionEpoch);
      config.modelManifestDigest = input.get<std::string>("role.model_manifest_digest");
      if (argc == 3) {
        config.fetchGrant = [path = std::string(argv[2])] (const std::string&) {
          std::ifstream file(path);
          if (!file) throw std::runtime_error("wire fixture is missing");
          return std::string(std::istreambuf_iterator<char>(file), {});
        };
      }
      else {
        config.fetchGrant = [] (const std::string& name) {
          return fetchNativeProtectedGrant(name, 5000);
        };
      }
      ProtectedRuntime runtime(binding, std::move(config));
      runtime.verifyGrant(binding, nowMs());
      std::string observed;
      runtime.withContentKey(nowMs(), [&] (const auto& key) { observed = digest(key); });
      if (observed != input.get<std::string>("contentKeyDigest"))
        throw std::runtime_error("verified content key differs from requester-owned key");
      runtime.complete();
      if (runtime.state() != ProtectedRuntimeState::Zeroized)
        throw std::runtime_error("native runtime did not zeroize");
    }
    result.put("status", "VERIFIED");
    result.put("zeroized", true);
  }
  catch (const std::exception& error) {
    result.put("status", "REJECTED");
    result.put("reason", error.what());
  }
  std::cout << "SPEC181_NATIVE_GRANT_RESULT ";
  boost::property_tree::write_json(std::cout, result, false);
  return result.get<std::string>("status") == "VERIFIED" ? 0 : 2;
}

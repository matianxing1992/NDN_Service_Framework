#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <openssl/rand.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name USER_IDENTITY("/example/hello/user");
const ndn::Name SERVICE_NAME("/AI/FNN/Inference");

bool
hasFlag(int argc, char** argv, const std::string& option)
{
  for (int i = 1; i < argc; ++i) {
    if (argv[i] == option) {
      return true;
    }
  }
  return false;
}

std::string
getOption(int argc, char** argv, const std::string& option, const std::string& fallback)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == option) {
      return argv[i + 1];
    }
  }
  return fallback;
}

int
parseIntOption(int argc, char** argv, const std::string& option, int fallback)
{
  try {
    return std::stoi(getOption(argc, argv, option, std::to_string(fallback)));
  }
  catch (const std::exception&) {
    return fallback;
  }
}

ndn::security::Certificate
getOrCreateIdentity(ndn::security::KeyChain& keyChain, const ndn::Name& identity)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, ndn::RsaKeyParams(2048))
      .getDefaultKey().getDefaultCertificate();
  }
}

ndn::Buffer
toBuffer(const std::string& text)
{
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(text.data()), text.size());
}

std::vector<uint8_t>
toVector(const ndn::Buffer& buffer)
{
  return std::vector<uint8_t>(buffer.begin(), buffer.end());
}

ndn::Buffer
randomScopeKey()
{
  ndn::Buffer key(32);
  if (RAND_bytes(key.data(), static_cast<int>(key.size())) != 1) {
    throw std::runtime_error("RAND_bytes failed");
  }
  return key;
}

void
appendU32(ndn::Buffer& out, uint32_t value)
{
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }
}

void
appendDouble(ndn::Buffer& out, double value)
{
  uint8_t bytes[sizeof(double)];
  std::memcpy(bytes, &value, sizeof(double));
  out.insert(out.end(), bytes, bytes + sizeof(double));
}

ndn::Buffer
artifactForRole(const std::string& role)
{
  if (role.size() != 3 || role[0] != 'p') {
    throw std::runtime_error("unexpected role name: " + role);
  }
  const int stage = role[1] - '0';
  const int shard = role[2] - '0';
  if (stage < 0 || stage > 2 || shard < 0 || shard > 2) {
    throw std::runtime_error("unexpected role index: " + role);
  }

  ndn::Buffer out;
  out.push_back(stage < 2 ? 'H' : 'O');
  appendU32(out, 1);
  appendU32(out, 3);

  for (int c = 0; c < 3; ++c) {
    const double sign = ((stage + shard + c) % 2 == 0) ? 1.0 : -1.0;
    const double weight =
      sign * (0.08 * static_cast<double>(stage + 1) +
              0.05 * static_cast<double>(shard + 1) +
              0.03 * static_cast<double>(c + 1));
    appendDouble(out, weight);
  }
  appendDouble(out, 0.01 * static_cast<double>(stage + 1) -
                    0.015 * static_cast<double>(shard + 1));
  return out;
}

ndn_service_framework::CollaborationRoleSpec
makeRoleSpec(const std::string& role)
{
  ndn_service_framework::CollaborationRoleSpec spec;
  spec.role = role;
  spec.service = SERVICE_NAME;
  spec.requiredArtifact = ndn::Name("/artifacts/random-fnn").append(role);
  return spec;
}

std::vector<double>
parseVector(const ndn::Buffer& payload)
{
  const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
  std::vector<double> values;
  std::stringstream ss(text);
  std::string item;
  while (std::getline(ss, item, ',')) {
    if (!item.empty()) {
      values.push_back(std::stod(item));
    }
  }
  return values;
}

std::string
field(const std::string& text, const std::string& key)
{
  const auto marker = key + "=";
  const auto begin = text.find(marker);
  if (begin == std::string::npos) {
    return "";
  }
  const auto valueBegin = begin + marker.size();
  const auto valueEnd = text.find(';', valueBegin);
  return text.substr(valueBegin, valueEnd - valueBegin);
}

class RoleSelectionPolicy final : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  RoleSelectionPolicy(std::map<std::string, ndn::Buffer> scopeKeys,
                      std::map<std::string, std::vector<std::string>> roleScopes)
    : m_scopeKeys(std::move(scopeKeys))
    , m_roleScopes(std::move(roleScopes))
  {
  }

  void setArtifactDataNames(std::map<std::string, ndn::Name> names)
  {
    m_artifactDataNames = std::move(names);
  }

  void setScopeKeyDataNames(std::map<std::string, ndn::Name> names)
  {
    m_scopeKeyDataNames = std::move(names);
  }

  std::vector<ndn_service_framework::SelectedParticipant>
  select(const std::vector<ndn_service_framework::AckCandidate>& candidates,
         const std::vector<ndn_service_framework::CollaborationRoleSpec>& roles) const override
  {
    std::vector<ndn_service_framework::SelectedParticipant> selected;
    std::map<std::string, ndn_service_framework::AckCandidate> byRole;

    for (const auto& candidate : candidates) {
      if (!candidate.ack.getStatus()) {
        continue;
      }
      const auto payload = candidate.ack.getPayload();
      const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
      const auto role = field(text, "role");
      if (!role.empty() && byRole.find(role) == byRole.end()) {
        byRole.emplace(role, candidate);
      }
    }

    for (const auto& role : roles) {
      auto it = byRole.find(role.role);
      if (it == byRole.end()) {
        continue;
      }
      std::string assignment =
        "role=" + role.role +
        ";artifact=" + role.requiredArtifact.toUri() +
        ";requiresProvisioning=1;provisioningTimeoutMs=5000;";
      auto artifactName = m_artifactDataNames.find(role.role);
      if (artifactName != m_artifactDataNames.end()) {
        assignment += "artifactDataName=" + artifactName->second.toUri() + ";";
      }
      auto scopesForRole = m_roleScopes.find(role.role);
      if (scopesForRole != m_roleScopes.end()) {
        for (const auto& scopeName : scopesForRole->second) {
          auto scope = m_scopeKeyDataNames.find(scopeName);
          if (scope != m_scopeKeyDataNames.end()) {
            assignment += "scopeKeyData." + scope->first + "=" +
                          scope->second.toUri() + ";";
          }
        }
      }
      selected.push_back({
        role.role,
        it->second.serviceName,
        it->second.providerName,
        role.requiredArtifact,
        false,
        0,
        toBuffer(assignment),
        it->second
      });
    }
    return selected;
  }

private:
  std::map<std::string, ndn::Buffer> m_scopeKeys;
  std::map<std::string, std::vector<std::string>> m_roleScopes;
  std::map<std::string, ndn::Name> m_artifactDataNames;
  std::map<std::string, ndn::Name> m_scopeKeyDataNames;
};

std::vector<double>
expectedOutput(const std::vector<double>& x)
{
  auto applyStage = [] (int stage, const std::vector<double>& input) {
    std::vector<double> out;
    out.reserve(3);
    for (int shard = 0; shard < 3; ++shard) {
      double y = 0.01 * static_cast<double>(stage + 1) -
                 0.015 * static_cast<double>(shard + 1);
      for (int c = 0; c < 3; ++c) {
        const double sign = ((stage + shard + c) % 2 == 0) ? 1.0 : -1.0;
        const double weight =
          sign * (0.08 * static_cast<double>(stage + 1) +
                  0.05 * static_cast<double>(shard + 1) +
                  0.03 * static_cast<double>(c + 1));
        y += weight * input[static_cast<size_t>(c)];
      }
      out.push_back(stage < 2 ? std::max(0.0, y) : y);
    }
    return out;
  };
  return applyStage(2, applyStage(1, applyStage(0, x)));
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;

    const int timeoutMs = parseIntOption(argc, argv, "--timeout-ms", 12000);
    const int ackMs = parseIntOption(argc, argv, "--ack-timeout-ms", 1000);
    const int permissionWaitMs = parseIntOption(argc, argv, "--permission-wait-ms", 1500);

    auto userCert = getOrCreateIdentity(keyChain, USER_IDENTITY);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(USER_IDENTITY));

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (!hasFlag(argc, argv, "--no-serve-certificates")) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, userCert.getName());
    }

    ndn_service_framework::ServiceUser user(face,
                                            GROUP_PREFIX,
                                            userCert,
                                            controllerCert,
                                            "examples/trust-schema.conf");
    user.setUseTokens(!hasFlag(argc, argv, "--disable-tokens"));
    user.setHandlerThreads(2);
    user.setAckProcessingThreads(2);

    ndn_service_framework::CollaborationPlan plan;
    std::map<std::string, ndn::Buffer> scopeKeys = {
      {"stage0-internal", randomScopeKey()},
      {"stage1-internal", randomScopeKey()},
      {"stage2-internal", randomScopeKey()},
      {"stage0-to-stage1", randomScopeKey()},
      {"stage1-to-stage2", randomScopeKey()},
    };
    plan.ackCollectionTimeMs = ackMs;
    plan.timeoutMs = timeoutMs;
    plan.roles = {
      makeRoleSpec("p00"),
      makeRoleSpec("p01"),
      makeRoleSpec("p02"),
      makeRoleSpec("p10"),
      makeRoleSpec("p11"),
      makeRoleSpec("p12"),
      makeRoleSpec("p20"),
      makeRoleSpec("p21"),
      makeRoleSpec("p22"),
    };
    plan.keyScopes = {
      {"stage0-internal", {"p00", "p01", "p02"}},
      {"stage1-internal", {"p10", "p11", "p12"}},
      {"stage2-internal", {"p20", "p21", "p22"}},
      {"stage0-to-stage1", {"p00", "p01", "p02", "p10", "p11", "p12"}},
      {"stage1-to-stage2", {"p10", "p11", "p12", "p20", "p21", "p22"}},
    };
    plan.dependencies = {
      {{"p00", "p01", "p02"}, {"p10", "p11", "p12"}, "stage0-to-stage1", ndn::Name("/stage0"), true},
      {{"p10", "p11", "p12"}, {"p20", "p21", "p22"}, "stage1-to-stage2", ndn::Name("/stage1"), true},
      {{"p20", "p21", "p22"}, {"p20", "p21", "p22"}, "stage2-internal", ndn::Name("/output"), true},
    };
    std::map<std::string, std::vector<std::string>> roleScopes = {
      {"p00", {"stage0-internal", "stage0-to-stage1"}},
      {"p01", {"stage0-internal", "stage0-to-stage1"}},
      {"p02", {"stage0-internal", "stage0-to-stage1"}},
      {"p10", {"stage0-to-stage1", "stage1-internal", "stage1-to-stage2"}},
      {"p11", {"stage0-to-stage1", "stage1-internal", "stage1-to-stage2"}},
      {"p12", {"stage0-to-stage1", "stage1-internal", "stage1-to-stage2"}},
      {"p20", {"stage1-to-stage2", "stage2-internal"}},
      {"p21", {"stage1-to-stage2", "stage2-internal"}},
      {"p22", {"stage1-to-stage2", "stage2-internal"}},
    };
    auto selector =
      std::make_shared<RoleSelectionPolicy>(scopeKeys, roleScopes);
    plan.participantSelector = selector;

    const std::vector<double> input = {0.2, -0.4, 0.6};
    const std::string inputText = "0.2,-0.4,0.6";
    const auto expected = expectedOutput(input);

    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};

    user.fetchPermissionsFromController(CONTROLLER_PREFIX);
    user.init();
    const auto permissionDeadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(permissionWaitMs);
    while (std::chrono::steady_clock::now() < permissionDeadline) {
      face.processEvents(ndn::time::milliseconds(10));
    }

    auto artifactCtx = user.prepareServiceRequest(SERVICE_NAME.toUri());
    std::map<std::string, ndn::Name> artifactDataNames;
    for (const auto& role : std::vector<std::string>{
           "p00", "p01", "p02", "p10", "p11", "p12", "p20", "p21", "p22"}) {
      auto result = user.publishEncryptedLargeData(
        artifactCtx,
        toVector(artifactForRole(role)),
        "artifact-" + role,
        ndn::time::seconds(60));
      if (!result.success) {
        std::cerr << "failed to publish artifact " << role
                  << ": " << result.errorMessage << std::endl;
        return 4;
      }
      artifactDataNames[role] = result.encryptedDataName;
    }

    std::map<std::string, ndn::Name> scopeKeyDataNames;
    for (const auto& scope : scopeKeys) {
      auto result = user.publishEncryptedLargeData(
        artifactCtx,
        toVector(scope.second),
        "scope-key-" + scope.first,
        ndn::time::seconds(60));
      if (!result.success) {
        std::cerr << "failed to publish scope key " << scope.first
                  << ": " << result.errorMessage << std::endl;
        return 5;
      }
      scopeKeyDataNames[scope.first] = result.encryptedDataName;
    }
    selector->setArtifactDataNames(std::move(artifactDataNames));
    selector->setScopeKeyDataNames(std::move(scopeKeyDataNames));

    user.RequestCollaboration(
      SERVICE_NAME,
      toBuffer(inputText),
      std::move(plan),
      [&](const ndn_service_framework::ResponseMessage& response) {
        const auto output = parseVector(response.getPayload());
        std::cout << "AI_COLLAB_RESULT status=" << response.getStatus()
                  << " output=";
        for (size_t i = 0; i < output.size(); ++i) {
          if (i > 0) {
            std::cout << ",";
          }
          std::cout << output[i];
        }
        std::cout << " expected=";
        for (size_t i = 0; i < expected.size(); ++i) {
          if (i > 0) {
            std::cout << ",";
          }
          std::cout << expected[i];
        }
        std::cout << std::endl;
        ok = output.size() == expected.size();
        for (size_t i = 0; ok && i < expected.size(); ++i) {
          ok = std::fabs(output[i] - expected[i]) < 1e-5;
        }
        done = true;
      },
      [&](const ndn::Name& requestId) {
        std::cout << "AI_COLLAB_TIMEOUT requestId=" << requestId << std::endl;
        done = true;
      });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(10));
    }

    if (!done) {
      std::cerr << "AI collaboration did not finish before local deadline" << std::endl;
      return 2;
    }
    return ok ? 0 : 3;
  }
  catch (const std::exception& e) {
    std::cerr << "AI_User error: " << e.what() << std::endl;
    return 1;
  }
}

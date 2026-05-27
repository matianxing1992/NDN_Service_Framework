#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceUser.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <openssl/rand.h>

#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name USER_IDENTITY("/example/hello/user");
const ndn::Name SERVICE_NAME("/Payment/Checkout");

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
  return text.substr(valueBegin,
                     (valueEnd == std::string::npos ? text.size() : valueEnd) -
                       valueBegin);
}

ndn_service_framework::CollaborationRoleSpec
makeRoleSpec(const std::string& role)
{
  ndn_service_framework::CollaborationRoleSpec spec;
  spec.role = role;
  spec.service = SERVICE_NAME;
  return spec;
}

class PaymentSelectionPolicy final : public ndn_service_framework::ParticipantSelectionPolicy
{
public:
  PaymentSelectionPolicy(std::map<std::string, std::vector<std::string>> roleScopes)
    : m_roleScopes(std::move(roleScopes))
  {
  }

  void setScopeKeyDataNames(std::map<std::string, ndn::Name> names)
  {
    m_scopeKeyDataNames = std::move(names);
  }

  std::vector<ndn_service_framework::SelectedParticipant>
  select(const std::vector<ndn_service_framework::AckCandidate>& candidates,
         const std::vector<ndn_service_framework::CollaborationRoleSpec>& roles) const override
  {
    std::map<std::string, ndn_service_framework::AckCandidate> byRole;
    for (const auto& candidate : candidates) {
      if (!candidate.ack.getStatus()) {
        continue;
      }
      const auto payload = candidate.ack.getPayload();
      const std::string text(reinterpret_cast<const char*>(payload.data()), payload.size());
      const auto role = field(text, "role");
      if (!role.empty() && byRole.count(role) == 0) {
        byRole.emplace(role, candidate);
      }
    }

    std::vector<ndn_service_framework::SelectedParticipant> selected;
    for (const auto& role : roles) {
      auto candidate = byRole.find(role.role);
      if (candidate == byRole.end()) {
        continue;
      }
      std::string assignment = "role=" + role.role + ";";
      auto scopes = m_roleScopes.find(role.role);
      if (scopes != m_roleScopes.end()) {
        for (const auto& scopeName : scopes->second) {
          auto scope = m_scopeKeyDataNames.find(scopeName);
          if (scope != m_scopeKeyDataNames.end()) {
            assignment += "scopeKeyData." + scope->first + "=" +
                          scope->second.toUri() + ";";
          }
        }
      }
      selected.push_back({
        role.role,
        candidate->second.serviceName,
        candidate->second.providerName,
        ndn::Name(),
        false,
        0,
        toBuffer(assignment),
        candidate->second
      });
    }
    return selected;
  }

private:
  std::map<std::string, std::vector<std::string>> m_roleScopes;
  std::map<std::string, ndn::Name> m_scopeKeyDataNames;
};

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;

    const int timeoutMs = parseIntOption(argc, argv, "--timeout-ms", 12000);
    const int ackMs = parseIntOption(argc, argv, "--ack-timeout-ms", 1000);
    const int permissionWaitMs =
      parseIntOption(argc, argv, "--permission-wait-ms", 1500);

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
      {"payment-checks", randomScopeKey()},
      {"payment-settlement", randomScopeKey()},
    };
    std::map<std::string, std::vector<std::string>> roleScopes = {
      {"fraud", {"payment-checks"}},
      {"inventory", {"payment-checks"}},
      {"payment", {"payment-checks", "payment-settlement"}},
      {"receipt", {"payment-settlement"}},
    };

    plan.ackCollectionTimeMs = ackMs;
    plan.timeoutMs = timeoutMs;
    plan.roles = {
      makeRoleSpec("fraud"),
      makeRoleSpec("inventory"),
      makeRoleSpec("payment"),
      makeRoleSpec("receipt"),
    };
    plan.keyScopes = {
      {"payment-checks", {"fraud", "inventory", "payment"}},
      {"payment-settlement", {"payment", "receipt"}},
    };
    plan.dependencies = {
      {{"fraud", "inventory"}, {"payment"}, "payment-checks", ndn::Name("/"), true},
      {{"payment"}, {"receipt"}, "payment-settlement", ndn::Name("/authorization"), true},
    };
    auto selector = std::make_shared<PaymentSelectionPolicy>(roleScopes);
    plan.participantSelector = selector;

    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};

    user.fetchPermissionsFromController(CONTROLLER_PREFIX);
    user.init();
    const auto permissionDeadline = std::chrono::steady_clock::now() +
                                    std::chrono::milliseconds(permissionWaitMs);
    while (std::chrono::steady_clock::now() < permissionDeadline) {
      face.processEvents(ndn::time::milliseconds(10));
    }

    auto keyCtx = user.prepareServiceRequest(SERVICE_NAME.toUri());
    std::map<std::string, ndn::Name> scopeKeyDataNames;
    for (const auto& scope : scopeKeys) {
      auto result = user.publishEncryptedLargeData(
        keyCtx,
        toVector(scope.second),
        "scope-key-" + scope.first,
        ndn::time::seconds(60));
      if (!result.success) {
        std::cerr << "failed to publish payment scope key "
                  << scope.first << ": " << result.errorMessage << std::endl;
        return 4;
      }
      scopeKeyDataNames[scope.first] = result.encryptedDataName;
    }
    selector->setScopeKeyDataNames(std::move(scopeKeyDataNames));

    user.RequestCollaboration(
      SERVICE_NAME,
      toBuffer("order=demo-42;amount=19.99;"),
      std::move(plan),
      [&](const ndn_service_framework::ResponseMessage& response) {
        const std::string payload(reinterpret_cast<const char*>(response.getPayload().data()),
                                  response.getPayload().size());
        std::cout << "PAYMENT_COLLAB_RESULT status=" << response.getStatus()
                  << " payload=" << payload << std::endl;
        ok = response.getStatus() &&
             payload.find("receipt=ok") != std::string::npos &&
             payload.find("auth=approved") != std::string::npos;
        done = true;
      },
      [&](const ndn::Name& requestId) {
        std::cout << "PAYMENT_COLLAB_TIMEOUT requestId=" << requestId << std::endl;
        done = true;
      });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs + 3000);
    while (!done && std::chrono::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(10));
    }

    if (!done) {
      std::cerr << "payment collaboration did not finish before local deadline"
                << std::endl;
      return 2;
    }
    return ok ? 0 : 3;
  }
  catch (const std::exception& e) {
    std::cerr << "Payment_User error: " << e.what() << std::endl;
    return 1;
  }
}

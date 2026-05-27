#include "ndn-service-framework/CertificatePublisher.hpp"
#include "ndn-service-framework/ServiceProvider.hpp"

#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/key-params.hpp>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

namespace {

const ndn::Name GROUP_PREFIX("/example/hello/group");
const ndn::Name CONTROLLER_PREFIX("/example/hello/controller");
const ndn::Name PROVIDER_IDENTITY("/example/hello/provider");
const ndn::Name SERVICE_NAME("/Payment/Checkout");

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

std::string
toString(const ndn::Buffer& payload)
{
  return std::string(reinterpret_cast<const char*>(payload.data()), payload.size());
}

std::string
field(const std::string& text, const std::string& key, const std::string& fallback = "")
{
  const auto marker = key + "=";
  const auto begin = text.find(marker);
  if (begin == std::string::npos) {
    return fallback;
  }
  const auto valueBegin = begin + marker.size();
  const auto valueEnd = text.find(';', valueBegin);
  return text.substr(valueBegin,
                     (valueEnd == std::string::npos ? text.size() : valueEnd) -
                       valueBegin);
}

std::string
assignmentRole(const ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
               const std::string& fallback)
{
  if (!ctx.assignment().role.empty()) {
    return ctx.assignment().role;
  }
  return field(toString(ctx.assignment().assignmentPayload), "role", fallback);
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    ndn::Face face;
    ndn::KeyChain keyChain;

    const std::string providerId = getOption(argc, argv, "--provider-id", "");
    const std::string configuredRole =
      getOption(argc, argv, "--role", providerId.empty() ? "fraud" : providerId);
    const ndn::Name providerIdentity = providerId.empty()
      ? PROVIDER_IDENTITY
      : ndn::Name(PROVIDER_IDENTITY).append(providerId);

    auto providerCert = getOrCreateIdentity(keyChain, providerIdentity);
    auto controllerCert = getOrCreateIdentity(keyChain, CONTROLLER_PREFIX);
    keyChain.setDefaultIdentity(keyChain.getPib().getIdentity(providerIdentity));

    std::unique_ptr<ndn_service_framework::CertificatePublisher> certPublisher;
    if (!hasFlag(argc, argv, "--no-serve-certificates")) {
      certPublisher = std::make_unique<ndn_service_framework::CertificatePublisher>(
        face, keyChain, providerCert.getName());
    }

    ndn_service_framework::ServiceProvider provider(face,
                                                    GROUP_PREFIX,
                                                    providerCert,
                                                    controllerCert,
                                                    "examples/trust-schema.conf");
    provider.setUseTokens(!hasFlag(argc, argv, "--disable-tokens"));
    provider.setHandlerThreads(4);
    provider.setAckThreads(2);

    provider.addCollaborationHandler(
      SERVICE_NAME,
      {configuredRole},
      [configuredRole](const ndn_service_framework::RequestMessage&) {
        ndn_service_framework::ServiceProvider::AckDecision decision;
        decision.status = true;
        decision.message = "payment collaboration provider ready";
        decision.payload = toBuffer("role=" + configuredRole + ";queue=0;");
        return decision;
      },
      [configuredRole](ndn_service_framework::ServiceProvider::CollaborationContext& ctx,
                       const ndn_service_framework::RequestMessage& request) {
        const auto role = assignmentRole(ctx, configuredRole);
        const auto order = toString(request.getPayload());
        const auto orderId = field(order, "order", "demo");
        const auto amount = field(order, "amount", "0");

        if (role == "fraud") {
          ctx.publish("payment-checks",
                      ndn::Name("/fraud"),
                      toBuffer("fraud=approved;score=7;order=" + orderId +
                               ";amount=" + amount + ";"));
          return;
        }

        if (role == "inventory") {
          ctx.publish("payment-checks",
                      ndn::Name("/inventory"),
                      toBuffer("inventory=reserved;item=book;order=" + orderId +
                               ";amount=" + amount + ";"));
          return;
        }

        if (role == "payment") {
          struct ChecksState
          {
            std::mutex mutex;
            bool fraudOk = false;
            bool inventoryOk = false;
            bool published = false;
          };
          auto state = std::make_shared<ChecksState>();
          ctx.subscribe(
            "payment-checks",
            ndn::Name("/"),
            [state, orderId, amount](
              ndn_service_framework::ServiceProvider::CollaborationContext& cbCtx,
              const ndn_service_framework::ServiceProvider::CollaborationData& check) {
            const auto text = toString(check.payload);
            const bool sameOrder = field(text, "order") == orderId &&
                                   field(text, "amount") == amount;
            std::lock_guard<std::mutex> lock(state->mutex);
            state->fraudOk =
              state->fraudOk || (sameOrder &&
                                 text.find("fraud=approved") != std::string::npos);
            state->inventoryOk =
              state->inventoryOk || (sameOrder &&
                                     text.find("inventory=reserved") != std::string::npos);
            if (state->published || !state->fraudOk || !state->inventoryOk) {
              return;
            }
            state->published = true;
            cbCtx.publish("payment-settlement",
                          ndn::Name("/authorization"),
                          toBuffer("auth=approved;order=" + orderId +
                                   ";amount=" + amount + ";"));
          });
          return;
        }

        if (role == "receipt") {
          auto done = std::make_shared<std::atomic<bool>>(false);
          ctx.subscribe(
            "payment-settlement",
            ndn::Name("/authorization"),
            [done, orderId, amount](
              ndn_service_framework::ServiceProvider::CollaborationContext& cbCtx,
              const ndn_service_framework::ServiceProvider::CollaborationData& auth) {
            bool expected = false;
            if (!done->compare_exchange_strong(expected, true)) {
              return;
            }
            const auto authText = toString(auth.payload);
            if (field(authText, "order") != orderId ||
                field(authText, "amount") != amount ||
                authText.find("auth=approved") == std::string::npos) {
              cbCtx.fail("payment authorization does not match request");
              return;
            }
            cbCtx.publishFinalResponse(
              toBuffer("receipt=ok;order=" + orderId +
                       ";amount=" + amount + ";auth=approved;"));
          });
        }
      });

    provider.fetchPermissionsFromController(CONTROLLER_PREFIX);
    provider.init();

    std::cout << "[Payment_Provider] identity=" << providerIdentity
              << " role=" << configuredRole << std::endl;
    while (true) {
      face.processEvents();
    }
  }
  catch (const std::exception& e) {
    std::cerr << "Payment_CollaborationProvider error: " << e.what() << std::endl;
    return 1;
  }
}

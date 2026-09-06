#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include <ndn-cxx/face.hpp>
#include <chrono>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace ndnsf::di {
std::string
fetchNativeProtectedGrant(const std::string& name, int timeoutMs,
                          const std::function<bool()>& cancelled)
{
  ndn::Face face;
  const ndn::Name exact(name);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  std::string payload;
  bool received = false;
  while (!received && std::chrono::steady_clock::now() < deadline) {
    if (cancelled && cancelled()) {
      throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant fetch cancelled");
    }
    ndn::Interest interest(exact);
    interest.setCanBePrefix(false);
    interest.setMustBeFresh(false);
    interest.setInterestLifetime(ndn::time::milliseconds(500));
    if (const char* hints = std::getenv("SPEC181_GRANT_FORWARDING_HINT")) {
      std::istringstream input(hints);
      std::string hint;
      std::vector<ndn::Name> delegations;
      while (std::getline(input, hint, ',')) {
        if (!hint.empty()) delegations.emplace_back(hint);
      }
      interest.setForwardingHint(delegations);
    }
    bool done = false;
    face.expressInterest(interest,
      [&] (const ndn::Interest&, const ndn::Data& data) {
        if (data.getName() != exact || data.getContent().value_size() > 65536) {
          done = true; return;
        }
        payload.assign(reinterpret_cast<const char*>(data.getContent().value()),
                       data.getContent().value_size());
        received = true; done = true;
      },
      [&] (const ndn::Interest&, const ndn::lp::Nack&) { done = true; },
      [&] (const ndn::Interest&) { done = true; });
    while (!done && std::chrono::steady_clock::now() < deadline) {
      face.processEvents(ndn::time::milliseconds(20));
      if (cancelled && cancelled()) {
        throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: grant fetch cancelled");
      }
    }
  }
  if (!received) {
    throw std::runtime_error("DI_PROTECTED_GRANT_REJECTED: exact grant Data fetch timed out");
  }
  // This transport exposes bytes only to ProtectedRuntime's pinned authority
  // verifier. No permissive Data validator grants authorization here.
  return payload;
}
} // namespace ndnsf::di

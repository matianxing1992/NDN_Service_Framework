#pragma once

#include <ndn-cxx/data.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>

namespace ndn_service_framework {

/** Trusted local namespace signing permissions, independent of service grants.
 * Missing optional file permits only the Producer's own identity. */
class ProducerSignerPolicy
{
public:
  explicit ProducerSignerPolicy(const std::string& permissionFile = {});

  /** Call only after configured certificate/signature validation succeeds.
   * This checks namespace ownership; it does not verify packet signatures. */
  bool allowsValidatedSigner(const ndn::Data& data, const ndn::Name& producer) const;

  /** Resolve invocation ownership only at an authorized actor's exact boundary.
   * Call after cryptographic validation. Reserved words within actors are data. */
  std::optional<ndn::Name> findValidatedInvocationProducer(const ndn::Data& data) const;

private:
  std::map<ndn::Name, std::set<ndn::Name>> m_delegates;
};

} // namespace ndn_service_framework

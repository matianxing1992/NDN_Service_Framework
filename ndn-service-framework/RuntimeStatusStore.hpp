#ifndef NDN_SERVICE_FRAMEWORK_RUNTIME_STATUS_STORE_HPP
#define NDN_SERVICE_FRAMEWORK_RUNTIME_STATUS_STORE_HPP

#include "ControllerVersion.hpp"
#include "PolicyStatus.hpp"

#include <ndn-cxx/encoding/buffer.hpp>
#include <ndn-cxx/name.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace ndn_service_framework {

/**
 * Durable, opt-in store of the per-service Controller statuses accepted by
 * one User/Provider runtime (Spec179 FR-039).
 *
 * The store holds, for each service, the signed PolicyStatus Data wire
 * exactly as validated (so a restarted process can re-verify it against the
 * configured trust anchor), the PolicyStatusData wire, the bound ABE
 * public-parameter name/digest, the ControllerVersion, and the install
 * time.  It deliberately never contains DKEY or request-key material.
 *
 * Durability mirrors ControllerGenerationStore (magic header, length-fenced
 * records, `.tmp` + atomic rename, corrupt -> load reports failure), but one
 * runtime process owns the file, so no cross-process writer fence exists.
 * Persist rewrites the whole file atomically; records are small and per
 * runtime the service count is bounded.
 */
class RuntimeStatusStore
{
public:
  struct Record
  {
    ndn::Name serviceName;
    ControllerVersion controllerVersion;
    uint64_t installTimeMs = 0;
    ndn::Name abePublicParametersName;
    std::string abePublicParametersDigest;
    ndn::Buffer policyStatusWire; // PolicyStatusData wireEncode() output
    ndn::Buffer statusDataWire;   // signed PolicyStatus Data wire, re-verifiable
  };

  /** Whether the opt-in env gate (NDNSF_PERSIST_RUNTIME_STATE) is enabled. */
  static bool enabled();

  /**
   * Resolve the store path for one runtime role+identity:
   * $NDNSF_RUNTIME_STATE_DIR/<role>-<identity>.rts, default directory
   * $XDG_STATE_HOME|~/.local/state/ndnsf/runtime/.
   */
  static std::filesystem::path defaultStorePath(const std::string& role,
                                                const ndn::Name& identity);

  explicit RuntimeStatusStore(std::filesystem::path storePath);

  /**
   * Load all records.
   * @return false when the file is missing (cold start) or structurally
   *         corrupt/truncated (fail closed: nothing is restored and the
   *         caller starts from today's process-local behavior).  One
   *         undecodable or invalid record fails the whole load.
   * @throw std::runtime_error when the file cannot be opened at all.
   */
  bool load(std::vector<Record>& out) const;

  /** Atomically replace the store with the given records (mode 0600). */
  bool persist(const std::vector<Record>& records) const;

private:
  std::filesystem::path m_storePath;
};

} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_RUNTIME_STATUS_STORE_HPP

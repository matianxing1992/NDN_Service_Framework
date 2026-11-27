#ifndef NDNSF_EXAMPLES_DEPLOYMENT_EVIDENCE_ORACLE_HPP
#define NDNSF_EXAMPLES_DEPLOYMENT_EVIDENCE_ORACLE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace ndnsf::di::oracle {

[[noreturn]] inline void
deploymentEvidenceFailure(const std::string& reason)
{
  throw std::runtime_error("SPEC189_CPP_ORACLE_FAIL boundary=CONVERSATION reason=" + reason);
}

inline std::string
readFile(const std::filesystem::path& path)
{
  constexpr auto MAX_BYTES = 64U << 20;
  if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > MAX_BYTES)
    deploymentEvidenceFailure("missing-round-file");
  std::ifstream input(path, std::ios::binary);
  std::ostringstream data;
  if (!input) deploymentEvidenceFailure("missing-round-file");
  data << input.rdbuf();
  if (input.bad()) deploymentEvidenceFailure("missing-round-file");
  return data.str();
}

inline NativeJson
readJson(const std::filesystem::path& path)
{
  return nativeParseJson(readFile(path));
}

inline std::uint64_t
nonnegativeInteger(const NativeJson& value)
{
  if (!value.is_number_integer() ||
      (value.is_number_unsigned() ? value.get<std::uint64_t>() > INT64_MAX
                                 : value.get<std::int64_t>() < 0))
    deploymentEvidenceFailure("invalid-integer");
  return value.get<std::uint64_t>();
}

inline NativeJson
readPrivateJson(const std::filesystem::path& path)
{
  struct stat info{};
  if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) ||
      info.st_uid != ::getuid() || (info.st_mode & 0777) != 0600 ||
      info.st_size > 64 * 1024 * 1024 ||
      std::filesystem::canonical(path) != path.lexically_normal())
    deploymentEvidenceFailure("deployment-private-evidence");
  return readJson(path);
}

inline std::string
managementRoleId(const std::string& kind, const std::string& node)
{
  const auto value = kind + "." + node;
  return value.size() <= 128 ? value : kind + "." + ndnsf::di::nativePlanningDigest(node).substr(7);
}

inline bool
matchesHost(const std::string& configured, const std::string& observed)
{
  return configured == observed || configured == observed.substr(0, observed.find('.'));
}

inline bool
matchesJob(const std::string& cgroup, const std::string& job, const std::string& step = "")
{
  if (job.empty() || job.find_first_not_of("0123456789") != std::string::npos ||
      (!step.empty() && step.find_first_not_of("0123456789") != std::string::npos))
    return false;
  return std::regex_search(cgroup, std::regex("(?:^|/)job[_-]" + job + "(?:/|\\.)")) &&
    (step.empty() || std::regex_search(cgroup,
      std::regex("(?:^|/)step[_-]" + step + "(?:/|\\.|$)")));
}

inline std::set<std::string>
validateDeploymentGroups(const NativeJson& state, const std::string& boot,
                         const std::string& digest, bool strictIdentity)
{
  std::set<std::string> launched, cleaned;
  for (const auto& launch : state.at("launches")) {
    const auto id = launch.at("id").get<std::string>();
    const auto& proof = launch.at("proof");
    const auto& bindings = proof.at("bindings");
    if (!launched.insert(id).second ||
        bindings.at("NDNSF_DEPLOYMENT_ACTOR_ID") != id ||
        bindings.at("NDNSF_DEPLOYMENT_BOOT_ID") != boot ||
        bindings.at("NDNSF_DEPLOYMENT_CONFIG_SHA256") != digest ||
        launch.at("bindings") != bindings)
      deploymentEvidenceFailure("deployment-process-binding");
    if (strictIdentity && (nonnegativeInteger(proof.at("pid")) <= 1 ||
        nonnegativeInteger(proof.at("uid")) != ::getuid() ||
        nonnegativeInteger(proof.at("start_ticks")) == 0 ||
        nonnegativeInteger(proof.at("pgid")) != nonnegativeInteger(proof.at("pid")) ||
        !std::filesystem::path(proof.at("exe").get<std::string>()).is_absolute()))
      deploymentEvidenceFailure("deployment-process-identity");
  }
  for (const auto& item : state.at("cleanup")) {
    if (item.at("status") != "ZERO" || item.at("forced") != false ||
        !cleaned.insert(item.at("id").get<std::string>()).second)
      deploymentEvidenceFailure("deployment-cleanup");
  }
  if (launched.empty() || cleaned != launched)
    deploymentEvidenceFailure("deployment-cleanup-coverage");
  return launched;
}

inline std::map<std::string, NativeJson>
validateAllocation(const std::filesystem::path& root, const NativeJson& state,
                   const NativeJson& config, const std::string& boot,
                   const std::string& digest)
{
  const auto job = state.at("job_id").get<std::string>();
  const auto release = state.at("release_sha256").get<std::string>();
  if (state.at("execution") != "COMPLETE" ||
      state.at("evidence_root") != (root / "supervisor" /
        config.at("deployment_id").get<std::string>()).string() ||
      ndnsf::di::nativePlanningDigest(readFile(config.at("release").at("manifest")
        .get<std::string>())) != "sha256:" + release)
    deploymentEvidenceFailure("deployment-allocation-binding");
  const auto operation = readPrivateJson(root / "operation.json");
  const auto& reference = operation.at("job_reference");
  const auto& scheduler = operation.at("scheduler");
  if (operation.at("schema") != "ndnsf-di-slurm-operation-v1" ||
      operation.at("deployment_id") != config.at("deployment_id") ||
      operation.at("evidence_root") != root.string() ||
      operation.at("boot_id") != boot || operation.at("configuration_sha256") != digest ||
      reference.at("boot_id") != boot || reference.at("configuration_sha256") != digest ||
      reference.at("release_sha256") != release || reference.at("job_id") != job ||
      nonnegativeInteger(reference.at("owner_uid")) != ::getuid() ||
      scheduler.at("reference") != reference || scheduler.at("terminal") != true ||
      scheduler.at("status") != "TERMINAL" || scheduler.at("state") != "COMPLETED" ||
      scheduler.at("exit_code") != "0:0")
    deploymentEvidenceFailure("deployment-scheduler-binding");
  const auto record = readJson(config.at("platform").at("build_record").get<std::string>());
  auto unsignedRecord = record;
  unsignedRecord.erase("recordDigest");
  if (record.at("schemaVersion") != "ndnsf-local-sif-build-v3" ||
      (record.at("status") != "BUILT_UNQUALIFIED" && record.at("status") != "PASS") ||
      nonnegativeInteger(record.at("sif").at("bytes")) == 0 ||
      record.at("sif").at("sha256") != config.at("platform").at("sif_sha256") ||
      record.at("recordDigest") != ndnsf::di::nativePlanningDigest(
        ndnsf::di::nativeCanonicalJson(unsignedRecord)))
    deploymentEvidenceFailure("deployment-image-binding");

  const auto checkBinding = [&] (const NativeJson& value) {
    if (value.at("boot_id") != boot || value.at("configuration_sha256") != digest ||
        value.at("release_sha256") != release || value.at("job_id") != job)
      deploymentEvidenceFailure("deployment-node-binding");
  };
  std::map<std::string, NativeJson> nodes, roles, receipts;
  for (const auto& node : config.at("nodes")) {
    const auto id = node.at("id").get<std::string>();
    if (!nodes.emplace(id, node).second) deploymentEvidenceFailure("deployment-node-coverage");
    const auto audit = readPrivateJson(root / "nodes" / id / "host-audit-final.json");
    checkBinding(audit);
    if (audit.at("schema") != "ndnsf-di-host-node-audit-v1" || audit.at("node_id") != id ||
        audit.at("status") != "ZERO" || audit.at("cleanup_scope") != "HOST_UID_JOB" ||
        audit.at("processes") != NativeJson::array() || audit.at("errors") != NativeJson::array() ||
        !matchesHost(node.at("host"), audit.at("host")) ||
        !matchesJob(audit.at("auditor_cgroup"), job, audit.at("auditor_step_id")))
      deploymentEvidenceFailure("deployment-node-cleanup");
    for (const auto* kind : {"nfd", "nfdc"}) {
      if (std::string(kind) == "nfd" && node.at("nfd").at("mode") != "managed") continue;
      const auto roleId = managementRoleId(kind, id);
      if (!roles.emplace(roleId, NativeJson{{"id", roleId}, {"kind", kind}, {"node", id}}).second)
        deploymentEvidenceFailure("deployment-role-coverage");
    }
  }
  for (const auto& actor : config.at("actors")) {
    if (actor.at("external") != false ||
        !roles.emplace(actor.at("id").get<std::string>(), actor).second)
      deploymentEvidenceFailure("deployment-external-actor-not-qualified");
  }
  std::set<std::string> seen;
  for (const auto& role : state.at("roles")) {
    const auto id = role.at("id").get<std::string>();
    const auto expected = roles.find(id);
    if (expected == roles.end() || !seen.insert(id).second ||
        role.at("node") != expected->second.at("node") ||
        role.at("kind") != expected->second.at("kind"))
      deploymentEvidenceFailure("deployment-role-coverage");
  }
  if (seen.size() != roles.size()) deploymentEvidenceFailure("deployment-role-coverage");
  for (const auto& [id, role] : roles) {
    const auto node = role.at("node").get<std::string>();
    const auto kind = role.at("kind").get<std::string>();
    const auto path = kind == "nfd" || kind == "nfdc"
      ? root / "nodes" / node / kind / "receipt.json"
      : root / "actors" / id / "receipt.json";
    const auto receipt = readPrivateJson(path);
    checkBinding(receipt);
    const bool finite = kind == "requester" || kind == "nfdc";
    const bool hostGroups = receipt.at("cleanup_scope") == "HOST_AND_NATIVE_PROCESS_GROUPS";
    if (receipt.at("schema") != "ndnsf-di-node-role-v1" || receipt.at("role_id") != id ||
        receipt.at("role_kind") != kind || receipt.at("node_id") != node ||
        (!hostGroups && receipt.at("cleanup_scope") != "PID_NAMESPACE") ||
        receipt.at("status") != "STOPPED" ||
        !receipt.at("failure").is_null() ||
        !matchesHost(nodes.at(node).at("host"), receipt.at("host")) ||
        receipt.at("execution") != (finite ? "COMPLETE" : "SERVICE_STOPPED") ||
        (finite && nonnegativeInteger(receipt.at("exit_code")) != 0))
      deploymentEvidenceFailure("deployment-role-terminal");
    if (hostGroups && kind != "nfd" && kind != "nfdc") {
      if (!receipt.contains("image_inspection"))
        deploymentEvidenceFailure("deployment-image-inspection");
      const auto& inspection = receipt.at("image_inspection");
      if (inspection.at("status") != "VERIFIED" || inspection.at("exitCode") != 2 ||
          inspection.at("cleanup").at("status") != "ZERO" ||
          inspection.at("cleanup").at("forced") != false ||
          inspection.at("launcher_cleanup").at("forced") != false)
        deploymentEvidenceFailure("deployment-image-inspection");
    }
    validateDeploymentGroups(receipt, boot, digest, true);
    if (kind != "nfdc") {
      const auto audit = readPrivateJson(root / "nodes" / node / ("host-audit-" + id + ".json"));
      checkBinding(audit);
      if (audit.at("schema") != "ndnsf-di-host-node-audit-v1" || audit.at("node_id") != node ||
          audit.at("role_id") != id || audit.at("status") != "OBSERVED" ||
          !matchesHost(nodes.at(node).at("host"), audit.at("host")))
        deploymentEvidenceFailure("deployment-native-host-binding");
      const auto& proof = audit.at("native_process");
      const auto launch = std::find_if(receipt.at("launches").begin(), receipt.at("launches").end(),
        [&] (const auto& row) { return row.at("id") == id; });
      if (launch == receipt.at("launches").end()) deploymentEvidenceFailure("deployment-actor-not-launched");
      if (hostGroups && !launch->contains("namespace_proof"))
        deploymentEvidenceFailure("deployment-native-host-proof");
      const auto& native = launch->at(hostGroups ? "namespace_proof" : "proof");
      if (hostGroups) {
        const auto& hostProof = launch->at("proof");
        if (hostProof.at("pid") != proof.at("pid") ||
            hostProof.at("pgid") != hostProof.at("pid") ||
            hostProof.at("start_ticks") != native.at("start_ticks") ||
            hostProof.at("uid") != native.at("uid") || hostProof.at("exe") != native.at("exe") ||
            hostProof.at("bindings") != native.at("bindings"))
          deploymentEvidenceFailure("deployment-native-host-proof");
      }
      if (!proof.at("nspid").is_array() || proof.at("nspid").empty() ||
          nonnegativeInteger(proof.at("pid")) <= 1 ||
          nonnegativeInteger(proof.at("nspid").back()) != nonnegativeInteger(native.at("pid")) ||
          nonnegativeInteger(proof.at("start_ticks")) != nonnegativeInteger(native.at("start_ticks")) ||
          nonnegativeInteger(proof.at("uid")) != nonnegativeInteger(native.at("uid")) ||
          proof.at("exe") != native.at("exe") ||
          proof.at("bindings") != native.at("bindings") ||
          proof.at("pid_namespace") != receipt.at("pid_namespace") ||
          !matchesJob(proof.at("cgroup"), job, receipt.at("step_id")))
        deploymentEvidenceFailure("deployment-native-host-proof");
    }
    receipts.emplace(id, receipt);
  }
  return receipts;
}

inline std::vector<std::string>
validateGpuAdmission(const std::filesystem::path& root, const NativeJson& state,
                     const NativeJson& config, const NativeJson& actor,
                     const NativeJson& receipt)
{
  const auto node = actor.at("node").get<std::string>();
  const auto pool = readPrivateJson(root / "nodes" / node / "gpu-allocation.json");
  if (pool != state.at("gpu_allocations").at(node))
    deploymentEvidenceFailure("deployment-gpu-pool-binding");
  const auto nodeConfig = std::find_if(config.at("nodes").begin(), config.at("nodes").end(),
    [&] (const auto& item) { return item.at("id") == node; });
  if (nodeConfig == config.at("nodes").end()) deploymentEvidenceFailure("deployment-gpu-node");
  const auto check = [&] (const NativeJson& value, std::size_t expectedCount) {
    const auto& probe = value.at("native_probe");
    if (value.at("schema") != "ndnsf-di-gpu-allocation-v1" || value.at("node_id") != node ||
        value.at("boot_id") != state.at("boot_id") ||
        value.at("configuration_sha256") != state.at("configuration_sha256") ||
        value.at("release_sha256") != state.at("release_sha256") ||
        value.at("job_id") != state.at("job_id") ||
        !matchesHost(nodeConfig->at("host"), value.at("host")) ||
        probe.at("schema") != "ndnsf-di-gpu-probe-v1" ||
        probe.at("event") != "NDNSF_DI_GPU_PROBE" || probe.at("device_order") != "PCI_BUS_ID" ||
        probe.at("step_id").get<std::string>().empty() ||
        probe.at("step_id").get<std::string>().find_first_not_of("0123456789") != std::string::npos ||
        value.at("devices").size() != expectedCount ||
        probe.at("visible_uuids").size() != expectedCount)
      deploymentEvidenceFailure("deployment-gpu-probe-binding");
    for (const auto* key : {"boot_id", "configuration_sha256", "release_sha256", "job_id", "host", "visibility"}) {
      if (probe.at(key) != value.at(key)) deploymentEvidenceFailure("deployment-gpu-probe-binding");
    }
    std::vector<std::string> uuids;
    std::set<std::string> physicalIds, identities;
    for (const auto& item : value.at("devices")) {
      const auto uuid = item.at("uuid").get<std::string>();
      const auto physical = item.at("physical_id").get<std::string>();
      if (!identities.insert(uuid).second || !physicalIds.insert(physical).second ||
          uuid.compare(0, 4, "GPU-") != 0 || physical.empty() ||
          physical.find_first_not_of("0123456789") != std::string::npos ||
          probe.at("visible_uuids").at(uuids.size()) != uuid ||
          std::count_if(probe.at("inventory").begin(), probe.at("inventory").end(),
            [&] (const auto& row) { return row.at("physical_id") == physical && row.at("uuid") == uuid; }) != 1)
        deploymentEvidenceFailure("deployment-gpu-uuid-inventory");
      uuids.push_back(uuid);
    }
    return uuids;
  };
  const auto count = nonnegativeInteger(config.at("platform").at("slurm").at("gpu").at("count"));
  if (count == 0 || count > 64) deploymentEvidenceFailure("deployment-gpu-count");
  check(pool, count);
  std::string fullVisibility;
  for (std::uint64_t index = 0; index < count; ++index)
    fullVisibility += (index ? "," : "") + std::to_string(index);
  if (pool.at("visibility") != fullVisibility)
    deploymentEvidenceFailure("deployment-gpu-pool-binding");
  if (pool.at("native_probe").at("role_id") != managementRoleId("gpu-probe", node))
    deploymentEvidenceFailure("deployment-gpu-pool-binding");
  std::set<std::string> declared;
  for (const auto& selector : actor.at("resources").at("gpu_devices")) {
    const auto selected = selector.get<std::string>();
    std::vector<std::string> matches;
    if (selected.compare(0, 10, "allocated:") == 0) {
      const auto index = selected.substr(10);
      if (index.empty() || index.size() > 2 ||
          (index.size() > 1 && index.front() == '0') ||
          index.find_first_not_of("0123456789") != std::string::npos)
        deploymentEvidenceFailure("deployment-gpu-selector");
      const auto slot = std::stoul(index);
      if (slot >= count)
        deploymentEvidenceFailure("deployment-gpu-selector");
      matches.push_back(pool.at("devices").at(slot).at("uuid").get<std::string>());
    }
    else {
      for (const auto& item : pool.at("devices")) {
        if (item.at("physical_id") == selected || item.at("uuid") == selected)
          matches.push_back(item.at("uuid").get<std::string>());
      }
    }
    if (matches.size() != 1 || !declared.insert(matches.front()).second)
      deploymentEvidenceFailure("deployment-gpu-selector");
  }
  const auto& admission = receipt.at("gpu_admission");
  const auto selectedUuids = check(admission, declared.size());
  const auto& probe = admission.at("native_probe");
  const bool hostGroups = receipt.at("cleanup_scope") == "HOST_AND_NATIVE_PROCESS_GROUPS";
  if (hostGroups && (!admission.contains("query_launch_id") ||
      probe.at("role_id") != receipt.at("role_id")))
    deploymentEvidenceFailure("deployment-gpu-role-binding");
  const auto query = std::find_if(receipt.at("launches").begin(), receipt.at("launches").end(),
    [&] (const auto& row) { return row.at("id") ==
      (hostGroups ? admission.at("query_launch_id") : probe.at("role_id")); });
  const NativeJson command = NativeJson::array({
    (std::filesystem::path(config.at("release").at("prefix").get<std::string>()) /
      "libexec/ndnsf-di/ndnsf-di-device-query").string(), "--count", std::to_string(declared.size())});
  if (std::set<std::string>(selectedUuids.begin(), selectedUuids.end()) != declared ||
      probe.at("step_id") != receipt.at("step_id") ||
      admission.at("visibility") != receipt.at("gpu_visibility") ||
      query == receipt.at("launches").end() ||
      (hostGroups && !query->contains("native_argv")) ||
      query->at(hostGroups ? "native_argv" : "argv") != command ||
      nonnegativeInteger(query->at("exit_code")) != 0)
    deploymentEvidenceFailure("deployment-gpu-role-binding");
  return selectedUuids;
}

} // namespace ndnsf::di::oracle

#endif // NDNSF_EXAMPLES_DEPLOYMENT_EVIDENCE_ORACLE_HPP

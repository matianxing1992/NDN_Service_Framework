#include "wan-video-reference.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"
#include "examples/deployment-evidence-oracle.hpp"

#include <ndn-cxx/name.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {

using namespace ndnsf::di;
namespace fixture = wan::fixture;
namespace fs = std::filesystem;

constexpr std::size_t MIB = 1U << 20;
constexpr std::size_t MAX_EVIDENCE_ROW_BYTES = 64 * MIB;

void
require(bool condition, const std::string& failure)
{
  if (!condition) throw std::runtime_error(failure);
}

std::string
readFile(const fs::path& path, std::uintmax_t limit = MIB)
{
  const auto size = fs::file_size(path);
  require(size <= limit && fs::is_regular_file(path), "receipt file exceeds its bound");
  std::string data(size, '\0');
  std::ifstream stream(path, std::ios::binary);
  require(bool(stream.read(data.data(), data.size())) && stream.peek() == EOF,
          "invalid receipt file: " + path.string());
  return data;
}

NativeJson
readJson(const fs::path& path)
{
  return nativeParseJson(readFile(path));
}

template<typename T>
ndn::span<const std::uint8_t>
bytes(ndn::span<const T> data)
{
  return {reinterpret_cast<const std::uint8_t*>(data.data()), data.size_bytes()};
}

std::string
inputDigest(const std::vector<NamedTensorView>& values)
{
  const auto wire = encodeTensorBundleViews(values);
  return nativePlanningDigest(wire.data(), wire.size());
}

using RequestRows = std::map<std::string, std::map<std::string, NativeJson>>;

struct ProviderTrace
{
  bool expectedSession = false;
  std::set<std::pair<std::string, std::string>> calls;
  // Session, producing role and consuming role -> matching original DATA_V1 names.
  using EdgeNames = std::map<std::array<std::string, 3>, std::set<std::string>>;
  EdgeNames sent;
  EdgeNames received;
};

struct DeploymentEvidence
{
  RequestRows requests;
  std::map<std::string, ProviderTrace> traces;
};

ProviderTrace
readProviderTrace(const fs::path& path, const ndn::Name& producer, std::size_t byteLimit,
                  bool cuda)
{
  require(fs::file_size(path) <= byteLimit, "Provider timing exceeds fixture budget");
  ProviderTrace result;
  std::ifstream log(path);
  std::vector<char> rowBuffer(MAX_EVIDENCE_ROW_BYTES + 1);
  while (log.getline(rowBuffer.data(), rowBuffer.size())) {
    const auto rowLength = static_cast<std::size_t>(log.gcount()) - (log.eof() ? 0 : 1);
    const std::string_view line(rowBuffer.data(), rowLength);
    constexpr std::string_view LOAD = "NDNSF_DI_ONNX_SESSION_LOAD ";
    constexpr std::string_view CALL = "NDNSF_DI_ONNX_TIMING ";
    constexpr std::string_view OBJECT = "NDNSF_DI_DEPENDENCY_OBJECT ";
    if (const auto at = line.find(LOAD); at != std::string_view::npos) {
      result.expectedSession |= line.find(cuda ? " provider=cuda " : " provider=cpu ", at) !=
                                std::string_view::npos;
      continue;
    }
    const auto call = line.find(CALL);
    const auto object = line.find(OBJECT);
    if (call == std::string_view::npos && object == std::string_view::npos) continue;
    std::istringstream fields{std::string(line.substr(call != std::string_view::npos ?
      call + CALL.size() : object + OBJECT.size()))};
    std::map<std::string, std::string> row;
    for (std::string field; fields >> field;) {
      const auto split = field.find('=');
      if (split != std::string::npos) row.emplace(field.substr(0, split), field.substr(split + 1));
    }
    const auto& session = row.at("session");
    if (call != std::string_view::npos) {
      if (session != "native-runtime-warmup") result.calls.emplace(session, row.at("role"));
      continue;
    }
    const auto& direction = row.at("direction");
    if (row.at("status") != "ok" ||
        (direction != "publish-exact-ndn" && direction != "fetch-exact-ndn")) continue;
    const auto& name = row.at("planned_name");
    const std::array<std::string, 3> key{session, row.at("producer"), row.at("consumer")};
    if (direction == "publish-exact-ndn") {
      require(producer.isPrefixOf(ndn::Name(name)), "exact publication outside Provider namespace");
      result.sent[key].insert(name);
    }
    else {
      result.received[key].insert(name);
    }
  }
  require(log.eof() && !log.bad(), "Provider timing log read failed or has an oversized row");
  require(result.expectedSession && !result.calls.empty(), "missing actual ORT session or calls");
  return result;
}

void
verifyCudaRequest(const NativeJson& row, const NativeJson& process,
                  const std::string& uuid, const std::string& visibility,
                  const std::set<std::pair<std::string, std::string>>& admittedRequests)
{
  require(!row.at("finalExecutionEvidence").is_null(), "missing CUDA request evidence");
  const auto evidence = executionEvidenceFromJson(row.at("finalExecutionEvidence").dump());
  const auto role = row.at("role").get<std::string>();
  const auto artifact = evidence.artifactDigests.find(role);
  require(evidence.runnerKind == RunnerKind::OnnxRuntimeCuda && evidence.deviceKind == "cuda" &&
          evidence.deviceId == "0" && evidence.realCompute && !evidence.cpuFallbackUsed &&
          evidence.loadCompleted && evidence.warmupCompleted && evidence.executionCompleted &&
          !evidence.exactForwardCacheHit && evidence.gpuUuid == uuid &&
          evidence.gpuIdentitySource == "cuda-runtime-pci+driver-uuid" &&
          evidence.gpuUuids == std::vector<std::string>{uuid} &&
          evidence.cudaVisibleDevices == visibility && evidence.processId == process.at("pid") &&
          evidence.requestId == row.at("requestId") && evidence.attemptEpoch == 1 &&
          evidence.providerName == row.at("provider") &&
          evidence.providerBootId == row.at("providerBootId") &&
          evidence.planDigest == row.at("planDigest") &&
          evidence.modelDigest == row.at("admittedModelDigest") &&
          evidence.roles == std::vector<std::string>{role} &&
          artifact != evidence.artifactDigests.end() &&
          artifact->second == row.at("admittedArtifactDigest"),
          "CUDA request, device or artifact evidence mismatch");

  const auto entries = row.at("sealedComputeEntries").get<std::vector<std::string>>();
  const std::set<std::string> expected(entries.begin(), entries.end());
  std::set<std::string> profiles, calls, kernels;
  require(!expected.empty() && expected.size() == entries.size(), "missing sealed CUDA entries");
  for (const auto& profile : evidence.entryProfiles) {
    require(profile.attemptEpoch == 1 &&
            admittedRequests.count({profile.requestId, artifact->second}) != 0 &&
            profiles.insert(profile.entryId).second, "CUDA profile request or entry mismatch");
  }
  for (const auto& metrics : row.at("runtimeMetrics")) {
    for (const auto& step : metrics.at("stepBackendExecutions")) {
      if (step.at("operation") != "CALL_SUBGRAPH") continue;
      require(step.at("completed") == true, "incomplete CUDA entry call");
      calls.insert(step.at("entryId").get<std::string>());
    }
  }
  for (const auto& node : evidence.getNodeProviderAssignments()) {
    if (!node.modelNode) continue;
    require(!node.nodeName.empty() && node.provider == "CUDAExecutionProvider",
            "model node executed outside CUDA EP");
    kernels.insert(node.role);
  }
  require(profiles == expected && calls == expected && kernels == expected,
          "incomplete sealed CUDA entry evidence");
}

void
readProviderStatistics(DeploymentEvidence& result, const fs::path& statistics,
                       const std::string& provider, const NativeJson& launch,
                       std::size_t byteLimit, const std::string& uuid = "",
                       const std::string& visibility = "")
{
  require(fs::file_size(statistics) <= byteLimit, "Provider statistics exceed fixture budget");
  std::ifstream stats(statistics);
  require(bool(stats), "missing Provider runtime statistics");
  std::vector<char> rowBuffer(MAX_EVIDENCE_ROW_BYTES + 1);
  std::set<std::pair<std::string, std::string>> admittedRequests;
  while (stats.getline(rowBuffer.data(), rowBuffer.size())) {
    const auto rowLength = static_cast<std::size_t>(stats.gcount()) - (stats.eof() ? 0 : 1);
    const auto row = nativeParseJson(std::string(rowBuffer.data(), rowLength));
    require(row.at("schema") == "ndnsf-di-native-request-statistics-v1" &&
            row.at("provider") == provider && row.at("providerBootId") == launch.at("native_boot_id"),
            "Provider statistics identity");
    if (row.at("type") != "request") continue;
    const auto request = row.at("requestId").get<std::string>();
    const auto artifact = row.at("admittedArtifactDigest").get<std::string>();
    require(!request.empty() && row.at("completionAdmitted") == true &&
            row.at("failureReason").is_null() && row.at("attemptEpoch") == 1 &&
            !artifact.empty() && row.at("role").is_string() && row.at("planDigest").is_string() &&
            result.requests[request].emplace(provider, NativeJson{
              {"role", row.at("role")}, {"planDigest", row.at("planDigest")}}).second,
            "Provider completion or artifact binding");
    admittedRequests.emplace(request, artifact);
    if (!uuid.empty()) {
      verifyCudaRequest(row, launch.at(launch.contains("namespace_proof") ?
                                      "namespace_proof" : "proof"),
                        uuid, visibility, admittedRequests);
    }
  }
  require(stats.eof() && !stats.bad(), "Provider statistics read failed or has an oversized row");
  require(!admittedRequests.empty(), "missing Provider runtime statistics");
}

DeploymentEvidence
readSlurmDeployment(const NativeJson& state, const NativeJson& config, const fs::path& root,
                    std::size_t logByteLimit, std::size_t statisticsByteLimit,
                    bool requireDistinctGpuNodes)
{
  const auto boot = state.at("boot_id").get<std::string>();
  const auto digest = state.at("configuration_sha256").get<std::string>();
  require(state.at("schema") == "ndnsf-di-host-operation-v1" && state.at("status") == "STOPPED" &&
          state.at("execution") == "COMPLETE" && state.at("external_references").empty() &&
          state.at("failure").is_null() && state.at("deployment_id") == config.at("deployment_id") &&
          digest == nativePlanningDigest(nativeCanonicalJson(config)).substr(7),
          "invalid Slurm allocation state");
  oracle::readPrivateJson(root / "allocation.json");
  oracle::readPrivateJson(root / "configuration.json");
  oracle::validateDeploymentGroups(state, boot, digest, true);
  const auto receipts = oracle::validateAllocation(root, state, config, boot, digest);
  DeploymentEvidence result;
  std::set<std::string> nodes, devices;
  for (const auto& actor : config.at("actors")) {
    require(actor.at("kind") != "repo", "unexpected Repo in Wan deployment");
    if (actor.at("kind") != "provider") continue;
    const auto id = actor.at("id").get<std::string>();
    const auto provider = actor.at("identity").get<std::string>();
    const auto& receipt = receipts.at(id);
    const auto uuid = oracle::validateGpuAdmission(root, state, config, actor, receipt);
    require(!requireDistinctGpuNodes || nodes.insert(actor.at("node").get<std::string>()).second,
            "Wan checkpoint Providers require distinct nodes");
    require(uuid.size() == 1 && devices.insert(uuid.front()).second,
            "Wan Providers require distinct devices");
    const auto launches = receipt.at("launches");
    const auto launch = std::find_if(launches.begin(), launches.end(),
                                     [&] (const auto& row) { return row.at("id") == id; });
    require(launch != launches.end() && launch->at("native_identity") == provider,
            "missing Slurm native Provider launch");
    const auto runtime = root / "actors" / id / "runtime" / config.at("deployment_id").get<std::string>();
    const auto statistics = actor.at("parameters").value(
      "statistics-file", (runtime / (id + "-statistics.jsonl")).string());
    readProviderStatistics(result, statistics, provider, *launch, statisticsByteLimit, uuid.front(),
                           receipt.at("gpu_visibility").get<std::string>());
    result.traces.emplace(provider, readProviderTrace(runtime / (id + ".log"),
                                                    ndn::Name(provider), logByteLimit, true));
  }
  return result;
}

DeploymentEvidence
readDeployment(const NativeJson& state, const NativeJson& configuration, const fs::path& root,
               std::size_t logByteLimit = 64 * MIB,
               std::size_t statisticsByteLimit = 8 * MIB, bool requireDistinctGpuNodes = true)
{
  if (configuration.at("platform").at("kind") == "slurm-apptainer") {
    return readSlurmDeployment(state, configuration, root, logByteLimit, statisticsByteLimit,
                               requireDistinctGpuNodes);
  }
  const auto boot = state.at("boot_id");
  const auto digest = state.at("configuration_sha256");
  require(state.at("schema") == "ndnsf-di-host-operation-v1" &&
          state.at("execution") == "COMPLETE" && state.at("status") == "STOPPED" &&
          state.value("failure", NativeJson(nullptr)).is_null() &&
          !state.at("needs_closeout").get<bool>() &&
          state.at("external_references").empty() &&
          configuration.at("platform").at("kind") == "minindn" &&
          digest == nativePlanningDigest(nativeCanonicalJson(configuration)).substr(7) &&
          state.at("deployment_id") == configuration.at("deployment_id") &&
          state.at("requester").at("completion") == "EXITED" &&
          state.at("requester").at("exit_code") == 0, "deployment state or config binding");
  const auto& facility = state.at("facility");
  require(facility.at("status") == "STOPPED" && facility.at("failure").is_null() &&
          facility.at("namespace_survivors").empty() &&
          facility.at("remaining_interfaces").empty(), "MiniNDN facility closeout");
  const auto checkCleanup = [] (const NativeJson& receipts, const NativeJson& launches) {
    std::map<int, std::string> pending;
    for (const auto& launch : launches) {
      const auto& proof = launch.at("proof");
      const auto pid = proof.at("pid").get<int>();
      require(pid > 0 && proof.at("start_ticks").get<std::uint64_t>() > 0 &&
              pending.emplace(pid, launch.at("id").get<std::string>()).second,
              "launch process identity");
    }
    for (const auto& receipt : receipts) {
      const auto pid = receipt.at("pid").get<int>();
      require(receipt.at("status") == "ZERO" && receipt.at("forced") == false &&
              pending.count(pid) && receipt.at("id") == pending.at(pid), "forced or unmatched cleanup");
      pending.erase(pid);
    }
    require(pending.empty(), "missing process cleanup receipt");
  };
  checkCleanup(state.at("cleanup"), state.at("launches"));
  checkCleanup(facility.at("cleanup"), facility.at("launches"));
  std::map<std::string, std::string> namespaces;
  std::set<std::string> uniqueNamespaces;
  for (const auto& node : configuration.at("nodes")) {
    const auto id = node.at("id").get<std::string>();
    const auto& hosted = facility.at("nodes").at(id);
    const auto ns = hosted.at("network_namespace").get<std::string>();
    require(ns.compare(0, 5, "net:[") == 0 && uniqueNamespaces.insert(ns).second &&
            hosted.at("host") == node.at("host"), "MiniNDN namespace identity");
    namespaces.emplace(id, ns);
  }
  require(namespaces.size() == 4 && facility.at("nodes").size() == namespaces.size(),
          "MiniNDN topology size");
  std::map<std::string, NativeJson> launches;
  for (const auto& launch : state.at("launches")) {
    const auto id = launch.at("id").get<std::string>();
    const auto& proof = launch.at("proof");
    const auto& binding = proof.at("bindings");
    require(binding.at("NDNSF_DEPLOYMENT_ACTOR_ID") == id &&
            binding.at("NDNSF_DEPLOYMENT_BOOT_ID") == boot &&
            binding.at("NDNSF_DEPLOYMENT_CONFIG_SHA256") == digest &&
            launch.at("expected_exe") == proof.at("exe") &&
            launches.emplace(id, launch).second, "launch/configuration binding");
  }
  for (const auto& [id, ns] : namespaces)
    require(launches.at("nfd." + id).at("network_namespace") == ns, "NFD namespace binding");

  const auto routes = readJson(root / "routes.json");
  std::set<std::string> routed;
  for (const auto& command : routes.at("commands")) {
    require(command.at("exit_code") == 0, "MiniNDN route setup failed");
    const auto& args = command.at("arguments");
    if (args.size() > 3 && args[1] == "route" && args[2] == "add")
      routed.insert(args[3].get<std::string>());
  }
  DeploymentEvidence result;
  std::set<std::string> providerNamespaces;
  for (const auto& actor : configuration.at("actors")) {
    const auto id = actor.at("id").get<std::string>();
    const auto& launch = launches.at(id);
    const auto& node = actor.at("node").get<std::string>();
    require(launch.at("network_namespace") == namespaces.at(node), "actor network namespace");
    if (actor.at("kind") != "provider") continue;
    const auto provider = actor.at("identity").get<std::string>();
    const auto exe = fs::path(launch.at("proof").at("exe").get<std::string>());
    const auto prefix = fs::path(configuration.at("release").at("prefix").get<std::string>());
    require(exe == prefix / "bin/di-native-provider" && launch.at("native_identity") == provider &&
            providerNamespaces.insert(namespaces.at(node)).second && routed.count(provider) &&
            routed.count(provider + "/KEY"), "Provider identity, route or executable");
    readProviderStatistics(result, root / (id + "-statistics.jsonl"), provider, launch,
                           statisticsByteLimit);
    result.traces.emplace(provider, readProviderTrace(root / (id + ".log"), ndn::Name(provider),
                                                    logByteLimit, false));
  }
  require(launches.size() == configuration.at("actors").size() + namespaces.size(),
          "unexpected actor launches");
  return result;
}

void
verifyRoles(const std::string& phase, const NativeJson& receipt, const NativeJson& mode,
            const NativeJson& placement, DeploymentEvidence& observed)
{
  const auto id = receipt.at("request_id").get<std::string>();
  const auto component = phase.compare(0, 5, "text/") == 0 ? "umt5" :
                         phase == "decode" ? "vae" : "dit";
  const auto& actual = observed.requests.at(id);
  const auto sessionFor = [&] (const ProviderTrace& trace, const std::string& role) {
    if (trace.calls.count({id, role})) return id;
    const auto scoped = ExecutionAttemptKey{id, 1}.scopedSessionId();
    require(trace.calls.count({scoped, role}) != 0, "missing actual ORT role call: " + role);
    return scoped;
  };
  std::set<std::string> wanted;
  for (const auto& stage : mode.at(component).at("stages")) {
    for (const auto& role : stage) {
      const auto roleName = role.get<std::string>();
      const auto provider = placement.at(roleName).get<std::string>();
      const auto& report = actual.at(provider);
      require(wanted.insert(provider).second && report.at("role") == role &&
              report.at("planDigest") == receipt.at("plan_digest"),
              "selected Provider binding: " + phase);
      (void)sessionFor(observed.traces.at(provider), roleName);
    }
    if (stage.size() == 2) {
      for (std::size_t rank = 0; rank < 2; ++rank) {
        const auto sender = stage.at(rank).get<std::string>();
        const auto receiver = stage.at(1 - rank).get<std::string>();
        const auto& senderTrace = observed.traces.at(placement.at(sender).get<std::string>());
        const auto& receiverTrace = observed.traces.at(placement.at(receiver).get<std::string>());
        const auto& sent = senderTrace.sent.at({sessionFor(senderTrace, sender), sender, receiver});
        const auto& received = receiverTrace.received.at(
          {sessionFor(receiverTrace, receiver), sender, receiver});
        require(std::any_of(sent.begin(), sent.end(), [&](const std::string& name) {
          return received.count(name) != 0;
        }), "missing paired TP publish and consumption: " + phase + "/" + sender);
      }
    }
  }
  require(wanted.size() == actual.size(), "unexpected Provider completion: " + phase);
  observed.requests.erase(id);
}

class ClipVerifier
{
public:
  ClipVerifier(fs::path path, NativeJson manifest, std::size_t byteLimit = MIB)
    : m_path(std::move(path))
    , m_manifest(std::move(manifest))
    , m_byteLimit(byteLimit)
  {
  }

  std::vector<float>
  tensor(const std::string& phase, const std::string& name,
         const std::vector<std::int64_t>& shape, const std::vector<double>& expected,
         double absolute, double relative, const NativeJson& binding)
  {
    const auto& entry = m_manifest.at("fixture_trace").at(m_index);
    const auto filename = "trace-" + std::to_string(m_index++) + ".bundle";
    const auto wire = readFile(m_path / filename, std::min(512 * MIB, m_byteLimit - m_total));
    m_total += wire.size();
    require(entry.size() == 3 + binding.size() && entry.at("phase") == phase &&
            entry.at("file") == filename && entry.at("digest") == nativePlanningDigest(wire),
            "invalid trace receipt: " + phase);
    for (const auto& [key, value] : binding.items())
      require(entry.at(key) == value, "trace context mismatch: " + phase);
    const std::vector<std::uint8_t> payload(wire.begin(), wire.end());
    const auto decoded = decodeTensorBundleViews(payload);
    require(decoded.size() == 1 && decoded[0].name == name &&
            decoded[0].elementType == TensorElementType::Float32 && decoded[0].shape == shape &&
            decoded[0].payload.size() == expected.size() * sizeof(float),
            "invalid trace tensor: " + phase);
    std::vector<float> actual(expected.size());
    std::memcpy(actual.data(), decoded[0].payload.data(), decoded[0].payload.size());
    double maximum = 0;
    std::size_t failures = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
      const auto error = std::abs(actual[i] - expected[i]);
      if (!std::isfinite(actual[i]) || !std::isfinite(expected[i]))
        throw std::runtime_error("nonfinite tensor value: " + phase + "/" + std::to_string(i));
      if (error > absolute + relative * std::abs(expected[i])) {
        ++failures;
        if (m_firstNumericalFailure.empty())
          m_firstNumericalFailure = phase + "/" + std::to_string(i);
      }
      maximum = std::max(maximum, error);
    }
    std::cout << phase << " max_abs=" << maximum << " cells=" << actual.size()
              << " out_of_tolerance=" << failures << '\n';
    return actual;
  }

  void
  requireNumericalMatch() const
  {
    require(m_firstNumericalFailure.empty(),
            "FP32 value differs from the independent oracle: " + m_firstNumericalFailure);
  }

  void
  frames(const std::vector<double>& expected,
         const std::array<std::int64_t, 3>& geometry = {9, 32, 32}, std::int64_t fps = 16,
         bool float32Conversion = false) const
  {
    require(m_manifest.at("fixture_trace").size() == m_index &&
            m_manifest.at("schema") == "ndnsf-di-wan-rgb24-v1" &&
            m_manifest.at("file") == "frames.rgb24" && m_manifest.at("format") == "rgb24" &&
            m_manifest.at("frames") == geometry[0] && m_manifest.at("height") == geometry[1] &&
            m_manifest.at("width") == geometry[2] && m_manifest.at("first_pts") == 0 &&
            m_manifest.at("ticks_per_frame") == 1 &&
            m_manifest.at("time_base") == NativeJson::array({1, fps}), "RGB frame manifest");
    const auto wire = readFile(m_path / "frames.rgb24", expected.size());
    require(wire.size() == expected.size() && m_manifest.at("bytes") == wire.size() &&
            m_manifest.at("digest") == nativePlanningDigest(wire), "RGB file binding");
    const auto pixels = expected.size() / 3;
    for (std::size_t i = 0; i < pixels; ++i) {
      for (std::size_t c = 0; c < 3; ++c) {
        unsigned char value;
        if (float32Conversion) {
          const float sample = static_cast<float>(expected[c * pixels + i]);
          const float normalized = (std::clamp(sample, -1.0f, 1.0f) + 1.0f) * 0.5f;
          value = static_cast<unsigned char>(normalized * 255.0f);
        }
        else {
          const auto sample = (std::clamp(expected[c * pixels + i], -1.0, 1.0) + 1) * 127.5;
          value = static_cast<unsigned char>(sample);
        }
        require(static_cast<unsigned char>(wire[3 * i + c]) == value,
                "RGB24 byte differs from the verified tensor: " + std::to_string(3 * i + c));
      }
    }
  }

private:
  fs::path m_path;
  NativeJson m_manifest;
  std::size_t m_index = 0;
  std::size_t m_total = 0;
  std::size_t m_byteLimit;
  std::string m_firstNumericalFailure;
};

std::size_t
referenceTensorBytes(const std::vector<std::int64_t>& shape)
{
  require(!shape.empty() && shape.size() <= 5, "reference tensor rank");
  std::size_t size = sizeof(float);
  for (const auto extent : shape) {
    require(extent > 0 && static_cast<std::uint64_t>(extent) <= 512 * MIB / size,
            "reference tensor byte bound");
    size *= static_cast<std::size_t>(extent);
  }
  return size;
}

std::vector<double>
referenceValues(const NativeJson& row, const fs::path& root)
{
  require(row.at("dtype") == "float32", "reference tensor dtype");
  const auto shape = row.at("shape").get<std::vector<std::int64_t>>();
  const auto wire = readFile(root / row.at("file").get<std::string>(), referenceTensorBytes(shape));
  require(wire.size() == referenceTensorBytes(shape) &&
          row.at("digest") == nativePlanningDigest(wire), "reference tensor identity");
  std::vector<double> result(wire.size() / sizeof(float));
  for (std::size_t i = 0; i < result.size(); ++i) {
    float value;
    std::memcpy(&value, wire.data() + i * sizeof(float), sizeof(float));
    require(std::isfinite(value), "nonfinite reference tensor");
    result[i] = value;
  }
  return result;
}

void
validateCheckpoint(const fs::path& operation, const fs::path& referenceFile,
                   const fs::path& workloadFile, const fs::path& initialFile, bool conditioned)
{
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
  const std::uint16_t endian = 1;
  require(*reinterpret_cast<const std::uint8_t*>(&endian) == 1, "reference needs little-endian FP32");
  const auto reference = readJson(referenceFile);
  const auto workloadWire = readFile(workloadFile);
  const auto workload = nativeParseJson(workloadWire);
  const auto* schema = conditioned ? "ndnsf-di-wan22-conditioned-trajectory-reference-v1" :
                                     "ndnsf-di-wan22-trajectory-reference-v1";
  require(reference.at("schema") == schema &&
          workload.at("schema") == "ndnsf-di-wan-checkpoint-workload-v1" &&
          reference.at("generation") == workload.at("generation"), "checkpoint reference workload");
  if (conditioned) {
    require(reference.at("provenance").at("workload_digest") == nativePlanningDigest(workloadWire),
            "conditioned reference workload digest differs");
  }
  require(reference.at("absolute_tolerance") == 2e-4 &&
          reference.at("relative_tolerance") == 2e-4, "checkpoint reference criterion");
  const auto& generation = reference.at("generation");
  const auto shape = generation.at("latent_shape").get<std::array<std::int64_t, 4>>();
  const bool compactGeometry = shape[2] >= 2 && shape[2] <= 20 && shape[2] % 2 == 0 &&
                               shape[3] >= 2 && shape[3] <= 20 && shape[3] % 2 == 0;
  const bool qualityGeometry = shape[1] == 7 && shape[2] == 44 && shape[3] == 80;
  require(shape[0] == 48 && shape[1] > 0 && shape[1] <= 91 &&
          (compactGeometry || qualityGeometry) &&
          generation.at("vae_spatial_stride") == 16 && generation.at("text_width") == 4096 &&
          generation.at("frames_per_second") == 24 && generation.at("shift") == 5 &&
          generation.at("guidance") == 5, "checkpoint geometry or sampler profile");
  const auto steps = generation.at("steps").get<std::size_t>();
  require(steps > 0 && steps <= 50 && reference.at("tensors").size() == 3 + 3 * steps,
          "incomplete checkpoint reference trajectory");
  const std::vector<std::int64_t> latentShape{1, shape[0], shape[1], shape[2], shape[3]};
  const auto& initial = reference.at("initial");
  const auto initialWire = readFile(initialFile, referenceTensorBytes(latentShape));
  require(initialWire.size() == referenceTensorBytes(latentShape) &&
          initial.at("shape") == latentShape && initial.at("bytes") == initialWire.size() &&
          initial.at("seed") == generation.at("seed") && initial.at("dtype") == "float32" &&
          initial.at("byte_order") == "little" &&
          initial.at("digest") == nativePlanningDigest(initialWire), "frozen initial input binding");
  std::vector<float> latent(initialWire.size() / sizeof(float));
  std::memcpy(latent.data(), initialWire.data(), initialWire.size());
  require(std::all_of(latent.begin(), latent.end(), [] (float value) { return std::isfinite(value); }),
          "nonfinite frozen initial input");
  const auto state = readJson(operation);
  const auto root = operation.parent_path();
  const auto config = readJson(root / "configuration.json");
  const bool slurm = config.at("platform").at("kind") == "slurm-apptainer";
  require(slurm ? operation.filename() == "allocation.json" :
          fs::equivalent(root, state.at("evidence_root").get<std::string>()),
          "unexpected operation evidence root");
  const auto& requester = config.at("requester");
  require(requester.at("test_only") == true && requester.at("test_wan_fixture_trace") == true,
          "checkpoint trace profile");
  const auto& options = requester.at("wan_generation");
  for (const auto& [key, value] : generation.items())
    require(options.at(key) == value, "checkpoint generation option: " + key);
  const auto deployment = state.at("deployment_id").get<std::string>();
  require(deployment.compare(0, 10, "wan-video-") == 0 &&
          workload.at("mode") == deployment.substr(10), "checkpoint layout binding");
  const auto storageLimit = config.at("storage").at("max_bytes").get<std::size_t>();
  require(storageLimit > 0 &&
          (slurm || state.at("storage_bytes").get<std::size_t>() <= storageLimit),
          "checkpoint evidence exceeds deployment storage budget");
  // One readiness record, two text requests, two requests per denoising step,
  // and one decode. Keep the file budget tied to the frozen trajectory.
  const auto statisticsLimit = std::min(storageLimit,
    (4 + 2 * steps) * (MAX_EVIDENCE_ROW_BYTES + 1));
  auto observed = readDeployment(state, config, root, storageLimit, statisticsLimit);
  const auto& mode = workload.at("modes").at(deployment.substr(10));
  const auto& placement = requester.at("fixed_placement").at("role_providers");
  const fs::path clip = options.at("output_directory").get<std::string>();
  const auto manifestWire = readFile(clip / "manifest.json");
  const auto manifest = nativeParseJson(manifestWire);
  if (conditioned) {
    require(reference.at("native_clip_manifest_digest") == nativePlanningDigest(manifestWire),
            "conditioned reference clip manifest digest differs");
  }
  require(manifest.at("initial_latent_digest") == initial.at("digest"), "initial latent seed binding");
  const auto& receipts = manifest.at("children");
  require(receipts.size() == 3 + 2 * steps && observed.requests.size() == receipts.size(),
          "missing or surplus checkpoint child requests");
  std::size_t child = 0, point = 0;
  const auto bindChild = [&] (const std::string& phase, const std::string& digest) {
    const auto& receipt = receipts.at(child++);
    require(receipt.at("phase") == phase && receipt.at("input_digest") == digest,
            "checkpoint child input: " + phase);
    verifyRoles(phase, receipt, mode, placement, observed);
    return NativeJson{{"request_id", receipt.at("request_id")},
                      {"plan_digest", receipt.at("plan_digest")}};
  };
  ClipVerifier verifier(clip, manifest, 2ULL << 30);
  const auto check = [&] (const std::string& phase, const std::string& name,
                          const std::vector<std::int64_t>& expectedShape, const NativeJson& binding) {
    const auto& row = reference.at("tensors").at(point++);
    require(row.at("phase") == phase && row.at("name") == name && row.at("shape") == expectedShape,
            "checkpoint reference phase or shape: " + phase);
    return verifier.tensor(phase, name, expectedShape, referenceValues(row, referenceFile.parent_path()),
                            2e-4, 2e-4, binding);
  };
  const std::array<std::string, 2> branches{"positive", "negative"};
  std::array<std::int64_t, 2> valid{};
  std::array<std::vector<float>, 2> contexts;
  for (std::size_t branch = 0; branch < branches.size(); ++branch) {
    const auto& prompt = reference.at(branches[branch] + "_tokens");
    const auto ids = prompt.at("ids").get<std::vector<std::int64_t>>();
    valid[branch] = prompt.at("valid_tokens").get<std::int64_t>();
    require(ids.size() == 512 && valid[branch] > 0 && valid[branch] <= 512 &&
            ids.at(valid[branch] - 1) == 1, "checkpoint text input");
    std::vector<std::uint8_t> mask(512, 0);
    std::fill_n(mask.begin(), valid[branch], 1);
    const auto phase = "text/" + branches[branch];
    const auto binding = bindChild(phase, inputDigest({
      {"token-ids", TensorElementType::Int64, {1, 512}, bytes<std::int64_t>(ids)},
      {"key-mask", TensorElementType::Bool, {1, 512}, mask}}));
    contexts[branch] = check(phase, "text-context", {1, valid[branch], 4096}, binding);
  }
  const auto videoTokens = shape[1] * (shape[2] / 2) * (shape[3] / 2);
  const auto padded = generation.at("padded_video_tokens").get<std::int64_t>();
  require(padded == (videoTokens + 127) / 128 * 128, "checkpoint video padding");
  std::vector<std::uint8_t> mask(padded, 0);
  std::fill_n(mask.begin(), videoTokens, 1);
  for (std::size_t step = 0; step < steps; ++step) {
    const auto& row = reference.at("tensors").at(point + 2);
    const std::array<std::int64_t, 1> timestep{row.at("timestep").get<std::int64_t>()};
    for (std::size_t branch = 0; branch < branches.size(); ++branch) {
      const auto phase = "denoise/" + std::to_string(step) + "/" + branches[branch];
      const auto binding = bindChild(phase, inputDigest({
        {"latent", TensorElementType::Float32, latentShape, bytes<float>(latent)},
        {"context", TensorElementType::Float32, {1, valid[branch], 4096}, bytes<float>(contexts[branch])},
        {"timestep", TensorElementType::Int64, {1}, bytes<std::int64_t>(timestep)},
        {"video-valid", TensorElementType::Bool, {1, 1, 1, padded}, mask}}));
      (void)check(phase, "prediction", latentShape, binding);
    }
    latent = check("latent/" + std::to_string(step), "latent", latentShape,
      {{"timestep", row.at("timestep")}, {"predictor_order", row.at("predictor_order")},
       {"corrector_order", row.at("corrector_order")}});
  }
  const auto binding = bindChild("decode", inputDigest({
    {"latent-clip", TensorElementType::Float32, latentShape, bytes<float>(latent)}}));
  const std::array<std::int64_t, 3> geometry{4 * (shape[1] - 1) + 1, shape[2] * 16, shape[3] * 16};
  const auto rgb = check("decode", "rgb", {1, 3, geometry[0], geometry[1], geometry[2]}, binding);
  require(std::all_of(rgb.begin(), rgb.end(), [] (float value) { return value >= -1 && value <= 1; }),
          "checkpoint RGB output is not clamped");
  verifier.frames(std::vector<double>(rgb.begin(), rgb.end()), geometry, 24, true);
  require(point == reference.at("tensors").size() && child == receipts.size() &&
          observed.requests.empty(), "unmatched checkpoint trajectory records");
  verifier.requireNumericalMatch();
  std::cout << deployment << " checkpoint " << (conditioned ? "same-input " : "")
            << "trajectory verified, boot=" << state.at("boot_id") << '\n';
}

void
validate(const fs::path& operation, const fs::path& fixtureFile)
{
  const auto state = readJson(operation);
  const auto root = operation.parent_path();
  const auto config = readJson(root / "configuration.json");
  const bool slurm = config.at("platform").at("kind") == "slurm-apptainer";
  require(slurm ? operation.filename() == "allocation.json" :
          fs::equivalent(root, state.at("evidence_root").get<std::string>()),
          "unexpected operation evidence root");
  // A tiny diagnostic can use different devices in the same allocation node.
  // The checkpoint entry point above keeps the distinct-node requirement.
  auto observed = readDeployment(state, config, root, 64 * MIB, 8 * MIB, false);
  const auto& requester = config.at("requester");
  const auto& options = requester.at("wan_generation");
  const auto fixtureInput = readJson(fixtureFile);
  const std::string deployment = state.at("deployment_id");
  require(deployment.compare(0, 10, "wan-video-") == 0 &&
          requester.at("test_only") == true && requester.at("test_wan_fixture_trace") == true,
          "requester fixture profile");
  for (const auto* key : {"latent_shape", "seed", "shift", "guidance", "text_width",
                          "padded_video_tokens", "steps", "positive_tokens", "negative_tokens"})
    require(options.at(key) == fixtureInput.at(key), std::string("fixture input: ") + key);
  const auto reference = fixture::referenceWanVideo();
  const auto& mode = fixtureInput.at("modes").at(deployment.substr(10));
  const auto& placement = requester.at("fixed_placement").at("role_providers");
  const fs::path clip = options.at("output_directory").get<std::string>();
  const auto manifest = readJson(clip / "manifest.json");
  ClipVerifier verifier(clip, manifest);
  const auto& receipts = manifest.at("children");
  require(receipts.size() == 9 && observed.requests.size() == 9, "missing or surplus child requests");
  std::size_t child = 0;
  const auto bindChild = [&] (const std::string& phase, const std::string& input) {
    const auto& receipt = receipts.at(child++);
    require(receipt.at("phase") == phase && receipt.at("input_digest") == input,
            "child input binding: " + phase);
    verifyRoles(phase, receipt, mode, placement, observed);
    return NativeJson{{"request_id", receipt.at("request_id")},
                      {"plan_digest", receipt.at("plan_digest")}};
  };

  const auto initial = bytes<float>(reference.initialLatent);
  require(manifest.at("initial_latent_digest") == nativePlanningDigest(initial.data(), initial.size()),
          "initial latent seed binding");
  std::vector<float> latent = reference.initialLatent;
  const std::vector<std::int64_t> latentShape{1, 2, 3, 4, 4};
  const std::array<std::string, 2> branches{"positive", "negative"};
  const std::array<std::int64_t, 2> valid{3, 1};
  const std::array<std::array<std::int64_t, 3>, 2> tokens{{{2, 0, 1}, {1, 0, 0}}};
  std::array<std::vector<float>, 2> contexts;
  for (std::size_t branch = 0; branch < branches.size(); ++branch) {
    std::array<std::uint8_t, 3> mask{};
    std::fill_n(mask.begin(), valid[branch], 1);
    const auto phase = "text/" + branches[branch];
    const auto binding = bindChild(phase, inputDigest({
      {"token-ids", TensorElementType::Int64, {1, 3}, bytes<std::int64_t>(tokens[branch])},
      {"key-mask", TensorElementType::Bool, {1, 3}, mask}}));
    contexts[branch] = verifier.tensor(phase, "text-context", {1, valid[branch], 2},
                                      reference.textContexts[branch], 2e-6, 0, binding);
  }
  constexpr std::array<int, 3> predictor{1, 2, 1}, corrector{0, 1, 2};
  std::array<std::uint8_t, 12> keyMask;
  keyMask.fill(1);
  for (std::size_t index = 0; index < reference.steps.size(); ++index) {
    const auto& step = reference.steps[index];
    const std::array<std::int64_t, 1> timestep{step.timestep};
    for (std::size_t branch = 0; branch < branches.size(); ++branch) {
      const auto phase = "denoise/" + std::to_string(index) + "/" + branches[branch];
      const auto binding = bindChild(phase, inputDigest({
        {"latent", TensorElementType::Float32, latentShape, bytes<float>(latent)},
        {"context", TensorElementType::Float32, {1, valid[branch], 2},
                     bytes<float>(contexts[branch])},
        {"timestep", TensorElementType::Int64, {1}, bytes<std::int64_t>(timestep)},
        {"video-valid", TensorElementType::Bool, {1, 1, 1, 12}, keyMask}}));
      verifier.tensor(phase, "prediction", latentShape, step.predictions[branch], 2e-6, 2e-6,
                      binding);
    }
    latent = verifier.tensor("latent/" + std::to_string(index), "latent", latentShape, step.latent,
                             2e-6, 2e-6, {{"timestep", step.timestep},
                              {"predictor_order", predictor[index]},
                              {"corrector_order", corrector[index]}});
  }
  const auto binding = bindChild("decode", inputDigest({
    {"latent-clip", TensorElementType::Float32, latentShape, bytes<float>(latent)}}));
  verifier.tensor("decode", "rgb", {1, 3, 9, 32, 32}, reference.rgb, 1e-6, 0, binding);
  verifier.frames(reference.rgb);
  require(child == receipts.size() && observed.requests.empty(), "unmatched child receipts");
  verifier.requireNumericalMatch();
  std::cout << deployment << " verified, boot=" << state.at("boot_id") << '\n';
}

} // namespace

int
main(int argc, char** argv)
{
  const bool conditioned = argc == 9 && std::string_view(argv[3]) == "--conditioned-reference";
  if (argc == 9 && std::string_view(argv[1]) == "--deployment-state" &&
      (conditioned || std::string_view(argv[3]) == "--reference") &&
      std::string_view(argv[5]) == "--workload" &&
      std::string_view(argv[7]) == "--initial-latent") {
    try {
      validateCheckpoint(fs::absolute(argv[2]), fs::absolute(argv[4]),
                          fs::absolute(argv[6]), fs::absolute(argv[8]), conditioned);
      return 0;
    }
    catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  if (argc != 5 || std::string_view(argv[1]) != "--deployment-state" ||
      std::string_view(argv[3]) != "--fixture") {
    std::cerr << "usage: di-wan-video-oracle --deployment-state OPERATION --fixture FIXTURE\n"
                 "       di-wan-video-oracle --deployment-state OPERATION "
                 "(--reference | --conditioned-reference) REFERENCE "
                 "--workload WORKLOAD --initial-latent INPUT\n";
    return 2;
  }
  try {
    validate(fs::absolute(argv[2]), argv[4]);
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "Wan video oracle: " << error.what() << '\n';
    return 1;
  }
}

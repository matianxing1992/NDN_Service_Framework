#ifndef NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP
#define NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/native-private-checkpoint-file.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <sys/stat.h>

namespace ndnsf::di {

// Two complete 28-layer Qwen-0.6B turns need up to 1,848 records per rank.
inline constexpr std::size_t MAX_COMPLETE_KV_CAPTURE_RECORDS = 4096;
inline constexpr std::uint64_t MAX_COMPLETE_KV_CAPTURE_BYTES = 4ULL * 1024U * 1024U * 1024U;

/** Bounded private qualification output shared by this Provider's runners. */
class OnnxKvStateProbe final : public OnnxKvStateObserver,
                              public OnnxCompleteKvStateObserver,
                              public OnnxOutputTensorObserver
{
public:
  OnnxKvStateProbe(std::filesystem::path destination, std::string provider,
                   bool captureComplete = false, std::string logitsOutput = {})
    : m_destination(std::move(destination))
    , m_provider(std::move(provider))
    , m_captureComplete(captureComplete)
    , m_logitsOutput(std::move(logitsOutput))
  {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(m_destination, error);
    if (status.type() != std::filesystem::file_type::not_found ||
        (error && error != std::errc::no_such_file_or_directory))
      throw std::runtime_error("qualification KV destination must be absent");
    if (!m_destination.is_absolute() || m_provider.empty() || m_provider.size() > 4096)
      throw std::invalid_argument("invalid qualification KV destination or provider");
    if ((!m_logitsOutput.empty() && !m_captureComplete) || m_logitsOutput.size() > 1024)
      throw std::invalid_argument("logits qualification requires complete KV mode");
    if (m_captureComplete && ::mkdir(m_destination.c_str(), S_IRWXU) != 0)
      throw std::runtime_error("cannot create private qualification KV directory");
  }

  bool
  wants(const RoleExecutionContext& context) const final
  {
    if (!context.generationLineage)
      return false;
    const auto& lineage = *context.generationLineage;
    return lineage.transitionKind == GenerationEpochLineageV1::PREFILL ||
      lineage.transitionKind == GenerationEpochLineageV1::CHECKPOINT_FINALIZE ||
      (lineage.transitionKind == GenerationEpochLineageV1::DECODE &&
       (m_captureComplete || context.inferenceEpoch == 1));
  }

  bool
  wantsCompleteSmallState(const RoleExecutionContext& context) const final
  {
    return m_captureComplete && wants(context);
  }

  void
  record(const RoleExecutionContext& context, std::vector<OnnxKvStateSample> samples) final
  {
    recordTensors(context, std::move(samples), false);
  }

  bool
  wantsOutput(const RoleExecutionContext& context, const std::string& name) const final
  {
    return !m_logitsOutput.empty() && name == m_logitsOutput && wants(context);
  }

  void
  recordOutput(const RoleExecutionContext& context, OnnxKvStateSample sample) final
  {
    if (!wantsOutput(context, sample.inputName) ||
        (sample.elementType != "float32" && sample.elementType != "float16"))
      throw std::invalid_argument("invalid qualification logits output");
    std::vector<OnnxKvStateSample> samples;
    samples.push_back(std::move(sample));
    recordTensors(context, std::move(samples), true);
  }

private:
  void
  recordTensors(const RoleExecutionContext& context, std::vector<OnnxKvStateSample> samples,
                bool logits)
  {
    if (!wants(context) || samples.empty() || samples.size() > 256 ||
        context.requestId.empty() || context.providerBootId.empty() || context.attemptEpoch == 0)
      throw std::invalid_argument("invalid qualification KV observation");
    for (const auto* text : {&context.requestId, &context.sessionId, &context.role,
                             &context.providerBootId})
      if (text->empty() || text->size() > 4096)
        throw std::invalid_argument("qualification KV context bound exceeded");
    const auto& lineage = *context.generationLineage;
    lineage.validateCore();
    if (lineage.requestId != context.requestId || lineage.attemptEpoch != context.attemptEpoch ||
        lineage.inferenceEpoch != context.inferenceEpoch)
      throw std::invalid_argument("qualification KV lineage mismatch");
    NativeJson tensors = NativeJson::array();
    std::vector<NativeJson> batches;
    std::set<std::string> names;
    std::size_t elements = 0;
    for (const auto& sample : samples) {
      if (sample.inputName.empty() || sample.inputName.size() > 1024 ||
          !names.insert(sample.inputName).second || sample.shape.empty() ||
          sample.shape.size() > 8 ||
          sample.indices.empty() ||
          sample.indices.size() > (m_captureComplete ? MAX_COMPLETE_ONNX_KV_ELEMENTS : 16) ||
          sample.indices.size() != sample.values.size())
        throw std::invalid_argument("qualification KV sample bound exceeded");
      if (sample.elementType != "float32" && sample.elementType != "float16" &&
          sample.elementType != "int64" && sample.elementType != "int32")
        throw std::invalid_argument("unsupported qualification KV sample type");
      for (const auto value : sample.values)
        if (!std::isfinite(value))
          throw std::invalid_argument("nonfinite qualification KV observation");
      NativeJson tensor{{"inputName", sample.inputName}, {"elementType", sample.elementType},
                        {"shape", sample.shape}, {"values", sample.values}};
      if (m_captureComplete) {
        std::size_t extent = 1;
        for (auto dimension : sample.shape) {
          if (dimension <= 0 ||
              static_cast<std::uint64_t>(dimension) > MAX_COMPLETE_ONNX_KV_ELEMENTS / extent)
            throw std::runtime_error("complete qualification KV tensor exceeds element bound");
          extent *= static_cast<std::size_t>(dimension);
        }
        if (extent != sample.indices.size())
          throw std::runtime_error("complete qualification KV coverage failed");
        for (std::size_t index = 0; index < extent; ++index) {
          if (sample.indices[index] != index)
            throw std::runtime_error("complete qualification KV coverage is not contiguous");
        }
        if (extent > MAX_COMPLETE_RECORD_ELEMENTS - elements) {
          batches.push_back(std::move(tensors));
          tensors = NativeJson::array();
          elements = 0;
        }
        elements += extent;
      }
      else {
        tensor["indices"] = sample.indices;
      }
      tensors.push_back(std::move(tensor));
    }
    NativeJson observation{{"requestId", context.requestId}, {"sessionId", context.sessionId},
      {"role", context.role}, {"providerBootId", context.providerBootId},
      {"attemptEpoch", context.attemptEpoch}, {"inferenceEpoch", context.inferenceEpoch},
      {"transitionKind", lineage.transitionKind}, {"planDigest", lineage.planDigest},
      {"streamEpoch", lineage.streamEpoch}, {"positionDigest", lineage.positionDigest},
      {"operationIndex", lineage.operationIndex},
      {"generationId", lineage.generationId},
      {"logicalPrefixDigest", lineage.logicalPrefixDigest},
      {"logicalPrefixTokenCount", lineage.logicalPrefixTokenCount}};
    std::lock_guard<std::mutex> lock(m_mutex);
    const EpochKey key{context.requestId, context.attemptEpoch, context.role,
                       context.inferenceEpoch, logits};
    if (m_captureComplete) {
      // Stateful child entries may observe disjoint tensors for the same epoch.
      batches.push_back(std::move(tensors));
      if (batches.size() > MAX_COMPLETE_KV_CAPTURE_RECORDS - m_completeRecords)
        throw std::runtime_error("complete qualification KV record bound exceeded");
      const auto found = m_completeKeys.find(key);
      if (found != m_completeKeys.end()) {
        for (const auto& name : names) {
          if (found->second.count(name))
            throw std::runtime_error("duplicate complete qualification KV tensor");
        }
      }
      auto& seen = m_completeKeys[key];
      for (auto& batch : batches) {
        std::set<std::string> batchNames;
        for (const auto& tensor : batch)
          batchNames.insert(tensor.at("inputName").get<std::string>());
        NativeJson document{{"schema", "ndnsf-di-kv-complete-v1"},
          {"provider", m_provider}, {"record", observation}};
        document["record"][logits ? "logits" : "tensors"] = std::move(batch);
        // Encode each record once while retaining round-trip floating-point precision.
        const auto wire = document.dump();
        if (wire.size() > MAX_COMPLETE_KV_CAPTURE_BYTES - m_completeBytes)
          throw std::runtime_error("complete qualification KV directory byte bound exceeded");
        detail::writePrivateCheckpointBytes(
          m_destination / (std::to_string(m_completeRecords) + ".json"), wire);
        seen.merge(batchNames);
        ++m_completeRecords;
        m_completeBytes += wire.size();
      }
      return;
    }
    if (m_keys.size() >= 64 || !m_keys.insert(key).second)
      throw std::runtime_error("qualification KV record bound or duplicate observation");
    observation["tensors"] = std::move(tensors);
    m_records.push_back(std::move(observation));
    const NativeJson document{{"schema", "ndnsf-di-kv-value-samples-v1"},
      {"provider", m_provider}, {"records", m_records}};
    if (nativeCanonicalJson(document).size() > 8 * 1024 * 1024)
      throw std::runtime_error("qualification KV file bound exceeded");
    nativeExportPrivateCheckpoint(m_destination, document);
  }

  static constexpr std::size_t MAX_COMPLETE_RECORD_ELEMENTS = 2 * MAX_COMPLETE_ONNX_KV_ELEMENTS;
  using EpochKey = std::tuple<std::string, std::uint64_t, std::string, std::uint64_t, bool>;

  const std::filesystem::path m_destination;
  const std::string m_provider;
  const bool m_captureComplete;
  const std::string m_logitsOutput;
  std::mutex m_mutex;
  std::uint64_t m_completeBytes = 0;
  std::size_t m_completeRecords = 0;
  std::map<EpochKey, std::set<std::string>> m_completeKeys;
  NativeJson m_records = NativeJson::array();
  std::set<EpochKey> m_keys;
};

} // namespace ndnsf::di

#endif // NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP

#ifndef NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP
#define NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP

#include "NDNSF-DistributedInference/cpp/adapters/onnx/OnnxRuntimeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"

#include <cmath>
#include <filesystem>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>

namespace ndnsf::di {

/** One bounded private qualification file, shared by this Provider's runners. */
class OnnxKvStateProbe final : public OnnxKvStateObserver
{
public:
  OnnxKvStateProbe(std::filesystem::path destination, std::string provider)
    : m_destination(std::move(destination))
    , m_provider(std::move(provider))
  {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(m_destination, error);
    if (status.type() != std::filesystem::file_type::not_found ||
        (error && error != std::errc::no_such_file_or_directory))
      throw std::runtime_error("qualification KV destination must be absent");
    if (!m_destination.is_absolute() || m_provider.empty() || m_provider.size() > 4096)
      throw std::invalid_argument("invalid qualification KV destination or provider");
  }

  bool
  wants(const RoleExecutionContext& context) const final
  {
    if (!context.generationLineage)
      return false;
    const auto& lineage = *context.generationLineage;
    return lineage.transitionKind == GenerationEpochLineageV1::PREFILL ||
      lineage.transitionKind == GenerationEpochLineageV1::CHECKPOINT_FINALIZE ||
      (lineage.transitionKind == GenerationEpochLineageV1::DECODE && context.inferenceEpoch == 1);
  }

  void
  record(const RoleExecutionContext& context, std::vector<OnnxKvStateSample> samples) final
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
    std::set<std::string> names;
    for (const auto& sample : samples) {
      if (sample.inputName.empty() || sample.inputName.size() > 1024 ||
          !names.insert(sample.inputName).second || sample.shape.empty() ||
          sample.shape.size() > 8 ||
          sample.indices.empty() || sample.indices.size() > 16 ||
          sample.indices.size() != sample.values.size())
        throw std::invalid_argument("qualification KV sample bound exceeded");
      if (sample.elementType != "float32" && sample.elementType != "float16" &&
          sample.elementType != "int64" && sample.elementType != "int32")
        throw std::invalid_argument("unsupported qualification KV sample type");
      for (const auto value : sample.values)
        if (!std::isfinite(value))
          throw std::invalid_argument("nonfinite qualification KV observation");
      tensors.push_back({{"inputName", sample.inputName}, {"elementType", sample.elementType},
        {"shape", sample.shape}, {"indices", sample.indices}, {"values", sample.values}});
    }
    NativeJson observation{{"requestId", context.requestId}, {"sessionId", context.sessionId},
      {"role", context.role}, {"providerBootId", context.providerBootId},
      {"attemptEpoch", context.attemptEpoch}, {"inferenceEpoch", context.inferenceEpoch},
      {"transitionKind", lineage.transitionKind}, {"planDigest", lineage.planDigest},
      {"streamEpoch", lineage.streamEpoch}, {"positionDigest", lineage.positionDigest},
      {"operationIndex", lineage.operationIndex},
      {"generationId", lineage.generationId},
      {"logicalPrefixDigest", lineage.logicalPrefixDigest},
      {"logicalPrefixTokenCount", lineage.logicalPrefixTokenCount},
      {"tensors", std::move(tensors)}};
    std::lock_guard<std::mutex> lock(m_mutex);
    const auto key = std::make_tuple(context.requestId, context.attemptEpoch, context.role,
                                     context.inferenceEpoch);
    if (m_records.size() >= 64 || !m_keys.insert(key).second)
      throw std::runtime_error("qualification KV record bound or duplicate observation");
    m_records.push_back(std::move(observation));
    const NativeJson document{{"schema", "ndnsf-di-kv-value-samples-v1"},
      {"provider", m_provider}, {"records", m_records}};
    if (nativeCanonicalJson(document).size() > 8 * 1024 * 1024)
      throw std::runtime_error("qualification KV file bound exceeded");
    nativeExportPrivateCheckpoint(m_destination, document);
  }

private:
  const std::filesystem::path m_destination;
  const std::string m_provider;
  std::mutex m_mutex;
  NativeJson m_records = NativeJson::array();
  std::set<std::tuple<std::string, std::uint64_t, std::string, std::uint64_t>> m_keys;
};

} // namespace ndnsf::di

#endif // NDNSF_EXAMPLES_ONNX_KV_STATE_PROBE_HPP

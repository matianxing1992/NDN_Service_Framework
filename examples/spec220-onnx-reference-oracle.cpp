#include "spec220-onnx-reference-oracle.hpp"
#include "onnx-kv-state-probe.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeGenerationLimits.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationWire.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <onnxruntime_cxx_api.h>
#include <ndn-cxx/util/scope.hpp>
#include <openssl/crypto.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace spec220::oracle {
namespace {

using namespace ndnsf::di;
using Stamp = std::array<std::uint64_t, 7>;

[[noreturn]] void
reject(const std::string& reason)
{
  throw std::runtime_error("SPEC220_CPP_MODEL_REFERENCE_FAIL reason=" + reason);
}

Stamp
stamp(const std::filesystem::path& path)
{
  struct stat value{};
  if (::stat(path.c_str(), &value) || !S_ISREG(value.st_mode)) reject("source-file");
  return {static_cast<std::uint64_t>(value.st_dev), static_cast<std::uint64_t>(value.st_ino),
    static_cast<std::uint64_t>(value.st_size), static_cast<std::uint64_t>(value.st_mtim.tv_sec),
    static_cast<std::uint64_t>(value.st_mtim.tv_nsec),
    static_cast<std::uint64_t>(value.st_ctim.tv_sec),
    static_cast<std::uint64_t>(value.st_ctim.tv_nsec)};
}

Stamp
verifyFile(const std::filesystem::path& path, std::uint64_t bytes,
           const std::string& expected, std::uint64_t bound)
{
  const auto before = stamp(path);
  if (!bytes || bytes > bound || before[2] != bytes) reject("source-size");
  std::ifstream input(path, std::ios::binary);
  SHA256_CTX context;
  SHA256_Init(&context);
  std::array<char, 65536> buffer;
  std::uint64_t total = 0;
  while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
    const auto count = static_cast<std::uint64_t>(input.gcount());
    if (count > bytes - total) reject("source-grew");
    SHA256_Update(&context, buffer.data(), count);
    total += count;
  }
  if (!input.eof() || total != bytes || stamp(path) != before) reject("source-read");
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest;
  SHA256_Final(digest.data(), &context);
  std::string text = "sha256:";
  for (auto value : digest) {
    text += "0123456789abcdef"[value >> 4];
    text += "0123456789abcdef"[value & 15];
  }
  if (text != expected) reject("source-digest");
  return before;
}

std::string
readSmall(const std::filesystem::path& path)
{
  const auto size = std::filesystem::file_size(path);
  if (!size || size > 16U * 1024U * 1024U) reject("input-file-bound");
  std::string value(size, '\0');
  std::ifstream input(path, std::ios::binary);
  if (!input.read(value.data(), size) || input.peek() != EOF) reject("input-file-read");
  return value;
}

std::size_t
width(ONNXTensorElementDataType type)
{
  switch (type) {
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT: return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16: return 2;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64: return 8;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32: return 4;
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL:
    case ONNX_TENSOR_ELEMENT_DATA_TYPE_UINT8: return 1;
    default: reject("unsupported-state-type");
  }
}

std::uint64_t
tensorBytes(const std::vector<std::int64_t>& shape, ONNXTensorElementDataType type,
            std::uint64_t bound)
{
  std::uint64_t count = width(type);
  for (auto dimension : shape) {
    if (dimension < 0 || (dimension && count > bound / dimension)) reject("state-budget");
    count *= dimension;
  }
  if (count > bound) reject("state-budget");
  return count;
}

std::vector<std::int64_t>
stateShape(const Ort::ConstTensorTypeAndShapeInfo& info, std::uint64_t prefix)
{
  auto shape = info.GetShape();
  std::vector<const char*> symbols(shape.size());
  info.GetSymbolicDimensions(symbols.data(), symbols.size());
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (shape[i] >= 0) continue;
    const std::string symbol = symbols[i] ? symbols[i] : "";
    if (symbol == "batch" || symbol == "batch_size") shape[i] = 1;
    else if (symbol == "past_sequence_length" || symbol == "total_sequence_length" ||
             symbol == "past_sequence" || symbol == "sequence_length")
      shape[i] = static_cast<std::int64_t>(prefix);
    else reject("unknown-state-dimension");
  }
  return shape;
}

class CompleteKvReplay
{
public:
  void
  loadEvidence(const NativeJson& catalog, const std::vector<CompleteKvCapture>& captures,
               const std::map<std::string, std::string>& referenceSuccessors)
  {
    if (captures.empty())
      return;
    const auto& splitter = catalog.at("splitter");
    const auto& stages = splitter.at("roles");
    const auto& degrees = splitter.at("tensor_degrees");
    const auto& stateInputs = catalog.at("state_inputs");
    const auto layout = catalog.at("recipe").at("layout").get<std::string>();
    // Qwen's recipe describes native assembly layout. The independent
    // reference tensors below still have to prove the BNSH state geometry.
    const bool qwenNativeLayout = layout == "native" && splitter.value("kind", "") == "QWEN";
    if (!stages.is_array() || !degrees.is_array() || stages.empty() ||
        stages.size() != degrees.size() || !stateInputs.is_object() ||
        stateInputs.size() != stages.size())
      reject("complete-kv-layout");
    std::set<std::string> allRoles;
    for (std::size_t stage = 0; stage < stages.size(); ++stage) {
      const auto stageName = stages.at(stage).get<std::string>();
      const auto degree = degrees.at(stage).get<std::size_t>();
      if (stageName.empty() || degree == 0 || degree > captures.size() - allRoles.size())
        reject("complete-kv-degree");
      std::vector<std::string> roles;
      for (std::size_t rank = 0; rank < degree; ++rank) {
        // The rank component is what marks a role as a sharded unit; a name without it is read
        // as a component set and gets no layer range (NativeRequestCatalog.cpp:1257). Both
        // producers therefore always emit it, so the suffix is dropped only for the layout that
        // has a single source-defined stage, which is the case this special form was written for.
        // The bare stage name is only correct for a layout with one source-defined stage and one rank:
        // that is the single unit the comment above describes. Every other case is a sharded unit and
        // must carry the rank component, or a two-rank stage would produce two identical role names.
        const bool bareSingleUnit = stages.size() == 1 && degree == 1;
        roles.push_back(bareSingleUnit ? stageName : stageName + "/Tensor/Rank/" +
                       std::to_string(rank));
        if (!allRoles.insert(roles.back()).second)
          reject("complete-kv-duplicate-role");
      }
      if (degree > 1 && layout != "BNSH" && !qwenNativeLayout)
        reject("complete-kv-shard-axis");
      if (qwenNativeLayout && stages.size() == 1) {
        // One source-defined Qwen stage owns all verified ONNX KV inputs.
        // Older catalog hints may list only the initial subset of layers.
        for (const auto& successor : referenceSuccessors)
          m_owners.emplace(successor.first, roles);
      }
      else {
        for (const auto& group : stateInputs.at(stageName)) {
          for (const auto& input : group) {
            if (!m_owners.emplace(input.get<std::string>(), roles).second)
              reject("complete-kv-duplicate-owner");
          }
        }
      }
    }
    if (m_owners.empty() || allRoles.size() != captures.size())
      reject("complete-kv-role-cover");
    std::uintmax_t totalBytes = 0;
    for (const auto& capture : captures) {
      m_requireLogits = m_requireLogits || capture.captureLogits;
      struct stat directory{};
      const auto& path = capture.directory;
      if (!path.is_absolute() || ::lstat(path.c_str(), &directory) != 0 ||
          !S_ISDIR(directory.st_mode) || directory.st_uid != ::getuid() ||
          (directory.st_mode & 0777) != 0700 || std::filesystem::canonical(path) != path)
        reject("complete-kv-private-directory");
      std::uintmax_t bytes = 0;
      std::size_t files = 0;
      for (const auto& file : std::filesystem::directory_iterator(path)) {
        struct stat info{};
        if (++files > MAX_COMPLETE_KV_CAPTURE_RECORDS || ::lstat(file.path().c_str(), &info) != 0 ||
            !S_ISREG(info.st_mode) || info.st_uid != ::getuid() ||
            (info.st_mode & 0777) != 0600 || info.st_size <= 0)
          reject("complete-kv-private-record");
        const auto size = static_cast<std::uintmax_t>(info.st_size);
        bytes += size;
        totalBytes += size;
        if (bytes > MAX_COMPLETE_KV_CAPTURE_BYTES || totalBytes > 2 * MAX_COMPLETE_KV_CAPTURE_BYTES)
          reject("complete-kv-record-budget");
        load(nativeParseJson(readSmall(file.path())), capture);
      }
      if (files == 0)
        reject("complete-kv-empty-provider");
    }
  }

  void
  compare(std::size_t round, std::size_t epoch, const std::string& input,
          const Ort::Value& value, std::uint64_t prefixTokens)
  {
    if (m_owners.empty())
      return;
    const auto owner = m_owners.find(input);
    if (owner == m_owners.end())
      reject("complete-kv-missing-owner:" + input);
    const auto info = value.GetTensorTypeAndShapeInfo();
    auto shape = info.GetShape();
    const auto degree = static_cast<std::int64_t>(owner->second.size());
    if (degree > 1) {
      if (shape.size() != 4 || shape.front() != 1 || shape[1] <= 0 || shape[1] % degree != 0 ||
          shape[2] <= 0 || static_cast<std::uint64_t>(shape[2]) != prefixTokens || shape[3] <= 0)
        reject("complete-kv-head-cover:" + input);
      shape.at(1) /= degree;
    }
    const auto extent = info.GetElementCount() / owner->second.size();
    if (extent == 0 || extent > MAX_COMPLETE_ONNX_KV_ELEMENTS)
      reject("complete-kv-value-bound");
    const auto type = info.GetElementType();
    const char* elementType = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? "float32" :
      type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? "float16" :
      type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 ? "int64" :
      type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT32 ? "int32" : nullptr;
    if (!elementType)
      reject("complete-kv-type");
    for (std::size_t rank = 0; rank < owner->second.size(); ++rank) {
      const auto context = ":round=" + std::to_string(round) + ":epoch=" +
        std::to_string(epoch) + ":role=" + owner->second[rank] + ":tensor=" + input;
      const auto found = m_tensors.find({round, epoch, input, rank});
      if (found == m_tensors.end())
        reject("complete-kv-missing-shard" + context);
      const auto& tensor = found->second;
      const auto& scalars = tensor.values;
      if (tensor.shape != shape || tensor.elementType != elementType || scalars.size() != extent)
        reject("complete-kv-shape-or-type" + context);
      for (std::size_t i = 0; i < extent; ++i) {
        const auto index = rank * extent + i;
        const double actual = scalars[i];
        double reference;
        switch (type) {
          case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT:
            reference = value.GetTensorData<float>()[index]; break;
          case ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16:
            reference = value.GetTensorData<Ort::Float16_t>()[index].ToFloat(); break;
          case ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64:
            reference = static_cast<double>(value.GetTensorData<std::int64_t>()[index]); break;
          default:
            reference = value.GetTensorData<std::int32_t>()[index]; break;
        }
        // The observer stores scalar values as doubles. Outside the consecutive
        // integer range two different INT64 states can have the same observation.
        constexpr double INT64_OBSERVATION_LIMIT = 9007199254740992.0;
        if (type == ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64 &&
            (std::abs(reference) >= INT64_OBSERVATION_LIMIT ||
             std::abs(actual) >= INT64_OBSERVATION_LIMIT))
          reject("complete-kv-inexact-int64" + context);
        const auto error = std::abs(reference - actual);
        const double tolerance = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
          type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? 1e-5 + 1e-4 * std::abs(reference) : 0;
        if (!std::isfinite(error) || error > tolerance)
          reject("complete-kv-value" + context + ":index=" + std::to_string(index));
        maxError = std::max(maxError, error);
        ++valuesCompared;
      }
      m_tensors.erase(found);
    }
  }

  void
  compareLogits(std::size_t round, std::size_t epoch, const std::string& name,
                const Ort::Value& value)
  {
    if (!m_requireLogits)
      return;
    const auto context = ":round=" + std::to_string(round) + ":epoch=" + std::to_string(epoch);
    const auto found = m_logits.find({round, epoch});
    if (found == m_logits.end())
      reject("complete-logits-missing" + context);
    const auto& tensor = found->second;
    const auto info = value.GetTensorTypeAndShapeInfo();
    const auto type = info.GetElementType();
    const auto& scalars = tensor.at("values");
    const char* elementType = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ? "float32" :
      type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16 ? "float16" : nullptr;
    if (!elementType || tensor.at("inputName") != name ||
        tensor.at("shape") != NativeJson(info.GetShape()) || tensor.at("elementType") != elementType ||
        scalars.size() != info.GetElementCount())
      reject("complete-logits-shape-or-type" + context);
    for (std::size_t index = 0; index < scalars.size(); ++index) {
      if (!scalars[index].is_number())
        reject("complete-logits-scalar" + context);
      const double reference = type == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT
        ? value.GetTensorData<float>()[index] : value.GetTensorData<Ort::Float16_t>()[index].ToFloat();
      const double error = std::abs(reference - scalars[index].get<double>());
      if (!std::isfinite(error) || error > 1e-5 + 1e-4 * std::abs(reference))
        reject("complete-logits-value" + context + ":index=" + std::to_string(index));
      maxLogitsError = std::max(maxLogitsError, error);
      ++logitsCompared;
    }
    m_logits.erase(found);
  }

  void
  finish() const
  {
    if (!m_tensors.empty())
      reject("complete-kv-unconsumed-shards");
    if (!m_logits.empty())
      reject("complete-logits-unconsumed");
  }

  std::size_t valuesCompared = 0;
  double maxError = 0;
  std::size_t logitsCompared = 0;
  double maxLogitsError = 0;

private:
  struct ObservedState
  {
    std::vector<std::int64_t> shape;
    std::string elementType;
    std::vector<double> values;
  };

  void
  load(const NativeJson& document, const CompleteKvCapture& capture)
  {
    if (document.at("schema") != "ndnsf-di-kv-complete-v1" ||
        document.at("provider") != capture.provider)
      reject("complete-kv-provider");
    const auto& record = document.at("record");
    const auto request = std::find_if(capture.requests.begin(), capture.requests.end(),
      [&] (const auto& item) { return item.at("requestId") == record.at("requestId"); });
    if (request == capture.requests.end() || record.at("providerBootId") != capture.boot ||
        record.at("role") != request->at("role") ||
        record.at("attemptEpoch") != request->at("attemptEpoch") ||
        record.at("planDigest") != request->at("planDigest"))
      reject("complete-kv-request-binding");
    const auto epoch = record.at("inferenceEpoch").get<std::size_t>();
    const auto count = request->at("epochsExecuted").get<std::size_t>();
    if (epoch >= count || record.at("sessionId") != request->at("runtimeMetrics").at(epoch)
          .at("lastBackendExecution").at("sessionId") ||
        record.at("logicalPrefixTokenCount") != request->at("cacheObservations").at(epoch)
          .at("representedPrefixTokenCount") ||
        record.at("transitionKind") !=
          (epoch == 0 ? "PREFILL" : epoch + 1 == count ? "CHECKPOINT_FINALIZE" : "DECODE"))
      reject("complete-kv-epoch-binding");
    const auto round = static_cast<std::size_t>(request - capture.requests.begin());
    const auto role = record.at("role").get<std::string>();
    if (record.contains("logits")) {
      const auto& logits = record.at("logits");
      if (!capture.captureLogits || record.contains("tensors") ||
          request->at("eventsPublished") != count - 1 ||
          !logits.is_array() || logits.size() != 1)
        reject("complete-logits-publisher");
      const auto& values = logits.at(0).at("values");
      if (!values.is_array() || values.empty() || values.size() > 4096)
        reject("complete-logits-value-bound");
      if (!m_logits.emplace(std::make_pair(round, epoch), logits.at(0)).second)
        reject("complete-logits-duplicate");
      return;
    }
    const auto& tensors = record.at("tensors");
    if (!tensors.is_array() || tensors.empty() || tensors.size() > 256)
      reject("complete-kv-tensor-cover");
    for (const auto& tensor : tensors) {
      const auto input = tensor.at("inputName").get<std::string>();
      const auto& values = tensor.at("values");
      if (!values.is_array() || values.empty() || values.size() > MAX_COMPLETE_ONNX_KV_ELEMENTS)
        reject("complete-kv-value-bound");
      const auto owner = m_owners.find(input);
      if (owner == m_owners.end())
        reject("complete-kv-unexpected-input:" + input);
      const auto rank = std::find(owner->second.begin(), owner->second.end(), role);
      if (rank == owner->second.end())
        reject("complete-kv-unexpected-role:" + role + ":tensor=" + input);
      const auto index = static_cast<std::size_t>(rank - owner->second.begin());
      ObservedState state{tensor.at("shape").get<std::vector<std::int64_t>>(),
                          tensor.at("elementType").get<std::string>(), {}};
      state.values.reserve(values.size());
      for (const auto& value : values) {
        if (!value.is_number())
          reject("complete-kv-scalar:" + input);
        state.values.push_back(value.get<double>());
      }
      if (!m_tensors.emplace(TensorKey{round, epoch, input, index}, std::move(state)).second)
        reject("complete-kv-duplicate-shard:" + input);
    }
  }

  using TensorKey = std::tuple<std::size_t, std::size_t, std::string, std::size_t>;
  std::map<std::string, std::vector<std::string>> m_owners;
  std::map<TensorKey, ObservedState> m_tensors;
  std::map<std::pair<std::size_t, std::size_t>, NativeJson> m_logits;
  bool m_requireLogits = false;
};

} // namespace

NativeJson
validateOnnxReference(const NativeJson& requester, const std::string& expectedRuntimeVersion,
                      const std::vector<CompleteKvCapture>& captures)
{
  if (expectedRuntimeVersion.empty() || expectedRuntimeVersion != Ort::GetVersionString())
    reject("runtime-version");
  const auto& turns = requester.at("turns");
  if (turns.empty() || turns.size() > 8) reject("turn-bound");
  const bool grouped = turns.at(0).contains("conversation_id");
  std::set<std::string> declaredConversations;
  for (std::size_t i = 0; i < turns.size(); ++i) {
    const auto& turn = turns.at(i);
    if (turn.contains("conversation_id") != grouped)
      reject("conversation-groups");
    if (!grouped) continue;
    if (turns.size() % 2 != 0 || !turn.at("conversation_id").is_string())
      reject("conversation-groups");
    const auto id = turn.at("conversation_id").get<std::string>();
    if (id.size() < 16 || id.find_first_of("/\\") != std::string::npos ||
        (i % 2 == 0 && !declaredConversations.insert(id).second) ||
        (i % 2 != 0 && turn.at("conversation_id") != turns.at(i - 1).at("conversation_id")))
      reject("conversation-groups");
  }
  std::vector<NativeConversationJournalKey> journalKeys;
  std::vector<std::vector<std::uint8_t>> authenticationKeys;
  const auto wipeKeys = ndn::make_scope_exit([&] {
    for (auto& key : journalKeys) OPENSSL_cleanse(key.bytes.data(), key.bytes.size());
    for (auto& key : authenticationKeys) OPENSSL_cleanse(key.data(), key.size());
  });
  const auto& conversation = requester.at("conversation");
  if (grouped) {
    if (!conversation.contains("turn")) reject("conversation-group-configuration");
    const auto& turn = conversation.at("turn");
    if (turn.value("mode", std::string{}) != "FULL_CONTEXT" ||
        turn.contains("conversation_id") || turn.contains("parent_state_file") ||
        turn.contains("parent_checkpoint_digest") || conversation.contains("checkpoint_output_file"))
      reject("conversation-group-configuration");
  }
  const auto& journal = conversation.at("journal");
  const auto& keyFiles = journal.at("keys");
  if (!keyFiles.is_array() || keyFiles.empty() || keyFiles.size() > 16)
    reject("checkpoint-key-configuration");
  journalKeys.reserve(keyFiles.size());
  std::set<std::string> keyIds;
  for (const auto& key : keyFiles) {
    const auto id = key.at("id").get<std::string>();
    const std::filesystem::path path = key.at("file").get<std::string>();
    if (id.empty() || !keyIds.insert(id).second || !path.is_absolute())
      reject("checkpoint-key-configuration");
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) reject("checkpoint-private-key");
    const auto closeKey = ndn::make_scope_exit([fd] { ::close(fd); });
    struct stat info{};
    if (::fstat(fd, &info) || !S_ISREG(info.st_mode) || info.st_uid != ::geteuid() ||
        info.st_nlink != 1 || (info.st_mode & 077) != 0 || info.st_size != 32)
      reject("checkpoint-private-key");
    journalKeys.push_back({id, std::vector<std::uint8_t>(32)});
    auto& bytes = journalKeys.back().bytes;
    std::size_t offset = 0;
    while (offset != bytes.size()) {
      const auto size = ::read(fd, bytes.data() + offset, bytes.size() - offset);
      if (size < 0 && errno == EINTR) continue;
      if (size <= 0) reject("checkpoint-private-key");
      offset += static_cast<std::size_t>(size);
    }
  }
  authenticationKeys = nativeConversationAuthenticationKeys(
    journal.at("identity").get<std::string>(), journalKeys);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  std::vector<NativeJson> admittedCheckpoints;
  admittedCheckpoints.reserve(turns.size());
  for (const auto& turn : turns) {
    NativeJson checkpoint;
    try {
      checkpoint = nativeReadConversationCheckpoint(
        readSmall(turn.at("checkpoint_output_file").get<std::string>()), authenticationKeys,
        static_cast<std::uint64_t>(now));
    }
    catch (const std::exception&) {
      reject("checkpoint-authentication");
    }
    const auto& owner = conversation.at("owner");
    if (checkpoint.at("requesterIdentity") != owner.at("requester_identity") ||
        checkpoint.at("serviceName") != owner.at("service_name") ||
        checkpoint.at("securityDomainDigest") != owner.at("security_domain_digest"))
      reject("checkpoint-owner-binding");
    admittedCheckpoints.push_back(std::move(checkpoint));
  }
  const auto& catalog = requester.at("catalog");
  CompleteKvReplay completeKv;
  const auto& source = catalog.at("source");
  const auto sourceBound = requester.at("limits").at("max_source_bytes").get<std::uint64_t>();
  const std::filesystem::path modelPath = source.at("file").get<std::string>();
  std::map<std::filesystem::path, Stamp> files;
  files.emplace(modelPath,
    verifyFile(modelPath, source.at("bytes"), source.at("digest"), sourceBound));
  if (source.value("initializer_bytes", std::uint64_t{0})) {
    const std::filesystem::path initializer = source.at("initializer_file").get<std::string>();
    if (initializer.parent_path() != modelPath.parent_path()) reject("source-layout");
    files.emplace(initializer, verifyFile(initializer, source.at("initializer_bytes"),
      source.at("initializer_digest"), sourceBound));
  }
  std::uint64_t stateBound = 0;
  for (const auto& profile : catalog.at("splitter").at("resources_by_role")) {
    const auto bytes = profile.at("kv_bytes").get<std::uint64_t>();
    if (bytes > std::numeric_limits<std::uint64_t>::max() - stateBound) reject("state-budget");
    stateBound += bytes;
  }
  if (!stateBound) reject("state-budget");
  Ort::Env environment(ORT_LOGGING_LEVEL_WARNING, "ndnsf-unsplit-reference");
  Ort::SessionOptions sessionOptions;
  sessionOptions.SetIntraOpNumThreads(1);
  sessionOptions.SetInterOpNumThreads(1);
  // Match the installed CPU runner policy so the comparison isolates splitting,
  // rather than additional ORT graph rewrites of quantized operators.
  sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);
  const auto loadStart = std::chrono::steady_clock::now();
  Ort::Session session(environment, modelPath.c_str(), sessionOptions);
  const auto loadSeconds = std::chrono::duration<double>(
    std::chrono::steady_clock::now() - loadStart).count();
  Ort::AllocatorWithDefaultOptions allocator;
  auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::vector<std::string> inputs, outputs;
  std::map<std::string, std::size_t> outputIndices;
  for (std::size_t i = 0; i < session.GetInputCount(); ++i)
    inputs.emplace_back(session.GetInputNameAllocated(i, allocator).get());
  for (std::size_t i = 0; i < session.GetOutputCount(); ++i) {
    outputs.emplace_back(session.GetOutputNameAllocated(i, allocator).get());
    if (!outputIndices.emplace(outputs.back(), i).second) reject("duplicate-output");
  }
  std::vector<const char*> inputNames, outputNames;
  for (const auto& name : inputs) inputNames.push_back(name.c_str());
  for (const auto& name : outputs) outputNames.push_back(name.c_str());
  std::map<std::string, std::vector<std::uint8_t>> initialStorage;
  std::map<std::string, Ort::Value> states;
  std::map<std::string, std::string> successors;
  std::uint64_t prefix = 0;
  NativeJson rounds = NativeJson::array();
  std::string previousConversation;
  for (std::size_t round = 0; round < turns.size(); ++round) {
    const auto start = std::chrono::steady_clock::now();
    const auto cpu = std::clock();
    const auto& turn = turns.at(round);
    const auto groupRound = grouped ? round % 2 : round;
    if (groupRound == 0) {
      // Release tensor views before their initial backing buffers. Model/session
      // reuse must never carry one conversation's KV into the next group.
      states.clear();
      initialStorage.clear();
      prefix = 0;
      previousConversation.clear();
    }
    std::array<double, 3> backendSeconds{};
    std::array<std::size_t, 3> backendCalls{};
    const auto options = nativeParseJson(readSmall(turn.at("options_file").get<std::string>()));
    const auto& sampling = options.at("sampling");
    if (sampling.at("mode") != "Greedy" || sampling.value("repetitionPenalty", 1.0) != 1.0 ||
        !options.value("stopStrings", NativeJson::array()).empty()) reject("sampling-policy");
    const auto maximum = options.at("maxNewTokens").get<std::size_t>();
    if (!maximum || maximum > MAX_NATIVE_GENERATED_TOKENS) reject("generation-bound");
    const auto tokenName = options.at("tokenInputName").get<std::string>();
    const auto maskName = options.at("attentionMaskInputName").get<std::string>();
    const auto positionName = options.at("positionIdsInputName").get<std::string>();
    if (options.at("positionInputPolicy") != "qwen-causal-position-v1") reject("position-policy");
    std::map<std::string, std::string> currentSuccessors;
    std::istringstream pairs(options.at("stateSuccessorMap").get<std::string>());
    std::string pair;
    std::set<std::string> stateOutputs;
    while (std::getline(pairs, pair, ',')) {
      const auto at = pair.find('=');
      if (!at || at == std::string::npos || at + 1 == pair.size() ||
          !currentSuccessors.emplace(pair.substr(0, at), pair.substr(at + 1)).second ||
          !stateOutputs.insert(pair.substr(at + 1)).second) reject("state-map");
    }
    if (currentSuccessors.empty() || (round && currentSuccessors != successors))
      reject("state-map");
    successors = std::move(currentSuccessors);
    std::uint64_t initialBytes = 0;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      const auto& name = inputs[i];
      if (!successors.count(name)) {
        if (name != tokenName && name != maskName && name != positionName) reject("unknown-input");
        continue;
      }
      if (!outputIndices.count(successors.at(name))) reject("missing-state-output");
      if (groupRound) continue;
      const auto typeInfo = session.GetInputTypeInfo(i);
      const auto info = typeInfo.GetTensorTypeAndShapeInfo();
      const auto shape = stateShape(info, 0);
      const auto type = info.GetElementType();
      const auto bytes = tensorBytes(shape, type, stateBound - initialBytes);
      initialBytes += bytes;
      auto& storage = initialStorage[name];
      storage.resize(std::max<std::uint64_t>(1, bytes), 0);
      states.emplace(name, Ort::Value::CreateTensor(memory, storage.data(), bytes,
        shape.data(), shape.size(), type));
    }
    if (states.size() != successors.size()) reject("missing-state-input");
    if (round == 0)
      completeKv.loadEvidence(catalog, captures, successors);
    const auto raw = readSmall(turn.at("input_file").get<std::string>());
    const auto tensors = decodeTensorBundle(std::vector<std::uint8_t>(raw.begin(), raw.end()));
    if (tensors.size() != 1 || tensors.front().name != tokenName ||
        tensors.front().elementType != TensorElementType::Int64 ||
        tensors.front().shape.size() != 2 || tensors.front().shape.front() != 1)
      reject("token-input");
    std::vector<std::int64_t> tokens(tensors.front().payload.size() / 8);
    if (tokens.empty()) reject("token-input");
    std::memcpy(tokens.data(), tensors.front().payload.data(), tokens.size() * 8);
    if (std::any_of(tokens.begin(), tokens.end(), [] (auto value) { return value < 0; }))
      reject("token-input");
    const auto initialStateTokens = prefix;
    const auto inputTokens = tokens.size();
    const auto observed = nativeParseJson(readSmall(turn.at("output_file").get<std::string>()));
    const auto expected = observed.at("tokenIds").get<std::vector<std::int64_t>>();
    if (expected.empty() || expected.size() > maximum) reject("output-count");
    const auto eos = options.at("eosTokenIds").get<std::vector<std::int64_t>>();
    if (expected.size() < maximum &&
        std::find(eos.begin(), eos.end(), expected.back()) == eos.end())
      reject("unexplained-termination");
    std::vector<std::int64_t> generated;
    std::uint64_t stateBytes = 0;
    NativeJson stateDigests = NativeJson::object();
    for (std::size_t step = 0; step <= expected.size(); ++step) {
      if (tokens.size() > std::numeric_limits<std::int64_t>::max() - prefix)
        reject("prefix-bound");
      const auto nextPrefix = prefix + tokens.size();
      std::uint64_t estimatedBytes = 0;
      for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!successors.count(inputs[i])) continue;
        const auto typeInfo = session.GetInputTypeInfo(i);
        const auto info = typeInfo.GetTensorTypeAndShapeInfo();
        estimatedBytes += tensorBytes(stateShape(info, nextPrefix), info.GetElementType(),
                                      stateBound - estimatedBytes);
      }
      std::vector<std::int64_t> mask(nextPrefix, 1), positions(tokens.size());
      for (std::size_t i = 0; i < positions.size(); ++i) positions[i] = prefix + i;
      std::vector<Ort::Value> controls;
      std::vector<const OrtValue*> feeds;
      for (const auto& name : inputs) {
        if (successors.count(name)) feeds.push_back(states.at(name));
        else {
          auto& data = name == tokenName ? tokens : name == maskName ? mask : positions;
          const std::array<std::int64_t, 2> shape{1, static_cast<std::int64_t>(data.size())};
          controls.push_back(Ort::Value::CreateTensor<std::int64_t>(memory, data.data(),
            data.size(), shape.data(), shape.size()));
          feeds.push_back(controls.back());
        }
      }
      std::vector<OrtValue*> rawOutputs(outputs.size(), nullptr);
      std::vector<Ort::Value> values;
      values.reserve(outputs.size());
      for (std::size_t i = 0; i < outputs.size(); ++i) values.emplace_back(nullptr);
      const auto runStarted = std::chrono::steady_clock::now();
      auto* status = Ort::GetApi().Run(session, Ort::RunOptions{nullptr}, inputNames.data(),
        feeds.data(), feeds.size(), outputNames.data(), outputNames.size(), rawOutputs.data());
      const auto phase = step == 0 ? 0 : step == expected.size() ? 2 : 1;
      backendSeconds[phase] += std::chrono::duration<double>(
        std::chrono::steady_clock::now() - runStarted).count();
      ++backendCalls[phase];
      for (std::size_t i = 0; i < values.size(); ++i) values[i] = Ort::Value(rawOutputs[i]);
      Ort::ThrowOnError(status);
      prefix = nextPrefix;
      const auto logitsName = options.value("logitsOutputName", "logits");
      const auto& logits = values.at(outputIndices.at(logitsName));
      const auto info = logits.GetTensorTypeAndShapeInfo();
      const auto shape = info.GetShape();
      if (shape.size() < 2 || shape.front() != 1 || shape.back() <= 0 ||
          info.GetElementCount() < static_cast<std::uint64_t>(shape.back()))
        reject("logits-shape");
      completeKv.compareLogits(round, step, logitsName, logits);
      if (step < expected.size()) {
        const auto vocab = static_cast<std::size_t>(shape.back());
        const auto offset = info.GetElementCount() - vocab;
        const auto score = [&](std::size_t index) {
          if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT)
            return logits.GetTensorData<float>()[offset + index];
          if (info.GetElementType() == ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT16)
            return logits.GetTensorData<Ort::Float16_t>()[offset + index].ToFloat();
          reject("logits-type");
        };
        std::size_t best = 0;
        float bestScore = -std::numeric_limits<float>::infinity();
        for (std::size_t i = 0; i < vocab; ++i) {
          const auto value = score(i);
          if (!std::isfinite(value)) reject("logits-nonfinite");
          if (value > bestScore) { best = i; bestScore = value; }
        }
        if (static_cast<std::int64_t>(best) != expected[step]) {
          reject("token-mismatch:round=" + std::to_string(round) +
                 ":step=" + std::to_string(step) + ":reference=" + std::to_string(best) +
                 ":observed=" + std::to_string(expected[step]));
        }
        generated.push_back(best);
        if (step + 1 < expected.size() && std::find(eos.begin(), eos.end(), best) != eos.end())
          reject("early-eos");
        tokens = {static_cast<std::int64_t>(best)};
      }
      std::map<std::string, Ort::Value> nextStates;
      stateBytes = 0;
      for (const auto& entry : successors) {
        auto& value = values.at(outputIndices.at(entry.second));
        const auto stateInfo = value.GetTensorTypeAndShapeInfo();
        const auto inputIndex =
          std::find(inputs.begin(), inputs.end(), entry.first) - inputs.begin();
        const auto typeInfo = session.GetInputTypeInfo(inputIndex);
        const auto inputInfo = typeInfo.GetTensorTypeAndShapeInfo();
        if (stateInfo.GetElementType() != inputInfo.GetElementType() ||
            stateInfo.GetShape() != stateShape(inputInfo, prefix)) reject("state-extents");
        completeKv.compare(round, step, entry.first, value, prefix);
        stateBytes += tensorBytes(stateInfo.GetShape(), stateInfo.GetElementType(),
                                  stateBound - stateBytes);
        if (step == expected.size()) {
          const auto bytes =
            tensorBytes(stateInfo.GetShape(), stateInfo.GetElementType(), stateBound);
          stateDigests[entry.first] = nativePlanningDigest(
            static_cast<const std::uint8_t*>(value.GetTensorRawData()), bytes);
        }
        nextStates.emplace(entry.first, std::move(value));
      }
      states.swap(nextStates);
    }
    const auto& checkpoint = admittedCheckpoints.at(round);
    const auto conversation = checkpoint.at("conversationId").get<std::string>();
    if ((groupRound && conversation != previousConversation) ||
        (grouped && (turn.at("conversation_id") != conversation ||
          checkpoint.at("parentContextEpoch").get<std::uint64_t>() != groupRound)) ||
        checkpoint.at("contextEpoch").get<std::uint64_t>() != groupRound + 1 ||
        checkpoint.at("prefixTokenCount").get<std::uint64_t>() != prefix)
      reject("checkpoint-state-progression");
    previousConversation = conversation;
    rounds.push_back({{"turn", round}, {"tokenIds", generated}, {"prefixTokens", prefix},
      {"conversationId", conversation}, {"conversationTurn", groupRound},
      {"initialStateTokens", initialStateTokens}, {"inputTokens", inputTokens},
      {"startingContextTokens", initialStateTokens + inputTokens},
      {"onnxSeconds", backendSeconds[0] + backendSeconds[1] + backendSeconds[2]},
      {"onnxPhasesSeconds", {{"prefill", backendSeconds[0]},
        {"decode", backendSeconds[1]}, {"finalize", backendSeconds[2]}}},
      {"onnxPhaseCalls", {{"prefill", backendCalls[0]},
        {"decode", backendCalls[1]}, {"finalize", backendCalls[2]}}},
      {"referenceStateBytes", stateBytes}, {"modelCalls", expected.size() + 1},
      {"referenceStateDigests", stateDigests},
      {"wallSeconds",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()},
      {"processCpuSeconds", static_cast<double>(std::clock() - cpu) / CLOCKS_PER_SEC}});
  }
  for (const auto& file : files) if (stamp(file.first) != file.second) reject("source-changed");
  completeKv.finish();
  return {{"scope", "unsplit-greedy-output-and-state-progression"},
    {"runtimeVersion", Ort::GetVersionString()}, {"executionProvider", "CPUExecutionProvider"},
    {"sourceDigest", source.at("digest")}, {"loadWallSeconds", loadSeconds}, {"rounds", rounds},
    {"providerStateValuesCompared", completeKv.valuesCompared != 0},
    {"providerStateValueCount", completeKv.valuesCompared},
    {"providerStateMaxAbsoluteError", completeKv.maxError},
    {"providerLogitsValueCount", completeKv.logitsCompared},
    {"providerLogitsMaxAbsoluteError", completeKv.maxLogitsError},
    {"checkpointAuthentication", "VERIFIED"},
    {"performanceQualification", "NOT_RUN"}};
}

} // namespace spec220::oracle

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "automatic-onnx-model.hpp"
#include "automatic-onnx-generation.hpp"
#include "automatic-onnx-inference.hpp"
#include "onnx-tensor-type.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/native-owned-initializer-file.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/RuntimeTiming.hpp"

#include <ndn-cxx/util/sha256.hpp>

// Spec 182 unifies the DI ONNX world on the official 1.17 full-protobuf
// headers (ONNX_USE_LITE_PROTO=OFF) installed by the configured ONNX prefix;
// the previous vendored lite trio no longer exists in this tree.
#include <onnx/checker.h>
#include <onnx/onnx_pb.h>
#include <onnx/shape_inference/implementation.h>

#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
#include <onnxruntime_cxx_api.h>
#endif

#include <google/protobuf/io/coded_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/io/zero_copy_stream_impl_lite.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {

[[noreturn]] void fail(const char* code);

bool
isBuiltinConstant(const onnx::NodeProto& node, bool localFunction)
{
  return !localFunction && (node.domain().empty() || node.domain() == "ai.onnx") &&
    node.op_type() == "Constant" && node.overload().empty() && node.output_size() == 1;
}

class NativeShapeSchemaRegistry final : public onnx::ISchemaRegistry
{
public:
  explicit NativeShapeSchemaRegistry(const onnx::ModelProto& model)
  {
    for (const auto& function : model.functions()) {
      if (function.domain() == "com.microsoft" && function.name() == "MatMulNBits")
        m_hasLocalNBits = true;
    }
    m_nBits.SetName("MatMulNBits").SetDomain("com.microsoft").SinceVersion(1)
      .Input(0, "A", "Activation", "T")
      .Input(1, "B", "Packed weights", "TQ")
      .Input(2, "scales", "Block scales", "T")
      .Input(3, "zero_points", "Optional zero points", "TZ", onnx::OpSchema::Optional)
      .Input(4, "g_idx", "Optional group indices", "tensor(int32)", onnx::OpSchema::Optional)
      .Input(5, "bias", "Optional bias", "T", onnx::OpSchema::Optional)
      .Output(0, "Y", "Matrix product", "T")
      .TypeConstraint("T", {"tensor(float)", "tensor(float16)", "tensor(bfloat16)"},
                      "Activation type")
      .TypeConstraint("TQ", {"tensor(uint8)"}, "Packed weights")
      .TypeConstraint("TZ", {"tensor(uint8)", "tensor(float)", "tensor(float16)",
                             "tensor(bfloat16)"},
                      "Zero-point representation")
      .TypeAndShapeInferenceFunction([] (onnx::InferenceContext& context) {
        onnx::propagateElemTypeFromInputToOutput(context, 0, 0);
        const auto* k = context.getAttribute("K");
        const auto* n = context.getAttribute("N");
        if (k == nullptr || n == nullptr || k->type() != onnx::AttributeProto::INT ||
            n->type() != onnx::AttributeProto::INT || k->i() <= 0 || n->i() <= 0)
          fail_shape_inference("MatMulNBits requires positive K and N");
        if (!onnx::hasInputShape(context, 0))
          fail_shape_inference("MatMulNBits activation rank is unknown");
        const auto& input = onnx::getInputShape(context, 0);
        if (input.dim_size() < 2)
          fail_shape_inference("MatMulNBits activation rank must be at least two");
        const auto& width = input.dim(input.dim_size() - 1);
        if (width.has_dim_value() && width.dim_value() != k->i())
          fail_shape_inference("MatMulNBits activation width differs from K");
        auto output = input;
        auto* last = output.mutable_dim(output.dim_size() - 1);
        last->clear_dim_param();
        last->set_dim_value(n->i());
        onnx::updateOutputShape(context, 0, output);
      });
    m_nBits.Finalize();
  }

  const onnx::OpSchema*
  GetSchema(const std::string& name, const int version,
            const std::string& domain) const override
  {
    if (!m_hasLocalNBits && name == "MatMulNBits" && domain == "com.microsoft" && version == 1)
      return &m_nBits;
    return onnx::OpSchemaRegistry::Instance()->GetSchema(name, version, domain);
  }

private:
  onnx::OpSchema m_nBits;
  bool m_hasLocalNBits = false;
};

void
inferNativeShapes(onnx::ModelProto& model)
{
  const NativeShapeSchemaRegistry registry(model);
  onnx::shape_inference::InferShapes(model, &registry);
}

std::string digest(const std::uint8_t* data, std::size_t size)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(data, size, hash);
  std::ostringstream out;
  out << "sha256:";
  for (unsigned char value : hash) {
    out << "0123456789abcdef"[value >> 4]
        << "0123456789abcdef"[value & 0x0f];
  }
  return out.str();
}

std::string digest(const std::vector<std::uint8_t>& bytes)
{
  return digest(bytes.data(), bytes.size());
}

std::string digestModelFile(const NativeOnnxModelFileInput& file)
{
  if (file.fd < 0 || file.bytes == 0 || file.digest.empty())
    fail("SOURCE_FILE");
  struct stat status{};
  if (fstat(file.fd, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 || static_cast<std::uint64_t>(status.st_size) != file.bytes)
    fail("SOURCE_FILE");
  if (lseek(file.fd, 0, SEEK_SET) < 0)
    fail("SOURCE_FILE");
  SHA256_CTX context;
  SHA256_Init(&context);
  std::array<std::uint8_t, 1U << 20> buffer{};
  std::uint64_t total = 0;
  for (;;) {
    const ssize_t count = read(file.fd, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      fail("SOURCE_FILE");
    }
    const auto received = static_cast<std::uint64_t>(count);
    if (received > file.bytes || total > file.bytes - received)
      fail("SOURCE_FILE");
    SHA256_Update(&context, buffer.data(), static_cast<std::size_t>(count));
    total += received;
  }
  if (total != file.bytes || lseek(file.fd, 0, SEEK_SET) < 0)
    fail("SOURCE_FILE");
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256_Final(hash, &context);
  std::ostringstream out;
  out << "sha256:";
  for (unsigned char value : hash) {
    out << "0123456789abcdef"[value >> 4]
        << "0123456789abcdef"[value & 0x0f];
  }
  const auto computed = out.str();
  if (computed != file.digest)
    fail("SOURCE_DIGEST");
  return computed;
}

[[noreturn]] void fail(const char* code)
{
  throw std::runtime_error(std::string("DI_NATIVE_ONNX_") + code);
}

[[noreturn]] void failGraph(const char* stage, const std::exception& error)
{
  std::string detail = error.what();
  for (char& value : detail) {
    if (value == '\r' || value == '\n' || value == '\t') value = ' ';
    else if (static_cast<unsigned char>(value) < 0x20) value = ' ';
  }
  if (detail.size() > 512) detail.resize(512);
  logRuntimeEvidence(std::string("NDNSF_DI_NATIVE_ONNX_GRAPH_DETAIL stage=") + stage + " error=" + detail);
  fail("GRAPH");
}

[[noreturn]] void failGraphStage(const char* stage)
{
  logRuntimeEvidence(std::string("NDNSF_DI_NATIVE_ONNX_GRAPH_DETAIL stage=") + stage);
  fail("GRAPH");
}

bool
usesOrtLegacyDefaultDomainOp(const onnx::ModelProto& model,
                              const NativeCertifiedRecipe& recipe)
{
  if (recipe.adapterId != "onnx" || recipe.backend != "onnxruntime" ||
      recipe.backendAbi != "onnxruntime-cpu-v1" || !model.has_graph())
    return false;
  for (const auto& node : model.graph().node()) {
    if (node.domain().empty() && node.op_type() == "SimplifiedLayerNormalization")
      return true;
  }
  return false;
}

class ScopedOrtCompatibilityCheckerDomains
{
public:
  ScopedOrtCompatibilityCheckerDomains(onnx::ModelProto& model,
                                       const NativeCertifiedRecipe& recipe)
  {
    if (!usesOrtLegacyDefaultDomainOp(model, recipe)) return;
    for (auto& node : *model.mutable_graph()->mutable_node()) {
      if (node.domain().empty() && node.op_type() == "SimplifiedLayerNormalization") {
        node.set_domain("com.microsoft");
        m_normalized.push_back(&node);
      }
    }
  }

  ~ScopedOrtCompatibilityCheckerDomains() noexcept
  {
    for (auto* node : m_normalized) node->clear_domain();
  }

  bool active() const noexcept { return !m_normalized.empty(); }

private:
  std::vector<onnx::NodeProto*> m_normalized;
};

void
checkNativeOnnxModel(onnx::ModelProto& model, const NativeCertifiedRecipe& recipe)
{
  ScopedOrtCompatibilityCheckerDomains compatibility(model, recipe);
  if (compatibility.active()) {
    // The fixed ORT profile contains a legacy default-domain ORT operator that
    // the generic checker cannot resolve. Keep structural checker coverage;
    // the native shape and ORT checks remain mandatory below.
    onnx::checker::check_model(model, false, false, false);
  }
  else {
    onnx::checker::check_model(model, true);
  }
}

void checkActive(const NativeAssemblyControl& control)
{
  if (!control.requireActive ||
      std::chrono::steady_clock::now() >= control.deadline) {
    fail("ASSEMBLY_TIMEOUT");
  }
  control.requireActive();
}

std::string
digestInitializerRange(const NativeCanonicalSource& source, std::uint64_t offset,
                       std::uint64_t length, const NativeAssemblyControl& control)
{
  checkActive(control);
  const auto size = nativeCanonicalInitializerByteSize(source);
  if (control.maxSourceBytes == 0 || size > control.maxSourceBytes ||
      offset > size || length > size - offset)
    fail("INITIALIZER_RANGE");
  auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1)
    fail("INITIALIZER_DIGEST");
  for (std::uint64_t consumed = 0; consumed < length;) {
    checkActive(control);
    const auto count = std::min(NativeCanonicalMaterialBundleMaxBytes, length - consumed);
    if (source.initializerRangeSource) {
      const auto bytes = readNativeCanonicalInitializerRange(source, offset + consumed,
                                                             count, control);
      if (EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1)
        fail("INITIALIZER_DIGEST");
    }
    else {
      if (!source.initializerBytes ||
          EVP_DigestUpdate(context.get(), source.initializerBytes->data() +
            static_cast<std::size_t>(offset + consumed), static_cast<std::size_t>(count)) != 1)
        fail("INITIALIZER_DIGEST");
    }
    consumed += count;
  }
  std::array<unsigned char, EVP_MAX_MD_SIZE> hash{};
  unsigned hashSize = 0;
  if (EVP_DigestFinal_ex(context.get(), hash.data(), &hashSize) != 1 ||
      hashSize != SHA256_DIGEST_LENGTH)
    fail("INITIALIZER_DIGEST");
  checkActive(control);
  std::string result = "sha256:";
  for (unsigned i = 0; i < hashSize; ++i) {
    result += "0123456789abcdef"[hash[i] >> 4];
    result += "0123456789abcdef"[hash[i] & 0x0f];
  }
  return result;
}

std::uint64_t checkedAdd(std::uint64_t left, std::uint64_t right)
{
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    fail("SIZE_OVERFLOW");
  }
  return left + right;
}

class InitializerFileRangeSource final : public NativeCanonicalByteRangeSource
{
public:
  InitializerFileRangeSource(const std::string& path, std::uint64_t bytes)
    : m_size(bytes)
  {
    m_fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (m_fd < 0) fail("INITIALIZER_FILE");
    if (!validFile()) {
      ::close(m_fd);
      m_fd = -1;
      fail("INITIALIZER_FILE");
    }
  }

  ~InitializerFileRangeSource() override
  {
    if (m_fd >= 0) ::close(m_fd);
  }

  InitializerFileRangeSource(const InitializerFileRangeSource&) = delete;
  InitializerFileRangeSource& operator=(const InitializerFileRangeSource&) = delete;

  void
  consumeTemporaryPath(const std::string& path)
  {
    struct stat descriptor{}, named{};
    if (::fstat(m_fd, &descriptor) != 0 || ::lstat(path.c_str(), &named) != 0 ||
        descriptor.st_dev != named.st_dev || descriptor.st_ino != named.st_ino ||
        descriptor.st_uid != ::geteuid() || descriptor.st_nlink != 1 ||
        (descriptor.st_mode & 07777) != 0600)
      fail("INITIALIZER_PRIVATE_FILE");
    // A brief read lease proves that no writable descriptor survives. Once
    // unlinked, ordinary opens cannot introduce a new writer to this snapshot.
    const bool leased = ::fcntl(m_fd, F_SETLEASE, F_RDLCK) == 0;
    const int removed = ::unlink(path.c_str());
    struct stat unlinked{};
    const bool privateSnapshot = removed == 0 && ::fstat(m_fd, &unlinked) == 0 &&
      unlinked.st_nlink == 0;
    const int released = leased ? ::fcntl(m_fd, F_SETLEASE, F_UNLCK) : 0;
    if (removed != 0 || released != 0) fail("INITIALIZER_PRIVATE_FILE");
    m_snapshot = leased && privateSnapshot;
  }

  std::optional<std::string>
  snapshotDigest() const
  {
    if (!validFile()) fail("INITIALIZER_FILE");
    struct stat status{};
    if (::fstat(m_fd, &status) != 0 || status.st_nlink != 0)
      return std::nullopt;
    std::lock_guard<std::mutex> lock(m_digestMutex);
    return m_snapshot ? m_digest : std::nullopt;
  }

  void
  rememberSnapshotDigest(const std::string& digest) const
  {
    if (!validFile()) fail("INITIALIZER_FILE");
    if (!m_snapshot) return;
    std::lock_guard<std::mutex> lock(m_digestMutex);
    m_digest = digest;
  }

  std::uint64_t
  size() const noexcept override
  {
    return m_size;
  }

  std::vector<std::uint8_t>
  read(std::uint64_t offset, std::uint64_t length) const override
  {
    if (offset > m_size || length > m_size - offset ||
        length > NativeCanonicalMaterialBundleMaxBytes || !validFile())
      fail("INITIALIZER_RANGE");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    std::size_t received = 0;
    unsigned interrupted = 0;
    while (received < bytes.size()) {
      const auto count = ::pread(m_fd, bytes.data() + received, bytes.size() - received,
                                 static_cast<off_t>(offset + received));
      if (count < 0 && errno == EINTR && ++interrupted <= 3) continue;
      if (count <= 0) fail("INITIALIZER_FILE");
      received += static_cast<std::size_t>(count);
      interrupted = 0;
    }
    if (!validFile()) fail("INITIALIZER_FILE");
    return bytes;
  }

private:
  bool
  validFile() const noexcept
  {
    struct stat status{};
    return m_fd >= 0 && ::fstat(m_fd, &status) == 0 && S_ISREG(status.st_mode) &&
      status.st_size > 0 && static_cast<std::uint64_t>(status.st_size) == m_size;
  }

  int m_fd = -1;
  const std::uint64_t m_size;
  bool m_snapshot = false;
  mutable std::mutex m_digestMutex;
  mutable std::optional<std::string> m_digest;
};

/**
 * Keep the certified model on disk while ONNX Runtime constructs its own
 * graph/session representation.  Loading from the in-memory vector leaves
 * the serialized model alive beside ORT's copy and is particularly expensive
 * for a cold multi-provider assembly.  The file is private, unlinked on every
 * exit path, and read back only after the ORT session has been destroyed so
 * the worker still returns the exact bytes validated by the parent protocol.
 */
class ScopedOnnxRuntimeModelFile
{
public:
  explicit ScopedOnnxRuntimeModelFile(const std::vector<std::uint8_t>& bytes)
  {
    std::array<char, 64> pattern{};
    std::snprintf(pattern.data(), pattern.size(), "/tmp/ndnsf-di-onnx-XXXXXX");
    const int fd = ::mkstemp(pattern.data());
    if (fd < 0) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_CREATE");
    }
    m_path = pattern.data();
    if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
      ::close(fd);
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_PERMISSIONS");
    }
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
      if (written > 0) {
        offset += static_cast<std::size_t>(written);
        continue;
      }
      if (written < 0 && errno == EINTR) continue;
      ::close(fd);
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_WRITE");
    }
    if (::fsync(fd) != 0 || ::close(fd) != 0) {
      cleanup();
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_FLUSH");
    }
  }

  ~ScopedOnnxRuntimeModelFile() noexcept
  {
    cleanup();
  }

  ScopedOnnxRuntimeModelFile(const ScopedOnnxRuntimeModelFile&) = delete;
  ScopedOnnxRuntimeModelFile& operator=(const ScopedOnnxRuntimeModelFile&) = delete;

  const char* c_str() const noexcept { return m_path.c_str(); }

  std::vector<std::uint8_t> read(std::uint64_t maxBytes) const
  {
    std::error_code error;
    const auto size = std::filesystem::file_size(m_path, error);
    if (error || size == 0 || size > maxBytes) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    std::ifstream input(m_path, std::ios::binary);
    if (!input.good()) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    if (!input.read(reinterpret_cast<char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()))) {
      throw std::runtime_error("DI_NATIVE_ONNX_RUNTIME_STAGING_READ");
    }
    return bytes;
  }

private:
  void cleanup() noexcept
  {
    if (!m_path.empty()) {
      std::error_code ignored;
      std::filesystem::remove(m_path, ignored);
      m_path.clear();
    }
  }

private:
  std::string m_path;
};

std::uint64_t parseUint(const std::string& value, const char* field)
{
  if (value.empty() || (value.size() > 1 && value.front() == '0') ||
      !std::all_of(value.begin(), value.end(), [] (unsigned char ch) {
        return std::isdigit(ch) != 0;
      })) {
    throw std::runtime_error(std::string("DI_NATIVE_ONNX_EXTERNAL_") + field);
  }
  try {
    return std::stoull(value);
  }
  catch (...) {
    throw std::runtime_error(std::string("DI_NATIVE_ONNX_EXTERNAL_") + field);
  }
}

std::string externalValue(const onnx::TensorProto& tensor, const std::string& key)
{
  for (int i = 0; i < tensor.external_data_size(); ++i) {
    const auto& entry = tensor.external_data(i);
    if (entry.key() == key) return entry.value();
  }
  return {};
}

void inlineExternal(onnx::TensorProto& tensor,
                    const std::vector<std::uint8_t>& bytes)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL) return;
  const auto location = externalValue(tensor, "location");
  if (location.empty() || location.front() == '/' || location.find('\\') != std::string::npos) {
    fail("EXTERNAL_LOCATION");
  }
  std::size_t component = 0;
  while (component < location.size()) {
    const auto slash = location.find('/', component);
    const auto part = location.substr(component, slash == std::string::npos ?
                                      std::string::npos : slash - component);
    if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
    if (slash == std::string::npos) break;
    component = slash + 1;
  }
  const auto offsetText = externalValue(tensor, "offset");
  const auto lengthText = externalValue(tensor, "length");
  const auto offset = offsetText.empty() ? 0 : parseUint(offsetText, "OFFSET");
  if (offset > bytes.size()) fail("EXTERNAL_RANGE");
  const auto length = lengthText.empty() ? bytes.size() - offset : parseUint(lengthText, "LENGTH");
  if (length > bytes.size() - offset || length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("EXTERNAL_RANGE");
  tensor.set_raw_data(bytes.data() + offset, static_cast<int>(length));
  tensor.clear_external_data();
  tensor.set_data_location(onnx::TensorProto::DEFAULT);
}

bool graphHasExternal(const onnx::GraphProto& graph);

bool tensorHasExternal(const onnx::TensorProto& tensor)
{
  return tensor.data_location() == onnx::TensorProto::EXTERNAL;
}

bool graphHasExternal(const onnx::GraphProto& graph)
{
  for (const auto& tensor : graph.initializer()) {
    if (tensorHasExternal(tensor)) return true;
  }
  for (const auto& node : graph.node()) {
    for (const auto& attribute : node.attribute()) {
      if (attribute.has_t() && tensorHasExternal(attribute.t())) return true;
      for (const auto& tensor : attribute.tensors()) {
        if (tensorHasExternal(tensor)) return true;
      }
      if (attribute.has_g() && graphHasExternal(attribute.g())) return true;
      for (const auto& nested : attribute.graphs()) {
        if (graphHasExternal(nested)) return true;
      }
    }
  }
  return false;
}

void inlineGraph(onnx::GraphProto& graph,
                 const std::vector<std::uint8_t>& initializerBytes)
{
  for (int i = 0; i < graph.initializer_size(); ++i) {
    inlineExternal(*graph.mutable_initializer(i), initializerBytes);
  }
  for (int i = 0; i < graph.node_size(); ++i) {
    auto* node = graph.mutable_node(i);
    for (int j = 0; j < node->attribute_size(); ++j) {
      auto* attribute = node->mutable_attribute(j);
      if (attribute->has_t()) inlineExternal(*attribute->mutable_t(), initializerBytes);
      for (int k = 0; k < attribute->tensors_size(); ++k)
        inlineExternal(*attribute->mutable_tensors(k), initializerBytes);
      if (attribute->has_g()) inlineGraph(*attribute->mutable_g(), initializerBytes);
      for (int k = 0; k < attribute->graphs_size(); ++k)
        inlineGraph(*attribute->mutable_graphs(k), initializerBytes);
    }
  }
}

void validateContracts(const std::vector<NativeAssemblyTensorContractV3>& contracts,
                       std::set<std::string>& names)
{
  for (const auto& contract : contracts) {
    if (contract.name.empty() || contract.dtype.empty() ||
        !names.insert(contract.name).second) fail("IO_CONTRACT");
  }
}

std::vector<std::uint8_t> deterministicWire(const onnx::ModelProto& model,
                                            std::uint64_t limit)
{
  const auto size = model.ByteSizeLong();
  if (size == 0 || size > limit || size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!model.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return bytes;
}

// ---------------------------------------------------------------------------
// S5/S6 certified extraction (OA07/OA08).  This port mirrors onnx 1.17
// python utils.py::Extractor running on an InferShapes copy of the inlined
// source, plus executor.py::assemble_certified_onnx_model S6 cover and io
// comparisons.  All failures stay in the registered DI_NATIVE_ONNX_* family.
// ---------------------------------------------------------------------------

std::string deterministicMessageBytes(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return std::string(bytes.begin(), bytes.end());
}

std::vector<std::uint8_t> deterministicMessageVector(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SERIALIZE_LIMIT");
  // Keep the large material-backed model in one serialization buffer.  The
  // previous string-then-vector path retained two full copies until the
  // temporary string was destroyed, which inflated the first Provider's
  // working set immediately before the worker boundary.
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  return bytes;
}

// Reverse DFS from one requested output tensor, stopping at graph input and
// declared role-boundary names; node producers are exact matches on the node
// output list, exactly like utils.py _dfs_search_reachable_nodes.  Every
// recursion either stops or moves at least one index from `unreachable` into
// `reachable`, so the depth is bounded by the (recipe-capped) node count even
// on cyclic graphs.
void
dfsReachNodes(const std::string& outputName,
              const std::unordered_set<std::string>& graphInputNames,
              const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes,
              std::unordered_set<std::size_t>& unreachable,
              std::unordered_set<std::size_t>& reachable)
{
  if (graphInputNames.count(outputName) != 0) return;  // extraction boundary
  std::vector<std::size_t> producers;
  for (const std::size_t index : unreachable) {
    const auto& outs = nodes.Get(static_cast<int>(index)).output();
    if (std::find(outs.begin(), outs.end(), outputName) != outs.end())
      producers.push_back(index);
  }
  for (const std::size_t index : producers) {
    reachable.insert(index);
    unreachable.erase(index);
  }
  for (const std::size_t index : producers) {
    for (const auto& input : nodes.Get(static_cast<int>(index)).input())
      dfsReachNodes(input, graphInputNames, nodes, unreachable, reachable);
  }
}

// Local functions reachable from the selected nodes, in first-reference
// order, including functions referenced from function bodies
// (utils.py _collect_referred_local_functions).  A node refers the first
// model function whose name/domain pair matches its op_type/domain.
std::vector<const onnx::FunctionProto*>
referredLocalFunctions(const onnx::ModelProto& model,
                       const std::vector<std::size_t>& reachable,
                       const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes)
{
  std::vector<const onnx::FunctionProto*> referred;
  std::vector<const onnx::NodeProto*> frontier;
  frontier.reserve(reachable.size());
  for (const std::size_t index : reachable)
    frontier.push_back(&nodes.Get(static_cast<int>(index)));
  while (!frontier.empty()) {
    std::vector<const onnx::NodeProto*> children;
    for (const onnx::NodeProto* node : frontier) {
      const onnx::FunctionProto* match = nullptr;
      for (const auto& fn : model.functions()) {
        if (fn.name() == node->op_type() && fn.domain() == node->domain()) {
          match = &fn;
          break;
        }
      }
      if (match == nullptr ||
          std::find(referred.begin(), referred.end(), match) != referred.end())
        continue;
      referred.push_back(match);
      for (int i = 0; i < match->node_size(); ++i)
        children.push_back(&match->node(i));
    }
    frontier.swap(children);
  }
  return referred;
}

// Boundary value_infos in the requested order: a name that is an original
// graph io keeps that entry, an internal name comes from the shape-inferred
// value_info, and a name in neither is an extraction failure (the python
// reference hits a KeyError there; both reject).
std::vector<const onnx::ValueInfoProto*>
collectBoundaryIo(const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& originalIo,
                  const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& inferredValueInfo,
                  const std::vector<NativeAssemblyTensorContractV3>& requested)
{
  std::unordered_map<std::string, const onnx::ValueInfoProto*> original;
  for (const auto& value : originalIo) original[value.name()] = &value;
  std::unordered_map<std::string, const onnx::ValueInfoProto*> inferred;
  for (const auto& value : inferredValueInfo) inferred[value.name()] = &value;
  std::vector<const onnx::ValueInfoProto*> result;
  result.reserve(requested.size());
  for (const auto& contract : requested) {
    const auto* value = original.count(contract.name) != 0
      ? original[contract.name]
      : (inferred.count(contract.name) != 0 ? inferred[contract.name] : nullptr);
    if (value == nullptr) failGraphStage("boundary-io");
    result.push_back(value);
  }
  return result;
}

// Rebuild of the extractor metadata, field set exactly like the python
// 1.17 make_model(graph, ...) call: ir_version, opset_import copies,
// producer_name, then a graph holding name, selected nodes in original
// order, referenced initializers and inferred value_info in original order,
// and the boundary io; sparse initializers and quantization annotations are
// rejections (python _collect_reachable_tensors).  No other source metadata
// is copied.
onnx::ModelProto
makeExtractedModel(onnx::ModelProto& inferredSource,
                   const std::vector<std::size_t>& reachable,
                   const std::vector<const onnx::ValueInfoProto*>& inputs,
                   const std::vector<const onnx::ValueInfoProto*>& outputs,
                   const std::vector<const onnx::FunctionProto*>& functions)
{
  const auto& graph = inferredSource.graph();
  std::unordered_set<std::string> keepNames;
  for (const std::size_t index : reachable) {
    const auto& node = graph.node(static_cast<int>(index));
    for (const auto& input : node.input()) keepNames.insert(input);
    for (const auto& output : node.output()) keepNames.insert(output);
  }
  if (graph.sparse_initializer_size() != 0 ||
      graph.quantization_annotation_size() != 0)
    failGraphStage("unsupported-graph-fields");
  onnx::ModelProto result;
  result.set_ir_version(inferredSource.ir_version());
  result.set_producer_name("onnx.utils.extract_model");
  for (const auto& opset : inferredSource.opset_import())
    *result.add_opset_import() = opset;
  auto* outGraph = result.mutable_graph();
  outGraph->set_name("Extracted from {" + graph.name() + "}");
  for (const std::size_t index : reachable)
    *outGraph->add_node() = graph.node(static_cast<int>(index));
  for (int i = 0; i < graph.initializer_size(); ++i) {
    const auto& initializer = graph.initializer(i);
    if (keepNames.count(initializer.name()) == 0)
      continue;
    // The source graph is discarded immediately after extraction. Transfer
    // the selected TensorProto instead of copying its potentially GiB-sized
    // raw_data string while the source graph is still resident.
    auto* output = outGraph->add_initializer();
    output->Swap(inferredSource.mutable_graph()->mutable_initializer(i));
  }
  for (const auto& value : graph.value_info())
    if (keepNames.count(value.name()) != 0)
      *outGraph->add_value_info() = value;
  for (const auto* value : inputs) *outGraph->add_input() = *value;
  for (const auto* value : outputs) *outGraph->add_output() = *value;
  for (const auto* fn : functions) *result.add_functions() = *fn;
  return result;
}

// executor.py S6 dtype label: the pinned _ONNX_DTYPE_NAMES table; element
// types outside the table compare as their decimal string, so the label
// helper returns nullptr and the caller falls back to to_string.
const char*
onnxDtypeLabel(std::int32_t elemType)
{
  switch (elemType) {
    case 1: return "float32";
    case 2: return "uint8";
    case 3: return "int8";
    case 4: return "uint16";
    case 5: return "int16";
    case 6: return "int32";
    case 7: return "int64";
    case 9: return "bool";
    case 10: return "float16";
    case 11: return "float64";
    case 12: return "uint32";
    case 13: return "uint64";
    case 16: return "bfloat16";
    default: return nullptr;
  }
}

std::string normalizeShapeDimText(const std::string& value)
{
  // python normalize_shape_dimension: int-parseable decimal text compares
  // numerically with ints; symbolic text stays raw.
  if (!value.empty()) {
    std::size_t consumed = 0;
    try {
      const long long parsed = std::stoll(value, &consumed);
      if (consumed == value.size())
        return std::to_string(parsed);
    }
    catch (const std::exception&) {}
  }
  return value;
}

// S6 io semantic comparison against the recipe contracts, with the python
// dtype normalization (_ONNX_DTYPE_NAMES + decimal fallback) and the
// normalize_shape_dimension dimension semantics.  A missing expected name
// is an IO_CONTRACT rejection; any dtype/shape difference is IO_DTYPE.
void
compareBoundaryContracts(const std::vector<NativeAssemblyTensorContractV3>& expected,
                         const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& assembledIo)
{
  std::unordered_map<std::string, const onnx::ValueInfoProto*> byName;
  for (const auto& value : assembledIo) byName[value.name()] = &value;
  for (const auto& contract : expected) {
    const auto found = byName.find(contract.name);
    if (found == byName.end()) fail("IO_CONTRACT");
    const auto& typeProto = found->second->type();
    if (!typeProto.has_tensor_type()) fail("IO_DTYPE");
    const auto& tensorType = typeProto.tensor_type();
    std::string expectedDtype = contract.dtype;
    if (!expectedDtype.empty() &&
        std::all_of(expectedDtype.begin(), expectedDtype.end(),
                    [](char c) { return c >= '0' && c <= '9'; })) {
      char* end = nullptr;
      const long parsed = std::strtol(expectedDtype.c_str(), &end, 10);
      if (end != nullptr && *end == '\0' && parsed >= 0 &&
          static_cast<std::uint64_t>(parsed) <=
            static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        const char* label = onnxDtypeLabel(static_cast<int>(parsed));
        if (label != nullptr) expectedDtype = label;
      }
    }
    const char* observedLabel = onnxDtypeLabel(tensorType.elem_type());
    const std::string observedDtype = observedLabel != nullptr
      ? std::string(observedLabel) : std::to_string(tensorType.elem_type());
    if (observedDtype != expectedDtype) fail("IO_DTYPE");
    const auto& dims = tensorType.shape().dim();
    if (static_cast<std::size_t>(dims.size()) != contract.shape.size()) fail("IO_DTYPE");
    for (int i = 0; i < dims.size(); ++i) {
      const std::string observedDim = dims.Get(i).has_dim_value()
        ? std::to_string(dims.Get(i).dim_value()) : dims.Get(i).dim_param();
      const auto& dimension = contract.shape[i];
      const auto expectedDim = std::holds_alternative<std::int64_t>(dimension)
        ? std::to_string(std::get<std::int64_t>(dimension)) : std::get<std::string>(dimension);
      if (normalizeShapeDimText(observedDim) != normalizeShapeDimText(expectedDim))
        fail("IO_DTYPE");
    }
  }
}

} // namespace

namespace {

// Shared certified-assembly chain (the OA04 core).  One implementation runs
// the S1-S7 gates for both entry styles: OA01 (in-process provider calls with
// control caps and a cancellation-checking onRound) and the bounded worker
// child (certified recipe caps from the metadata, no-op onRound).  onRound is
// invoked at every deadline/cancellation checkpoint exactly where the old
// in-process chain called checkActive(control).
NativeOnnxIdentity
canonicalOnnxModelIdentity(const onnx::ModelProto& model,
                           const NativeAssemblyControl& control,
                           const std::vector<std::uint8_t>* externalBytes = nullptr);

void materializeShapeInferenceInitializers(
  onnx::ModelProto& inferred,
  const onnx::ModelProto& sourceModel,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl& control,
  std::uint64_t* materializedBudget,
  const NativeCanonicalSource* externalSource = nullptr);

void materializeSelectedExternalInitializers(
  onnx::ModelProto& model,
  const std::vector<std::size_t>& selectedNodes,
  const std::vector<std::uint8_t>& externalBytes,
  const NativeAssemblyControl& control);

onnx::ModelProto
ownedSourceModel(const NativeCanonicalSource& source,
                 const NativeAssemblyControl& control,
                 bool inlineExternal = true,
                 std::uint64_t* materializedBudget = nullptr,
                 const NativeOnnxModelFileInput* modelFile = nullptr);

NativeCertifiedAssembly
assembleCertifiedOnnxChain(const NativeCanonicalSource& source,
                           const NativeCertifiedRecipe& recipe,
                           std::uint64_t maxSourceBytes,
                           std::uint64_t maxAssembledBytes,
                           const std::function<void()>& onRound,
                           NativeCanonicalSource* sourceToReleaseAfterParse,
                           bool loadRuntimeSession,
                           const NativeOnnxModelFileInput* modelFile = nullptr)
{
  onRound();
  const std::uint64_t sourceBytes = modelFile != nullptr
    ? modelFile->bytes : static_cast<std::uint64_t>(source.modelBytes.size());
  if (maxSourceBytes == 0 || maxAssembledBytes == 0 ||
      sourceBytes == 0 || sourceBytes > maxSourceBytes ||
      (modelFile == nullptr &&
       source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    fail("SOURCE_LIMIT");
  if (modelFile != nullptr && !source.modelBytes.empty())
    fail("SOURCE_OWNERSHIP");
  if (source.initializerBytes &&
      (source.initializerBytes->empty() ||
       source.initializerBytes->size() > maxSourceBytes ||
       source.initializerBytes->size() >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
       checkedAdd(sourceBytes, source.initializerBytes->size()) >
         checkedAdd(maxSourceBytes, maxSourceBytes)))
    fail("INITIALIZER_LIMIT");
  if (recipe.adapterId.empty() || recipe.backend.empty() || recipe.roleKind.empty() ||
      recipe.nodeIndices.empty() || recipe.maxNodes == 0 ||
      recipe.nodeIndices.size() > recipe.maxNodes) fail("RECIPE");
  std::set<std::string> inputNames;
  std::set<std::string> outputNames;
  validateContracts(recipe.expectedInputs, inputNames);
  validateContracts(recipe.expectedOutputs, outputNames);
  if (inputNames.empty() || outputNames.empty()) fail("IO_CONTRACT");

  // OA04 materialized-role certificate path.  The parent has already
  // authenticated the manifest, selected node payloads, initializer payloads,
  // role boundary, deterministic wire, and role graph/initializer identity in
  // materializeNativeCanonicalModel/canonicalOnnxSourceIdentity.  It then
  // writes that exact byte vector through an atomic staging file.  Re-parsing
  // the same 752 MiB role in the child would recreate the complete protobuf
  // graph and run the second full checker without adding an independent
  // authorization decision; run-46 showed that copy as the first owned-swap
  // boundary beside another Provider's resident runner.
  //
  // Keep the child-side certificate bounded to the immutable file identity and
  // the recipe/contract shape.  The parent reads the file only after the child
  // has exited, and the Provider creates the single authoritative ORT session
  // afterwards.  Inline callers and the ORT-enabled parity entry retain the
  // full parse/check path below.
  if (modelFile != nullptr && recipe.materializedRole && !loadRuntimeSession) {
    if (source.initializerBytes)
      fail("EXTERNAL_BINDING");
    if (modelFile->bytes > maxAssembledBytes)
      fail("SERIALIZE_LIMIT");
    for (std::size_t index = 0; index < recipe.nodeIndices.size(); ++index) {
      if (recipe.nodeIndices[index] != index)
        fail("NODE_COVER");
    }
    const auto modelDigest = digestModelFile(*modelFile);
    onRound();
    NativeCertifiedAssembly result;
    result.modelDigest = modelDigest;
    result.nodeCount = recipe.nodeIndices.size();
    for (const auto& contract : recipe.expectedInputs)
      result.inputNames.push_back(contract.name);
    for (const auto& contract : recipe.expectedOutputs)
      result.outputNames.push_back(contract.name);
    return result;
  }

  NativeAssemblyControl sourceControl;
  sourceControl.deadline = std::chrono::steady_clock::time_point::max();
  sourceControl.requireActive = onRound;
  sourceControl.maxSourceBytes = maxSourceBytes;
  sourceControl.maxAssembledBytes = maxAssembledBytes;
  // The cold canonical-source path must present the same owned, in-memory
  // model to ONNX full-checking that the old Python/native preparation path
  // validated.  ONNX checker resolves EXTERNAL TensorProto locations through
  // `model.onnx.data`; Provider memory has no such filesystem object.  The
  // material-backed path is handled separately below and never enters this
  // full-source copy: it already carries a selected role model rebuilt from
  // authenticated material payloads.
  const auto* externalBytes = source.initializerBytes
    ? &source.initializerBytes->asVector() : nullptr;
  onnx::ModelProto original = ownedSourceModel(source, sourceControl, true,
                                               nullptr, modelFile);
  if (!original.has_graph() || original.graph().node_size() == 0 ||
      original.graph().node_size() > static_cast<int>(recipe.maxNodes))
    failGraphStage("source-graph");

  std::set<std::uint64_t> selected;
  for (const auto index : recipe.nodeIndices) {
    if (index >= static_cast<std::uint64_t>(original.graph().node_size()) ||
        !selected.insert(index).second) fail("NODE_COVER");
  }
  // COMPONENT_SET identifies an adapter-defined, exact node set.  It does
  // not mean that the role must own the whole canonical source graph: a
  // semantic component may be a certified subset of a larger model.  The
  // selected set is checked against the extracted graph below, where every
  // reachable node and every certified index must agree byte-for-byte.
  if (recipe.roleKind != "COMPONENT_SET" &&
      (recipe.layerEnd <= recipe.layerBegin ||
       recipe.layerEnd > static_cast<std::uint64_t>(original.graph().node_size())))
    fail("LAYER_RANGE");

  // S3: a complete canonical source must pass the full checker before any
  // certificate comparison (executor.py check_model(model, full_check=True)).
  // A post-Selection materialized role is intentionally a compact graph: its
  // selected nodes may expose an inter-role tensor and cannot retain the
  // complete source graph's final outputs.  Its authenticated manifest and
  // recipe therefore bind a separate worker path; shape inference, boundary
  // extraction, assembled checking and ORT loading below remain mandatory.
  if (!recipe.materializedRole) {
    try {
      checkNativeOnnxModel(original, recipe);
    }
    catch (const std::exception& error) {
      failGraph("canonical-check", error);
    }
  }

  // S4: compare the authenticated graph/initializer identity without making
  // a protobuf-owned copy of the external bytes.
  const auto identity = canonicalOnnxModelIdentity(original, sourceControl,
                                                    externalBytes);
  if (identity.graphDigest != recipe.graphDigest ||
      identity.initializerDigest != recipe.canonicalInitializerDigest)
    fail("RECIPE");
  onRound();

  // S5: retain the authenticated materialized role directly, or infer shapes
  // and extract the selected role from a canonical source.  The latter path
  // materializes only shape-value initializers; ordinary Qwen weight tensors
  // remain external until the selected role is extracted.  A deep
  // `inferred = original` copy here duplicates the complete source graph and
  // its external-backed tensors before extraction, which is precisely the
  // peak this bounded worker is intended to avoid.
  std::vector<std::string> certifiedNodeBytes;
  certifiedNodeBytes.reserve(recipe.nodeIndices.size());
  for (const auto index : recipe.nodeIndices) {
    certifiedNodeBytes.push_back(deterministicMessageBytes(
      original.graph().node(static_cast<int>(index))));
  }
  onnx::ModelProto assembled;
  if (recipe.materializedRole) {
    // A materialized role was rebuilt from authenticated selected node and
    // initializer payloads immediately before this worker was spawned.  The
    // parent has already bound its graph/initializer identity to that role
    // model, and the recipe is rewritten to the compact contiguous cover.
    // Re-running source shape inference and reachability extraction here
    // creates a second model-sized protobuf graph without adding a new
    // authorization or integrity check.  Keep the identity, node-byte,
    // contract, full-check and ORT checks below; only reuse this already
    // certified role graph.
    for (std::size_t index = 0; index < recipe.nodeIndices.size(); ++index) {
      if (recipe.nodeIndices[index] != index)
        fail("NODE_COVER");
    }
    if (original.graph().node_size() !=
        static_cast<int>(recipe.nodeIndices.size()))
      fail("NODE_COVER");
    assembled.Swap(&original);
  }
  else {
      materializeShapeInferenceInitializers(
        original, original, externalBytes, sourceControl, nullptr);
      try {
        inferNativeShapes(original);
      }
      catch (const std::exception& error) {
        failGraph("shape-inference", error);
      }
      onRound();
      const auto& inferredGraph = original.graph();
      std::unordered_set<std::string> extractionBoundaryNames;
      for (const auto& input : inferredGraph.input()) extractionBoundaryNames.insert(input.name());
      for (const auto& contract : recipe.expectedInputs)
        extractionBoundaryNames.insert(contract.name);
      std::unordered_set<std::size_t> unreachable;
      for (int i = 0; i < inferredGraph.node_size(); ++i)
        unreachable.insert(static_cast<std::size_t>(i));
      std::unordered_set<std::size_t> reachable;
      for (const auto& contract : recipe.expectedOutputs)
        dfsReachNodes(contract.name, extractionBoundaryNames, inferredGraph.node(),
                      unreachable, reachable);
      std::vector<std::size_t> selectedNodes;
      // The certified recipe owns the complete node cover for this role.  Output
      // reachability still determines the required execution subgraph, but a
      // valid source may contain certified side-effect-free nodes that are not on
      // a path to one of the declared outputs.  Retain those explicitly selected
      // nodes so the assembled graph remains byte-identical to the authenticated
      // cover instead of silently changing the recipe identity.
      std::set<std::size_t> selectedAndReachable(reachable.begin(), reachable.end());
      selectedAndReachable.insert(selected.begin(), selected.end());
      selectedNodes.reserve(selectedAndReachable.size());
      for (const std::size_t index : selectedAndReachable) selectedNodes.push_back(index);
      std::sort(selectedNodes.begin(), selectedNodes.end());  // original order
      if (externalBytes != nullptr) {
        materializeSelectedExternalInitializers(
          original, selectedNodes, *externalBytes, sourceControl);
      }
      // The selected TensorProto values now own their authenticated bytes, so
      // the worker request buffer can be scrubbed before serialization and ORT
      // load. This is the key cache-compatible memory boundary.
      if (sourceToReleaseAfterParse != nullptr) {
        if (sourceToReleaseAfterParse != &source)
          fail("SOURCE_OWNERSHIP");
        std::fill(sourceToReleaseAfterParse->modelBytes.begin(),
                  sourceToReleaseAfterParse->modelBytes.end(), 0);
        std::vector<std::uint8_t>{}.swap(sourceToReleaseAfterParse->modelBytes);
        if (sourceToReleaseAfterParse->initializerBytes) {
          auto& initializerBytes = sourceToReleaseAfterParse->initializerBytes->asVector();
          if (!initializerBytes.empty())
            OPENSSL_cleanse(initializerBytes.data(), initializerBytes.size());
          std::vector<std::uint8_t>{}.swap(initializerBytes);
          sourceToReleaseAfterParse->initializerBytes.reset();
        }
        sourceToReleaseAfterParse->initializerRangeSource.reset();
        sourceToReleaseAfterParse->externalFileRanges.clear();
      }
      const auto inputs = collectBoundaryIo(inferredGraph.input(),
                                            inferredGraph.value_info(),
                                            recipe.expectedInputs);
      const auto outputs = collectBoundaryIo(inferredGraph.output(),
                                             inferredGraph.value_info(),
                                             recipe.expectedOutputs);
      const auto functions = referredLocalFunctions(original, selectedNodes,
                                                    inferredGraph.node());
      assembled = makeExtractedModel(original, selectedNodes,
                                     inputs, outputs, functions);
  }
  if (sourceToReleaseAfterParse != nullptr && recipe.materializedRole) {
    if (sourceToReleaseAfterParse != &source)
      fail("SOURCE_OWNERSHIP");
    std::fill(sourceToReleaseAfterParse->modelBytes.begin(),
              sourceToReleaseAfterParse->modelBytes.end(), 0);
    std::vector<std::uint8_t>{}.swap(sourceToReleaseAfterParse->modelBytes);
    if (sourceToReleaseAfterParse->initializerBytes) {
      auto& initializerBytes = sourceToReleaseAfterParse->initializerBytes->asVector();
      if (!initializerBytes.empty())
        OPENSSL_cleanse(initializerBytes.data(), initializerBytes.size());
      std::vector<std::uint8_t>{}.swap(initializerBytes);
      sourceToReleaseAfterParse->initializerBytes.reset();
    }
    sourceToReleaseAfterParse->initializerRangeSource.reset();
    sourceToReleaseAfterParse->externalFileRanges.clear();
  }

  // `original` still owns the complete authenticated source graph and all of
  // its initializers after extraction.  Keeping it alive through the second
  // checker, wire serialization, and ORT session load makes the worker retain
  // the full source plus the selected role simultaneously.  Extraction has
  // copied every field needed by S6/S7, so release that graph before entering
  // the final validation/runtime phase.  The scope ensures protobuf storage is
  // actually destroyed rather than merely cleared and retained by an arena.
  {
    onnx::ModelProto releasedOriginal;
    original.Swap(&releasedOriginal);
  }

  // S6: the assembled model passes the full checker again, its node cover
  // equals the certified indices with byte-identical nodes, and its io
  // matches the recipe contracts semantically (executor.py S6 block).
  try {
    checkNativeOnnxModel(assembled, recipe);
  }
  catch (const std::exception& error) {
    failGraph("assembled-check", error);
  }
  onRound();
  if (assembled.graph().node_size() != static_cast<int>(recipe.nodeIndices.size()))
    fail("NODE_COVER");
  for (std::size_t i = 0; i < recipe.nodeIndices.size(); ++i) {
    const auto& expectedBytes = certifiedNodeBytes[i];
    const auto assembledBytes = deterministicMessageBytes(
      assembled.graph().node(static_cast<int>(i)));
    if (expectedBytes != assembledBytes) fail("NODE_COVER");
  }
  compareBoundaryContracts(recipe.expectedInputs, assembled.graph().input());
  compareBoundaryContracts(recipe.expectedOutputs, assembled.graph().output());
  onRound();

  // S7: deterministic wire within the resource envelope.  The public
  // in-process parity path also loads a real CPU session here, while the
  // production worker skips that duplicate load: its parent creates and
  // warms the authoritative OnnxRuntimeModelRunner before RUNNER_READY.
  // Keeping the switch at this internal seam preserves the parity helper
  // without retaining a second model-sized ORT graph in the worker.
  // A materialized role is already an authenticated, deterministic ONNX file.
  // The file-backed worker has parsed that file and completed S1-S6 against
  // it, while the parent will read the same immutable file after the child
  // exits.  Serializing the protobuf once more here would create a second
  // model-sized buffer solely to rediscover the file digest, which is the
  // cold-worker peak this path is intended to avoid.  Inline/parity callers
  // still take the original deterministic serialization path.
  std::vector<std::uint8_t> bytes;
  std::string assembledDigest;
  if (modelFile != nullptr) {
    if (!recipe.materializedRole || modelFile->bytes == 0 ||
        modelFile->bytes > maxAssembledBytes || modelFile->digest.empty())
      fail("SERIALIZE_LIMIT");
    assembledDigest = modelFile->digest;
  }
  else {
    bytes = deterministicWire(assembled, maxAssembledBytes);
    assembledDigest = digest(bytes);
  }
  {
    onnx::ModelProto releasedAssembled;
    assembled.Swap(&releasedAssembled);
  }
  onRound();
#ifdef NDNSF_DI_ENABLE_ONNXRUNTIME_CPP
  if (loadRuntimeSession) {
    try {
      // Loading from a byte vector keeps the complete serialized role model
      // resident while ORT builds its graph/session representation.  Stage the
      // already-certified bytes privately, release the vector, and read it back
      // only after the validation session is destroyed.  The returned bytes and
      // digest therefore remain identical to the protocol artifact.
      ScopedOnnxRuntimeModelFile stagedModel(bytes);
      std::vector<std::uint8_t>{}.swap(bytes);
      {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "ndnsf-certified-assembly");
        Ort::SessionOptions options;
        // Match the production OnnxRuntimeModelRunner's bounded CPU session.
        // The default ORT intra-op pool allocates per-thread scratch state while
        // validating a model-sized role and needlessly raises the cold-worker
        // peak beside another Provider's resident runner.
        options.SetIntraOpNumThreads(1);
        // Do not retain a second prepacked weight copy while another Provider's
        // runner is resident.  The production runner uses the same setting;
        // this validation session must exercise the same bounded CPU contract.
        options.AddConfigEntry("session.disable_prepacking", "1");
        // This worker only proves that the certified bytes can create a real CPU
        // session; S6 has already performed the full graph/IO checks.  Do not
        // build an optimized execution graph here: the selected Provider creates
        // the production BASIC session after the worker exits, and performing
        // BASIC optimization twice is the cold-start peak seen with two roles.
        options.SetGraphOptimizationLevel(ORT_DISABLE_ALL);
        Ort::Session session(env, stagedModel.c_str(), options);
      }
      bytes = stagedModel.read(maxAssembledBytes);
    }
    catch (const std::exception& error) {
      failGraph("ort-session", error);
    }
  }
#else
  if (loadRuntimeSession) {
    // The ONNX graph/protobuf helpers remain linkable in a disabled build, but
    // the certified in-process parity gate requires the optional ORT runtime.
    fail("RUNTIME_UNAVAILABLE");
  }
#endif
  onRound();
  NativeCertifiedAssembly result;
  result.modelDigest = assembledDigest;
  result.modelBytes = std::move(bytes);
  result.nodeCount = recipe.nodeIndices.size();
  for (const auto& contract : recipe.expectedInputs) result.inputNames.push_back(contract.name);
  for (const auto& contract : recipe.expectedOutputs) result.outputNames.push_back(contract.name);
  return result;
}

} // namespace

NativeCertifiedAssembly
assembleNativeCertifiedOnnxModel(const NativeCanonicalSource& source,
                                 const NativeCertifiedRecipe& recipe,
                                 const NativeAssemblyControl& control)
{
  // OA01 entry: the caller keeps a valid cancellation/deadline control and
  // its own resource budget; every round of the shared chain re-checks it.
  checkActive(control);
  return assembleCertifiedOnnxChain(source, recipe, control.maxSourceBytes,
                                    control.maxAssembledBytes,
                                    [&control] { checkActive(control); }, nullptr,
                                    true);
}

NativeCertifiedAssembly
assembleInProcess(const NativeCanonicalSource& source,
                  const NativeCertifiedRecipe& recipe,
                  NativeCanonicalSource* sourceToReleaseAfterParse,
                  const NativeOnnxModelFileInput* modelFile)
{
  // OA04: the bounded worker child runs the identical chain against the
  // certified recipe budget with no cancellation callback of its own; the
  // parent transport owns cancellation by killing this process.  The worker
  // performs structural/digest/IO validation; the parent owns the single
  // authoritative ORT load and warmup before publishing RUNNER_READY.
  return assembleCertifiedOnnxChain(source, recipe, recipe.maxSourceBytes,
                                    recipe.maxAssembledBytes, [] {},
                                    sourceToReleaseAfterParse, false, modelFile);
}

// ---------------------------------------------------------------------------
// T006-A canonical source identity (OA05/OA06 seam).
//
// Python-identity compatibility target: the frozen 24 numeric + 14 extended
// reference vectors (tests/fixtures/spec182/dependency-probes) were produced
// by the pinned graph.py e5532328.. reference; this code reproduces that
// graph-facts JSON and ordered initializer digest, with the v2 normalization
// rules from initializer-normalization.md overriding the payload encoding
// (BFLOAT16 raw/typed, STRING framing, typed COMPLEX, FLOAT8/INT4/UINT4).
// Source filenames, external packing layout, protobuf field order and
// shape-inferred graphs never enter the identity.
// ---------------------------------------------------------------------------

namespace {

void failNormalization(const char* code)
{
  // Registered DI_ONNX_* reason codes (normalization contract): the adapter
  // error text is not a protocol oracle, the reason family is.
  throw std::runtime_error(std::string("DI_ONNX_") + code);
}

// elementCount of TensorProto.dims with overflow checks; every dim must be
// non-negative; a scalar (no dims) counts as one element.
std::uint64_t tensorElementCount(const onnx::TensorProto& tensor)
{
  std::uint64_t count = 1;
  for (int i = 0; i < tensor.dims_size(); ++i) {
    const auto dim = tensor.dims(i);
    if (dim < 0) failNormalization("INITIALIZER_ENCODING_INVALID");
    if (count != 0 && static_cast<std::uint64_t>(dim) >
        std::numeric_limits<std::uint64_t>::max() / count)
      failNormalization("INITIALIZER_ENCODING_INVALID");
    count *= static_cast<std::uint64_t>(dim);
  }
  return count;
}

std::vector<std::uint8_t> u64le(std::uint64_t value)
{
  std::vector<std::uint8_t> bytes(8);
  for (int i = 0; i < 8; ++i) {
    bytes[i] = static_cast<std::uint8_t>(value & 0xff);
    value >>= 8;
  }
  return bytes;
}

void appendU16le(std::vector<std::uint8_t>& out, std::uint16_t value)
{
  out.push_back(static_cast<std::uint8_t>(value & 0xff));
  out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
}

void appendU32le(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    value >>= 8;
  }
}

void appendU64le(std::vector<std::uint8_t>& out, std::uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
    value >>= 8;
  }
}

template<typename T>
void appendRaw(std::vector<std::uint8_t>& out, const T& value)
{
  static_assert(sizeof(T) == 4 || sizeof(T) == 8);
  if constexpr (sizeof(T) == 4) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU32le(out, bits);
  }
  else {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    appendU64le(out, bits);
  }
}

const char* dtypeLabel(int dataType)
{
  switch (dataType) {
    case onnx::TensorProto::FLOAT: return "float32";
    case onnx::TensorProto::UINT8: return "uint8";
    case onnx::TensorProto::INT8: return "int8";
    case onnx::TensorProto::UINT16: return "uint16";
    case onnx::TensorProto::INT16: return "int16";
    case onnx::TensorProto::INT32: return "int32";
    case onnx::TensorProto::INT64: return "int64";
    case onnx::TensorProto::STRING: return "string";
    case onnx::TensorProto::BOOL: return "bool";
    case onnx::TensorProto::FLOAT16: return "float16";
    case onnx::TensorProto::DOUBLE: return "float64";
    case onnx::TensorProto::UINT32: return "uint32";
    case onnx::TensorProto::UINT64: return "uint64";
    case onnx::TensorProto::COMPLEX64: return "complex64";
    case onnx::TensorProto::COMPLEX128: return "complex128";
    // Fixed literal labels from the independent reference extraction; the
    // C++ must not "improve" these names, they enter the graph digest.
    case onnx::TensorProto::BFLOAT16: return "(numpy.uint16, [('bfloat16', '<u2')])";
    case onnx::TensorProto::FLOAT8E4M3FN: return "(numpy.uint8, [('e4m3fn', 'u1')])";
    case onnx::TensorProto::FLOAT8E4M3FNUZ: return "(numpy.uint8, [('e4m3fnuz', 'u1')])";
    case onnx::TensorProto::FLOAT8E5M2: return "(numpy.uint8, [('e5m2', 'u1')])";
    case onnx::TensorProto::FLOAT8E5M2FNUZ: return "(numpy.uint8, [('e5m2fnuz', 'u1')])";
    case onnx::TensorProto::UINT4: return "(numpy.uint8, [('uint4', 'u1')])";
    case onnx::TensorProto::INT4: return "(numpy.int8, [('int4', 'i1')])";
    default: failNormalization("INITIALIZER_ENCODING_INVALID"); return "";
  }
}

std::string byteOrderOf(int dataType)
{
  switch (dataType) {
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::INT8:
    case onnx::TensorProto::BOOL:
    case onnx::TensorProto::STRING:
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
    case onnx::TensorProto::UINT4:
    case onnx::TensorProto::INT4: return "na";
    default: return "little";
  }
}

// Exact number of packed raw bytes one count-sized tensor of dataType
// requires; throws when the byte size would overflow.
std::uint64_t rawByteLength(int dataType, std::uint64_t count)
{
  const auto scaled = [&] (std::uint64_t elementBytes) {
    if (elementBytes != 0 && count > std::numeric_limits<std::uint64_t>::max() / elementBytes)
      failNormalization("INITIALIZER_ENCODING_INVALID");
    return count * elementBytes;
  };
  switch (dataType) {
    case onnx::TensorProto::FLOAT:
    case onnx::TensorProto::INT32:
    case onnx::TensorProto::UINT32: return scaled(4);
    case onnx::TensorProto::DOUBLE:
    case onnx::TensorProto::INT64:
    case onnx::TensorProto::UINT64: return scaled(8);
    case onnx::TensorProto::UINT16:
    case onnx::TensorProto::INT16:
    case onnx::TensorProto::FLOAT16:
    case onnx::TensorProto::BFLOAT16: return scaled(2);
    case onnx::TensorProto::COMPLEX64: return scaled(8);
    case onnx::TensorProto::COMPLEX128: return scaled(16);
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::INT8:
    case onnx::TensorProto::BOOL: return scaled(1);
    case onnx::TensorProto::UINT4:
    case onnx::TensorProto::INT4:
      return (count + 1) / 2;  // packed bytes, ceil(count / 2)
    default: failNormalization("INITIALIZER_ENCODING_INVALID"); return 0;
  }
}

std::vector<std::uint8_t> encodeTypedComplex(const onnx::TensorProto& tensor,
                                             int dataType, std::uint64_t count)
{
  // typed COMPLEX stores 2 * count real/imag components in float_data
  // (complex64) or double_data (complex128), one component pair per element.
  std::vector<std::uint8_t> content;
  if (dataType == onnx::TensorProto::COMPLEX64) {
    if (tensor.float_data_size() != static_cast<int>(2 * count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 2))
      failNormalization("INITIALIZER_ENCODING_INVALID");
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.float_data_size(); ++i)
      appendRaw(content, tensor.float_data(i));
  } else {
    if (tensor.double_data_size() != static_cast<int>(2 * count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max() / 2))
      failNormalization("INITIALIZER_ENCODING_INVALID");
    content.reserve(static_cast<std::size_t>(16 * count));
    for (int i = 0; i < tensor.double_data_size(); ++i)
      appendRaw(content, tensor.double_data(i));
  }
  return content;
}

std::vector<std::uint8_t> encodeTypedInt32Based(const onnx::TensorProto& tensor,
                                                int dataType, std::uint64_t count)
{
  // Typed storage for small/8-bit types is int32_data.  INT4/UINT4 tensors
  // pack two 4-bit elements per int32 byte (low nibble first), matching the
  // raw packed layout; every other type stores one element per int32 slot.
  const auto required = [&] {
    if (dataType == onnx::TensorProto::UINT4 || dataType == onnx::TensorProto::INT4)
      return (count + 1) / 2;
    return count;
  }();
  if (tensor.int32_data_size() != static_cast<int>(required) ||
      required > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    failNormalization("INITIALIZER_ENCODING_INVALID");

  std::vector<std::uint8_t> content;
  switch (dataType) {
    case onnx::TensorProto::INT8:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(static_cast<std::uint8_t>(static_cast<std::int8_t>(tensor.int32_data(i))));
      return content;
    case onnx::TensorProto::UINT8:
    case onnx::TensorProto::FLOAT8E4M3FN:
    case onnx::TensorProto::FLOAT8E4M3FNUZ:
    case onnx::TensorProto::FLOAT8E5M2:
    case onnx::TensorProto::FLOAT8E5M2FNUZ:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(static_cast<std::uint8_t>(tensor.int32_data(i) & 0xff));
      return content;
    case onnx::TensorProto::BOOL:
      content.reserve(static_cast<std::size_t>(count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        content.push_back(tensor.int32_data(i) != 0 ? 1 : 0);
      return content;
    case onnx::TensorProto::INT16:
      content.reserve(static_cast<std::size_t>(2 * count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        appendU16le(content, static_cast<std::uint16_t>(static_cast<std::int16_t>(tensor.int32_data(i))));
      return content;
    case onnx::TensorProto::UINT16:
    case onnx::TensorProto::FLOAT16:
    case onnx::TensorProto::BFLOAT16:
      content.reserve(static_cast<std::size_t>(2 * count));
      for (int i = 0; i < tensor.int32_data_size(); ++i)
        appendU16le(content, static_cast<std::uint16_t>(tensor.int32_data(i) & 0xffff));
      return content;
    case onnx::TensorProto::INT4: {
      // packed bytes, sign-extended to int8 per nibble, low nibble first
      content.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto packed = static_cast<std::uint8_t>(
          tensor.int32_data(static_cast<int>(i / 2)) & 0xff);
        const auto nibble = (i % 2 == 0) ? (packed & 0x0f) : (packed >> 4);
        content.push_back(static_cast<std::uint8_t>(
          static_cast<std::int8_t>(nibble & 8 ? nibble - 16 : nibble)));
      }
      return content;
    }
    case onnx::TensorProto::UINT4: {
      content.reserve(static_cast<std::size_t>(count));
      for (std::uint64_t i = 0; i < count; ++i) {
        const auto packed = static_cast<std::uint8_t>(
          tensor.int32_data(static_cast<int>(i / 2)) & 0xff);
        content.push_back((i % 2 == 0) ? (packed & 0x0f) : (packed >> 4));
      }
      return content;
    }
    default:
      failNormalization("INITIALIZER_ENCODING_INVALID");
      return {};
  }
}

std::vector<std::uint8_t> encodeTyped64(const onnx::TensorProto& tensor,
                                        int dataType, std::uint64_t count)
{
  const auto check = [&] (int present) {
    if (present != static_cast<int>(count) ||
        count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
      failNormalization("INITIALIZER_ENCODING_INVALID");
  };
  std::vector<std::uint8_t> content;
  if (dataType == onnx::TensorProto::INT64) {
    check(tensor.int64_data_size());
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.int64_data_size(); ++i)
      appendU64le(content, static_cast<std::uint64_t>(tensor.int64_data(i)));
  } else if (dataType == onnx::TensorProto::UINT64) {
    check(tensor.uint64_data_size());
    content.reserve(static_cast<std::size_t>(8 * count));
    for (int i = 0; i < tensor.uint64_data_size(); ++i)
      appendU64le(content, tensor.uint64_data(i));
  } else {  // UINT32 typed is stored in uint64_data by the onnx helpers
    check(tensor.uint64_data_size());
    content.reserve(static_cast<std::size_t>(4 * count));
    for (int i = 0; i < tensor.uint64_data_size(); ++i)
      appendU32le(content, static_cast<std::uint32_t>(tensor.uint64_data(i) & 0xffffffffULL));
  }
  return content;
}

std::vector<std::uint8_t> encodeString(const onnx::TensorProto& tensor,
                                       std::uint64_t count)
{
  // Explicit UTF-8 framing: ASCII "NDNSF-ONNX-STRING-v2" + NUL, uint64
  // little-endian elementCount, then per item (row-major) a uint64 LE byte
  // length followed by the raw UTF-8 bytes.  No Unicode normalization, no
  // numpy object memory; string_data must hold exactly elementCount items
  // and raw_data never substitutes for STRING encoding.
  if (tensor.string_data_size() != static_cast<int>(count) ||
      count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    failNormalization("INITIALIZER_ENCODING_INVALID");
  if (!tensor.raw_data().empty()) failNormalization("INITIALIZER_ENCODING_INVALID");
  std::vector<std::uint8_t> content;
  const char header[] = "NDNSF-ONNX-STRING-v2";
  content.insert(content.end(), header, header + 20);
  content.push_back(0);
  const auto countBytes = u64le(count);
  content.insert(content.end(), countBytes.begin(), countBytes.end());
  for (int i = 0; i < tensor.string_data_size(); ++i) {
    const auto& item = tensor.string_data(i);
    // Strict UTF-8 validation of each item; embedded NUL is a content byte.
    std::size_t pos = 0;
    while (pos < item.size()) {
      const auto first = static_cast<unsigned char>(item[pos]);
      std::size_t width = 0;
      if (first < 0x80) width = 1;
      else if ((first & 0xe0) == 0xc0) width = 2;
      else if ((first & 0xf0) == 0xe0) width = 3;
      else if ((first & 0xf8) == 0xf0) width = 4;
      else failNormalization("INITIALIZER_ENCODING_INVALID");
      if (pos + width > item.size()) failNormalization("INITIALIZER_ENCODING_INVALID");
      for (std::size_t k = 1; k < width; ++k) {
        if ((static_cast<unsigned char>(item[pos + k]) & 0xc0) != 0x80)
          failNormalization("INITIALIZER_ENCODING_INVALID");
      }
      pos += width;
    }
    const auto lengthBytes = u64le(item.size());
    content.insert(content.end(), lengthBytes.begin(), lengthBytes.end());
    content.insert(content.end(), item.begin(), item.end());
  }
  return content;
}

} // namespace

NormalizedInitializerPayload
normalizedOnnxInitializerPayload(const std::vector<std::uint8_t>& serializedTensorProto)
{
  onnx::TensorProto tensor;
  if (serializedTensorProto.empty() ||
      !tensor.ParseFromArray(serializedTensorProto.data(),
                             static_cast<int>(serializedTensorProto.size())) ||
      static_cast<std::size_t>(tensor.ByteSizeLong()) != serializedTensorProto.size())
    failNormalization("INITIALIZER_ENCODING_INVALID");
  const int dataType = tensor.data_type();
  const auto count = tensorElementCount(tensor);
  if (count == 0 && (!tensor.raw_data().empty() || tensor.float_data_size() != 0 ||
      tensor.double_data_size() != 0 || tensor.int32_data_size() != 0 ||
      tensor.int64_data_size() != 0 || tensor.uint64_data_size() != 0 ||
      tensor.string_data_size() != 0))
    failNormalization("INITIALIZER_ENCODING_INVALID");

  NormalizedInitializerPayload payload;
  payload.shape.reserve(static_cast<std::size_t>(tensor.dims_size()));
  for (int i = 0; i < tensor.dims_size(); ++i) payload.shape.push_back(tensor.dims(i));
  payload.dtype = dtypeLabel(dataType);
  payload.byteOrder = byteOrderOf(dataType);

  if (dataType == onnx::TensorProto::STRING) {
    payload.content = encodeString(tensor, count);
  } else {
    // raw_data has priority exactly when it carries bytes (the same
    // precedence the onnx python helpers apply); an explicitly empty raw
    // field falls through to the typed representation.  The v2 rules fix
    // BFLOAT16/STRING raw handling by validating the raw bytes instead of
    // ignoring them.
    const bool typedComplex = (dataType == onnx::TensorProto::COMPLEX64 ||
                               dataType == onnx::TensorProto::COMPLEX128) &&
                              tensor.raw_data().empty();
    if (!tensor.raw_data().empty()) {
      const auto expected = rawByteLength(dataType, count);
      if (tensor.raw_data().size() != expected)
        failNormalization("INITIALIZER_ENCODING_INVALID");
      if (dataType == onnx::TensorProto::UINT4 || dataType == onnx::TensorProto::INT4) {
        // Raw INT4/UINT4 carry ceil(count/2) packed bytes; the canonical
        // payload expands every nibble to one byte (low nibble first),
        // sign-extending INT4, mirroring the typed encoders and the frozen
        // python reference bit patterns.
        payload.content.reserve(static_cast<std::size_t>(count));
        for (std::uint64_t i = 0; i < count; ++i) {
          const auto packed = static_cast<std::uint8_t>(
            tensor.raw_data()[static_cast<std::size_t>(i / 2)]);
          const auto nibble = (i % 2 == 0) ? (packed & 0x0f) : (packed >> 4);
          if (dataType == onnx::TensorProto::INT4)
            payload.content.push_back(static_cast<std::uint8_t>(
              static_cast<std::int8_t>(nibble & 8 ? nibble - 16 : nibble)));
          else
            payload.content.push_back(nibble);
        }
      } else {
        payload.content.assign(tensor.raw_data().begin(), tensor.raw_data().end());
      }
    } else if (typedComplex) {
      payload.content = encodeTypedComplex(tensor, dataType, count);
    } else {
      // Missing raw bytes and a non-empty tensor have no legal encoding.
      if (count > 0) {
        switch (dataType) {
          case onnx::TensorProto::FLOAT:
            if (tensor.float_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(4 * count));
            for (int i = 0; i < tensor.float_data_size(); ++i)
              appendRaw(payload.content, tensor.float_data(i));
            break;
          case onnx::TensorProto::DOUBLE:
            if (tensor.double_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(8 * count));
            for (int i = 0; i < tensor.double_data_size(); ++i)
              appendRaw(payload.content, tensor.double_data(i));
            break;
          case onnx::TensorProto::INT32:
            if (tensor.int32_data_size() != static_cast<int>(count) ||
                count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
              failNormalization("INITIALIZER_ENCODING_INVALID");
            payload.content.reserve(static_cast<std::size_t>(4 * count));
            for (int i = 0; i < tensor.int32_data_size(); ++i)
              appendU32le(payload.content, static_cast<std::uint32_t>(tensor.int32_data(i)));
            break;
          case onnx::TensorProto::INT8:
          case onnx::TensorProto::UINT8:
          case onnx::TensorProto::INT16:
          case onnx::TensorProto::UINT16:
          case onnx::TensorProto::FLOAT16:
          case onnx::TensorProto::BFLOAT16:
          case onnx::TensorProto::BOOL:
          case onnx::TensorProto::FLOAT8E4M3FN:
          case onnx::TensorProto::FLOAT8E4M3FNUZ:
          case onnx::TensorProto::FLOAT8E5M2:
          case onnx::TensorProto::FLOAT8E5M2FNUZ:
          case onnx::TensorProto::UINT4:
          case onnx::TensorProto::INT4:
            payload.content = encodeTypedInt32Based(tensor, dataType, count);
            break;
          case onnx::TensorProto::INT64:
          case onnx::TensorProto::UINT64:
          case onnx::TensorProto::UINT32:
            payload.content = encodeTyped64(tensor, dataType, count);
            break;
          default:
            failNormalization("INITIALIZER_ENCODING_INVALID");
        }
      }
      // The zero-element payload was checked before entering the type branches.
    }
  }
  return payload;
}

namespace {

// Deep scan of every tensor an external tensor can hide in: graph and nested
// graph initializers, node attribute tensors, and function attribute tensors
// (S2 _get_all_tensors coverage).  The identity path validates and inlines
// strictly from the bytes the caller authenticated; it never opens a path
// declared inside the model.
void collectAttributeTensors(const onnx::AttributeProto& attribute,
                             std::vector<onnx::TensorProto*>& tensors,
                             bool rejectSparse = false)
{
  if (rejectSparse && (attribute.type() == onnx::AttributeProto::SPARSE_TENSOR ||
                       attribute.type() == onnx::AttributeProto::SPARSE_TENSORS))
    fail("AUTOMATIC_SPARSE_UNSUPPORTED");
  if (attribute.has_t()) tensors.push_back(const_cast<onnx::TensorProto*>(&attribute.t()));
  for (int i = 0; i < attribute.tensors_size(); ++i)
    tensors.push_back(const_cast<onnx::TensorProto*>(&attribute.tensors(i)));
}

void collectGraphTensors(onnx::GraphProto& graph, std::vector<onnx::TensorProto*>& tensors,
                         bool rejectSparse = false)
{
  if (rejectSparse && graph.sparse_initializer_size() != 0)
    fail("AUTOMATIC_SPARSE_UNSUPPORTED");
  for (int i = 0; i < graph.initializer_size(); ++i)
    tensors.push_back(graph.mutable_initializer(i));
  for (int i = 0; i < graph.node_size(); ++i) {
    auto* node = graph.mutable_node(i);
    for (int j = 0; j < node->attribute_size(); ++j) {
      auto* attribute = node->mutable_attribute(j);
      collectAttributeTensors(*attribute, tensors, rejectSparse);
      if (attribute->has_g()) collectGraphTensors(*attribute->mutable_g(), tensors, rejectSparse);
      for (int k = 0; k < attribute->graphs_size(); ++k)
        collectGraphTensors(*attribute->mutable_graphs(k), tensors, rejectSparse);
    }
  }
}

void collectModelTensors(onnx::ModelProto& model, std::vector<onnx::TensorProto*>& tensors,
                         bool rejectSparse = false)
{
  collectGraphTensors(*model.mutable_graph(), tensors, rejectSparse);
  for (int i = 0; i < model.functions_size(); ++i) {
    auto* function = model.mutable_functions(i);
    for (int j = 0; j < function->attribute_proto_size(); ++j) {
      auto* attribute = function->mutable_attribute_proto(j);
      collectAttributeTensors(*attribute, tensors, rejectSparse);
      if (attribute->has_g()) collectGraphTensors(*attribute->mutable_g(), tensors, rejectSparse);
      for (int k = 0; k < attribute->graphs_size(); ++k)
        collectGraphTensors(*attribute->mutable_graphs(k), tensors, rejectSparse);
    }
    for (int j = 0; j < function->node_size(); ++j) {
      auto* node = function->mutable_node(j);
      for (int k = 0; k < node->attribute_size(); ++k) {
        auto* attribute = node->mutable_attribute(k);
        collectAttributeTensors(*attribute, tensors, rejectSparse);
        if (attribute->has_g()) collectGraphTensors(*attribute->mutable_g(), tensors, rejectSparse);
        for (int m = 0; m < attribute->graphs_size(); ++m)
          collectGraphTensors(*attribute->mutable_graphs(m), tensors, rejectSparse);
      }
    }
  }
}

// S2 external rules for the identity path: every external tensor has exactly
// one location, no absolute/..//empty-basename components, all locations
// identical, offset/length bounded with length-0 meaning "rest from offset"
// (the 1.17 loader behavior).  Validation of the whole model happens before
// any copy.
std::string validateAndPinLocation(const std::vector<onnx::TensorProto*>& tensors)
{
  std::string pinnedLocation;
  for (auto* tensor : tensors) {
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    std::set<std::string> metadataKeys;
    std::string location;
    int locationEntries = 0;
    for (int i = 0; i < tensor->external_data_size(); ++i) {
      const auto& entry = tensor->external_data(i);
      if (!metadataKeys.insert(entry.key()).second) fail("EXTERNAL_METADATA");
      if (entry.key() == "location") {
        location = entry.value();
        ++locationEntries;
      }
    }
    if (locationEntries != 1 || location.empty() || location.front() == '/' ||
        location.find('\\') != std::string::npos)
      fail("EXTERNAL_LOCATION");
    std::size_t component = 0;
    while (component < location.size()) {
      const auto slash = location.find('/', component);
      const auto part = location.substr(component,
                                        slash == std::string::npos ? std::string::npos
                                                                   : slash - component);
      if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
      if (slash == std::string::npos) break;
      component = slash + 1;
    }
    if (!pinnedLocation.empty() && pinnedLocation != location) fail("EXTERNAL_LOCATION");
    pinnedLocation = location;
  }
  return pinnedLocation;
}

struct ExternalByteRange
{
  std::uint64_t offset = 0;
  std::uint64_t length = 0;
};

ExternalByteRange validatedExternalByteRange(const onnx::TensorProto& tensor,
                                             std::uint64_t bytes)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL)
    fail("EXTERNAL_BINDING");
  std::set<std::string> metadataKeys;
  std::string location;
  int locationEntries = 0;
  std::string offsetText;
  std::string lengthText;
  for (const auto& entry : tensor.external_data()) {
    if (!metadataKeys.insert(entry.key()).second) fail("EXTERNAL_METADATA");
    if (entry.key() == "location") {
      location = entry.value();
      ++locationEntries;
    } else if (entry.key() == "offset") {
      offsetText = entry.value();
    } else if (entry.key() == "length") {
      lengthText = entry.value();
    }
  }
  if (locationEntries != 1 || location.empty() || location.front() == '/' ||
      location.find('\\') != std::string::npos)
    fail("EXTERNAL_LOCATION");
  std::size_t component = 0;
  while (component < location.size()) {
    const auto slash = location.find('/', component);
    const auto part = location.substr(component,
                                      slash == std::string::npos ? std::string::npos
                                                                 : slash - component);
    if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
    if (slash == std::string::npos) break;
    component = slash + 1;
  }
  const auto offset = offsetText.empty() ? 0 : parseUint(offsetText, "OFFSET");
  if (offset > bytes) fail("EXTERNAL_RANGE");
  std::uint64_t length = bytes - offset;
  if (!lengthText.empty()) {
    length = parseUint(lengthText, "LENGTH");
    // ONNX 1.17 treats an explicit zero length as the remainder of the
    // authenticated external file. Keep this rule identical to inlining.
    if (length == 0) length = bytes - offset;
  }
  if (length > bytes - offset)
    fail("EXTERNAL_RANGE");
  return {offset, length};
}

ExternalByteRange
validatedExternalByteRange(const onnx::TensorProto& tensor, const NativeCanonicalSource& source)
{
  if (source.externalFileRanges.empty()) {
    return validatedExternalByteRange(tensor, nativeCanonicalInitializerByteSize(source));
  }
  std::string location;
  for (const auto& item : tensor.external_data()) {
    if (item.key() == "location") location = item.value();
  }
  const auto found = source.externalFileRanges.find(location);
  if (found == source.externalFileRanges.end()) fail("EXTERNAL_LOCATION");
  auto range = validatedExternalByteRange(tensor, found->second.bytes);
  range.offset = checkedAdd(found->second.offset, range.offset);
  return range;
}

void
validateExternalLocations(const std::vector<onnx::TensorProto*>& tensors,
                          const NativeCanonicalSource& source)
{
  constexpr std::size_t MAX_EXTERNAL_LOCATION_BYTES = 4096;
  if (source.externalFileRanges.empty()) {
    (void)validateAndPinLocation(tensors);
    return;
  }
  if (!source.initializerRangeSource || source.initializerBytes ||
      source.externalFileRanges.size() > NATIVE_ONNX_MAX_EXTERNAL_FILES) fail("EXTERNAL_BINDING");
  std::uint64_t end = 0;
  std::set<std::string> referenced;
  for (const auto& [location, range] : source.externalFileRanges) {
    if (location.empty() || location.size() > MAX_EXTERNAL_LOCATION_BYTES ||
        location.front() == '/' || location.back() == '/' ||
        location.find('\\') != std::string::npos || location.find('\0') != std::string::npos ||
        range.bytes == 0 || range.offset != end) fail("EXTERNAL_LOCATION");
    std::size_t begin = 0;
    while (begin < location.size()) {
      const auto slash = location.find('/', begin);
      const auto part = location.substr(begin, slash == std::string::npos
        ? std::string::npos : slash - begin);
      if (part.empty() || part == "." || part == "..") fail("EXTERNAL_LOCATION");
      if (slash == std::string::npos) break;
      begin = slash + 1;
    }
    end = checkedAdd(end, range.bytes);
  }
  if (end != source.initializerRangeSource->size()) fail("EXTERNAL_RANGE");
  for (const auto* tensor : tensors) {
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    (void)validatedExternalByteRange(*tensor, source);
    for (const auto& item : tensor->external_data()) {
      if (item.key() == "location") referenced.insert(item.value());
    }
  }
  if (referenced.size() != source.externalFileRanges.size()) fail("EXTERNAL_BINDING");
}

ExternalByteRange
validatedExternalByteRange(const onnx::TensorProto& tensor,
                           const std::vector<std::uint8_t>& bytes)
{
  const auto range = validatedExternalByteRange(tensor, bytes.size());
  if (range.length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("EXTERNAL_RANGE");
  return range;
}

void inlineValidatedExternals(const std::vector<onnx::TensorProto*>& tensors,
                              const std::vector<std::uint8_t>& bytes,
                              const NativeAssemblyControl* control = nullptr,
                              std::uint64_t* copiedBytes = nullptr)
{
  for (auto* tensor : tensors) {
    if (control != nullptr) checkActive(*control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    const auto range = validatedExternalByteRange(*tensor, bytes);
    if (copiedBytes != nullptr) {
      *copiedBytes = checkedAdd(*copiedBytes, range.length);
      if (control != nullptr &&
          (control->maxAssembledBytes == 0 || *copiedBytes > control->maxAssembledBytes))
        fail("MATERIAL_LIMIT");
    }
    tensor->set_raw_data(reinterpret_cast<const char*>(bytes.data() + range.offset),
                         static_cast<int>(range.length));
    tensor->clear_external_data();
    tensor->set_data_location(onnx::TensorProto::DEFAULT);
    if (control != nullptr) checkActive(*control);
  }
}

void
inlineValidatedExternals(const std::vector<onnx::TensorProto*>& tensors,
                         const NativeCanonicalSource& source,
                         const NativeAssemblyControl* control,
                         std::uint64_t* copiedBytes = nullptr)
{
  if (!source.initializerRangeSource) {
    if (!source.initializerBytes) fail("EXTERNAL_BINDING");
    inlineValidatedExternals(tensors, source.initializerBytes->asVector(), control, copiedBytes);
    return;
  }
  if (control == nullptr) fail("SOURCE_OWNERSHIP");
  std::uint64_t localBytes = 0;
  auto& total = copiedBytes ? *copiedBytes : localBytes;
  for (auto* tensor : tensors) {
    checkActive(*control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    const auto range = validatedExternalByteRange(*tensor, source);
    if (range.length > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
      fail("EXTERNAL_RANGE");
    total = checkedAdd(total, range.length);
    if (control->maxAssembledBytes == 0 || total > control->maxAssembledBytes)
      fail("MATERIAL_LIMIT");
    auto* raw = tensor->mutable_raw_data();
    raw->clear();
    raw->reserve(static_cast<std::size_t>(range.length));
    for (std::uint64_t offset = 0; offset < range.length;) {
      const auto length = std::min(NativeCanonicalMaterialBundleMaxBytes, range.length - offset);
      const auto bytes = readNativeCanonicalInitializerRange(source, range.offset + offset,
                                                            length, *control);
      raw->append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      offset += length;
    }
    tensor->clear_external_data();
    tensor->set_data_location(onnx::TensorProto::DEFAULT);
    checkActive(*control);
  }
}

onnx::TensorProto materializeExternalTensor(
  const onnx::TensorProto& tensor,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl* control = nullptr,
  const NativeCanonicalSource* externalSource = nullptr)
{
  if (tensor.data_location() != onnx::TensorProto::EXTERNAL)
    fail("EXTERNAL_BINDING");
  if (externalBytes == nullptr && externalSource == nullptr)
    fail("EXTERNAL_BINDING");
  auto result = tensor;
  std::vector<onnx::TensorProto*> tensors{&result};
  if (externalSource != nullptr)
    inlineValidatedExternals(tensors, *externalSource, control);
  else
    inlineValidatedExternals(tensors, *externalBytes, control);
  return result;
}

template<typename InitializerSource>
void inlineDeepExternals(onnx::ModelProto& model,
                         const InitializerSource& bytes,
                         const NativeAssemblyControl* control,
                         std::uint64_t* materializedBudget = nullptr)
{
  std::vector<onnx::TensorProto*> all;
  collectModelTensors(model, all);
  std::unordered_set<onnx::TensorProto*> topLevel;
  topLevel.reserve(static_cast<std::size_t>(model.graph().initializer_size()));
  for (int i = 0; i < model.graph().initializer_size(); ++i)
    topLevel.insert(model.mutable_graph()->mutable_initializer(i));
  std::vector<onnx::TensorProto*> deep;
  std::vector<onnx::TensorProto*> externalDeep;
  deep.reserve(all.size());
  for (auto* tensor : all)
    if (topLevel.count(tensor) == 0) {
      deep.push_back(tensor);
      if (tensor->data_location() == onnx::TensorProto::EXTERNAL)
        externalDeep.push_back(tensor);
    }
  std::uint64_t localBudget = 0;
  auto* copiedBytes = materializedBudget != nullptr ? materializedBudget : &localBudget;
  inlineValidatedExternals(deep, bytes, control, copiedBytes);
  for (auto* tensor : externalDeep) {
    if (control != nullptr) checkActive(*control);
    const auto serialized = deterministicMessageVector(*tensor);
    normalizedOnnxInitializerPayload(serialized);
    if (control != nullptr) checkActive(*control);
  }
}

// Shape inference needs values only for a bounded set of operator parameters
// (for example Reshape shape, Slice bounds, and TopK K).  Keep the large
// weight initializers external and materialize only those named inputs in the
// inference copy.  This preserves external/inline shape parity without
// recreating the complete model-sized peak.
void materializeShapeInferenceInitializers(
  onnx::ModelProto& inferred,
  const onnx::ModelProto& sourceModel,
  const std::vector<std::uint8_t>* externalBytes,
  const NativeAssemblyControl& control,
  std::uint64_t* materializedBudget,
  const NativeCanonicalSource* externalSource)
{
  if (externalBytes == nullptr &&
      (externalSource == nullptr || !externalSource->initializerRangeSource)) return;
  struct ShapePolicyKey {
    std::string domain;
    std::string opType;
    std::int64_t opset = 0;
    bool operator<(const ShapePolicyKey& other) const
    {
      return std::tie(domain, opType, opset) <
        std::tie(other.domain, other.opType, other.opset);
    }
  };
  std::map<std::string, std::int64_t> opsets;
  for (const auto& opset : sourceModel.opset_import())
    opsets[opset.domain()] = opset.version();
  const auto opsetFor = [&] (const std::string& domain) {
    const auto found = opsets.find(domain);
    return found == opsets.end() ? std::int64_t{0} : found->second;
  };
  std::map<ShapePolicyKey, std::vector<int>> policies;
  const auto shapePolicy = [&] (const char* domain, const char* op,
                                std::initializer_list<int> indices) {
    policies[{domain, op, opsetFor(domain)}] = std::vector<int>(indices);
  };
  const auto shapeIndependent = [&] (const char* domain, const char* op) {
    shapePolicy(domain, op, {});
  };
  const auto shapeValue = [&] (const char* op, std::initializer_list<int> indices) {
    shapePolicy("", op, indices);
  };
  shapeValue("Reshape", {1}); shapeValue("Expand", {1});
  shapeValue("Gather", {1}); shapeValue("Tile", {1});
  shapeValue("OneHot", {1}); shapeValue("TopK", {1});
  shapeValue("Split", {1}); shapeValue("Squeeze", {1});
  shapeValue("Unsqueeze", {1}); shapeValue("ConstantOfShape", {0});
  shapeValue("Range", {0, 1, 2}); shapeValue("Slice", {1, 2, 3, 4});
  shapeValue("Resize", {1, 2, 3}); shapeValue("Pad", {1});
  shapeValue("CumSum", {1}); shapeValue("NonMaxSuppression", {2, 3, 4});
  shapeValue("Compress", {1}); shapeValue("If", {0}); shapeValue("Loop", {0, 1});
  for (const char* op : {"ReduceL1", "ReduceL2", "ReduceLogSum", "ReduceLogSumExp",
                         "ReduceMax", "ReduceMean", "ReduceMin", "ReduceProd",
                         "ReduceSum", "ReduceSumSquare"})
    shapeValue(op, {1});
  // Explicitly register the standard data-only operators used by Qwen and
  // common ONNX transformer exports. An unknown op/domain with an external
  // initializer input is rejected below instead of entering inference with
  // an unproven graph-only value.
  for (const char* op : {"Abs", "Add", "BatchNormalization", "Cast", "Clip", "Concat",
                         "Conv", "ConvTranspose", "DequantizeLinear", "Div", "Dropout",
                         "Einsum", "Equal", "Exp", "Flatten", "Gelu", "GemmaRotaryEmbedding",
                         "Gemm", "Greater", "GreaterOrEqual", "Identity", "LayerNormalization",
                         "Less", "LessOrEqual", "Log", "LogSoftmax", "MatMul", "MatMulInteger",
                         "Mul",
                         "Neg", "Pow", "QLinearConv", "QLinearMatMul", "QuantizeLinear",
                         "Relu", "RotaryEmbedding", "ScatterElements", "ScatterND", "Sigmoid",
                         "SimplifiedLayerNormalization", "Size", "Softmax", "Sqrt", "Sub",
                         "Tanh", "Transpose", "Where"})
    shapeIndependent("", op);
  for (const char* domain : {"com.microsoft", "ai.onnx.contrib"})
    for (const char* op : {"Attention", "FusedMatMul", "Gelu", "GroupQueryAttention",
                           "LayerNormalization", "RotaryEmbedding", "SkipLayerNormalization",
                           "SkipSimplifiedLayerNormalization"})
      shapeIndependent(domain, op);
  // MatMulNBits v1 derives its output extent from A and the N attribute,
  // not from packed weight, scale, or zero-point values.
  policies[{"com.microsoft", "MatMulNBits", 1}] = {};
  // CausalConvWithState v1 uses weight dimensions, not weight or bias values.
  policies[{"com.microsoft", "CausalConvWithState", 1}] = {};
  // ORT v1.31.1 bert_defs.cc: these v1 outputs inherit activation shapes;
  // gate/norm weights and rotary cache values do not determine output extents.
  for (const char* op : {"LinearAttentionGate", "GatedRMSNorm", "MRotaryEmbedding"})
    policies[{"com.microsoft", op, 1}] = {};
  std::unordered_set<std::string> externalInitializers;
  for (const auto& initializer : sourceModel.graph().initializer())
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL)
      externalInitializers.insert(initializer.name());
  std::unordered_set<std::string> required;
  std::function<void(const onnx::GraphProto&, bool)> scanGraph;
  std::function<void(const google::protobuf::RepeatedPtrField<onnx::NodeProto>&,
                     const std::unordered_set<std::string>&, bool)> scanNodes;
  const auto addInput = [&] (const onnx::NodeProto& node, int index,
                             const std::unordered_set<std::string>& localNames,
                             bool root) {
    if (index >= 0 && index < node.input_size() && !node.input(index).empty() &&
        externalInitializers.count(node.input(index)) != 0 &&
        (root || localNames.count(node.input(index)) == 0))
      required.insert(node.input(index));
  };
  scanNodes = [&] (const google::protobuf::RepeatedPtrField<onnx::NodeProto>& nodes,
                   const std::unordered_set<std::string>& localNames, bool root) {
    for (const auto& node : nodes) {
      const auto policy = policies.find({node.domain(), node.op_type(), opsetFor(node.domain())});
      bool hasExternalInput = false;
      for (const auto& input : node.input())
        hasExternalInput = hasExternalInput ||
          (externalInitializers.count(input) != 0 &&
           (root || localNames.count(input) == 0));
      if (policy == policies.end()) {
        if (hasExternalInput) fail("SHAPE_INPUT_UNSUPPORTED");
      }
      else {
        for (const auto index : policy->second) addInput(node, index, localNames, root);
      }
      for (const auto& attribute : node.attribute()) {
        if (attribute.has_g()) scanGraph(attribute.g(), false);
        for (const auto& graph : attribute.graphs()) scanGraph(graph, false);
      }
    }
  };
  scanGraph = [&] (const onnx::GraphProto& graph, bool root) {
    std::unordered_set<std::string> localNames;
    if (!root) {
      for (const auto& initializer : graph.initializer()) localNames.insert(initializer.name());
      for (const auto& input : graph.input()) localNames.insert(input.name());
      for (const auto& node : graph.node())
        for (const auto& output : node.output())
          if (!output.empty()) localNames.insert(output);
    }
    scanNodes(graph.node(), localNames, root);
  };
  scanGraph(sourceModel.graph(), true);
  for (const auto& function : sourceModel.functions()) {
    std::unordered_set<std::string> localNames;
    for (const auto& input : function.input()) localNames.insert(input);
    for (const auto& output : function.output()) localNames.insert(output);
    for (const auto& node : function.node())
      for (const auto& output : node.output())
        if (!output.empty()) localNames.insert(output);
    scanNodes(function.node(), localNames, false);
  }

  std::uint64_t localBudget = 0;
  auto* materializedBytes = materializedBudget != nullptr ? materializedBudget : &localBudget;
  for (int i = 0; i < sourceModel.graph().initializer_size(); ++i) {
    checkActive(control);
    const auto& sourceInitializer = sourceModel.graph().initializer(i);
    if (required.count(sourceInitializer.name()) == 0 ||
        sourceInitializer.data_location() != onnx::TensorProto::EXTERNAL)
      continue;
    auto material = materializeExternalTensor(sourceInitializer, externalBytes, &control,
                                               externalSource);
    *materializedBytes = checkedAdd(*materializedBytes,
                                   static_cast<std::uint64_t>(material.ByteSizeLong()));
    if (control.maxAssembledBytes == 0 || *materializedBytes > control.maxAssembledBytes)
      fail("MATERIAL_LIMIT");
    *inferred.mutable_graph()->mutable_initializer(i) = std::move(material);
  }
  checkActive(control);
}

// Materialize only the external initializers referenced by the selected role.
// The complete source graph remains external, so the worker does not create a
// second full-size raw_data copy before extraction.  The extracted model later
// takes ownership of these TensorProto values with Swap().
void materializeSelectedExternalInitializers(
  onnx::ModelProto& model,
  const std::vector<std::size_t>& selectedNodes,
  const std::vector<std::uint8_t>& externalBytes,
  const NativeAssemblyControl& control)
{
  std::unordered_set<std::string> required;
  const auto& graph = model.graph();
  for (const auto index : selectedNodes) {
    if (index >= static_cast<std::size_t>(graph.node_size()))
      fail("NODE_COVER");
    for (const auto& input : graph.node(static_cast<int>(index)).input())
      if (!input.empty())
        required.insert(input);
  }
  std::vector<onnx::TensorProto*> tensors;
  collectModelTensors(model, tensors);
  for (auto* tensor : tensors) {
    checkActive(control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL ||
        required.count(tensor->name()) == 0)
      continue;
    auto material = materializeExternalTensor(*tensor, &externalBytes, &control);
    tensor->Swap(&material);
  }
  checkActive(control);
}

// Shared OA05-for-identity entry: source limits, parse, external binding and
// memory-only inlining.  It deliberately does not full-check or shape-infer.
onnx::ModelProto ownedSourceModel(const NativeCanonicalSource& source,
                                  const NativeAssemblyControl& control,
                                  bool inlineExternal,
                                  std::uint64_t* materializedBudget,
                                  const NativeOnnxModelFileInput* modelFile)
{
  checkActive(control);
  if (!source.externalFileRanges.empty() &&
      (!source.initializerRangeSource || source.initializerBytes))
    fail("EXTERNAL_BINDING");
  const std::uint64_t sourceBytes = modelFile != nullptr
    ? modelFile->bytes : static_cast<std::uint64_t>(source.modelBytes.size());
  if (control.maxSourceBytes == 0 || sourceBytes == 0 ||
      sourceBytes > control.maxSourceBytes ||
      (modelFile == nullptr &&
       source.modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max())))
    fail("SOURCE_LIMIT");
  if (modelFile != nullptr && !source.modelBytes.empty())
    fail("SOURCE_OWNERSHIP");
  const auto initializerBytes = nativeCanonicalInitializerByteSize(source);
  const bool hasInitializer = source.initializerBytes || source.initializerRangeSource;
  if (hasInitializer &&
      (initializerBytes > control.maxSourceBytes ||
       (!source.initializerRangeSource && initializerBytes >
         static_cast<std::uint64_t>(std::numeric_limits<int>::max())) ||
       checkedAdd(sourceBytes, initializerBytes) >
         checkedAdd(control.maxSourceBytes, control.maxSourceBytes)))
    fail("INITIALIZER_LIMIT");

  onnx::ModelProto model;
  if (modelFile != nullptr) {
    (void) digestModelFile(*modelFile);
    google::protobuf::io::FileInputStream input(modelFile->fd);
    input.SetCloseOnDelete(false);
    if (!model.ParseFromZeroCopyStream(&input))
      fail("PARSE");
  }
  else if (!model.ParseFromArray(source.modelBytes.data(),
                                 static_cast<int>(source.modelBytes.size()))) {
    fail("PARSE");
  }
  std::vector<onnx::TensorProto*> externalTensors;
  collectModelTensors(model, externalTensors);
  const bool hasExternal = std::any_of(
    externalTensors.begin(), externalTensors.end(), [] (const onnx::TensorProto* tensor) {
      return tensor->data_location() == onnx::TensorProto::EXTERNAL;
    });
  if (hasExternal != hasInitializer) fail("EXTERNAL_BINDING");
  if (hasInitializer) {
    validateExternalLocations(externalTensors, source);
    if (source.initializerBytes && source.initializerRangeSource)
      (void)nativeCanonicalInitializerDigest(source, control);
    if (source.initializerRangeSource) {
      if (inlineExternal)
        inlineValidatedExternals(externalTensors, source, &control);
      else
        inlineDeepExternals(model, source, &control, materializedBudget);
    }
    else {
      if (inlineExternal)
        inlineValidatedExternals(externalTensors, *source.initializerBytes, &control);
      else
        inlineDeepExternals(model, *source.initializerBytes, &control, materializedBudget);
    }
  }
  checkActive(control);
  return model;
}

// --- python-identity-equivalent JSON composition -------------------------

std::string jsonEscape(const std::string& value)
{
  std::string out;
  out.reserve(value.size() + 2);
  for (const auto ch : value) {
    const auto byte = static_cast<unsigned char>(ch);
    switch (ch) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (byte < 0x20) {
          const char hex[] = "0123456789abcdef";
          out += "\\u00";
          out.push_back(hex[byte >> 4]);
          out.push_back(hex[byte & 0x0f]);
        } else {
          out.push_back(ch);  // UTF-8 bytes pass through untouched
        }
    }
  }
  return out;
}

void jsonString(std::string& out, const std::string& value)
{
  out.push_back('"');
  out += jsonEscape(value);
  out.push_back('"');
}

// Deterministic protobuf wire, lowercase hex, exactly like the python
// reference proto_hex helper.  onnx-ml.pb.h is generated for the lite
// runtime, so every onnx message derives MessageLite.
std::string protoHex(const google::protobuf::MessageLite& message)
{
  const auto size = message.ByteSizeLong();
  if (size > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) fail("SERIALIZE_LIMIT");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  google::protobuf::io::ArrayOutputStream array(bytes.data(), static_cast<int>(bytes.size()));
  google::protobuf::io::CodedOutputStream coded(&array);
  coded.SetSerializationDeterministic(true);
  if (!message.SerializeToCodedStream(&coded) || coded.HadError()) fail("SERIALIZE");
  static const char hex[] = "0123456789abcdef";
  std::string out;
  out.reserve(2 * bytes.size());
  for (const auto byte : bytes) {
    out.push_back(hex[byte >> 4]);
    out.push_back(hex[byte & 0x0f]);
  }
  return out;
}

void jsonHexBytes(std::string& out, const google::protobuf::MessageLite& message)
{
  jsonString(out, protoHex(message));
}

std::string sha256HexOf(const std::string& utf8)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(utf8.data()), utf8.size(), hash);
  std::string result = "sha256:";
  for (const auto byte : hash) {
    result += "0123456789abcdef"[byte >> 4];
    result += "0123456789abcdef"[byte & 0x0f];
  }
  return result;
}

struct TensorIndexEntry
{
  std::string name;
  std::string dtype;
  std::vector<std::int64_t> shape;
  std::string byteOrder;
  std::size_t byteLength = 0;
  std::string contentDigest;
  std::string sharedReference;
};

// Python reference layout: sorted by tensor name; contentDigest of the
// canonical bytes; sharedReference is the first (smallest) alias name when
// more than one tensor shares the digest, else empty.  Top-level external
// tensors stay graph-only in the model protobuf; a single temporary
// TensorProto receives only its authenticated range, so the index never
// retains a second complete initializer object. Deep attribute tensors have
// already been materialized because their protobuf wire contributes to the
// graph identity and template payload.
std::vector<TensorIndexEntry> buildTensorIndex(
  const onnx::ModelProto& model,
  const std::vector<std::uint8_t>* externalBytes = nullptr,
  const NativeAssemblyControl* control = nullptr,
  const NativeCanonicalSource* externalSource = nullptr)
{
  std::vector<TensorIndexEntry> entries;
  entries.reserve(static_cast<std::size_t>(model.graph().initializer_size()));
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    if (control != nullptr) checkActive(*control);
    const auto& initializer = model.graph().initializer(i);
    if (initializer.name().empty()) failNormalization("INITIALIZER_ENCODING_INVALID");
    TensorIndexEntry entry;
    entry.name = initializer.name();
    const auto* material = &initializer;
    std::optional<onnx::TensorProto> externalMaterial;
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL) {
      if (externalBytes == nullptr && externalSource == nullptr) fail("EXTERNAL_BINDING");
      // External numeric raw_data is already the canonical little-endian
      // payload.  Hash the authenticated range directly instead of creating
      // a temporary TensorProto, serializing it, parsing it again and copying
      // the raw bytes into a second normalization vector.  Nibble-packed
      // INT4/UINT4 remain on the materialized path because normalization
      // expands each nibble to one canonical byte.
      if (initializer.data_type() != onnx::TensorProto::INT4 &&
          initializer.data_type() != onnx::TensorProto::UINT4) {
        const auto count = tensorElementCount(initializer);
        const auto expected = rawByteLength(initializer.data_type(), count);
        const auto range = externalSource
          ? validatedExternalByteRange(initializer, *externalSource)
          : validatedExternalByteRange(initializer, *externalBytes);
        if (range.length != expected || expected > std::numeric_limits<std::size_t>::max())
          failNormalization("INITIALIZER_ENCODING_INVALID");
        entry.dtype = dtypeLabel(initializer.data_type());
        entry.shape.reserve(static_cast<std::size_t>(initializer.dims_size()));
        for (const auto dim : initializer.dims()) entry.shape.push_back(dim);
        entry.byteOrder = byteOrderOf(initializer.data_type());
        entry.byteLength = static_cast<std::size_t>(expected);
        if (externalSource != nullptr) {
          if (control == nullptr) fail("SOURCE_OWNERSHIP");
          entry.contentDigest = digestInitializerRange(*externalSource, range.offset, expected, *control);
        }
        else {
          entry.contentDigest = digest(externalBytes->data() + range.offset,
                                       static_cast<std::size_t>(expected));
        }
        entries.push_back(std::move(entry));
        if (control != nullptr) checkActive(*control);
        continue;
      }
      externalMaterial.emplace(materializeExternalTensor(initializer, externalBytes, control,
                                                          externalSource));
      material = &*externalMaterial;
    }
    std::vector<std::uint8_t> serialized(static_cast<std::size_t>(material->ByteSizeLong()));
    if (!serialized.empty() &&
        !material->SerializeToArray(serialized.data(), static_cast<int>(serialized.size())))
      fail("SERIALIZE");
    if (control != nullptr) checkActive(*control);
    auto normalized = normalizedOnnxInitializerPayload(serialized);
    if (control != nullptr) checkActive(*control);
    entry.dtype = std::move(normalized.dtype);
    entry.shape = std::move(normalized.shape);
    entry.byteOrder = std::move(normalized.byteOrder);
    entry.byteLength = normalized.content.size();
    entry.contentDigest = digest(normalized.content);
    entries.push_back(std::move(entry));
    if (control != nullptr) checkActive(*control);
  }
  std::stable_sort(entries.begin(), entries.end(),
            [] (const TensorIndexEntry& left, const TensorIndexEntry& right) {
              return left.name < right.name;
            });
  for (auto& entry : entries) {
    // alias resolution: gather same-digest names (index is name-sorted)
    std::vector<std::string> aliases;
    for (const auto& other : entries) {
      if (other.contentDigest == entry.contentDigest) aliases.push_back(other.name);
    }
    entry.sharedReference = aliases.size() > 1 ? aliases.front() : "";
  }
  return entries;
}

void appendShapeJson(std::string& out, const std::vector<std::int64_t>& shape)
{
  out.push_back('[');
  for (std::size_t i = 0; i < shape.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += std::to_string(shape[i]);
  }
  out.push_back(']');
}

void appendSortedHexList(std::string& out, const std::vector<std::string>& hexes)
{
  std::vector<std::string> sorted = hexes;
  std::sort(sorted.begin(), sorted.end());
  out.push_back('[');
  for (std::size_t i = 0; i < sorted.size(); ++i) {
    if (i != 0) out.push_back(',');
    jsonString(out, sorted[i]);
  }
  out.push_back(']');
}

std::string graphFactsJson(const onnx::ModelProto& model,
                           const std::vector<TensorIndexEntry>& entries)
{
  // python _sha256_canonical(graph_facts): compact JSON, keys sorted at
  // every level, UTF-8 preserved, no whitespace.  Object members are emitted
  // in alphabetical order below so the wire equals the sorted-key dump.
  std::string out;
  out.reserve(1024);
  out.push_back('{');

  // "functions"
  out += "\"functions\":";
  {
    std::vector<std::string> hexes;
    hexes.reserve(static_cast<std::size_t>(model.functions_size()));
    for (int f = 0; f < model.functions_size(); ++f)
      hexes.push_back(protoHex(model.functions(f)));
    appendSortedHexList(out, hexes);
  }

  // "initializerLayout"
  out += ",\"initializerLayout\":[";
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    const auto& entry = entries[i];
    out += "{\"byteLength\":";
    out += std::to_string(entry.byteLength);
    out += ",\"byteOrder\":";
    jsonString(out, entry.byteOrder);
    out += ",\"dtype\":";
    jsonString(out, entry.dtype);
    out += ",\"shape\":";
    appendShapeJson(out, entry.shape);
    out += ",\"sharedReference\":";
    jsonString(out, entry.sharedReference);
    out += ",\"tensorName\":";
    jsonString(out, entry.name);
    out.push_back('}');
  }
  out.push_back(']');

  // "irVersion"
  out += ",\"irVersion\":";
  out += std::to_string(static_cast<std::int64_t>(model.ir_version()));

  // "nodes"
  out += ",\"nodes\":[";
  for (int index = 0; index < model.graph().node_size(); ++index) {
    if (index != 0) out.push_back(',');
    const auto& node = model.graph().node(index);
    out += "{\"attributes\":[";
    {
      std::vector<const onnx::AttributeProto*> attributes;
      for (int a = 0; a < node.attribute_size(); ++a)
        attributes.push_back(&node.attribute(a));
      std::stable_sort(attributes.begin(), attributes.end(),
                [] (const onnx::AttributeProto* left, const onnx::AttributeProto* right) {
                  return left->name() < right->name();
                });
      for (std::size_t a = 0; a < attributes.size(); ++a) {
        if (a != 0) out.push_back(',');
        out += "{\"name\":";
        jsonString(out, attributes[a]->name());
        out += ",\"wire\":";
        jsonHexBytes(out, *attributes[a]);
        out.push_back('}');
      }
    }
    out += "],\"domain\":";
    jsonString(out, node.domain());
    out += ",\"index\":";
    out += std::to_string(static_cast<std::int64_t>(index));
    out += ",\"inputs\":[";
    for (int a = 0; a < node.input_size(); ++a) {
      if (a != 0) out.push_back(',');
      jsonString(out, node.input(a));
    }
    out += "],\"opType\":";
    jsonString(out, node.op_type());
    out += ",\"outputs\":[";
    for (int a = 0; a < node.output_size(); ++a) {
      if (a != 0) out.push_back(',');
      jsonString(out, node.output(a));
    }
    out += "]}";
  }
  out.push_back(']');

  // "opsets" sorted by (domain, version)
  out += ",\"opsets\":[";
  {
    std::vector<const onnx::OperatorSetIdProto*> opsets;
    for (int a = 0; a < model.opset_import_size(); ++a)
      opsets.push_back(&model.opset_import(a));
    std::stable_sort(opsets.begin(), opsets.end(),
              [] (const onnx::OperatorSetIdProto* left,
                  const onnx::OperatorSetIdProto* right) {
                if (left->domain() != right->domain()) return left->domain() < right->domain();
                return left->version() < right->version();
              });
    for (std::size_t a = 0; a < opsets.size(); ++a) {
      if (a != 0) out.push_back(',');
      out += "{\"domain\":";
      jsonString(out, opsets[a]->domain());
      out += ",\"version\":";
      out += std::to_string(static_cast<std::int64_t>(opsets[a]->version()));
      out.push_back('}');
    }
  }
  out.push_back(']');

  // "values"
  out += ",\"values\":{\"inputs\":[";
  for (int a = 0; a < model.graph().input_size(); ++a) {
    if (a != 0) out.push_back(',');
    jsonHexBytes(out, model.graph().input(a));
  }
  out += "],\"outputs\":[";
  for (int a = 0; a < model.graph().output_size(); ++a) {
    if (a != 0) out.push_back(',');
    jsonHexBytes(out, model.graph().output(a));
  }
  out += "],\"valueInfo\":";
  {
    std::vector<std::string> hexes;
    for (int v = 0; v < model.graph().value_info_size(); ++v)
      hexes.push_back(protoHex(model.graph().value_info(v)));
    appendSortedHexList(out, hexes);
  }
  out += "}}";
  return out;
}

std::string initializerContentJson(const std::vector<TensorIndexEntry>& entries)
{
  // ordered {tensorName, contentDigest} array; per-object keys sorted
  std::string out;
  out.push_back('[');
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (i != 0) out.push_back(',');
    out += "{\"contentDigest\":";
    jsonString(out, entries[i].contentDigest);
    out += ",\"tensorName\":";
    jsonString(out, entries[i].name);
    out.push_back('}');
  }
  out.push_back(']');
  return out;
}

std::uint32_t revisionOfInlinedModel(const onnx::ModelProto& model)
{
  // Classification over participating top-level initializers of the inlined
  // model only: STRING, BFLOAT16 raw (incl. external which inlines to raw),
  // and typed COMPLEX select revision 2; all other maintained numeric
  // representations stay on revision 1.
  std::uint32_t revision = 1;
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    const auto& initializer = model.graph().initializer(i);
    if (initializer.data_type() == onnx::TensorProto::STRING) return 2;
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL &&
        initializer.data_type() == onnx::TensorProto::BFLOAT16)
      return 2;
    if (initializer.data_type() == onnx::TensorProto::BFLOAT16 &&
        !initializer.raw_data().empty())
      return 2;
    if ((initializer.data_type() == onnx::TensorProto::COMPLEX64 ||
         initializer.data_type() == onnx::TensorProto::COMPLEX128) &&
        initializer.raw_data().empty() &&
        (initializer.float_data_size() != 0 || initializer.double_data_size() != 0))
      return 2;
  }
  return revision;
}

NativeJson materialReferenceJson(const NativeCanonicalSource::MaterialReference& reference)
{
  NativeJson dependencies = NativeJson::array();
  for (const auto& dependency : reference.dependencies)
    dependencies.push_back(dependency);
  NativeJson result{
    {"bytes", reference.bytes},
    {"dependencies", std::move(dependencies)},
    {"digest", reference.digest},
    {"kind", reference.kind},
    {"logicalName", reference.logicalName},
    {"nodeIndex", reference.nodeIndex},
    {"payloadId", reference.payloadId},
    {"sharedDigest", reference.sharedDigest}};
  // Keep the v1 canonical JSON byte-compatible for legacy single-payload
  // references.  The optional field is emitted only for chunked external or
  // large inline initializers, so parsing an old manifest does not change its digest.
  if (!reference.chunkPayloadIds.empty()) {
    NativeJson chunks = NativeJson::array();
    for (const auto& payloadId : reference.chunkPayloadIds)
      chunks.push_back(payloadId);
    result["chunkPayloadIds"] = std::move(chunks);
  }
  return result;
}

NativeJson
materialAutomaticSourceValue(const std::string& encoded)
{
  if (encoded.empty() || encoded.size() > NativeCanonicalMaterialPublicationMaxBytes)
    throw std::invalid_argument("native automatic material metadata exceeds its bound");
  auto value = nativeParseJson(encoded, 64);
  if (!value.is_object() || value.size() != 2 || !value.contains("contract") ||
      !value.at("contract").is_object() || !value.contains("externalFiles") ||
      !value.at("externalFiles").is_array() ||
      value.at("externalFiles").size() > NATIVE_ONNX_MAX_EXTERNAL_FILES ||
      nativeCanonicalJson(value) != encoded)
    throw std::invalid_argument("native automatic material metadata is invalid");
  // Complete inventory/source semantics belong to the admission owner. Keep
  // this structural material reader independently linkable in the worker.
  return value;
}

} // namespace

std::string NativeCanonicalSource::MaterialManifest::canonicalJson() const
{
  NativeJson references = NativeJson::array();
  for (const auto& reference : this->references)
    references.push_back(materialReferenceJson(reference));
  auto value = NativeJson{
    {"graphDigest", graphDigest},
    {"initializerDigest", initializerDigest},
    {"references", std::move(references)},
    {"schema", schema},
    {"sourceDigest", sourceDigest},
    {"templatePayloadId", templatePayloadId}};
  if (!automaticSourceJson.empty())
    value["automaticSource"] = materialAutomaticSourceValue(automaticSourceJson);
  auto encoded = nativeCanonicalJson(value);
  if (schema == "ndnsf-di-canonical-material-manifest-v2" &&
      encoded.size() > NativeCanonicalMaterialPublicationMaxBytes)
    throw std::invalid_argument("native canonical material metadata exceeds its bound");
  return encoded;
}

void NativeCanonicalSource::MaterialManifest::validate() const
{
  validate({});
}

void
NativeCanonicalSource::MaterialManifest::validate(
  const std::function<void()>& requireActive) const
{
  if (requireActive) requireActive();
  const auto validDigest = [] (const std::string& value) {
    return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
      std::all_of(value.begin() + 7, value.end(), [] (const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
      });
  };
  const bool legacy = schema == "ndnsf-di-canonical-material-manifest-v1";
  const bool automatic = schema == "ndnsf-di-canonical-material-manifest-v2";
  if ((!legacy && !automatic) || (legacy && !automaticSourceJson.empty()) ||
      (automatic && automaticSourceJson.empty()) ||
      !validDigest(sourceDigest) || !validDigest(graphDigest) ||
      !validDigest(initializerDigest) || !validDigest(manifestDigest) ||
      templatePayloadId.empty() || (payloadsComplete && payloads.empty()) ||
      nativePlanningDigest(canonicalJson()) != manifestDigest || references.empty() ||
      (payloadsComplete && payloads.empty()))
    throw std::invalid_argument("native canonical material manifest is incomplete");

  std::set<std::string> payloadIds;
  std::map<std::string, const MaterialPayload*> payloadById;
  for (const auto& payload : payloads) {
    if (requireActive) requireActive();
    const bool invalidRange = payload.rangeSource &&
      (payload.rangeSize == 0 || payload.rangeOffset > payload.rangeSource->size() ||
       payload.rangeSize > payload.rangeSource->size() - payload.rangeOffset);
    const bool invalidString = payload.stringBacking &&
      (payload.stringSize == 0 || payload.stringOffset > payload.stringBacking->size() ||
       payload.stringSize > payload.stringBacking->size() - payload.stringOffset);
    const unsigned backingKinds = static_cast<unsigned>(payload.backing != nullptr) +
      static_cast<unsigned>(payload.stringBacking != nullptr) +
      static_cast<unsigned>(payload.rangeSource != nullptr);
    if (payload.payloadId.empty() || !validDigest(payload.digest) || payload.empty() ||
        backingKinds > 1 || invalidRange || invalidString ||
        (payload.backing &&
         (payload.backingOffset > payload.backing->size() ||
          payload.backingSize > payload.backing->size() - payload.backingOffset)) ||
        payload.byteSize() > NativeCanonicalMaterialBundleMaxBytes ||
        payload.byteSize() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        !payloadIds.insert(payload.payloadId).second)
      throw std::invalid_argument("native canonical material payload is invalid");
    const auto payloadBytes = payload.copyBytes(requireActive);
    if (nativePlanningDigest(payloadBytes.data(), payloadBytes.size()) != payload.digest)
      throw std::invalid_argument("native canonical material payload digest differs");
    if (requireActive) requireActive();
    payloadById.emplace(payload.payloadId, &payload);
  }
  const auto templatePayload = payloadById.find(templatePayloadId);
  if (payloadsComplete && templatePayload == payloadById.end())
    throw std::invalid_argument("native canonical material template is missing");
  std::set<std::string> referenceIds;
  std::set<std::string> referencedPayloadIds;
  std::set<std::string> directReferencePayloadIds;
  for (const auto& reference : references) {
    if (requireActive) requireActive();
    directReferencePayloadIds.insert(reference.payloadId);
  }
  std::set<std::string> allChunkPayloadIds;
  for (const auto& reference : references) {
    if (requireActive) requireActive();
    const bool hasChunks = !reference.chunkPayloadIds.empty();
    if (reference.payloadId.empty() ||
        (payloadsComplete && !payloadIds.count(reference.payloadId)) ||
        reference.kind.empty() || reference.logicalName.empty() || !validDigest(reference.digest) ||
        reference.bytes == 0 ||
        (payloadIds.count(reference.payloadId) &&
         (reference.digest != payloadById.at(reference.payloadId)->digest ||
          reference.bytes != payloadById.at(reference.payloadId)->byteSize())) ||
        !referenceIds.insert(reference.logicalName + "\x1f" + reference.kind).second)
      throw std::invalid_argument("native canonical material reference is invalid");
    referencedPayloadIds.insert(reference.payloadId);
    std::set<std::string> chunkIds;
    for (const auto& chunkId : reference.chunkPayloadIds) {
      if (requireActive) requireActive();
      if (chunkId.empty() || chunkId == reference.payloadId || !chunkIds.insert(chunkId).second ||
          !allChunkPayloadIds.insert(chunkId).second ||
          directReferencePayloadIds.count(chunkId) != 0 ||
          (payloadsComplete && !payloadIds.count(chunkId)))
        throw std::invalid_argument("native canonical material initializer chunk is invalid");
      referencedPayloadIds.insert(chunkId);
    }
    if (reference.kind == "graph-template") {
      if (reference.payloadId != templatePayloadId || reference.logicalName != "__template__" ||
          reference.nodeIndex != 0 || !reference.dependencies.empty() || !reference.sharedDigest.empty() ||
          hasChunks)
        throw std::invalid_argument("native canonical material template reference is invalid");
    }
    else if (reference.kind == "graph-node") {
      if (reference.logicalName != "node/" + std::to_string(reference.nodeIndex) ||
          !reference.sharedDigest.empty() || hasChunks)
        throw std::invalid_argument("native canonical material node reference is invalid");
      for (const auto& dependency : reference.dependencies)
        if (dependency.empty())
          throw std::invalid_argument("native canonical material node dependency is invalid");
    }
    else if (reference.kind == "shared-initializer") {
      if (!validDigest(reference.sharedDigest) || !reference.dependencies.empty() || reference.nodeIndex != 0)
        throw std::invalid_argument("native canonical material initializer reference is invalid");
    }
    else {
      throw std::invalid_argument("native canonical material reference kind is unsupported");
    }
  }
  if (payloadsComplete && referencedPayloadIds.size() != payloadIds.size())
    throw std::invalid_argument("native canonical material payload is unreferenced");
  if (!payloadsComplete) {
    for (const auto& payload : payloads)
      if (!referencedPayloadIds.count(payload.payloadId))
        throw std::invalid_argument("native canonical selected payload is unreferenced");
  }
}

std::vector<std::string>
inspectNativeOnnxExternalLocations(const std::vector<std::uint8_t>& modelBytes,
                                    const NativeAssemblyControl& control)
{
  checkActive(control);
  if (modelBytes.empty() || control.maxSourceBytes == 0 ||
      modelBytes.size() > control.maxSourceBytes ||
      modelBytes.size() > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
    fail("SOURCE_LIMIT");
  onnx::ModelProto model;
  if (!model.ParseFromArray(modelBytes.data(), static_cast<int>(modelBytes.size())))
    fail("PARSE");
  checkActive(control);
  std::vector<onnx::TensorProto*> tensors;
  collectModelTensors(model, tensors, true);
  std::set<std::string> locations;
  for (auto* tensor : tensors) {
    checkActive(control);
    if (tensor->data_location() != onnx::TensorProto::EXTERNAL) continue;
    locations.insert(validateAndPinLocation({tensor}));
    if (locations.size() > NATIVE_ONNX_MAX_EXTERNAL_FILES) fail("EXTERNAL_BINDING");
  }
  return {locations.begin(), locations.end()};
}

std::uint64_t
nativeCanonicalInitializerByteSize(const NativeCanonicalSource& source)
{
  const auto bytes = source.initializerBytes
    ? static_cast<std::uint64_t>(source.initializerBytes->size()) : 0;
  if (source.initializerBytes && bytes == 0) fail("INITIALIZER_LIMIT");
  if (!source.initializerRangeSource) return bytes;
  const auto rangeBytes = source.initializerRangeSource->size();
  if (rangeBytes == 0 || (source.initializerBytes && bytes != rangeBytes))
    fail("INITIALIZER_RANGE");
  return rangeBytes;
}

std::vector<std::uint8_t>
readNativeCanonicalInitializerRange(const NativeCanonicalSource& source,
                                    std::uint64_t offset, std::uint64_t length,
                                    const NativeAssemblyControl& control)
{
  checkActive(control);
  const auto size = nativeCanonicalInitializerByteSize(source);
  if ((!source.initializerBytes && !source.initializerRangeSource) ||
      control.maxSourceBytes == 0 || size > control.maxSourceBytes ||
      offset > size || length > size - offset ||
      length > NativeCanonicalMaterialBundleMaxBytes)
    fail("INITIALIZER_RANGE");
  if (length == 0) return {};
  std::vector<std::uint8_t> bytes;
  if (source.initializerRangeSource) {
    bytes = source.initializerRangeSource->read(offset, length);
    if (bytes.size() != length) fail("INITIALIZER_RANGE");
    if (source.initializerBytes && !std::equal(bytes.begin(), bytes.end(),
        source.initializerBytes->data() + static_cast<std::size_t>(offset)))
      fail("INITIALIZER_RANGE");
  }
  else {
    const auto* first = source.initializerBytes->data() + static_cast<std::size_t>(offset);
    bytes.assign(first, first + static_cast<std::size_t>(length));
  }
  checkActive(control);
  return bytes;
}

std::string
nativeCanonicalInitializerDigest(const NativeCanonicalSource& source,
                                 const NativeAssemblyControl& control)
{
  if (!source.initializerBytes && !source.initializerRangeSource) {
    checkActive(control);
    return {};
  }
  checkActive(control);
  const auto bytes = nativeCanonicalInitializerByteSize(source);
  if (control.maxSourceBytes == 0 || bytes > control.maxSourceBytes)
    fail("INITIALIZER_RANGE");
  // Generic and dual representations still receive complete byte verification.
  const auto file = source.initializerBytes ? nullptr :
    std::dynamic_pointer_cast<const InitializerFileRangeSource>(source.initializerRangeSource);
  if (file) {
    if (const auto digest = file->snapshotDigest()) {
      checkActive(control);
      return *digest;
    }
  }
  const auto digest = digestInitializerRange(source, 0, bytes, control);
  if (file) file->rememberSnapshotDigest(digest);
  return digest;
}

static std::shared_ptr<const NativeCanonicalByteRangeSource>
openInitializerFile(const std::string& path, std::uint64_t bytes,
                    const std::string& expectedDigest, const NativeAssemblyControl& control,
                    bool consumeTemporary)
{
  checkActive(control);
  if (bytes == 0 || control.maxSourceBytes == 0 || bytes > control.maxSourceBytes ||
      expectedDigest.size() != 71 || expectedDigest.compare(0, 7, "sha256:") != 0 ||
      !std::all_of(expectedDigest.begin() + 7, expectedDigest.end(), [] (char value) {
        return std::string_view("0123456789abcdef").find(value) != std::string_view::npos;
      }))
    fail("INITIALIZER_FILE");
  auto owner = std::make_shared<InitializerFileRangeSource>(path, bytes);
  if (consumeTemporary) owner->consumeTemporaryPath(path);
  NativeCanonicalSource source;
  source.initializerRangeSource = owner;
  if (nativeCanonicalInitializerDigest(source, control) != expectedDigest)
    fail("INITIALIZER_DIGEST");
  return owner;
}

std::shared_ptr<const NativeCanonicalByteRangeSource>
openNativeCanonicalInitializerFile(const std::string& path, std::uint64_t bytes,
                                   const std::string& expectedDigest,
                                   const NativeAssemblyControl& control)
{
  return openInitializerFile(path, bytes, expectedDigest, control, false);
}

std::shared_ptr<const NativeCanonicalByteRangeSource>
takeNativeCanonicalInitializerFile(const std::string& path, std::uint64_t bytes,
  const std::string& expectedDigest, const NativeAssemblyControl& control)
{
  return openInitializerFile(path, bytes, expectedDigest, control, true);
}

std::shared_ptr<const NativeCanonicalSource::MaterialManifest>
deriveNativeCanonicalMaterialManifest(const NativeCanonicalSource& source,
                                      const NativeAssemblyControl& control)
{
  return deriveNativeCanonicalMaterialManifest(source, control, {});
}

std::shared_ptr<const NativeCanonicalSource::MaterialManifest>
deriveNativeCanonicalMaterialManifest(const NativeCanonicalSource& source,
                                      const NativeAssemblyControl& control,
                                      const std::string& automaticSourceJson)
{
  checkActive(control);
  if (!automaticSourceJson.empty())
    (void)materialAutomaticSourceValue(automaticSourceJson);
  auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr,
    &control, &source);
  auto result = std::make_shared<NativeCanonicalSource::MaterialManifest>();
  if (!automaticSourceJson.empty()) {
    result->schema = "ndnsf-di-canonical-material-manifest-v2";
    result->automaticSourceJson = automaticSourceJson;
  }
  std::uint64_t materialBytes = 0;
  const auto accountMaterial = [&] (std::size_t bytes) {
    materialBytes = checkedAdd(materialBytes, static_cast<std::uint64_t>(bytes));
    if (control.maxAssembledBytes == 0 || materialBytes > control.maxAssembledBytes)
      fail("MATERIAL_LIMIT");
  };
  result->sourceDigest = nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size());
  result->graphDigest = sha256HexOf(graphFactsJson(model, entries));
  result->initializerDigest = sha256HexOf(initializerContentJson(entries));

  auto templateModel = model;
  templateModel.mutable_graph()->clear_node();
  templateModel.mutable_graph()->clear_initializer();
  templateModel.mutable_graph()->clear_sparse_initializer();
  templateModel.mutable_graph()->clear_quantization_annotation();
  const auto templateBytes = deterministicMessageVector(templateModel);
  accountMaterial(templateBytes.size());
  NativeCanonicalSource::MaterialPayload templatePayload{
    "graph-template", digest(templateBytes), templateBytes};
  result->templatePayloadId = templatePayload.payloadId;
  result->payloads.push_back(std::move(templatePayload));
  result->references.push_back({
    "graph-template", {}, "graph-template", "__template__", 0,
    result->payloads.back().digest, result->payloads.back().bytes.size(), {}, {}});

  std::set<std::string> initializerNames;
  std::map<std::string, std::string> initializerPayloadByDigest;
  for (int i = 0; i < model.graph().initializer_size(); ++i) {
    checkActive(control);
    auto& initializer = *model.mutable_graph()->mutable_initializer(i);
    initializerNames.insert(initializer.name());
    const auto normalized = std::find_if(entries.begin(), entries.end(),
      [&initializer] (const auto& item) { return item.name == initializer.name(); });
    if (normalized == entries.end())
      failNormalization("INITIALIZER_ENCODING_INVALID");
    if (initializer.data_location() == onnx::TensorProto::EXTERNAL) {
      // Do not serialize a 300+ MiB TensorProto just to publish one weight.
      // Keep a small metadata header and address the authenticated raw range
      // as bounded chunks. Providers stream only their selected chunks to a
      // role sidecar; the legacy vector materializer remains compatible.
      if (!source.initializerBytes && !source.initializerRangeSource)
        fail("EXTERNAL_BINDING");
      auto header = initializer;
      header.clear_raw_data();
      header.clear_float_data();
      header.clear_int32_data();
      header.clear_string_data();
      header.clear_int64_data();
      header.clear_double_data();
      header.clear_uint64_data();
      header.clear_external_data();
      header.set_data_location(onnx::TensorProto::DEFAULT);
      const auto headerBytes = deterministicMessageVector(header);
      const auto headerDigest = digest(headerBytes);
      const auto headerPayloadId = std::string("initializer-header-") + headerDigest.substr(7);
      accountMaterial(headerBytes.size());
      result->payloads.push_back({headerPayloadId, headerDigest, headerBytes});

      const auto range = validatedExternalByteRange(initializer, source);
      const auto offset = range.offset;
      const auto length = range.length;

      std::vector<std::string> chunkPayloadIds;
      std::uint64_t consumed = 0;
      std::uint64_t chunkIndex = 0;
      while (consumed < length) {
        checkActive(control);
        const auto chunkBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
          NativeCanonicalMaterialBundleMaxBytes, length - consumed));
        const auto chunkOffset = offset + consumed;
        const auto chunkDigest = source.initializerRangeSource
          ? digest(readNativeCanonicalInitializerRange(source, chunkOffset, chunkBytes, control))
          : digest(source.initializerBytes->data() + static_cast<std::size_t>(chunkOffset),
                   chunkBytes);
        const auto chunkPayloadId = headerPayloadId + "-chunk-" + std::to_string(chunkIndex++);
        accountMaterial(chunkBytes);
        NativeCanonicalSource::MaterialPayload chunk;
        chunk.payloadId = chunkPayloadId;
        chunk.digest = chunkDigest;
        if (source.initializerRangeSource) {
          chunk.rangeSource = source.initializerRangeSource;
          chunk.rangeOffset = chunkOffset;
          chunk.rangeSize = chunkBytes;
        }
        else {
          chunk.backing = source.initializerBytes->shared();
          chunk.backingOffset = static_cast<std::size_t>(chunkOffset);
          chunk.backingSize = chunkBytes;
        }
        result->payloads.push_back(std::move(chunk));
        chunkPayloadIds.push_back(chunkPayloadId);
        consumed += chunkBytes;
      }
      result->references.push_back({
        headerPayloadId, std::move(chunkPayloadIds), "shared-initializer", initializer.name(), 0,
        headerDigest, headerBytes.size(), {}, normalized->contentDigest});
    }
    else if (initializer.raw_data().size() > NativeCanonicalMaterialBundleMaxBytes ||
             static_cast<std::uint64_t>(initializer.ByteSizeLong()) >
               NativeCanonicalMaterialBundleMaxBytes) {
      // Inline quantized ONNX commonly stores a large raw_data field directly
      // in the model protobuf. Use the serialized TensorProto size as well as
      // raw_data: a raw field exactly at the bundle limit can still exceed the
      // limit after protobuf metadata is included. Move that allocation into
      // shared string backing, emit a small authenticated TensorProto header,
      // and publish only bounded views. This keeps the inline source contract
      // equivalent to external initializer chunking without creating a second
      // full byte vector.
      auto rawBacking = std::make_shared<std::string>(std::move(*initializer.mutable_raw_data()));
      auto header = initializer;
      header.clear_raw_data();
      header.clear_float_data();
      header.clear_int32_data();
      header.clear_string_data();
      header.clear_int64_data();
      header.clear_double_data();
      header.clear_uint64_data();
      header.clear_external_data();
      header.set_data_location(onnx::TensorProto::DEFAULT);
      const auto headerBytes = deterministicMessageVector(header);
      const auto headerDigest = digest(headerBytes);
      const auto headerPayloadId = std::string("initializer-header-") + headerDigest.substr(7);
      accountMaterial(headerBytes.size());
      result->payloads.push_back({headerPayloadId, headerDigest, headerBytes});

      std::vector<std::string> chunkPayloadIds;
      std::uint64_t consumed = 0;
      std::uint64_t chunkIndex = 0;
      while (consumed < rawBacking->size()) {
        checkActive(control);
        const auto chunkBytes = static_cast<std::size_t>(std::min<std::uint64_t>(
          NativeCanonicalMaterialBundleMaxBytes,
          static_cast<std::uint64_t>(rawBacking->size()) - consumed));
        const auto chunkOffset = static_cast<std::size_t>(consumed);
        const auto chunkDigest = digest(
          reinterpret_cast<const std::uint8_t*>(rawBacking->data()) + chunkOffset, chunkBytes);
        const auto chunkPayloadId = headerPayloadId + "-chunk-" + std::to_string(chunkIndex++);
        accountMaterial(chunkBytes);
        NativeCanonicalSource::MaterialPayload chunk;
        chunk.payloadId = chunkPayloadId;
        chunk.digest = chunkDigest;
        chunk.stringBacking = rawBacking;
        chunk.stringOffset = chunkOffset;
        chunk.stringSize = chunkBytes;
        result->payloads.push_back(std::move(chunk));
        chunkPayloadIds.push_back(chunkPayloadId);
        consumed += chunkBytes;
      }
      result->references.push_back({
        headerPayloadId, std::move(chunkPayloadIds), "shared-initializer", initializer.name(), 0,
        headerDigest, headerBytes.size(), {}, normalized->contentDigest});
    }
    else {
      const auto bytes = deterministicMessageVector(initializer);
      const auto payloadDigest = digest(bytes);
      auto payloadId = std::string("initializer-") + payloadDigest.substr(7);
      const auto existing = initializerPayloadByDigest.find(payloadDigest);
      if (existing == initializerPayloadByDigest.end()) {
        initializerPayloadByDigest.emplace(payloadDigest, payloadId);
        accountMaterial(bytes.size());
        result->payloads.push_back({payloadId, payloadDigest, bytes});
      }
      else {
        payloadId = existing->second;
      }
      result->references.push_back({
        payloadId, {}, "shared-initializer", initializer.name(), 0, payloadDigest,
        bytes.size(), {}, normalized->contentDigest});
    }
  }

  for (int i = 0; i < model.graph().node_size(); ++i) {
    checkActive(control);
    const auto& node = model.graph().node(i);
    const auto bytes = deterministicMessageVector(node);
    const auto payloadDigest = digest(bytes);
    accountMaterial(bytes.size());
    const auto payloadId = "node-" + std::to_string(i);
    result->payloads.push_back({payloadId, payloadDigest, bytes});
    std::vector<std::string> dependencies;
    for (const auto& input : node.input()) {
      if (initializerNames.count(input) != 0)
        dependencies.push_back(input);
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(std::unique(dependencies.begin(), dependencies.end()), dependencies.end());
    result->references.push_back({
      payloadId, {}, "graph-node", "node/" + std::to_string(i),
      static_cast<std::uint64_t>(i), payloadDigest, bytes.size(), dependencies, {}});
    if ((i & 0x3f) == 0) control.requireActive();
  }
  control.requireActive();
  const auto materialManifestJson = result->canonicalJson();
  accountMaterial(materialManifestJson.size());
  result->manifestDigest = nativePlanningDigest(result->canonicalJson());
  result->validate([&] { checkActive(control); });
  return result;
}

std::shared_ptr<NativeCanonicalSource::MaterialManifest>
parseNativeCanonicalMaterialManifest(const std::vector<std::uint8_t>& bytes)
{
  if (bytes.empty() || bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
    throw std::invalid_argument("native canonical material manifest bytes are invalid");
  const auto root = nativeParseJson(std::string(bytes.begin(), bytes.end()), 64);
  if (!root.is_object())
    throw std::invalid_argument("native canonical material manifest is not an object");
  auto result = std::make_shared<NativeCanonicalSource::MaterialManifest>();
  result->schema = root.value("schema", std::string{});
  if (result->schema == "ndnsf-di-canonical-material-manifest-v2" &&
      bytes.size() > NativeCanonicalMaterialPublicationMaxBytes)
    throw std::invalid_argument("native automatic material metadata exceeds its bound");
  if (root.contains("automaticSource"))
    result->automaticSourceJson = nativeCanonicalJson(root.at("automaticSource"));
  result->sourceDigest = root.value("sourceDigest", std::string{});
  result->graphDigest = root.value("graphDigest", std::string{});
  result->initializerDigest = root.value("initializerDigest", std::string{});
  result->manifestDigest = digest(bytes);
  if (!root.contains("templatePayloadId") || !root.at("templatePayloadId").is_string())
    throw std::invalid_argument("native canonical material template is missing");
  result->templatePayloadId = root.at("templatePayloadId").get<std::string>();
  const auto references = root.value("references", NativeJson::array());
  if (!references.is_array())
    throw std::invalid_argument("native canonical material references are invalid");
  for (const auto& item : references) {
    if (!item.is_object())
      throw std::invalid_argument("native canonical material reference is invalid");
    NativeCanonicalSource::MaterialReference reference;
    reference.payloadId = item.value("payloadId", std::string{});
    const auto chunks = item.value("chunkPayloadIds", NativeJson::array());
    if (!chunks.is_array())
      throw std::invalid_argument("native canonical material initializer chunks are invalid");
    for (const auto& chunk : chunks) {
      if (!chunk.is_string())
        throw std::invalid_argument("native canonical material initializer chunk is invalid");
      reference.chunkPayloadIds.push_back(chunk.get<std::string>());
    }
    reference.kind = item.value("kind", std::string{});
    reference.logicalName = item.value("logicalName", std::string{});
    reference.nodeIndex = item.value("nodeIndex", std::uint64_t{0});
    reference.digest = item.value("digest", std::string{});
    reference.bytes = item.value("bytes", std::uint64_t{0});
    const auto dependencies = item.value("dependencies", NativeJson::array());
    if (!dependencies.is_array())
      throw std::invalid_argument("native canonical material dependencies are invalid");
    for (const auto& dependency : dependencies)
      if (!dependency.is_string())
        throw std::invalid_argument("native canonical material dependency is invalid");
      else
        reference.dependencies.push_back(dependency.get<std::string>());
    reference.sharedDigest = item.value("sharedDigest", std::string{});
    result->references.push_back(std::move(reference));
  }
  // The reference index is authenticated by manifestDigest.  Payload bytes
  // are deliberately supplied separately by the post-Selection reader.
  result->payloadsComplete = false;
  result->validate();
  return result;
}

namespace {
std::vector<std::uint8_t>
materializeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedInputs,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedOutputs,
                                const NativeAssemblyControl& control,
                                NativeCanonicalMaterializedFiles* files,
                                const std::function<void(const std::vector<std::uint8_t>&)>& append)
{
  checkActive(control);
  const auto checkMaterialActive = [&] { checkActive(control); };
  if (!source.materialManifest || nodeIndices.empty())
    fail("MATERIAL_SELECTION");
  source.materialManifest->validate(checkMaterialActive);
  std::map<std::string, NativeCanonicalSource::MaterialPayload*> payloads;
  for (auto& payload : source.materialPayloads) {
    const auto payloadBytes = payload.copyBytes(checkMaterialActive);
    if (payload.payloadId.empty() || payload.empty() ||
        digest(payloadBytes.data(), payloadBytes.size()) != payload.digest ||
        payloads.count(payload.payloadId) != 0)
      fail("MATERIAL_PAYLOAD");
    checkMaterialActive();
    payloads.emplace(payload.payloadId, &payload);
  }
  const auto referencesForPayload = [&] (const std::string& payloadId) {
    std::vector<const NativeCanonicalSource::MaterialReference*> matches;
    for (const auto& reference : source.materialManifest->references) {
      if (reference.payloadId == payloadId ||
          std::find(reference.chunkPayloadIds.begin(), reference.chunkPayloadIds.end(),
                    payloadId) != reference.chunkPayloadIds.end())
        matches.push_back(&reference);
    }
    return matches;
  };
  for (const auto& item : payloads) {
    const auto matches = referencesForPayload(item.first);
    if (matches.empty())
      fail("MATERIAL_PAYLOAD");
    for (const auto* match : matches) {
      const bool chunkPayload = match->payloadId != item.first;
      if ((!chunkPayload && (match->digest != item.second->digest ||
                             match->bytes != item.second->byteSize())) ||
          (chunkPayload && item.second->byteSize() > NativeCanonicalMaterialBundleMaxBytes))
        fail("MATERIAL_PAYLOAD");
    }
  }
  std::uint64_t retainedMaterialBytes = source.materialManifest->canonicalJson().size();
  for (const auto& payload : source.materialPayloads)
    retainedMaterialBytes = checkedAdd(retainedMaterialBytes, payload.byteSize());
  const auto findReference = [&] (const std::string& kind,
                                  const std::string& logicalName)
    -> const NativeCanonicalSource::MaterialReference* {
    for (const auto& reference : source.materialManifest->references)
      if (reference.kind == kind && reference.logicalName == logicalName)
        return &reference;
    return nullptr;
  };
  const auto templateReference = findReference("graph-template", "__template__");
  if (templateReference == nullptr)
    fail("MATERIAL_TEMPLATE");
  const auto templatePayload = payloads.find(templateReference->payloadId);
  if (templatePayload == payloads.end())
    fail("MATERIAL_TEMPLATE");
  const auto templateBytes = templatePayload->second->copyBytes(checkMaterialActive);
  onnx::ModelProto model;
  if (!model.ParseFromArray(templateBytes.data(),
                            static_cast<int>(templateBytes.size())) ||
      !model.has_graph() || model.graph().node_size() != 0 ||
      model.graph().initializer_size() != 0)
    fail("MATERIAL_TEMPLATE");

  // The legacy Qwen exporter created each stage with explicit role I/O.  A
  // canonical graph only contains the full-model boundary, so merely copying
  // its template leaves an internal handoff (for example
  // `hidden_states_out`) absent from graph.output and makes the subsequent
  // native extractor reject an otherwise valid selected node set.  Rebuild
  // the role boundary from the authenticated preparation contracts before
  // adding nodes.  The contracts carry the same dtype/shape information that
  // the old exporter wrote into its stage ONNX files.
  if (!expectedInputs.empty() || !expectedOutputs.empty()) {
    if (expectedInputs.empty() || expectedOutputs.empty())
      fail("MATERIAL_BOUNDARY");
    std::set<std::string> inputNames;
    std::set<std::string> outputNames;
    validateContracts(expectedInputs, inputNames);
    validateContracts(expectedOutputs, outputNames);
    const auto dtypeCode = [] (const std::string& value) -> std::int32_t {
      static const std::map<std::string, std::int32_t> labels{
        {"float32", onnx::TensorProto::FLOAT},
        {"uint8", onnx::TensorProto::UINT8},
        {"int8", onnx::TensorProto::INT8},
        {"uint16", onnx::TensorProto::UINT16},
        {"int16", onnx::TensorProto::INT16},
        {"int32", onnx::TensorProto::INT32},
        {"int64", onnx::TensorProto::INT64},
        {"string", onnx::TensorProto::STRING},
        {"bool", onnx::TensorProto::BOOL},
        {"float16", onnx::TensorProto::FLOAT16},
        {"float64", onnx::TensorProto::DOUBLE},
        {"uint32", onnx::TensorProto::UINT32},
        {"uint64", onnx::TensorProto::UINT64},
        {"complex64", onnx::TensorProto::COMPLEX64},
        {"complex128", onnx::TensorProto::COMPLEX128},
        {"bfloat16", onnx::TensorProto::BFLOAT16}};
      const auto named = labels.find(value);
      if (named != labels.end()) return named->second;
      if (value.empty() || !std::all_of(value.begin(), value.end(), [] (unsigned char c) {
            return std::isdigit(c) != 0;
          }))
        fail("MATERIAL_BOUNDARY");
      try {
        const auto parsed = std::stoll(value);
        if (parsed < 0 || parsed > std::numeric_limits<std::int32_t>::max())
          fail("MATERIAL_BOUNDARY");
        return static_cast<std::int32_t>(parsed);
      }
      catch (const std::exception&) {
        fail("MATERIAL_BOUNDARY");
      }
    };
    const auto valueInfo = [&] (const NativeAssemblyTensorContractV3& contract) {
      onnx::ValueInfoProto value;
      value.set_name(contract.name);
      auto* tensor = value.mutable_type()->mutable_tensor_type();
      tensor->set_elem_type(dtypeCode(contract.dtype));
      // An empty contract shape records an authenticated source tensor whose
      // metadata carried no rank (ONNX value_info without a shape field).  The
      // wire contract cannot distinguish that from a rank-0 scalar, so leave
      // the shape field absent: an empty TensorShapeProto would assert a
      // scalar boundary type, and ONNX consumers such as Reshape reject a
      // rank-0 shape input ("shape input must be 1D tensor").  Unknown rank
      // keeps the role boundary consistent with compareBoundaryContracts(),
      // which compares the dimension count only.
      if (!contract.shape.empty()) {
        auto* shape = tensor->mutable_shape();
        for (const auto& dimension : contract.shape) {
          auto* dim = shape->add_dim();
          if (std::holds_alternative<std::int64_t>(dimension)) {
            const auto number = std::get<std::int64_t>(dimension);
            if (number < 0) fail("MATERIAL_BOUNDARY");
            dim->set_dim_value(number);
          }
          else {
            const auto& symbol = std::get<std::string>(dimension);
            if (symbol.empty()) fail("MATERIAL_BOUNDARY");
            dim->set_dim_param(symbol);
          }
        }
      }
      return value;
    };
    model.mutable_graph()->clear_input();
    model.mutable_graph()->clear_output();
    for (const auto& contract : expectedInputs)
      *model.mutable_graph()->add_input() = valueInfo(contract);
    for (const auto& contract : expectedOutputs)
      *model.mutable_graph()->add_output() = valueInfo(contract);
  }

  std::set<std::uint64_t> seenNodes;
  std::set<std::string> dependencies;
  std::optional<std::uint64_t> previousNodeIndex;
  for (const auto nodeIndex : nodeIndices) {
    checkActive(control);
    if (previousNodeIndex && nodeIndex <= *previousNodeIndex)
      fail("MATERIAL_SELECTION");
    previousNodeIndex = nodeIndex;
    if (!seenNodes.insert(nodeIndex).second)
      fail("MATERIAL_SELECTION");
    const auto* reference = findReference("graph-node", "node/" + std::to_string(nodeIndex));
    if (reference == nullptr)
      fail("MATERIAL_NODE");
    const auto payload = payloads.find(reference->payloadId);
    if (payload == payloads.end())
      fail("MATERIAL_NODE");
    const auto nodeBytes = payload->second->copyBytes(checkMaterialActive);
    onnx::NodeProto node;
    if (!node.ParseFromArray(nodeBytes.data(), static_cast<int>(nodeBytes.size())))
      fail("MATERIAL_NODE");
    *model.mutable_graph()->add_node() = std::move(node);
    dependencies.insert(reference->dependencies.begin(), reference->dependencies.end());
  }
  for (const auto& dependency : dependencies) {
    const auto* match = findReference("shared-initializer", dependency);
    if (match == nullptr || payloads.find(match->payloadId) == payloads.end())
      fail("MATERIAL_INITIALIZER");
  }
  // A selected initializer may be backed by many authenticated bundle
  // objects.  Keep only the objects that still have a reference consumer:
  // once the final header/chunk has been copied into TensorProto, release its
  // backing immediately instead of retaining the whole bundle set until the
  // role model is complete.  The count also preserves correctness if a
  // manifest legitimately reuses one payload in more than one initializer.
  std::map<std::string, std::size_t> remainingPayloadUses;
  for (const auto& reference : source.materialManifest->references) {
    if (reference.kind != "shared-initializer" ||
        dependencies.count(reference.logicalName) == 0)
      continue;
    ++remainingPayloadUses[reference.payloadId];
    for (const auto& chunkPayloadId : reference.chunkPayloadIds)
      ++remainingPayloadUses[chunkPayloadId];
  }
  const auto releaseMaterialPayload = [&] (const std::string& payloadId) {
    const auto remaining = remainingPayloadUses.find(payloadId);
    if (remaining == remainingPayloadUses.end() || remaining->second == 0)
      return;
    if (--remaining->second == 0) {
      const auto payload = payloads.find(payloadId);
      if (payload != payloads.end())
        payload->second->release();
    }
  };
  ndn::util::Sha256 sidecarHash;
  const auto finishHash = [] (ndn::util::Sha256& hash) {
    auto value = hash.toString();
    std::transform(value.begin(), value.end(), value.begin(), [] (unsigned char ch) {
      return static_cast<char>(std::tolower(ch));
    });
    return "sha256:" + value;
  };
  const auto materializeInitializer = [&] (const auto& reference) {
    const auto header = payloads.find(reference.payloadId);
    if (header == payloads.end())
      fail("MATERIAL_INITIALIZER");
    const auto headerBytes = header->second->copyBytes(checkMaterialActive);
    if (reference.chunkPayloadIds.empty()) {
      onnx::TensorProto initializer;
      if (!initializer.ParseFromArray(headerBytes.data(),
                                     static_cast<int>(headerBytes.size())))
        fail("MATERIAL_INITIALIZER");
      if (reference.digest != header->second->digest || reference.bytes != headerBytes.size())
        fail("MATERIAL_INITIALIZER");
      releaseMaterialPayload(reference.payloadId);
      return initializer;
    }
    onnx::TensorProto initializer;
    if (!initializer.ParseFromArray(headerBytes.data(),
                                   static_cast<int>(headerBytes.size())) ||
        !initializer.raw_data().empty() || initializer.data_location() == onnx::TensorProto::EXTERNAL)
      fail("MATERIAL_INITIALIZER");
    std::uint64_t rawBytes = 0;
    std::vector<std::uint8_t> raw;
    for (const auto& chunkId : reference.chunkPayloadIds) {
      checkActive(control);
      const auto chunk = payloads.find(chunkId);
      if (chunk == payloads.end() ||
          chunk->second->byteSize() > NativeCanonicalMaterialBundleMaxBytes) {
        fail("MATERIAL_INITIALIZER");
      }
      rawBytes = checkedAdd(rawBytes, chunk->second->byteSize());
    }
    if (files != nullptr && initializer.data_type() != onnx::TensorProto::INT4 &&
        initializer.data_type() != onnx::TensorProto::UINT4 &&
        initializer.data_type() != onnx::TensorProto::STRING) {
      if (!append || rawBytes == 0 || control.maxAssembledBytes == 0 ||
          rawBytes != rawByteLength(initializer.data_type(), tensorElementCount(initializer)) ||
          files->initializerBytes > control.maxAssembledBytes ||
          rawBytes > control.maxAssembledBytes - files->initializerBytes)
        fail("MATERIAL_INITIALIZER");
      const auto offset = files->initializerBytes;
      ndn::util::Sha256 initializerHash;
      for (const auto& chunkId : reference.chunkPayloadIds) {
        checkActive(control);
        const auto bytes = payloads.at(chunkId)->copyBytes(checkMaterialActive);
        if (digest(bytes) != payloads.at(chunkId)->digest)
          fail("MATERIAL_PAYLOAD");
        append(bytes);
        checkActive(control);
        const auto view = ndn::span<const std::uint8_t>(bytes.data(), bytes.size());
        initializerHash.update(view);
        sidecarHash.update(view);
        files->initializerBytes = checkedAdd(files->initializerBytes, bytes.size());
        releaseMaterialPayload(chunkId);
      }
      if (finishHash(initializerHash) != reference.sharedDigest ||
          files->initializerBytes - offset != rawBytes)
        fail("MATERIAL_INITIALIZER");
      initializer.clear_external_data();
      initializer.set_data_location(onnx::TensorProto::EXTERNAL);
      for (const auto& entry : std::vector<std::pair<std::string, std::string>>{
             {"location", "model.onnx.data"}, {"offset", std::to_string(offset)},
             {"length", std::to_string(rawBytes)}}) {
        auto* field = initializer.add_external_data();
        field->set_key(entry.first);
        field->set_value(entry.second);
      }
      releaseMaterialPayload(reference.payloadId);
      return initializer;
    }
    const auto modelBytesBeforeInitializer = static_cast<std::uint64_t>(model.ByteSizeLong());
    const auto initializerHeaderBytes = static_cast<std::uint64_t>(initializer.ByteSizeLong());
    const auto finalModelWithoutRaw = checkedAdd(modelBytesBeforeInitializer,
                                                 initializerHeaderBytes);
    // maxAssembledBytes is the material-backed working-set ceiling here:
    // selected payloads remain owned by the source while the initializer is
    // concatenated, copied into TensorProto, and serialized into the result.
    // Include a small protobuf framing allowance before allocating raw.
    constexpr std::uint64_t protobufFramingAllowance = 128;
    const auto finalModelUpper = checkedAdd(
      checkedAdd(finalModelWithoutRaw, rawBytes), protobufFramingAllowance);
    const auto peakWithRawCopy = checkedAdd(
      retainedMaterialBytes, checkedAdd(rawBytes, checkedAdd(finalModelUpper, finalModelUpper)));
    if (rawBytes == 0 || rawBytes > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
        control.maxAssembledBytes == 0 || peakWithRawCopy > control.maxAssembledBytes)
      fail("MATERIAL_INITIALIZER");
    raw.reserve(static_cast<std::size_t>(rawBytes));
    for (const auto& chunkId : reference.chunkPayloadIds) {
      checkActive(control);
      const auto chunk = payloads.find(chunkId);
      if (chunk == payloads.end())
        fail("MATERIAL_INITIALIZER");
      const auto chunkBytes = chunk->second->copyBytes(checkMaterialActive);
      raw.insert(raw.end(), chunkBytes.begin(), chunkBytes.end());
      releaseMaterialPayload(chunkId);
    }
    releaseMaterialPayload(reference.payloadId);
    initializer.set_raw_data(reinterpret_cast<const char*>(raw.data()), static_cast<int>(raw.size()));
    raw.clear();
    raw.shrink_to_fit();
    initializer.set_data_location(onnx::TensorProto::DEFAULT);
    const auto normalized = normalizedOnnxInitializerPayload(
      deterministicMessageVector(initializer));
    if (digest(normalized.content) != reference.sharedDigest)
      fail("MATERIAL_INITIALIZER");
    return initializer;
  };
  // Initializer references are emitted in canonical source order.  Preserve
  // that order while fetching only dependencies of this selected role.
  for (const auto& reference : source.materialManifest->references) {
    checkActive(control);
    if (reference.kind != "shared-initializer" ||
        dependencies.count(reference.logicalName) == 0)
      continue;
    const auto payload = payloads.find(reference.payloadId);
    if (payload == payloads.end())
      fail("MATERIAL_INITIALIZER");
    *model.mutable_graph()->add_initializer() = materializeInitializer(reference);
  }
  // Every selected node and initializer has now been parsed, authenticated,
  // and copied into `model`.  Release the fetched material before serializing
  // the final model: keeping the shared bundle backing alive here duplicates
  // the largest initializer while protobuf constructs the result vector.
  // `retainedMaterialBytes` above remains the conservative budget accounting;
  // this release only lowers the actual working set after all reads finish.
  for (auto& payload : source.materialPayloads)
    payload.scrub();
  std::vector<NativeCanonicalSource::MaterialPayload>{}.swap(source.materialPayloads);
  if (model.graph().node_size() != static_cast<int>(nodeIndices.size()) ||
      model.graph().node_size() > static_cast<int>(std::numeric_limits<int>::max()))
    fail("MATERIAL_SELECTION");
  const auto result = deterministicMessageVector(model);
  const auto finalWorkingSet = checkedAdd(
    retainedMaterialBytes, checkedAdd(result.size(), model.ByteSizeLong()));
  if (control.maxAssembledBytes == 0 || result.size() > control.maxAssembledBytes ||
      finalWorkingSet > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  if (files != nullptr && files->initializerBytes != 0) {
    if (files->initializerBytes > control.maxAssembledBytes - result.size())
      fail("MATERIAL_LIMIT");
    files->initializerDigest = finishHash(sidecarHash);
  }
  return result;
}
} // namespace

std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedInputs,
                                const std::vector<NativeAssemblyTensorContractV3>& expectedOutputs,
                                const NativeAssemblyControl& control)
{
  return materializeCanonicalModel(source, nodeIndices, expectedInputs, expectedOutputs,
                                   control, nullptr, {});
}

NativeCanonicalMaterializedFiles
materializeNativeCanonicalModelFiles(
  NativeCanonicalSource& source, const std::vector<std::uint64_t>& nodeIndices,
  const std::vector<NativeAssemblyTensorContractV3>& expectedInputs,
  const std::vector<NativeAssemblyTensorContractV3>& expectedOutputs,
  const NativeAssemblyControl& control,
  const std::function<void(const std::vector<std::uint8_t>&)>& appendInitializer)
{
  NativeCanonicalMaterializedFiles files;
  files.modelBytes = materializeCanonicalModel(source, nodeIndices, expectedInputs, expectedOutputs,
                                              control, &files, appendInitializer);
  return files;
}

std::vector<std::uint8_t>
materializeNativeCanonicalModel(NativeCanonicalSource& source,
                                const std::vector<std::uint64_t>& nodeIndices,
                                const NativeAssemblyControl& control)
{
  return materializeNativeCanonicalModel(source, nodeIndices, {}, {}, control);
}

void validateNativeCanonicalMaterialManifest(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control)
{
  manifest.validate([&] { checkActive(control); });
  std::uint64_t materialBudget = 0;
  for (const auto& payload : manifest.payloads)
    materialBudget = checkedAdd(materialBudget, payload.byteSize());
  materialBudget = checkedAdd(materialBudget, manifest.canonicalJson().size());
  if (control.maxAssembledBytes == 0 || materialBudget > control.maxAssembledBytes)
    fail("MATERIAL_LIMIT");
  checkActive(control);
  const auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr,
    &control, &source);
  if (manifest.sourceDigest != nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) ||
      manifest.graphDigest != sha256HexOf(graphFactsJson(model, entries)) ||
      manifest.initializerDigest != sha256HexOf(initializerContentJson(entries)))
    throw std::invalid_argument("native canonical material manifest source identity differs");

  const auto templateIt = std::find_if(manifest.references.begin(), manifest.references.end(),
    [] (const auto& reference) { return reference.kind == "graph-template"; });
  if (templateIt == manifest.references.end())
    throw std::invalid_argument("native canonical material template reference is missing");
  const auto templatePayloadIt = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
    [&templateIt] (const auto& payload) { return payload.payloadId == templateIt->payloadId; });
  if (templatePayloadIt == manifest.payloads.end())
    throw std::invalid_argument("native canonical material template payload is missing");
  const auto templatePayloadBytes = templatePayloadIt->copyBytes();
  onnx::ModelProto templateModel;
  if (!templateModel.ParseFromArray(templatePayloadBytes.data(),
                                    static_cast<int>(templatePayloadBytes.size())) ||
      templateModel.graph().node_size() != 0 || templateModel.graph().initializer_size() != 0 ||
      templateModel.graph().sparse_initializer_size() != 0 ||
      templateModel.graph().quantization_annotation_size() != 0)
    throw std::invalid_argument("native canonical material template does not match source");
  auto expectedTemplate = model;
  expectedTemplate.mutable_graph()->clear_node();
  expectedTemplate.mutable_graph()->clear_initializer();
  expectedTemplate.mutable_graph()->clear_sparse_initializer();
  expectedTemplate.mutable_graph()->clear_quantization_annotation();
  if (deterministicMessageVector(expectedTemplate) != templatePayloadBytes)
    throw std::invalid_argument("native canonical material template identity is invalid");

  std::map<std::string, std::string> initializerDigestByName;
  for (const auto& entry : entries)
    initializerDigestByName.emplace(entry.name, entry.contentDigest);
  std::set<std::string> coveredInitializers;
  std::set<std::uint64_t> coveredNodes;
  for (const auto& reference : manifest.references) {
    checkActive(control);
    const auto payload = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
      [&reference] (const auto& item) { return item.payloadId == reference.payloadId; });
    if (payload == manifest.payloads.end())
      throw std::invalid_argument("native canonical material reference payload is missing");
    if (reference.kind == "shared-initializer") {
      const auto expected = initializerDigestByName.find(reference.logicalName);
      if (expected == initializerDigestByName.end() || expected->second != reference.sharedDigest ||
          !coveredInitializers.insert(reference.logicalName).second)
        throw std::invalid_argument("native canonical material initializer binding is invalid");
      const auto sourceInitializer = std::find_if(model.graph().initializer().begin(),
        model.graph().initializer().end(), [&reference] (const auto& initializer) {
          return initializer.name() == reference.logicalName;
        });
      if (sourceInitializer == model.graph().initializer().end())
        throw std::invalid_argument("native canonical material initializer payload is invalid");
      const bool hasChunks = !reference.chunkPayloadIds.empty();
      auto expectedHeader = *sourceInitializer;
      if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL || hasChunks) {
        expectedHeader.clear_raw_data();
        expectedHeader.clear_float_data();
        expectedHeader.clear_int32_data();
        expectedHeader.clear_string_data();
        expectedHeader.clear_int64_data();
        expectedHeader.clear_double_data();
        expectedHeader.clear_uint64_data();
        expectedHeader.clear_external_data();
        expectedHeader.set_data_location(onnx::TensorProto::DEFAULT);
      }
      if (deterministicMessageVector(expectedHeader) != payload->copyBytes())
        throw std::invalid_argument("native canonical material initializer payload is invalid");
      if (reference.chunkPayloadIds.empty()) {
        if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL) {
          const auto materialized = materializeExternalTensor(
            *sourceInitializer,
            source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, &control);
          if (deterministicMessageVector(materialized) != payload->copyBytes())
            throw std::invalid_argument("native canonical material initializer payload is invalid");
        }
        else if (deterministicMessageVector(*sourceInitializer) != payload->copyBytes()) {
          throw std::invalid_argument("native canonical material initializer payload is invalid");
        }
      }
      else {
        if (sourceInitializer->data_location() != onnx::TensorProto::EXTERNAL &&
            sourceInitializer->raw_data().size() <= NativeCanonicalMaterialBundleMaxBytes &&
            static_cast<std::uint64_t>(sourceInitializer->ByteSizeLong()) <=
              NativeCanonicalMaterialBundleMaxBytes)
          throw std::invalid_argument("native canonical material initializer chunks are invalid");
        std::uint64_t offset = 0;
        std::uint64_t length = 0;
        const std::uint8_t* expectedBytes = nullptr;
        bool expectedRange = false;
        if (sourceInitializer->data_location() == onnx::TensorProto::EXTERNAL) {
          if (!source.initializerBytes && !source.initializerRangeSource)
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          const auto range = validatedExternalByteRange(*sourceInitializer, source);
          offset = range.offset;
          length = range.length;
          expectedRange = static_cast<bool>(source.initializerRangeSource);
          if (!expectedRange)
            expectedBytes = source.initializerBytes->data() + static_cast<std::size_t>(offset);
        }
        else {
          length = sourceInitializer->raw_data().size();
          expectedBytes = reinterpret_cast<const std::uint8_t*>(sourceInitializer->raw_data().data());
        }
        std::uint64_t total = 0;
        for (const auto& chunkId : reference.chunkPayloadIds) {
          const auto chunk = std::find_if(manifest.payloads.begin(), manifest.payloads.end(),
            [&chunkId] (const auto& item) { return item.payloadId == chunkId; });
          if (chunk == manifest.payloads.end() ||
              chunk->byteSize() > NativeCanonicalMaterialBundleMaxBytes)
            throw std::invalid_argument("native canonical material initializer chunks are invalid");
          const auto chunkBytes = chunk->copyBytes();
          if (total > length || chunk->byteSize() > length - total)
            throw std::invalid_argument("native canonical material initializer chunks differ");
          if (expectedRange) {
            if (readNativeCanonicalInitializerRange(source, offset + total, chunk->byteSize(),
                                                    control) != chunkBytes)
              throw std::invalid_argument("native canonical material initializer chunks differ");
          }
          else if (!std::equal(chunkBytes.begin(), chunkBytes.end(), expectedBytes + total)) {
            throw std::invalid_argument("native canonical material initializer chunks differ");
          }
          total = checkedAdd(total, chunk->byteSize());
        }
        if (total != length)
          throw std::invalid_argument("native canonical material initializer chunks are incomplete");
      }
    }
    else if (reference.kind == "graph-node") {
      if (reference.nodeIndex >= static_cast<std::uint64_t>(model.graph().node_size()) ||
          !coveredNodes.insert(reference.nodeIndex).second)
        throw std::invalid_argument("native canonical material node coverage is invalid");
      const auto payloadBytes = payload->copyBytes();
      onnx::NodeProto node;
      if (!node.ParseFromArray(payloadBytes.data(), static_cast<int>(payloadBytes.size())) ||
          deterministicMessageVector(node) != payloadBytes ||
          deterministicMessageVector(model.graph().node(static_cast<int>(reference.nodeIndex))) != payloadBytes)
        throw std::invalid_argument("native canonical material node payload is invalid");
      std::set<std::string> expectedDependencies;
      for (const auto& input : model.graph().node(static_cast<int>(reference.nodeIndex)).input())
        if (initializerDigestByName.count(input) != 0) expectedDependencies.insert(input);
      std::set<std::string> actualDependencies(reference.dependencies.begin(), reference.dependencies.end());
      if (actualDependencies != expectedDependencies || actualDependencies.size() != reference.dependencies.size())
        throw std::invalid_argument("native canonical material node dependencies are invalid");
    }
  }
  if (coveredInitializers.size() != initializerDigestByName.size() ||
      coveredNodes.size() != static_cast<std::size_t>(model.graph().node_size()))
    throw std::invalid_argument("native canonical material coverage is incomplete");
}

void validateNativeCanonicalMaterialReferenceIndex(
  const NativeCanonicalSource& source,
  const NativeCanonicalSource::MaterialManifest& manifest,
  const NativeAssemblyControl& control)
{
  checkActive(control);
  if (manifest.payloadsComplete)
    throw std::invalid_argument("native canonical material index is complete");
  manifest.validate([&] { checkActive(control); });
  const auto identity = canonicalOnnxSourceIdentity(source, control);
  if (manifest.sourceDigest != nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size()) ||
      manifest.graphDigest != identity.graphDigest || manifest.initializerDigest != identity.initializerDigest)
    throw std::invalid_argument("native canonical material reference identity differs");
  checkActive(control);
}

namespace {
NativeOnnxIdentity
canonicalOnnxModelIdentity(const onnx::ModelProto& model,
                           const NativeAssemblyControl& control,
                           const std::vector<std::uint8_t>* externalBytes)
{
  checkActive(control);
  const auto entries = buildTensorIndex(model, externalBytes, &control);
  NativeOnnxIdentity identity;
  identity.graphDigest = sha256HexOf(graphFactsJson(model, entries));
  identity.initializerDigest = sha256HexOf(initializerContentJson(entries));
  checkActive(control);
  return identity;
}
} // namespace

NativeOnnxIdentity
canonicalOnnxSourceIdentity(const NativeCanonicalSource& source,
                            const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control, false);
  const auto entries = buildTensorIndex(
    model, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr,
    &control, &source);
  checkActive(control);
  NativeOnnxIdentity identity;
  identity.graphDigest = sha256HexOf(graphFactsJson(model, entries));
  identity.initializerDigest = sha256HexOf(initializerContentJson(entries));
  checkActive(control);
  return identity;
}

/** Bind already inspected graph facts to the adapter snapshot without loading model bytes. */
static std::string
planningGraphDigestFromMetadata(const NativeJson& metadata, const std::string& adapterDigest)
{
  const NativeJson identity{{"adapter_descriptor_digest", adapterDigest},
    {"inputs", metadata.at("inputs")}, {"outputs", metadata.at("outputs")},
    {"initializers", metadata.at("initializers")}, {"tensors", metadata.at("tensors")},
    {"nodes", metadata.at("nodes")}, {"tensor_producers", metadata.at("tensorProducers")},
    {"tensor_consumers", metadata.at("tensorConsumers")}};
  return nativePlanningDigest(nativeCanonicalJson(identity));
}

static NativeOnnxGraphInspection
inspectSourceGraphFacts(const NativeCanonicalSource& source,
                        const NativeModelDescriptor& expectedModel,
                        const NativeAssemblyControl& control,
                        const onnx::ModelProto& original,
                        const std::vector<TensorIndexEntry>& entries,
                        std::uint64_t materializedBudget,
                        std::vector<AutomaticTensorContract>* automaticTensors = nullptr)
{
  NativeOnnxGraphInspection result;
  result.canonicalIdentity = {sha256HexOf(graphFactsJson(original, entries)),
                              sha256HexOf(initializerContentJson(entries))};
  auto inferred = original;
  materializeShapeInferenceInitializers(
    inferred, original, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr, control,
    &materializedBudget, &source);
  // TensorProto owns initializer types and dimensions. Redundant value_info
  // can contain stale exporter annotations that abort official inference.
  // Normalize only this inspection copy; preserve all input/output contracts.
  std::set<std::string> inferenceInitializerNames;
  for (const auto& initializer : inferred.graph().initializer()) {
    checkActive(control);
    inferenceInitializerNames.insert(initializer.name());
  }
  auto* valueInfo = inferred.mutable_graph()->mutable_value_info();
  valueInfo->erase(std::remove_if(valueInfo->begin(), valueInfo->end(), [&](const auto& value) {
    checkActive(control);
    return inferenceInitializerNames.count(value.name()) != 0;
  }), valueInfo->end());
  bool inferenceSucceeded = false;
  try {
    inferNativeShapes(inferred);
    inferenceSucceeded = true;
  }
  catch (const std::exception&) {
    // Python infer_shapes returns a copy. A failed C++ inference may have
    // partially mutated its argument, so restore the original before fallback.
    inferred = original;
  }
  checkActive(control);
  const auto& graph = inferred.graph();
  if (graph.node_size() != original.graph().node_size()) fail("GRAPH_NODE_ORDER");

  if (automaticTensors != nullptr) {
    std::map<std::string, AutomaticTensorContract> contracts;
    std::uint64_t metadataBytes = 0;
    const auto automaticMetadataLimit = std::min(control.maxAssembledBytes,
                                                  NativeCanonicalMaterialPublicationMaxBytes);
    const auto charge = [&](std::uint64_t bytes) {
      metadataBytes = checkedAdd(metadataBytes, bytes);
      if (metadataBytes > automaticMetadataLimit) fail("AUTOMATIC_TENSOR_METADATA_SIZE");
      checkActive(control);
    };
    const auto addValues = [&](const auto& values, const char* provenance) {
      for (const auto& value : values) {
        const auto encodedBytes = value.ByteSizeLong();
        if (encodedBytes > automaticMetadataLimit / 8)
          fail("AUTOMATIC_TENSOR_METADATA_SIZE");
        charge(encodedBytes * 8 + sizeof(AutomaticTensorContract));
        if (value.name().empty()) fail("AUTOMATIC_TENSOR_NAME");
        auto& contract = contracts[value.name()];
        contract = AutomaticTensorContract{};
        contract.name = "graph/" + value.name();
        contract.provenance = {provenance};
        if (!value.type().has_tensor_type()) continue;
        const auto& type = value.type().tensor_type();
        contract.onnxElementType = type.elem_type();
        contract.wireElementType = onnxTensorWireType(type.elem_type());
        if (!type.has_shape()) continue;
        contract.dimensions.emplace();
        for (const auto& dim : type.shape().dim()) {
          charge(sizeof(DimensionConstraint) + dim.dim_param().size() + 64);
          DimensionConstraint axis;
          if (dim.has_dim_value()) {
            if (dim.dim_value() < 0) fail("AUTOMATIC_TENSOR_DIMENSION");
            axis.kind = DimensionConstraint::Kind::FIXED;
            axis.fixed = dim.dim_value();
            axis.upperBound = static_cast<std::uint64_t>(axis.fixed);
          }
          else if (dim.has_dim_param() && !dim.dim_param().empty()) {
            axis.kind = DimensionConstraint::Kind::SYMBOL;
            axis.symbol = "graph/" + dim.dim_param();
          }
          contract.dimensions->push_back(std::move(axis));
        }
      }
    };
    // Initializer headers are authoritative over redundant value_info.
    // Port declarations remain the graph's public contract when inference fails.
    addValues(graph.value_info(), inferenceSucceeded ? "onnx-inference-v1" : "graph-annotation-v1");
    addValues(graph.input(), "graph-input-v1");
    addValues(graph.output(), "graph-output-v1");
    const auto addLiteralHeader = [&](const std::string& name, int type, const auto& dimensions,
                                      const char* provenance) {
      charge(name.size() + sizeof(AutomaticTensorContract));
      AutomaticTensorContract contract;
      contract.name = "graph/" + name;
      contract.onnxElementType = type;
      contract.wireElementType = onnxTensorWireType(type);
      contract.provenance = {provenance};
      contract.dimensions.emplace();
      for (const auto dim : dimensions) {
        charge(sizeof(DimensionConstraint));
        if (dim < 0) fail("AUTOMATIC_TENSOR_DIMENSION");
        contract.dimensions->push_back({DimensionConstraint::Kind::FIXED, dim, {},
                                        static_cast<std::uint64_t>(dim)});
      }
      contracts[name] = std::move(contract);
    };
    for (const auto& tensor : graph.initializer())
      addLiteralHeader(tensor.name(), tensor.data_type(), tensor.dims(), "initializer-header-v1");
    std::set<std::string> localConstantDomains;
    for (const auto& function : original.functions()) {
      checkActive(control);
      if (function.name() == "Constant" && function.overload().empty())
        localConstantDomains.insert(function.domain());
    }
    for (const auto& node : original.graph().node()) {
      checkActive(control);
      if (!isBuiltinConstant(node, localConstantDomains.count(node.domain()))) continue;
      bool literal = false;
      for (const auto& attribute : node.attribute()) {
        checkActive(control);
        if (!attribute.ref_attr_name().empty()) continue;
        const auto add = [&](int type, const auto& dimensions) {
          if (literal) fail("AUTOMATIC_CONSTANT_LITERAL_CONFLICT");
          literal = true;
          addLiteralHeader(node.output(0), type, dimensions, "constant-header-v1");
        };
        if (attribute.name() == "value" && attribute.has_t())
          add(attribute.t().data_type(), attribute.t().dims());
        else if (attribute.name() == "value_int" && attribute.type() == onnx::AttributeProto::INT)
          add(onnx::TensorProto::INT64, std::vector<std::int64_t>{});
        else if (attribute.name() == "value_float" && attribute.type() == onnx::AttributeProto::FLOAT)
          add(onnx::TensorProto::FLOAT, std::vector<std::int64_t>{});
        else if (attribute.name() == "value_string" && attribute.type() == onnx::AttributeProto::STRING)
          add(onnx::TensorProto::STRING, std::vector<std::int64_t>{});
        else if (attribute.name() == "value_ints" && attribute.type() == onnx::AttributeProto::INTS)
          add(onnx::TensorProto::INT64, std::vector<std::int64_t>{attribute.ints_size()});
        else if (attribute.name() == "value_floats" && attribute.type() == onnx::AttributeProto::FLOATS)
          add(onnx::TensorProto::FLOAT, std::vector<std::int64_t>{attribute.floats_size()});
        else if (attribute.name() == "value_strings" && attribute.type() == onnx::AttributeProto::STRINGS)
          add(onnx::TensorProto::STRING, std::vector<std::int64_t>{attribute.strings_size()});
      }
    }
    // Values lacking annotations still exist and have explicitly unknown type/rank.
    for (const auto& node : graph.node()) {
      for (const auto& name : node.output()) {
        if (name.empty() || contracts.count(name)) continue;
        charge(name.size() + sizeof(AutomaticTensorContract));
        AutomaticTensorContract contract;
        contract.name = "graph/" + name;
        contract.provenance = {"graph-value-v1"};
        contracts.emplace(name, std::move(contract));
      }
    }
    automaticTensors->reserve(contracts.size());
    for (auto& [name, contract] : contracts) automaticTensors->push_back(std::move(contract));
  }

  static const std::map<int, std::pair<std::string, std::uint64_t>> types = {
    {1, {"float32", 4}}, {2, {"uint8", 1}}, {3, {"int8", 1}}, {4, {"uint16", 2}},
    {5, {"int16", 2}}, {6, {"int32", 4}}, {7, {"int64", 8}}, {9, {"bool", 1}},
    {10, {"float16", 2}}, {11, {"float64", 8}}, {12, {"uint32", 4}},
    {13, {"uint64", 8}}, {16, {"bfloat16", 2}}};
  const auto tensorInfo = [&](const std::string& name, int type, const NativeJson& shape) {
    const auto known = types.find(type);
    NativeJson bytes = nullptr;
    if (known != types.end()) {
      bool fixed = true, zero = false;
      for (const auto& dim : shape) {
        if (!dim.is_number_integer() || dim.get<std::int64_t>() < 0) fixed = false;
        else if (dim.get<std::int64_t>() == 0) zero = true;
      }
      if (fixed) {
        std::uint64_t count = zero ? 0 : known->second.second;
        if (!zero) for (const auto& dim : shape) {
          const auto n = dim.get<std::uint64_t>();
          if (n && count > std::numeric_limits<std::uint64_t>::max() / n) fail("GRAPH_TENSOR_SIZE");
          count *= n;
        }
        bytes = count;
      }
    }
    return NativeJson{{"name", name}, {"dtype", known == types.end() ? "onnx_type_" + std::to_string(type) : known->second.first},
                      {"shape", shape}, {"sizeBytes", bytes}};
  };
  auto tensors = NativeJson::object();
  const auto addValues = [&](const auto& values) {
    for (const auto& value : values) {
      if (value.name().empty()) continue;
      const auto& type = value.type().tensor_type();
      auto shape = NativeJson::array();
      for (const auto& dim : type.shape().dim()) {
        if (dim.has_dim_value()) shape.push_back(dim.dim_value());
        else if (dim.has_dim_param()) shape.push_back(dim.dim_param());
        else shape.push_back("?");
      }
      tensors[value.name()] = tensorInfo(value.name(), type.elem_type(), shape);
    }
  };
  addValues(graph.input()); addValues(graph.output()); addValues(graph.value_info());
  std::set<std::string> initializers;
  for (const auto& tensor : graph.initializer()) {
    auto shape = NativeJson::array();
    for (const auto dim : tensor.dims()) shape.push_back(dim);
    tensors[tensor.name()] = tensorInfo(tensor.name(), tensor.data_type(), shape);
    initializers.insert(tensor.name());
    result.initializerConsumers.emplace(tensor.name(), std::vector<std::uint64_t>{});
  }
  std::vector<std::string> inputs, outputs;
  for (const auto& value : graph.input()) if (!initializers.count(value.name())) inputs.push_back(value.name());
  for (const auto& value : graph.output()) outputs.push_back(value.name());
  auto nodes = NativeJson::array();
  std::map<std::string, std::uint64_t> producers;
  std::map<std::string, std::vector<std::uint64_t>> consumers;
  for (int index = 0; index < graph.node_size(); ++index) {
    checkActive(control);
    const auto& node = graph.node(index);
    const auto& before = original.graph().node(index);
    if (node.name() != before.name() || node.op_type() != before.op_type() ||
        !std::equal(node.input().begin(), node.input().end(), before.input().begin(), before.input().end()) ||
        !std::equal(node.output().begin(), node.output().end(), before.output().begin(), before.output().end()))
      fail("GRAPH_NODE_ORDER");
    const auto id = "onnx-node-" + std::to_string(index);
    const auto name = node.name().empty() ? std::to_string(index) + ":" + node.op_type() : node.name();
    std::vector<std::string> in, out;
    for (const auto& value : node.input()) {
      if (initializers.count(value)) result.initializerConsumers[value].push_back(index);
    }
    for (const auto& value : node.input()) if (!value.empty() && !initializers.count(value)) {
      in.push_back(value); consumers[value].push_back(index);
    }
    for (const auto& value : node.output()) if (!value.empty()) {
      out.push_back(value); producers[value] = index;
    }
    // Integer node attributes travel with the node. A tensor-parallel plan needs the
    // attention op's head counts, which exist only as attributes; recording them here is
    // what lets a reference-only restore derive that geometry without the serialized graph.
    auto attributes = NativeJson::object();
    for (const auto& attribute : node.attribute())
      if (attribute.has_i()) attributes[attribute.name()] = attribute.i();
    nodes.push_back(NativeJson{{"index", index}, {"name", name}, {"opType", node.op_type()},
                               {"inputs", in}, {"outputs", out},
                               {"integerAttributes", std::move(attributes)}});
    result.graph.nodes.push_back({id, node.op_type(), std::uint64_t(index)});
    result.graph.topologicalOrder.push_back(id);
    result.nodeNames.push_back(name);
    result.canonicalNodeIndices[id] = index;
  }
  const auto contract = [&](const std::string& name) {
    NativeTensorContract tensor;
    tensor.name = name; tensor.dtype = "unknown";
    const auto found = tensors.find(name);
    if (found == tensors.end()) return tensor;
    tensor.dtype = found->at("dtype").get<std::string>();
    for (const auto& dim : found->at("shape")) {
      if (dim.is_string()) tensor.shape.emplace_back(dim.get<std::string>());
      else tensor.shape.emplace_back(dim.get<std::int64_t>());
    }
    if (!found->at("sizeBytes").is_null()) tensor.estimatedBytes = found->at("sizeBytes").get<std::uint64_t>();
    return tensor;
  };
  for (const auto& producer : producers) {
    const auto users = consumers.find(producer.first);
    if (users == consumers.end() || users->second.empty()) continue;
    NativeGraphEdge edge;
    edge.id = producer.first; edge.producer = "onnx-node-" + std::to_string(producer.second);
    edge.tensor = contract(edge.id);
    // An ONNX node may consume one tensor in several operand slots. Keep
    // that multiplicity in metadata/identity, but a dependency consumer is
    // a node identity and must occur only once in the planning edge.
    std::set<std::uint64_t> uniqueUsers;
    for (const auto user : users->second)
      if (uniqueUsers.insert(user).second) edge.consumers.push_back("onnx-node-" + std::to_string(user));
    result.graph.edges.push_back(std::move(edge));
    // Every forward edge crosses the sequential cut after its producer.
    // The union of all maintained cuts is therefore exactly this edge set;
    // validate() below rejects backward or otherwise invalid dependencies.
    result.graph.legalCutEdges.push_back(producer.first);
  }
  for (const auto& name : inputs) result.graph.modelInputs.push_back(contract(name));
  for (const auto& name : outputs) result.graph.modelOutputs.push_back(contract(name));
  const std::vector<std::string> initializerNames(initializers.begin(), initializers.end());
  const NativeJson metadata{
    {"inputs", inputs}, {"outputs", outputs}, {"initializers", initializerNames}, {"tensors", tensors},
    {"nodes", nodes}, {"tensorProducers", producers}, {"tensorConsumers", consumers}};
  result.graph.graphDigest = planningGraphDigestFromMetadata(
    metadata, expectedModel.adapter.descriptorDigest());
  auto sourceModel = expectedModel;
  sourceModel.graphDigest = result.graph.graphDigest;
  result.graph.validate(sourceModel);
  result.graphMetadataJson = nativeCanonicalJson(metadata);
  checkActive(control);
  return result;
}

namespace {

using AutomaticConstants = std::map<std::string, const onnx::TensorProto*>;
using AutomaticTypes = std::map<std::string, std::int32_t>;
using AutomaticShapes = std::map<std::string, std::vector<std::optional<std::int64_t>>>;
using AutomaticOpsets = std::map<std::string, std::int64_t>;

std::string
standardDomain(std::string domain)
{
  return domain == "ai.onnx" ? "" : domain;
}

template<typename Imports>
AutomaticOpsets
automaticOpsets(const Imports& imports)
{
  AutomaticOpsets result;
  for (const auto& entry : imports) {
    const auto domain = standardDomain(entry.domain());
    if (entry.version() <= 0 || !result.emplace(domain, entry.version()).second)
      fail("AUTOMATIC_OPSET");
  }
  return result;
}

std::int64_t
requiredAutomaticInteger(const AutomaticOnnxOperatorFact& operation, const char* name)
{
  const auto found = operation.integerAttributes.find(name);
  if (found == operation.integerAttributes.end()) fail("AUTOMATIC_QUANT_ATTRIBUTE");
  return found->second;
}

std::uint64_t
automaticMultiply(std::uint64_t left, std::uint64_t right)
{
  if (right != 0 && left > std::numeric_limits<std::uint64_t>::max() / right)
    fail("AUTOMATIC_QUANT_OVERFLOW");
  return left * right;
}

std::uint64_t
automaticProduct(const std::vector<std::int64_t>& shape)
{
  std::uint64_t count = 1;
  for (const auto dim : shape) {
    if (dim < 0) fail("AUTOMATIC_QUANT_SHAPE");
    count = automaticMultiply(count, static_cast<std::uint64_t>(dim));
  }
  return count;
}

void
validateAutomaticNBits(const AutomaticOnnxOperatorFact& op, QuantizedOperatorFact& quant)
{
  const auto k = requiredAutomaticInteger(op, "K");
  const auto n = requiredAutomaticInteger(op, "N");
  const auto block = requiredAutomaticInteger(op, "block_size");
  const auto bits = requiredAutomaticInteger(op, "bits");
  if (k <= 0 || n <= 0 || block < 16 || (block & (block - 1)) != 0 || bits < 2 || bits > 8 ||
      op.inputs.size() < 3 || op.inputs.size() > 6 || op.outputs.size() != 1 ||
      op.inputs[0].empty() || op.inputs[1].empty() || op.inputs[2].empty())
    fail("AUTOMATIC_QUANT_ATTRIBUTE");
  quant.bits = bits;
  quant.blockSize = block;
  if (op.inputs.size() < 4 || op.inputs[3].empty())
    quant.implicitZeroPoint = std::int64_t{1} << (bits - 1);
  const auto blocks = 1 + (static_cast<std::uint64_t>(k) - 1) / block;
  const auto scales = automaticMultiply(static_cast<std::uint64_t>(n), blocks);
  const auto blob = automaticMultiply(static_cast<std::uint64_t>(block), bits) / 8;
  bool weightKnown = false, scaleKnown = false, zeroKnown = quant.implicitZeroPoint.has_value();
  for (const auto& value : quant.constantInputs) {
    const auto count = automaticProduct(value.shape);
    if (value.inputIndex == 1) {
      if (value.onnxElementType != onnx::TensorProto::UINT8 || value.shape.size() != 3 ||
          value.shape[0] != n || static_cast<std::uint64_t>(value.shape[1]) != blocks ||
          static_cast<std::uint64_t>(value.shape[2]) != blob ||
          count != automaticMultiply(scales, blob)) fail("AUTOMATIC_QUANT_PACKING");
      weightKnown = true;
    }
    if (value.inputIndex == 2 || value.inputIndex == 3 || value.inputIndex == 5) {
      const bool floating = value.onnxElementType == onnx::TensorProto::FLOAT ||
        value.onnxElementType == onnx::TensorProto::FLOAT16 ||
        value.onnxElementType == onnx::TensorProto::BFLOAT16;
      const auto expected = value.inputIndex == 5 ? static_cast<std::uint64_t>(n) : scales;
      if (value.inputIndex == 3 && value.onnxElementType == onnx::TensorProto::UINT8) {
        const auto columns = (automaticMultiply(blocks, static_cast<std::uint64_t>(bits)) + 7) / 8;
        const auto zeroBytes = automaticMultiply(static_cast<std::uint64_t>(n), columns);
        if (count != zeroBytes || (value.shape.empty() && zeroBytes != 1) ||
            value.shape.size() > 2 ||
            (value.shape.size() == 2 && (value.shape[0] != n ||
             static_cast<std::uint64_t>(value.shape[1]) != columns)))
          fail("AUTOMATIC_QUANT_ZERO_POINT");
      }
      else if (!floating || count != expected) fail("AUTOMATIC_QUANT_SCALE");
      else if ((value.shape.empty() && expected != 1) || value.shape.size() > 2 ||
               (value.shape.size() == 2 && (value.shape[0] != n ||
                static_cast<std::uint64_t>(value.shape[1]) != blocks)) ||
               (value.inputIndex == 5 && value.shape.size() != 1 && count != 1))
        fail("AUTOMATIC_QUANT_SCALE");
      if (value.inputIndex == 2) scaleKnown = true;
      if (value.inputIndex == 3) zeroKnown = true;
    }
    if (value.inputIndex == 4 && value.onnxElementType != onnx::TensorProto::INT32)
      fail("AUTOMATIC_QUANT_TYPE");
  }
  quant.staticParametersValidated = weightKnown && scaleKnown && zeroKnown;
}

void
validateAutomaticStandardQuantization(const AutomaticOnnxOperatorFact& op,
                                      QuantizedOperatorFact& quant,
                                      const AutomaticTypes& types,
                                      const AutomaticShapes& shapes)
{
  const auto* schema = onnx::OpSchemaRegistry::Schema(op.opType, static_cast<int>(op.opset), "");
  if (schema == nullptr) fail("AUTOMATIC_QUANT_SCHEMA");
  if (op.inputs.size() < static_cast<std::size_t>(schema->min_input()) ||
      op.inputs.size() > static_cast<std::size_t>(schema->max_input()) ||
      op.outputs.size() < static_cast<std::size_t>(schema->min_output()) ||
      op.outputs.size() > static_cast<std::size_t>(schema->max_output()))
    fail("AUTOMATIC_QUANT_ARITY");
  for (std::size_t k = 0; k < op.inputs.size(); ++k)
    if (op.inputs[k].empty() && k < schema->inputs().size() &&
        schema->inputs()[k].GetOption() == onnx::OpSchema::Single)
      fail("AUTOMATIC_QUANT_ARITY");
  for (std::size_t k = 0; k < op.outputs.size(); ++k)
    if (op.outputs[k].empty() && k < schema->outputs().size() &&
        schema->outputs()[k].GetOption() == onnx::OpSchema::Single)
      fail("AUTOMATIC_QUANT_ARITY");
  std::map<std::string, std::int32_t> boundTypes;
  for (std::size_t k = 0; k < op.inputs.size(); ++k) {
    const auto found = types.find(op.inputs[k]);
    if (found == types.end() || found->second == 0) continue;
    if (k >= schema->inputs().size()) fail("AUTOMATIC_QUANT_ARITY");
    const auto& parameter = schema->inputs()[k];
    onnx::TypeProto type;
    type.mutable_tensor_type()->set_elem_type(found->second);
    if (!parameter.GetTypes().count(onnx::Utils::DataTypeUtils::ToType(type)))
      fail("AUTOMATIC_QUANT_TYPE");
    const auto inserted = boundTypes.emplace(parameter.GetTypeStr(), found->second);
    if (!inserted.second && inserted.first->second != found->second)
      fail("AUTOMATIC_QUANT_TYPE");
  }
  for (std::size_t k = 0; k < op.outputs.size(); ++k) {
    const auto found = types.find(op.outputs[k]);
    if (found == types.end() || found->second == 0) continue;
    if (k >= schema->outputs().size()) fail("AUTOMATIC_QUANT_ARITY");
    const auto& parameter = schema->outputs()[k];
    onnx::TypeProto type;
    type.mutable_tensor_type()->set_elem_type(found->second);
    if (!parameter.GetTypes().count(onnx::Utils::DataTypeUtils::ToType(type)))
      fail("AUTOMATIC_QUANT_TYPE");
    const auto inserted = boundTypes.emplace(parameter.GetTypeStr(), found->second);
    if (!inserted.second && inserted.first->second != found->second)
      fail("AUTOMATIC_QUANT_TYPE");
  }
  const AutomaticOnnxConstantFact* scale = nullptr;
  const AutomaticOnnxConstantFact* zero = nullptr;
  for (const auto& value : quant.constantInputs) {
    if (value.inputIndex == 1) scale = &value;
    if (value.inputIndex == 2) zero = &value;
    if (op.opType == "QLinearMatMul" && value.inputIndex != 0 && value.inputIndex != 3 &&
        automaticProduct(value.shape) != 1) fail("AUTOMATIC_QUANT_SHAPE");
  }
  if (op.opType == "QuantizeLinear" || op.opType == "DequantizeLinear") {
    const auto axis = op.integerAttributes.find("axis");
    const auto block = op.integerAttributes.find("block_size");
    if (axis != op.integerAttributes.end()) quant.axis = axis->second;
    if (block != op.integerAttributes.end()) {
      if (block->second < 0) fail("AUTOMATIC_QUANT_ATTRIBUTE");
      quant.blockSize = block->second;
    }
    if (scale && zero && (automaticProduct(scale->shape) != automaticProduct(zero->shape) ||
        (automaticProduct(scale->shape) != 1 && scale->shape != zero->shape)))
      fail("AUTOMATIC_QUANT_ZERO_POINT");
    if (scale && automaticProduct(scale->shape) == 0) fail("AUTOMATIC_QUANT_SCALE");
    if (!op.attributeReferences.empty()) return;
    auto shape = shapes.find(op.inputs.front());
    bool shapeProven = scale && automaticProduct(scale->shape) == 1;
    if (scale && (!quant.blockSize || *quant.blockSize == 0)) {
      if (scale->shape.size() > 1) fail("AUTOMATIC_QUANT_SCALE");
      if (shape != shapes.end() && automaticProduct(scale->shape) > 1) {
        auto selected = quant.axis.value_or(1);
        if (selected < 0) selected += static_cast<std::int64_t>(shape->second.size());
        if (selected < 0 || static_cast<std::size_t>(selected) >= shape->second.size())
          fail("AUTOMATIC_QUANT_SCALE");
        if (shape->second[selected]) {
          if (scale->shape.front() != *shape->second[selected]) fail("AUTOMATIC_QUANT_SCALE");
          shapeProven = true;
        }
      }
    }
    else if (scale && quant.blockSize && *quant.blockSize > 0) {
      shapeProven = false;
      if (shape != shapes.end()) {
        auto selected = quant.axis.value_or(1);
        if (selected < 0) selected += static_cast<std::int64_t>(shape->second.size());
        if (selected < 0 || static_cast<std::size_t>(selected) >= shape->second.size() ||
            scale->shape.size() != shape->second.size()) fail("AUTOMATIC_QUANT_SCALE");
        shapeProven = true;
        for (std::size_t d = 0; d < scale->shape.size(); ++d) {
          if (!shape->second[d]) { shapeProven = false; continue; }
          const auto n = *shape->second[d];
          const auto expected = d == static_cast<std::size_t>(selected)
            ? (n == 0 ? 0 : 1 + (n - 1) / *quant.blockSize) : n;
          if (scale->shape[d] != expected) fail("AUTOMATIC_QUANT_SCALE");
        }
      }
    }
    quant.staticParametersValidated = scale != nullptr && shapeProven &&
      (op.inputs.size() < 3 || op.inputs[2].empty() || zero != nullptr);
  }
  else {
    quant.bits = 8;
    quant.staticParametersValidated = op.opType == "DynamicQuantizeLinear";
  }
}


NativeJson
automaticPorts(const google::protobuf::RepeatedPtrField<onnx::ValueInfoProto>& values,
                const std::set<std::string>& excluded = {})
{
  auto ports = NativeJson::array();
  for (const auto& value : values) {
    if (excluded.count(value.name())) continue;
    const auto& type = value.type().tensor_type();
    NativeJson shape = nullptr;
    if (type.has_shape()) {
      shape = NativeJson::array();
      for (const auto& dim : type.shape().dim()) {
        if (dim.has_dim_value()) shape.push_back(dim.dim_value());
        else if (dim.has_dim_param()) shape.push_back(dim.dim_param());
        else shape.push_back(nullptr);
      }
    }
    ports.push_back(NativeJson{{"name", value.name()}, {"onnxType", type.elem_type()},
                               {"shape", shape}});
  }
  return ports;
}

} // namespace

AutomaticOnnxModel
inspectAutomaticOnnxModel(OpenedAutomaticOnnxSource opened,
                          const NativeAssemblyControl& control)
{
  AutomaticOnnxModel result;
  result.opened = std::move(opened);
  result.contract.sourceDigest = result.opened.sourceDigest;
  const auto& source = result.opened.source;
  std::uint64_t materializedBudget = 0;
  const auto original = ownedSourceModel(source, control, false, &materializedBudget);
  const auto entries = buildTensorIndex(original, nullptr, &control, &source);
  std::map<std::string, const TensorIndexEntry*> indexed;
  for (const auto& entry : entries) indexed.emplace(entry.name, &entry);
  const auto rootOpsets = automaticOpsets(original.opset_import());
  std::uint64_t factBytes = 0;
  const auto factLimit = std::min(control.maxAssembledBytes,
                                 NativeCanonicalMaterialPublicationMaxBytes);
  const auto accountFacts = [&](std::uint64_t bytes) {
    if (factLimit == 0 || bytes > factLimit - factBytes)
      fail("AUTOMATIC_FACTS_LIMIT");
    factBytes += bytes;
  };
  accountFacts(0);
  using FunctionKey = std::tuple<std::string, std::string, std::string>;
  std::map<FunctionKey, std::size_t> functions;
  for (int i = 0; i < original.functions_size(); ++i) {
    checkActive(control);
    const auto& fn = original.functions(i);
    accountFacts(sizeof(FunctionKey) + fn.domain().size() + fn.name().size() + fn.overload().size());
    if (!functions.emplace(FunctionKey{fn.domain(), fn.name(), fn.overload()}, i).second)
      fail("AUTOMATIC_FUNCTION_AMBIGUOUS");
  }
  const auto addIntegerLiteral = [&](const std::string& name,
                                     std::vector<std::int64_t> values) {
    checkActive(control);
    if (values.size() > 64) fail("AUTOMATIC_LITERAL_COUNT");
    accountFacts(128 + name.size() + values.size() * sizeof(std::int64_t));
    if (!result.contract.integerLiterals.emplace("graph/" + name, std::move(values)).second)
      fail("AUTOMATIC_LITERAL_DUPLICATE");
  };
  const auto readIntegerTensor = [&](const onnx::TensorProto& tensor)
      -> std::optional<std::vector<std::int64_t>> {
    checkActive(control);
    if (tensor.data_type() != onnx::TensorProto::INT32 &&
        tensor.data_type() != onnx::TensorProto::INT64 &&
        tensor.data_type() != onnx::TensorProto::BOOL) return std::nullopt;
    const auto count = tensorElementCount(tensor);
    if (count > 64) return std::nullopt;
    const auto width = tensor.data_type() == onnx::TensorProto::BOOL ? 1U :
      tensor.data_type() == onnx::TensorProto::INT32 ? 4U : 8U;
    std::optional<onnx::TensorProto> material;
    if (tensor.data_location() == onnx::TensorProto::EXTERNAL) {
      const auto range = validatedExternalByteRange(tensor, source);
      if (range.length != count * width) fail("AUTOMATIC_LITERAL_RANGE");
      material = materializeExternalTensor(tensor, nullptr, &control, &source);
    }
    const auto& value = material ? *material : tensor;
    if (value.ByteSizeLong() > factLimit) fail("AUTOMATIC_LITERAL_LIMIT");
    if (value.data_type() == onnx::TensorProto::BOOL) {
      for (const auto bit : value.int32_data())
        if (bit != 0 && bit != 1) fail("AUTOMATIC_LITERAL_BOOL");
    }
    const auto encoded = deterministicMessageVector(value);
    const auto normalized = normalizedOnnxInitializerPayload(encoded);
    if (normalized.content.size() != count * width) fail("AUTOMATIC_LITERAL_BYTES");
    std::vector<std::int64_t> values;
    values.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      std::uint64_t bits = 0;
      for (unsigned byte = 0; byte < width; ++byte)
        bits |= static_cast<std::uint64_t>(normalized.content[i * width + byte]) << (byte * 8);
      if (width == 1) {
        if (bits > 1) fail("AUTOMATIC_LITERAL_BOOL");
        values.push_back(static_cast<std::int64_t>(bits));
      }
      else if (width == 4) {
        values.push_back(bits <= std::uint64_t(std::numeric_limits<std::int32_t>::max())
          ? static_cast<std::int64_t>(bits) : static_cast<std::int64_t>(bits) - (1LL << 32));
      }
      else {
        values.push_back(bits <= std::uint64_t(std::numeric_limits<std::int64_t>::max())
          ? static_cast<std::int64_t>(bits)
          : -1 - static_cast<std::int64_t>(std::numeric_limits<std::uint64_t>::max() - bits));
      }
    }
    return values;
  };
  const auto addIntegerTensor = [&](const std::string& name, const onnx::TensorProto& tensor) {
    if (tensor.data_type() != onnx::TensorProto::INT32 &&
        tensor.data_type() != onnx::TensorProto::INT64) return;
    auto values = readIntegerTensor(tensor);
    if (values) addIntegerLiteral(name, std::move(*values));
  };
  const auto addFloatingTensor = [&](const std::string& name, const onnx::TensorProto& tensor) {
    checkActive(control);
    if (tensor.data_type() != onnx::TensorProto::FLOAT &&
        tensor.data_type() != onnx::TensorProto::FLOAT16 &&
        tensor.data_type() != onnx::TensorProto::BFLOAT16) return;
    if (tensorElementCount(tensor) != 1) return;
    const auto width = tensor.data_type() == onnx::TensorProto::FLOAT ? 4U : 2U;
    std::optional<onnx::TensorProto> material;
    if (tensor.data_location() == onnx::TensorProto::EXTERNAL) {
      const auto range = validatedExternalByteRange(tensor, source);
      if (range.length != width) fail("AUTOMATIC_FLOAT_LITERAL_RANGE");
      material = materializeExternalTensor(tensor, nullptr, &control, &source);
    }
    const auto& value = material ? *material : tensor;
    if (value.ByteSizeLong() > factLimit) fail("AUTOMATIC_FLOAT_LITERAL_LIMIT");
    const auto normalized = normalizedOnnxInitializerPayload(deterministicMessageVector(value));
    if (normalized.content.size() != width) fail("AUTOMATIC_FLOAT_LITERAL_BYTES");
    accountFacts(128 + name.size() + width);
    if (!result.contract.floatingLiterals.emplace("graph/" + name, normalized.content).second)
      fail("AUTOMATIC_FLOAT_LITERAL_DUPLICATE");
  };
  for (const auto& tensor : original.graph().initializer()) {
    addIntegerTensor(tensor.name(), tensor);
    addFloatingTensor(tensor.name(), tensor);
  }
  for (const auto& node : original.graph().node()) {
    checkActive(control);
    if (!isBuiltinConstant(node, functions.count(FunctionKey{
          node.domain(), node.op_type(), node.overload()}))) continue;
    for (const auto& attr : node.attribute()) {
      if (!attr.ref_attr_name().empty()) continue;
      if (attr.name() == "value" && attr.has_t()) {
        addIntegerTensor(node.output(0), attr.t());
        addFloatingTensor(node.output(0), attr.t());
      }
      else if (attr.name() == "value_int" && attr.type() == onnx::AttributeProto::INT)
        addIntegerLiteral(node.output(0), {attr.i()});
      else if (attr.name() == "value_ints" && attr.type() == onnx::AttributeProto::INTS &&
               attr.ints_size() <= 64)
        addIntegerLiteral(node.output(0), {attr.ints().begin(), attr.ints().end()});
      else if ((attr.name() == "value_float" && attr.type() == onnx::AttributeProto::FLOAT) ||
               (attr.name() == "value_floats" && attr.type() == onnx::AttributeProto::FLOATS &&
                attr.floats_size() == 1)) {
        onnx::TensorProto scalar;
        scalar.set_data_type(onnx::TensorProto::FLOAT);
        if (attr.name() == "value_floats") scalar.add_dims(1);
        scalar.add_float_data(attr.name() == "value_float" ? attr.f() : attr.floats(0));
        addFloatingTensor(node.output(0), scalar);
      }
    }
  }
  std::set<std::size_t> visitedFunctions;
  std::vector<std::size_t> pendingFunctions;
  std::optional<std::size_t> currentFunction;
  std::set<std::pair<std::size_t, std::size_t>> functionEdges;
  using Nodes = google::protobuf::RepeatedPtrField<onnx::NodeProto>;
  std::function<void(const onnx::GraphProto&, const std::string&,
                     const AutomaticOpsets&)> walkGraph;
  std::function<void(const Nodes&, AutomaticConstants, AutomaticTypes, AutomaticShapes,
                     const std::string&, const AutomaticOpsets&)> walkNodes;
  walkNodes = [&](const Nodes& nodes, AutomaticConstants constants, AutomaticTypes types,
                  AutomaticShapes shapes, const std::string& scope, const AutomaticOpsets& opsets) {
    for (const auto& node : nodes) {
      checkActive(control);
      if (isBuiltinConstant(node, functions.count(FunctionKey{
            node.domain(), node.op_type(), node.overload()}))) {
        for (const auto& attr : node.attribute()) {
          if (attr.name() == "value" && attr.has_t() && attr.ref_attr_name().empty()) {
            constants.emplace(node.output(0), &attr.t());
            types[node.output(0)] = attr.t().data_type();
            auto& shape = shapes[node.output(0)];
            shape.clear();
            for (const auto dim : attr.t().dims()) shape.push_back(dim);
          }
        }
      }
    }
    for (int i = 0; i < nodes.size(); ++i) {
      checkActive(control);
      const auto& node = nodes.Get(i);
      accountFacts(sizeof(AutomaticOnnxOperatorFact) + scope.size() + node.domain().size() +
                   node.op_type().size());
      for (const auto& name : node.input()) accountFacts(sizeof(std::string) + name.size());
      for (const auto& name : node.output()) accountFacts(sizeof(std::string) + name.size());
      AutomaticOnnxOperatorFact op;
      op.scope = scope;
      op.nodeIndex = i;
      op.domain = node.domain();
      op.opType = node.op_type();
      op.overload = node.overload();
      const auto version = opsets.find(standardDomain(node.domain()));
      if (version == opsets.end() || version->second > std::numeric_limits<int>::max())
        fail("AUTOMATIC_OPSET");
      op.opset = version->second;
      op.inputs.assign(node.input().begin(), node.input().end());
      op.outputs.assign(node.output().begin(), node.output().end());
      std::set<std::string> attributes;
      for (const auto& attr : node.attribute()) {
        accountFacts(64 + attr.name().size() + attr.ref_attr_name().size() +
                     automaticMultiply(attr.ints_size(), sizeof(std::int64_t)));
        if (!attributes.insert(attr.name()).second) fail("AUTOMATIC_ATTRIBUTE");
        // These keys are inspection evidence, never ONNX source attributes.
        if (attr.name().compare(0, 7, "tensor.") == 0)
          fail("AUTOMATIC_ATTRIBUTE_RESERVED");
        if (!attr.ref_attr_name().empty()) {
          op.attributeReferences[attr.name()] = attr.ref_attr_name();
          continue;
        }
        if (attr.type() == onnx::AttributeProto::INT) op.integerAttributes[attr.name()] = attr.i();
        if (attr.type() == onnx::AttributeProto::INTS)
          op.integerListAttributes[attr.name()] = {attr.ints().begin(), attr.ints().end()};
        if (attr.has_g() || attr.graphs_size() != 0) op.atomicBody = true;
      }
      std::optional<std::size_t> functionIndex;
      const auto function = functions.find(FunctionKey{
        node.domain(), node.op_type(), node.overload()});
      if (function != functions.end()) {
        functionIndex = function->second;
        op.functionScope = "function/" + std::to_string(*functionIndex);
      }
      op.atomicBody = op.atomicBody || functionIndex.has_value();
      if (!op.atomicBody && !functionIndex && standardDomain(op.domain).empty() &&
          op.opType == "ConstantOfShape" && op.opset >= 9 && op.opset <= 21 &&
          op.overload.empty() && op.attributeReferences.empty() &&
          op.inputs.size() == 1 && !op.inputs[0].empty() &&
          op.outputs.size() == 1 && !op.outputs[0].empty()) {
        for (const auto& attr : node.attribute()) {
          if (attr.name() != "value" || attr.type() != onnx::AttributeProto::TENSOR ||
              !attr.has_t()) continue;
          const auto& tensor = attr.t();
          if (tensor.dims_size() != 1 || tensor.dims(0) != 1 || tensor.has_segment() ||
              tensor.data_location() != onnx::TensorProto::DEFAULT ||
              tensor.external_data_size() != 0) continue;
          const auto type = tensor.data_type();
          const char* key = type == onnx::TensorProto::INT32 ? "tensor.value.int32" :
            type == onnx::TensorProto::INT64 ? "tensor.value.int64" :
            type == onnx::TensorProto::BOOL ? "tensor.value.bool" : nullptr;
          if (!key) continue;
          auto value = readIntegerTensor(tensor);
          if (!value || value->size() != 1) fail("AUTOMATIC_SHAPE_FILL");
          accountFacts(96 + std::char_traits<char>::length(key));
          op.integerListAttributes.emplace(key, std::move(*value));
        }
      }
      const bool nbits = !functionIndex && node.domain() == "com.microsoft" &&
        node.op_type() == "MatMulNBits" && op.opset == 1;
      const bool standard = !functionIndex && standardDomain(node.domain()).empty() &&
        (node.op_type() == "MatMulInteger" || node.op_type() == "QLinearMatMul" ||
         node.op_type() == "QuantizeLinear" || node.op_type() == "DequantizeLinear" ||
         node.op_type() == "DynamicQuantizeLinear");
      if (nbits && (op.inputs.size() < 3 || op.inputs.size() > 6 || op.outputs.size() != 1 ||
          op.inputs[0].empty() || op.inputs[1].empty() || op.inputs[2].empty()))
        fail("AUTOMATIC_QUANT_ARITY");
      if (standard) {
        const auto* schema = onnx::OpSchemaRegistry::Schema(node.op_type(),
          static_cast<int>(op.opset), "");
        if (schema == nullptr) fail("AUTOMATIC_QUANT_SCHEMA");
        if (op.attributeReferences.empty()) schema->Verify(node);
        for (const auto& [name, attribute] : schema->attributes()) {
          if (attribute.default_value.type() == onnx::AttributeProto::INT &&
              !attributes.count(name))
            op.integerAttributes[name] = attribute.default_value.i();
        }
      }
      const auto operationIndex = result.contract.operators.size();
      result.contract.operators.push_back(op);
      if (nbits || standard) {
        QuantizedOperatorFact quant;
        quant.operatorIndex = operationIndex;
        for (int k = 0; k < node.input_size(); ++k) {
          const auto found = constants.find(node.input(k));
          if (found == constants.end()) continue;
          const auto& tensor = *found->second;
          accountFacts(sizeof(AutomaticOnnxConstantFact) + node.input(k).size() + 71 +
                       automaticMultiply(tensor.dims_size(), sizeof(std::int64_t)));
          AutomaticOnnxConstantFact fact;
          fact.inputIndex = k;
          fact.valueName = node.input(k);
          fact.onnxElementType = tensor.data_type();
          fact.shape.assign(tensor.dims().begin(), tensor.dims().end());
          fact.storageBytes = rawByteLength(tensor.data_type(), tensorElementCount(tensor));
          const auto top = scope == "graph" ? indexed.find(node.input(k)) : indexed.end();
          if (top != indexed.end()) fact.normalizedContentDigest = top->second->contentDigest;
          else {
            const auto bytes = deterministicMessageVector(tensor);
            fact.normalizedContentDigest = digest(normalizedOnnxInitializerPayload(bytes).content);
          }
          quant.constantInputs.push_back(std::move(fact));
        }
        if (nbits && op.attributeReferences.empty()) {
          validateAutomaticNBits(op, quant);
          const auto activation = types.find(node.input(0));
          std::optional<int> expected;
          if (activation != types.end() && activation->second != 0) expected = activation->second;
          for (const auto& value : quant.constantInputs) {
            if (value.inputIndex == 2) {
              if (expected && *expected != value.onnxElementType) fail("AUTOMATIC_QUANT_TYPE");
              expected = value.onnxElementType;
            }
          }
          if (expected && *expected != onnx::TensorProto::FLOAT &&
              *expected != onnx::TensorProto::FLOAT16 && *expected != onnx::TensorProto::BFLOAT16)
            fail("AUTOMATIC_QUANT_TYPE");
          const auto output = types.find(node.output(0));
          if (expected && output != types.end() && output->second != 0 &&
              output->second != *expected) fail("AUTOMATIC_QUANT_TYPE");
          for (const auto& value : quant.constantInputs) {
            if (expected && (value.inputIndex == 5 ||
                (value.inputIndex == 3 && value.onnxElementType != onnx::TensorProto::UINT8)) &&
                value.onnxElementType != *expected) fail("AUTOMATIC_QUANT_TYPE");
          }
        }
        else if (standard) {
          validateAutomaticStandardQuantization(op, quant, types, shapes);
          if (op.opType == "QuantizeLinear" || op.opType == "DequantizeLinear") {
            int type = 0;
            if (op.opType == "DequantizeLinear") {
              const auto found = types.find(node.input(0));
              if (found != types.end()) type = found->second;
            }
            else {
              if (node.input_size() > 2) {
                const auto found = types.find(node.input(2));
                if (found != types.end()) type = found->second;
              }
              if (type == 0 && op.integerAttributes.count("output_dtype"))
                type = static_cast<int>(op.integerAttributes.at("output_dtype"));
              if (type == 0 && (node.input_size() < 3 || node.input(2).empty()))
                type = onnx::TensorProto::UINT8;
            }
            if (type == onnx::TensorProto::INT4 || type == onnx::TensorProto::UINT4) quant.bits = 4;
            if (type == onnx::TensorProto::INT8 || type == onnx::TensorProto::UINT8) quant.bits = 8;
          }
        }
        result.contract.quantization.push_back(std::move(quant));
      }
      for (int j = 0; j < node.attribute_size(); ++j) {
        const auto& attr = node.attribute(j);
        const auto nested = scope + "/node/" + std::to_string(i) +
          "/attribute/" + std::to_string(j);
        if (attr.has_g()) walkGraph(attr.g(), nested, opsets);
        for (int g = 0; g < attr.graphs_size(); ++g)
          walkGraph(attr.graphs(g), nested + "/" + std::to_string(g), opsets);
      }
      if (functionIndex) {
        if (currentFunction) functionEdges.emplace(*currentFunction, *functionIndex);
        if (visitedFunctions.insert(*functionIndex).second)
          pendingFunctions.push_back(*functionIndex);
      }
    }
  };
  walkGraph = [&](const onnx::GraphProto& graph, const std::string& scope,
                   const AutomaticOpsets& opsets) {
    if (graph.sparse_initializer_size() != 0) fail("AUTOMATIC_SPARSE_UNSUPPORTED");
    AutomaticConstants constants;
    AutomaticTypes types;
    AutomaticShapes shapes;
    for (const auto& tensor : graph.initializer()) {
      if (!constants.emplace(tensor.name(), &tensor).second) fail("AUTOMATIC_INITIALIZER_NAME");
      types[tensor.name()] = tensor.data_type();
      auto& shape = shapes[tensor.name()];
      for (const auto dim : tensor.dims()) shape.push_back(dim);
    }
    for (const auto* values : {&graph.input(), &graph.output()})
      for (const auto& value : *values)
        if (!constants.count(value.name()) && !types.count(value.name())) {
          const auto& type = value.type().tensor_type();
          types[value.name()] = type.elem_type();
          if (type.has_shape()) {
            auto& shape = shapes[value.name()];
            for (const auto& dim : type.shape().dim())
              shape.push_back(dim.has_dim_value() && dim.dim_value() >= 0
                                ? std::optional<std::int64_t>(dim.dim_value())
                                                 : std::nullopt);
          }
        }
    walkNodes(graph.node(), std::move(constants), std::move(types), std::move(shapes),
              scope, opsets);
  };
  walkGraph(original.graph(), "graph", rootOpsets);
  for (std::size_t i = 0; i < pendingFunctions.size(); ++i) {
    currentFunction = pendingFunctions[i];
    const auto& fn = original.functions(*currentFunction);
    walkNodes(fn.node(), {}, {}, {}, "function/" + std::to_string(*currentFunction),
              automaticOpsets(fn.opset_import()));
  }
  std::map<std::size_t, std::size_t> indegree;
  for (const auto f : visitedFunctions) indegree.emplace(f, 0);
  std::map<std::size_t, std::vector<std::size_t>> functionConsumers;
  for (const auto& edge : functionEdges) {
    ++indegree.at(edge.second);
    functionConsumers[edge.first].push_back(edge.second);
  }
  std::vector<std::size_t> ready;
  for (const auto& [f, degree] : indegree) if (degree == 0) ready.push_back(f);
  for (std::size_t i = 0; i < ready.size(); ++i) {
    checkActive(control);
    const auto found = functionConsumers.find(ready[i]);
    if (found == functionConsumers.end()) continue;
    for (const auto next : found->second)
      if (--indegree.at(next) == 0) ready.push_back(next);
  }
  if (ready.size() != visitedFunctions.size()) fail("AUTOMATIC_FUNCTION_RECURSION");
  checkActive(control);
  const auto factsJson = result.contract.factsJson();
  if (factsJson.size() > control.maxAssembledBytes) fail("AUTOMATIC_FACTS_LIMIT");
  result.contract.factsDigest = nativePlanningDigest(factsJson);
  std::int64_t maximumBits = 0;
  bool dynamic = false;
  bool weightOnly = !result.contract.quantization.empty();
  for (const auto& quant : result.contract.quantization) {
    if (quant.bits) maximumBits = std::max(maximumBits, *quant.bits);
    const auto& op = result.contract.operators.at(quant.operatorIndex);
    dynamic = dynamic || op.opType == "DynamicQuantizeLinear";
    weightOnly = weightOnly && op.domain == "com.microsoft" && op.opType == "MatMulNBits";
  }
  std::string precision;
  if (maximumBits != 0) precision = "int" + std::to_string(maximumBits);
  else {
    std::map<int, std::uint64_t> storage;
    for (const auto& tensor : original.graph().initializer()) {
      if (tensor.data_type() == onnx::TensorProto::STRING) continue;
      const auto bytes = rawByteLength(tensor.data_type(), tensorElementCount(tensor));
      storage[tensor.data_type()] = checkedAdd(storage[tensor.data_type()], bytes);
    }
    int selected = 0;
    std::uint64_t largest = 0;
    for (const auto& [type, bytes] : storage) {
      if ((type == onnx::TensorProto::FLOAT || type == onnx::TensorProto::FLOAT16 ||
           type == onnx::TensorProto::BFLOAT16 || type == onnx::TensorProto::DOUBLE) &&
          bytes > largest) {
        selected = type;
        largest = bytes;
      }
    }
    if (selected == 0) {
      for (const auto& output : original.graph().output()) {
        if (output.type().has_tensor_type() && output.type().tensor_type().elem_type() != 0) {
          selected = output.type().tensor_type().elem_type();
          break;
        }
      }
    }
    if (selected == 0) fail("AUTOMATIC_PRECISION_UNSUPPORTED");
    if (selected == onnx::TensorProto::FLOAT16) precision = "fp16";
    else if (selected == onnx::TensorProto::BFLOAT16) precision = "bf16";
    else if (selected == onnx::TensorProto::DOUBLE) precision = "fp64";
    else if (selected == onnx::TensorProto::FLOAT) precision = "fp32";
    else precision = dtypeLabel(selected);
  }
  auto& descriptor = result.descriptor;
  auto& adapter = descriptor.adapter;
  adapter.name = "automatic-onnx";
  adapter.version = "1";
  adapter.abi = "ndnsf-di-automatic-onnx-v1";
  adapter.modelFormats = {"onnx"};
  adapter.tasks = {"tensor-forward"};
  adapter.backends = {"onnxruntime"};
  adapter.precisions = {precision};
  const auto schemaDigest = [](const NativeJson& schema) {
    return nativePlanningDigest(nativeCanonicalJson(schema));
  };
  std::set<std::string> initializerNames;
  for (const auto& tensor : original.graph().initializer()) initializerNames.insert(tensor.name());
  adapter.inputSchemaDigest = schemaDigest(NativeJson{{"format", "NDITB001"},
    {"ports", automaticPorts(original.graph().input(), initializerNames)}});
  adapter.resultSchemaDigest = schemaDigest(NativeJson{{"format", "NDITB001"},
    {"ports", automaticPorts(original.graph().output())}});
  adapter.optionsSchemaDigest = schemaDigest(NativeJson{{"owner", "native-request"},
    {"modelOptions", NativeJson::array()}});
  adapter.graphSchemaDigest = schemaDigest(NativeJson{{"schema", "native-source-graph-v1"},
    {"fields", {"graphDigest", "nodes", "topologicalOrder", "legalCutEdges", "modelInputs",
                 "modelOutputs", "edges"}},
    {"nodeFields", {"id", "opType", "ordinal"}},
    {"edgeFields", {"id", "producer", "consumers", "tensor"}},
    {"tensorFields", {"name", "dtype", "shape", "estimatedBytes"}},
    {"dimensions", {"int64", "symbol"}}, {"estimatedBytes", "optional-uint64"}});
  adapter.splitSchemaDigest = schemaDigest(NativeJson{{"enabled", false}});
  adapter.stateSchemaDigest = schemaDigest(NativeJson{{"generation", false}});
  adapter.stateDigest = schemaDigest(NativeJson{{"schema", adapter.abi},
    {"sourceDigest", result.opened.sourceDigest}, {"factsDigest", result.contract.factsDigest}});
  adapter.graphInspectable = true;
  descriptor.modelName = "onnx-" + result.opened.sourceDigest.substr(7);
  descriptor.contentDigest = result.opened.sourceDigest;
  descriptor.semanticsDigest = result.contract.factsDigest;
  descriptor.graphDigest = sha256HexOf(graphFactsJson(original, entries));
  descriptor.modelFormat = "onnx";
  descriptor.precision = precision;
  descriptor.adapterId = adapter.name;
  descriptor.adapterVersion = adapter.version;
  descriptor.sourceRevision = result.opened.modelDigest;
  if (dynamic) descriptor.quantizationSubtype = "dynamic_int8";
  else if (weightOnly && maximumBits == 4) descriptor.quantizationSubtype = "weight_only_int4";
  else if (weightOnly && maximumBits == 8) descriptor.quantizationSubtype = "weight_only_int8";
  descriptor.validate();
  result.inspection = inspectSourceGraphFacts(source, descriptor, control, original, entries,
                                             materializedBudget, &result.contract.tensors);
  for (const auto& tensor : result.inspection.graph.modelInputs)
    result.contract.modelInputs.push_back("graph/" + tensor.name);
  for (const auto& tensor : result.inspection.graph.modelOutputs)
    result.contract.modelOutputs.push_back("graph/" + tensor.name);
  // Call-site function scopes are checked by the automatic metadata pass.
  // An unrelated local overload must not hide a builtin schema at other nodes.
  const onnx::ModelProto builtinSchemaModel;
  const NativeShapeSchemaRegistry automaticSchemas(builtinSchemaModel);
  refineAutomaticOnnxTensorFacts(original, automaticSchemas, result.contract, control);
  result.contract.relations = deriveAutomaticOnnxShapeRelations(result.contract, control);
  result.contract.validateSourceFacts(control);
  std::exception_ptr ownerFailure;
  auto generationControl = control;
  generationControl.requireActive = [&] {
    try {
      control.requireActive();
    }
    catch (...) {
      ownerFailure = std::current_exception();
      throw;
    }
  };
  try {
    result.contract.generation = deriveAutomaticOnnxGeneration(result.contract, generationControl);
    if (!result.contract.generation)
      result.generationDiagnostic = "AUTOMATIC_GENERATION_UNPROVEN";
  }
  catch (const AutomaticGenerationUnproven& error) {
    if (ownerFailure) std::rethrow_exception(ownerFailure);
    result.generationDiagnostic = std::string(error.what()).substr(0, 128);
  }
  result.contract.validate(control);
  // The generation proof is derived after the adapter descriptor is built; an
  // adapter whose contract proves generation must advertise the generation
  // task, otherwise preparation rejects the request task as unsupported.
  if (result.contract.generation &&
      std::find(descriptor.adapter.tasks.begin(), descriptor.adapter.tasks.end(),
                "text-generation") == descriptor.adapter.tasks.end()) {
    descriptor.adapter.tasks.push_back("text-generation");
    result.inspection.graph.graphDigest = planningGraphDigestFromMetadata(
      nativeParseJson(result.inspection.graphMetadataJson), descriptor.adapter.descriptorDigest());
  }
  descriptor.graphDigest = result.inspection.graph.graphDigest;
  descriptor.validate();
  checkActive(control);
  return result;
}


AutomaticOnnxModel
inspectAutomaticOnnxModel(const OnnxModelInput& input, const NativeAssemblyControl& control)
{
  return inspectAutomaticOnnxModel(openAutomaticOnnxSource(input.modelFile, control), control);
}

NativeOnnxGraphInspection
inspectNativeOnnxSourceGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control)
{
  expectedModel.validate();
  if (expectedModel.modelFormat != "onnx") fail("GRAPH_MODEL_FORMAT");
  std::uint64_t materializedBudget = 0;
  const auto original = ownedSourceModel(source, control, false, &materializedBudget);
  const auto entries = buildTensorIndex(
    original, source.initializerBytes ? &source.initializerBytes->asVector() : nullptr,
    &control, &source);
  return inspectSourceGraphFacts(source, expectedModel, control, original, entries,
                                 materializedBudget);
}

NativeOnnxGraphInspection
inspectNativeOnnxPlanningGraph(const NativeCanonicalSource& source,
  const NativeModelDescriptor& expectedModel, const NativeAssemblyControl& control)
{
  auto result = inspectNativeOnnxSourceGraph(source, expectedModel, control);
  result.graph.validate(expectedModel);
  return result;
}

std::uint32_t
onnxInitializerNormalizationRevision(const NativeCanonicalSource& source,
                                     const NativeAssemblyControl& control)
{
  const auto model = ownedSourceModel(source, control, false);
  return revisionOfInlinedModel(model);
}

void
checkOnnxAssemblerDescriptorBinding(const std::string& assemblerDescriptorDigest,
                                    const NativeCanonicalSource& source,
                                    const NativeAssemblyControl& control)
{
  if (onnxInitializerNormalizationRevision(source, control) != 2) return;
  // v2-bound content must be explicitly declared by the recipe; there is no
  // silent substitution of a legacy descriptor (initializer normalization
  // compatibility matrix).
  static const std::string v2Descriptor =
    "sha256:5291ee00f425c59605f72e26c9b27a73aca43b976421218515fc1a38085c7a89";
  if (assemblerDescriptorDigest != v2Descriptor)
    failNormalization("NORMALIZATION_REVISION_REQUIRED");
}

} // namespace ndnsf::di

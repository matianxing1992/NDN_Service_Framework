#include "tests/boost-test.hpp"

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeAuthenticatedGrantClient.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeOfferAdmission.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderOfferV3.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeRequestPlanner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ConversationStateBinding.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Provider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProtectedRuntime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeProtectedProvider.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModelPackage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/detail/RuntimeTestAccess.hpp"
#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp"
#include "ndn-service-framework/PolicyStatus.hpp"
#include "ndnsf-integration-fixture.hpp"
#include "tests/fixtures/spec182/native-model-fixture.hpp"

#include <boost/test/unit_test.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace ndnsf::di;

// The C-04 qualification contract gives each native case a 60-second bound.
// Use that same bound for normal and sanitizer selectors: the Runtime drain
// includes native client and Face cleanup, and a busy host must not turn a
// valid lifecycle completion into a five-second scheduling failure. A true
// stuck worker still fails within this explicit qualification deadline.
std::chrono::seconds testDrainTimeout()
{
  return std::chrono::seconds(60);
}

// Keep the conversation result observation independent of libstdc++'s
// std::future shared-state bookkeeping.  The fixture pumps a borrowed Face
// while a native request blocks in another C++ thread; result state is shared
// separately from the thread owner so a worker can never destroy the object
// that owns (and joins) that same worker.
struct ConversationResultState
{
  std::mutex mutex;
  std::optional<Result> value;
  std::exception_ptr error;
  std::atomic<bool> ready{false};
};

struct ConversationResultObservation
{
  RequestHandle handle;
  std::shared_ptr<ConversationResultState> state;
  std::thread worker;

  ~ConversationResultObservation()
    noexcept
  {
    try {
      // A failed pump must actively release the native wait before joining;
      // otherwise cleanup would wait for the full result timeout.  The worker
      // captures only state and handle, never this owner.
      handle.cancel();
    }
    catch (...) {
      // Cleanup must remain non-throwing even if the production cancellation
      // callback reports an already-closing executor.
    }
    if (worker.joinable()) {
      try {
        worker.join();
      }
      catch (...) {
        // A joinable worker is never owned by itself; any join failure is an
        // unrecoverable fixture invariant violation and must not escape a
        // noexcept destructor.
        std::terminate();
      }
    }
  }

  Result get()
  {
    if (worker.joinable())
      worker.join();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->error)
      std::rethrow_exception(state->error);
    if (!state->value)
      throw std::runtime_error("Spec185 conversation result observation is empty");
    return std::move(*state->value);
  }
};

// Exercise the public blocking PreparedModel::run() entry point while the
// integration fixture pumps the same borrowed Face used by the native
// requester.  The worker owns only the immutable model/input/options and the
// result state; the observation owner joins it before the fixture can be
// destroyed.  The request deadline bounds cleanup if the production chain
// fails before producing a result.
struct PreparedRunObservation
{
  std::shared_ptr<ConversationResultState> state;
  std::thread worker;

  ~PreparedRunObservation() noexcept
  {
    if (worker.joinable()) {
      try {
        worker.join();
      }
      catch (...) {
        std::terminate();
      }
    }
  }

  Result get()
  {
    if (worker.joinable())
      worker.join();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->error)
      std::rethrow_exception(state->error);
    if (!state->value)
      throw std::runtime_error("Spec185 prepared run observation is empty");
    return std::move(*state->value);
  }
};

std::shared_ptr<PreparedRunObservation>
startPreparedRunObservation(PreparedModel model, Input input, RequestOptions options)
{
  auto observation = std::make_shared<PreparedRunObservation>();
  observation->state = std::make_shared<ConversationResultState>();
  const auto state = observation->state;
  observation->worker = std::thread([state, model = std::move(model), input = std::move(input),
                                     options] () mutable {
    try {
      auto result = model.run(input, options);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->value = std::move(result);
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = std::current_exception();
    }
    state->ready.store(true, std::memory_order_release);
  });
  return observation;
}

std::shared_ptr<ConversationResultObservation>
startConversationResultObservation(RequestHandle handle,
                                   std::chrono::seconds timeout)
{
  auto observation = std::make_shared<ConversationResultObservation>();
  observation->handle = handle;
  observation->state = std::make_shared<ConversationResultState>();
  const auto state = observation->state;
  observation->worker = std::thread([state, handle, timeout] {
    try {
      auto result = handle.result(timeout);
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->value = std::move(result);
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = std::current_exception();
    }
    state->ready.store(true, std::memory_order_release);
  });
  return observation;
}

// The same explicit state/owner split is used for drain() observations.  It
// removes std::async/future wait bookkeeping from the borrowed-Face fixture
// while retaining a join barrier for every detached native wait.
struct BooleanResultState
{
  std::mutex mutex;
  std::optional<bool> value;
  std::exception_ptr error;
  std::atomic<bool> ready{false};
};

struct BooleanResultObservation
{
  std::shared_ptr<BooleanResultState> state;
  std::thread worker;

  ~BooleanResultObservation() noexcept
  {
    if (worker.joinable()) {
      try {
        worker.join();
      }
      catch (...) {
        std::terminate();
      }
    }
  }

  bool get()
  {
    if (worker.joinable())
      worker.join();
    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->error)
      std::rethrow_exception(state->error);
    if (!state->value)
      throw std::runtime_error("Spec185 boolean observation is empty");
    return *state->value;
  }
};

std::shared_ptr<BooleanResultObservation>
startBooleanResultObservation(std::function<bool()> operation)
{
  auto observation = std::make_shared<BooleanResultObservation>();
  observation->state = std::make_shared<BooleanResultState>();
  const auto state = observation->state;
  observation->worker = std::thread([state, operation = std::move(operation)] {
    try {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->value = operation();
      }
    }
    catch (...) {
      std::lock_guard<std::mutex> lock(state->mutex);
      state->error = std::current_exception();
    }
    state->ready.store(true, std::memory_order_release);
  });
  return observation;
}

void
pumpUntilBooleanResultReady(
  ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
  const std::shared_ptr<BooleanResultObservation>& observation,
  std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!observation->state->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    environment.pumpUntil([] { return false; });
  }
  if (!observation->state->ready.load(std::memory_order_acquire))
    throw std::runtime_error("Spec185 boolean result pump deadline expired");
}

void
pumpUntilConversationResultReady(
  ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
  const std::shared_ptr<ConversationResultObservation>& observation,
  std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!observation->state->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    // Complete one bounded face-pump chunk before checking the worker flag.
    // Keeping the predicate out of pumpFaces avoids sanitizer-sensitive
    // exception unwinding through a callback that captures test state.
    environment.pumpUntil([] { return false; });
  }
  if (!observation->state->ready.load(std::memory_order_acquire))
    throw std::runtime_error("Spec185 conversation result pump deadline expired");
}

void
pumpUntilRequestTerminal(
  ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
  const RequestHandle& handle, std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (handle.status() == RequestStatus::Pending &&
         std::chrono::steady_clock::now() < deadline)
    environment.pumpUntil([] { return false; });
  if (handle.status() == RequestStatus::Pending)
    throw std::runtime_error("Spec185 request terminal pump deadline expired");
}

// Hold a public completion callback while a Conversation failure becomes
// visible. The native terminal hook must open the next turn before this gate
// is released, without adding a production test seam.
struct DelayedConversationCompletion
{
  DelayedConversationCompletion()
    : entered(std::make_shared<std::promise<void>>()),
      enteredFuture(entered->get_future().share()),
      releasePromise(std::make_shared<std::promise<void>>()),
      releaseFuture(releasePromise->get_future().share())
  {
  }

  ~DelayedConversationCompletion() noexcept
  {
    release();
  }

  std::function<void(std::exception_ptr, std::optional<Result>)> callback() const
  {
    const auto enteredPromise = entered;
    const auto release = releaseFuture;
    return [enteredPromise, release](std::exception_ptr,
                                     std::optional<Result>) {
      try { enteredPromise->set_value(); }
      catch (...) {}
      release.wait();
    };
  }

  bool wait(std::chrono::seconds timeout) const
  {
    return enteredFuture.wait_for(timeout) == std::future_status::ready;
  }

  void release() const
  {
    try { releasePromise->set_value(); }
    catch (...) {}
  }

  std::shared_ptr<std::promise<void>> entered;
  std::shared_future<void> enteredFuture;
  std::shared_ptr<std::promise<void>> releasePromise;
  std::shared_future<void> releaseFuture;
};

void
pumpUntilPreparedRunReady(
  ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
  const std::shared_ptr<PreparedRunObservation>& observation,
  std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!observation->state->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    environment.pumpUntil([] { return false; });
  }
  if (!observation->state->ready.load(std::memory_order_acquire))
    throw std::runtime_error("Spec185 prepared run pump deadline expired");
}

void
pumpUntilConversationResultsReady(
  ndn_service_framework::test::NdnsfIntegrationEnvironment& environment,
  const std::shared_ptr<ConversationResultObservation>& first,
  const std::shared_ptr<ConversationResultObservation>& second,
  std::chrono::seconds budget)
{
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while ((!first->state->ready.load(std::memory_order_acquire) ||
          !second->state->ready.load(std::memory_order_acquire)) &&
         std::chrono::steady_clock::now() < deadline) {
    // Keep the predicate-free bounded pump used by the conversation helper;
    // readiness is observed through explicit C++ state owned outside the
    // worker thread.
    environment.pumpUntil([] { return false; });
  }
  if (!first->state->ready.load(std::memory_order_acquire) ||
      !second->state->ready.load(std::memory_order_acquire))
    throw std::runtime_error("Spec185 conversation result pair pump deadline expired");
}

// Keep the synchronous busy-turn exception boundary out of the large
// integration test frame.  The production Conversation guard remains the
// subject under test; this helper only keeps the expected exception object
// and its unwinding in a small, independently protected C++ frame.
bool conversationRejectsBusyTurn(Conversation& conversation,
                                 const RequestOptions& options)
{
  try {
    (void)conversation.request(Input::inlineBytes({0x09}), options);
  }
  catch (const DiError& error) {
    return error.code() == "CONVERSATION_TURN_IN_PROGRESS";
  }
  return false;
}

void writeEd25519KeyPair(const std::filesystem::path& privatePath,
                         const std::filesystem::path& publicPath)
{
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
    EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1)
    throw std::runtime_error("cannot initialize fixture key generator");
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) != 1)
    throw std::runtime_error("cannot generate fixture key");
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> privateBio(
    BIO_new_file(privatePath.c_str(), "wb"), BIO_free);
  std::unique_ptr<BIO, decltype(&BIO_free)> publicBio(
    BIO_new_file(publicPath.c_str(), "wb"), BIO_free);
  if (!privateBio || !publicBio ||
      PEM_write_bio_PrivateKey(privateBio.get(), key.get(), nullptr, nullptr, 0,
                               nullptr, nullptr) != 1 ||
      PEM_write_bio_PUBKEY(publicBio.get(), key.get()) != 1)
    throw std::runtime_error("cannot write fixture key");
  std::filesystem::permissions(privatePath,
    std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
    std::filesystem::perm_options::replace);
}

std::string publicKeyDigest(const std::filesystem::path& path)
{
  std::ifstream input(path);
  const std::string pem((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(
    BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
    PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (!key) throw std::runtime_error("cannot read fixture public key");
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (EVP_PKEY_get_raw_public_key(key.get(),
        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != raw.size())
    throw std::runtime_error("fixture public key is not Ed25519");
  return nativePlanningDigest(raw);
}

std::shared_ptr<EVP_PKEY> deterministicEd25519Key(unsigned char seed)
{
  std::array<unsigned char, 32> bytes{};
  bytes.fill(seed);
  auto* raw = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                            bytes.data(), bytes.size());
  if (!raw) throw std::runtime_error("cannot create fixture Ed25519 key");
  return {raw, EVP_PKEY_free};
}

std::string rawPublicKey(const std::shared_ptr<EVP_PKEY>& key)
{
  std::string raw(32, '\0');
  std::size_t size = raw.size();
  if (!key || EVP_PKEY_get_raw_public_key(key.get(),
        reinterpret_cast<unsigned char*>(raw.data()), &size) != 1 || size != raw.size())
    throw std::runtime_error("cannot extract fixture Ed25519 public key");
  return raw;
}

std::string publicKeyPem(const std::shared_ptr<EVP_PKEY>& key)
{
  std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new(BIO_s_mem()), BIO_free);
  if (!bio || PEM_write_bio_PUBKEY(bio.get(), key.get()) != 1)
    throw std::runtime_error("cannot encode fixture Ed25519 public key");
  char* data = nullptr;
  const auto size = BIO_get_mem_data(bio.get(), &data);
  return std::string(data, size > 0 ? static_cast<std::size_t>(size) : 0);
}

std::string signDigest(const std::shared_ptr<EVP_PKEY>& key, const std::string& value)
{
  std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(
    EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, key.get()) != 1)
    throw std::runtime_error("cannot initialize fixture offer signer");
  std::array<unsigned char, 64> signature{};
  std::size_t size = signature.size();
  if (EVP_DigestSign(context.get(), signature.data(), &size,
                     reinterpret_cast<const unsigned char*>(value.data()), value.size()) != 1 ||
      size != signature.size())
    throw std::runtime_error("cannot sign fixture provider offer");
  std::array<unsigned char, 89> encoded{};
  if (EVP_EncodeBlock(encoded.data(), signature.data(), static_cast<int>(size)) != 88)
    throw std::runtime_error("cannot encode fixture provider offer signature");
  return std::string(reinterpret_cast<const char*>(encoded.data()), 88);
}

class RuntimeFixture
{
public:
  explicit RuntimeFixture(bool streaming = false, bool qwenStreaming = false)
    : root(std::filesystem::temp_directory_path() /
           ("spec185-prepared-request-" + std::to_string(::getpid()) + "-" +
            std::to_string(++sequence)))
  {
    std::filesystem::create_directories(root);
    write("trust.conf",
      "; Spec185 prepared request fixture\n"
      "rule\n"
      "{\n"
      "  id \"Testbed Validation Rule\"\n"
      "  for data\n"
      "  checker\n"
      "  {\n"
      "    type hierarchical\n"
      "    sig-type rsa-sha256\n"
      "  }\n"
      "}\n"
      "trust-anchor\n"
      "{\n"
      "  type any\n"
      "}\n");
    writeEd25519KeyPair(root / "requester.pem", root / "requester-public.pem");
    writeEd25519KeyPair(root / "authority.pem", root / "authority-public.pem");
    writeEd25519KeyPair(root / "offer.pem", root / "offer-public.pem");
    {
      std::ofstream key(root / "conversation.key", std::ios::binary);
      const std::string bytes(32, 'k');
      key.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
      key.close();
      std::filesystem::permissions(root / "conversation.key",
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace);
    }

    NativeCanonicalSource source;
    NativeModelDescriptor descriptor;
    NativeJson catalog = NativeJson::object();
    const auto control = NativeAssemblyControl{
      std::chrono::steady_clock::now() + std::chrono::seconds(30), [] {},
      1 << 20, 1 << 20};
    if (qwenStreaming) {
      std::ifstream qwenFile("tests/fixtures/spec182/qwen-native-config.onnx", std::ios::binary);
      if (!qwenFile.good()) throw std::runtime_error("missing Qwen native config fixture");
      const std::string qwenBytes((std::istreambuf_iterator<char>(qwenFile)),
                                  std::istreambuf_iterator<char>());
      source.modelBytes.assign(qwenBytes.begin(), qwenBytes.end());
      const auto sourceIdentity = canonicalOnnxSourceIdentity(source, control);
      const std::string role = "/LLM/Pipeline/Stage/0";
      const std::string manifestDigest = nativePlanningDigest("spec185-qwen-manifest");
      const std::string protectionEpoch = "epoch-1";
      // The provider fixture's grant publication source intentionally binds the
      // shared fixture profile; keep the Qwen source/model identity distinct
      // while using the same authorized artifact profile at this boundary.
      const std::string artifactProfileDigest = nativePlanningDigest("fixture-profile");
      const std::string qwenGraphDigest =
        "sha256:8f901f484f07440aef844373c95d64b6462bcbc90c0070087b4c85c78a0de2b2";
      descriptor = fixture::completeModel({
        "QwenFixture", nativePlanningDigest("spec185-qwen-content"),
        nativePlanningDigest("spec185-qwen-semantics"), qwenGraphDigest,
        "onnx", "float32", "qwen", "1"});
      descriptor.sourceRevision = "pinned-r1";
      const std::string sourceDigest = nativePlanningDigest(
        source.modelBytes.data(), source.modelBytes.size());
      const NativeJson stateInputs = {{role, {{"attention_kv_in", {"attention_kv_in"}},
        {"recurrent_state_in", {"recurrent_state_in"}},
        {"convolution_state_in", {"convolution_state_in"}}}}};
      const NativeJson stateOutputs = {{role, {{"attention_kv_out", {"attention_kv_out"}},
        {"recurrent_state_out", {"recurrent_state_out"}},
        {"convolution_state_out", {"convolution_state_out"}}}}};
      catalog = {
        {"schema", "ndnsf-di-native-request-catalog-v1"},
        {"model", nativeParseJson(descriptor.canonicalJson())},
        {"source", {{"file", "model.onnx"}, {"data_name", "/fixture/qwen/source"},
          {"digest", sourceDigest}, {"model_manifest_digest", manifestDigest},
          {"canonical_graph_digest", sourceIdentity.graphDigest}}},
        {"recipe", {{"artifact_profile_digest", artifactProfileDigest},
          {"assembler_descriptor_digest", nativePlanningDigest("spec185-qwen-assembler")},
          {"backend_abi", "fixture-abi"}, {"precision", "float32"},
          {"quantization", "none"}, {"layout", "native"}, {"padding", "none"},
          {"protection_epoch", protectionEpoch}, {"max_source_bytes", 1 << 20},
          {"max_assembled_bytes", 1 << 20}, {"max_nodes", 64}}},
        {"publication", {{"artifact_root", "/fixture/qwen/artifacts"}}},
        // The Qwen generation envelope carries tokenizer/state metadata in
        // addition to the inline input; use the maintained native-config
        // bound rather than the compact YOLO fixture's 32-byte probe limit.
        {"input_format", "OPAQUE"}, {"max_payload_bytes", 4096},
        {"conversation_input", {{"kind", "OPAQUE_BYTE_TOKEN_IDS"}}},
        {"splitter", {{"kind", "QWEN"},
          {"layer_ranges", NativeJson::array({NativeJson::array({0, 2})})},
          {"artifact_digests_by_role", {{role, nativePlanningDigest("spec185-qwen-artifact")}}},
          {"weight_bytes_by_role", {{role, 1}}}, {"roles", {role}},
          {"tensor_degrees", {1}}, {"input_ingress_role", role},
          {"result_egress_role", role}}},
        {"node_mapping", {{"embedding", {0}}, {"layer-00", {1}},
          {"layer-01", {2}}, {"final-norm-head", {3, 4, 5, 6}}}},
        {"state_inputs", stateInputs}, {"state_outputs", stateOutputs}};
    }
    else {
      std::ifstream oracleFile("tests/fixtures/spec182/yolo-semantic-oracle.json");
      if (!oracleFile.good()) throw std::runtime_error("missing YOLO semantic oracle");
      NativeJson oracle;
      oracleFile >> oracle;
      const auto hex = oracle.at("model_hex").get<std::string>();
      for (std::size_t i = 0; i < hex.size(); i += 2)
        source.modelBytes.push_back(static_cast<std::uint8_t>(
          std::stoul(hex.substr(i, 2), nullptr, 16)));
      std::ifstream graphFile("tests/fixtures/spec182/onnx-planning-graph-oracle.json");
      if (!graphFile.good()) throw std::runtime_error("missing ONNX graph oracle");
      NativeJson graphOracles;
      graphFile >> graphOracles;
      NativeJson graphOracle;
      for (const auto& item : graphOracles) {
        if (item.at("model_hex") == hex) {
          graphOracle = item;
          break;
        }
      }
      if (graphOracle.is_null()) throw std::runtime_error("missing matching graph oracle");
      descriptor = fixture::completeModel({
        "yolo26n", nativePlanningDigest("YOLOFixture-content"),
        nativePlanningDigest("YOLOFixture-semantics"), oracle.at("graph_digest"),
        "onnx", "float32", "YOLOFixture", "1"});
      const auto sourceIdentity = canonicalOnnxSourceIdentity(source, control);
      const auto planning = inspectNativeOnnxSourceGraph(source, descriptor, control);
      descriptor.graphDigest = planning.graph.graphDigest;
      if (sourceIdentity.graphDigest != graphOracle.at("canonical_graph_digest"))
        throw std::runtime_error("fixture graph identity differs from oracle");
      catalog["schema"] = "ndnsf-di-native-request-catalog-v1";
      catalog["model"] = nativeParseJson(descriptor.canonicalJson());
      catalog["source"] = {
        {"file", "model.onnx"}, {"data_name", "/fixture/source"},
        {"digest", nativePlanningDigest(source.modelBytes.data(), source.modelBytes.size())},
        {"model_manifest_digest", nativePlanningDigest("fixture-manifest")},
        {"canonical_graph_digest", sourceIdentity.graphDigest} };
      catalog["recipe"] = NativeJson::object();
      catalog["recipe"]["artifact_profile_digest"] = nativePlanningDigest("fixture-profile");
      catalog["recipe"]["assembler_descriptor_digest"] = nativePlanningDigest("fixture-assembler-v1");
      catalog["recipe"]["backend_abi"] = "fixture-abi";
      catalog["recipe"]["precision"] = descriptor.precision;
      catalog["recipe"]["quantization"] = "none";
      catalog["recipe"]["layout"] = "NCHW";
      catalog["recipe"]["padding"] = "none";
      catalog["recipe"]["protection_epoch"] = "epoch-1";
      catalog["recipe"]["max_source_bytes"] = 1 << 20;
      catalog["recipe"]["max_assembled_bytes"] = 1 << 20;
      catalog["recipe"]["max_nodes"] = 100;
      catalog["publication"] = {{"artifact_root", "/fixture/artifacts"}};
      catalog["input_format"] = "OPAQUE";
      catalog["max_payload_bytes"] = 32;
      const NativeJson component = {
        {"candidate_id", "semantic-v1"}, {"priority", 1},
        {"roles", {"Front", "Branch", "Merge"}},
        {"node_names_by_role", NativeJson::object()},
        {"input_ingress_role", "Front"}, {"result_egress_role", "Merge"},
        {"merge_kind", ""}, {"candidate_digest", oracle.at("registered_digest")},
        {"semantic_partition", oracle.at("partition")} };
      catalog["splitter"] = { {"kind", "YOLO"}, {"components", {component}} };
    }

    std::ofstream model(root / "model.onnx", std::ios::binary);
    model.write(reinterpret_cast<const char*>(source.modelBytes.data()),
                static_cast<std::streamsize>(source.modelBytes.size()));
    const auto digest = std::string("sha256:") + std::string(64, 'a');
    const auto offerKeyId = publicKeyDigest(root / "offer-public.pem");
    NativeJson offerAdmission = NativeJson::object();
    offerAdmission["policy"] = NativeJson::object();
    offerAdmission["policy"]["schema"] = "spec180-provider-offer-trust-v1";
    offerAdmission["policy"]["candidateId"] = "candidate";
    offerAdmission["policy"]["candidateDigest"] = digest;
    offerAdmission["policy"]["trustSchema"] = "/group/trust";
    offerAdmission["policy"]["entries"] = NativeJson::array({NativeJson{
      {"provider", "/provider"}, {"service", "/Inference"},
      {"keyLocatorPrefix", "/provider/KEY/offer"}, {"signerKeyId", offerKeyId},
      {"certificateName", "/provider/KEY/offer/ID-CERT"}}});
    offerAdmission["public_key_files"] = NativeJson::object();
    offerAdmission["public_key_files"][offerKeyId] = "offer-public.pem";
    offerAdmission["candidate_digest"] = digest;

    // Sanitizer instrumentation makes the cooperative extension boundary
    // substantially slower on this host.  Keep the normal fixture's strict
    // 100 ms policy budget, but give the instrumented C++ selector enough
    // wall-clock budget to exercise the same authenticated path without
    // turning tool overhead into a deadline failure.
#if defined(__SANITIZE_ADDRESS__)
    constexpr std::uint64_t policyBudgetMs = 5000;
#elif defined(__clang__)
#  if __has_feature(address_sanitizer)
    constexpr std::uint64_t policyBudgetMs = 5000;
#  else
    constexpr std::uint64_t policyBudgetMs = 100;
#  endif
#else
    constexpr std::uint64_t policyBudgetMs = 100;
#endif
    NativeJson request = {
      {"service", "/Inference"}, {"task", "task"},
      {"adapter_composition_digest", digest}, {"task_descriptor_digest", digest},
      {"input_layout_digest", digest}, {"security_policy_digest", digest},
      {"max_candidates", 1}, {"max_policy_ms", policyBudgetMs},
      {"generation_mode", streaming ? "TOKEN_STREAMING" : "TOKEN_DIAGNOSTIC"},
      {"timeout_ms", 30000}, {"ack_timeout_ms", 5000}};
    if (streaming) {
      const auto tokenizerDigest = nativePlanningDigest("spec185-fixture-tokenizer");
      request["tokenizer_digest"] = tokenizerDigest;
      request["generation_defaults"] = {
        {"useCache", true}, {"outputMode", "TOKEN_STREAMING"},
        {"maxNewTokens", 32}, {"tokenizerDigest", tokenizerDigest},
        {"eosTokenIds", {2}}, {"samplingMode", "Greedy"},
        {"stateInputNames", {"attention_kv_in", "recurrent_state_in", "convolution_state_in"}},
        {"stateOutputNames", {"attention_kv_out", "recurrent_state_out", "convolution_state_out"}}};
    }
    auto config = NativeJson{
      {"schema", "ndnsf-di-native-requester-v1"},
      {"core", {{"requester_identity", "/user"}, {"authority_identity", "/aa"},
        {"group", "/group"}, {"trust_schema_file", "trust.conf"}}},
      {"catalog", catalog}, {"offer_admission", offerAdmission},
      {"grant", {{"authority_identity", "/aa"}, {"authority_service", "/grant"},
        {"authority_public_key_file", "authority-public.pem"},
        {"requester_private_key_file", "requester.pem"},
        {"protection_epoch", "epoch-1"}}},
      {"limits", {{"bootstrap_ms", 1000}, {"max_source_bytes", 1 << 20},
        {"max_assembled_bytes", 1 << 20}}},
      {"request", request}};
    if (streaming) {
      config["conversation"] = {
        {"schema", "ndnsf-di-native-conversation-v1"},
        {"journal", {{"state_root", "conversation-state"}, {"identity", "runtime"},
          {"keys", NativeJson::array({NativeJson{{"id", "k1"}, {"file", "conversation.key"}}})},
          {"quota_bytes", 1 << 20}, {"test_only_allow_ephemeral_state_root", true}}},
        {"owner", {{"requester_identity", "/user"}, {"service_name", "/Inference"},
          {"security_domain_digest", digest}}}};
    }
    configPath = root / "requester.json";
    std::ofstream output(configPath);
    output << nativeCanonicalJson(config);
  }

  ~RuntimeFixture()
  {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }

  std::filesystem::path write(const std::string& name, const std::string& content)
  {
    const auto path = root / name;
    std::ofstream output(path);
    output << content;
    return path;
  }

  std::filesystem::path root;
  std::filesystem::path configPath;
  inline static std::atomic<unsigned> sequence{0};
};

RuntimeConfig runtimeConfig(const RuntimeFixture& fixture)
{
  RuntimeConfig config;
  config.nativeConfigPath = fixture.configPath.string();
  config.maxPreparedBytes = 4 * 1024 * 1024;
  config.maxPreparedEntries = 2;
  config.preparationJobTimeout = std::chrono::seconds(5);
  return config;
}

struct InProcessRuntimeBinding
{
  // Declared before Runtime in each test so the borrowed ServiceUser and Face
  // remain alive through Runtime destruction.
  std::shared_ptr<ndn_service_framework::test::NdnsfIntegrationEnvironment>
    environment;
  std::shared_ptr<ndn_service_framework::ServiceUser> user;
  std::shared_ptr<NativeAuthenticatedGrantClient> grants;
  std::shared_ptr<const NativeOfferAdmission> admission;
};

std::string readTextFile(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input.good())
    throw std::runtime_error("cannot read Spec185 fixture file: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

InProcessRuntimeBinding
bindInProcessRuntime(RuntimeFixture& fixture, const std::shared_ptr<Runtime>& runtime)
{
  if (!runtime)
    throw std::invalid_argument("Spec185 in-process binding requires Runtime");

  ndn_service_framework::test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/t013/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/t013/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/t013/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  profile.providerRoles = {"/Backbone"};
  auto environment = std::make_shared<
    ndn_service_framework::test::NdnsfIntegrationEnvironment>(profile);
  environment->bootstrap();
  environment->enableProductionIngressForTest();

  // Reuse the exact offer policy and candidate identity frozen in the native
  // requester fixture.  The lifecycle selectors do not need a Provider
  // response, but they must still carry a real immutable admission owner so
  // request construction cannot silently bypass Runtime's validation path.
  const auto config = nativeParseJson(readTextFile(fixture.configPath));
  const auto& offer = config.at("offer_admission");
  std::map<std::string, std::string> publicKeys;
  for (const auto& [keyId, relativePath] : offer.at("public_key_files").items()) {
    publicKeys.emplace(keyId, readTextFile(fixture.root / relativePath.get<std::string>()));
  }
  auto admission = std::make_shared<NativeOfferAdmission>(
    nativeCanonicalJson(offer.at("policy")), std::move(publicKeys),
    offer.at("candidate_digest").get<std::string>());

  // Cancellation and drain cases deliberately stop before ACK/grant
  // acquisition.  Use a real requester-bound grant client with a cancellable
  // transport callback; any accidental late acquisition still observes the
  // production control deadline instead of fabricating a successful grant.
  const auto requesterKey = readTextFile(fixture.root / "requester.pem");
  std::unique_ptr<BIO, decltype(&BIO_free)> requesterBio(
    BIO_new_mem_buf(requesterKey.data(), static_cast<int>(requesterKey.size())), BIO_free);
  std::shared_ptr<EVP_PKEY> requesterPrivate(
    PEM_read_bio_PrivateKey(requesterBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (!requesterPrivate)
    throw std::runtime_error("cannot load Spec185 requester fixture key");
  const auto authorityPublicPem = readTextFile(fixture.root / "authority-public.pem");
  std::unique_ptr<BIO, decltype(&BIO_free)> authorityBio(
    BIO_new_mem_buf(authorityPublicPem.data(), static_cast<int>(authorityPublicPem.size())), BIO_free);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> authorityPublic(
    PEM_read_bio_PUBKEY(authorityBio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
  if (!authorityPublic)
    throw std::runtime_error("cannot load Spec185 authority fixture key");
  std::string authorityPublicRaw(32, '\0');
  std::size_t authorityPublicSize = authorityPublicRaw.size();
  if (EVP_PKEY_get_raw_public_key(
        authorityPublic.get(), reinterpret_cast<unsigned char*>(authorityPublicRaw.data()),
        &authorityPublicSize) != 1 || authorityPublicSize != authorityPublicRaw.size())
    throw std::runtime_error("Spec185 authority fixture key is not Ed25519");
  const auto protectionEpoch = config.at("grant").at("protection_epoch").get<std::string>();
  NativeAuthenticatedGrantClient::Issue issue = [] (
    const NativeSignedGrantRequest&, const std::string&, std::uint64_t,
    const NativeGrantControl& control) {
    control.check();
    return NativeKeyGrant{};
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    environment->user().getName().toUri(), std::move(requesterPrivate),
    environment->profile().attributeAuthority.toUri(), std::move(authorityPublicRaw),
    protectionEpoch, std::move(issue), std::move(publish));

  auto user = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment->user(), [] (ndn_service_framework::ServiceUser*) {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    runtime, user, grants, admission);
  return {std::move(environment), std::move(user), std::move(grants), std::move(admission)};
}

bool drainWithInProcessPump(const InProcessRuntimeBinding& binding,
                            const std::shared_ptr<Runtime>& runtime,
                            std::chrono::seconds timeout)
{
  auto observation = startBooleanResultObservation([runtime, timeout] {
    return runtime->drain(timeout);
  });
  pumpUntilBooleanResultReady(*binding.environment, observation, timeout);
  return observation->get();
}

struct DrainNotificationState
{
  std::mutex mutex;
  std::optional<bool> value;
  std::atomic<bool> ready{false};
};

bool waitForInProcessDrainNotification(
  const InProcessRuntimeBinding& binding, const std::shared_ptr<Runtime>& runtime,
  std::chrono::seconds timeout, Subscription& subscription)
{
  auto state = std::make_shared<DrainNotificationState>();
  subscription = runtime->drainAsync(
    timeout, [state] (std::exception_ptr error, bool drained) {
      {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->value = !error && drained;
      }
      state->ready.store(true, std::memory_order_release);
    });
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (!state->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    binding.environment->pumpUntil([] { return false; });
  }
  if (!state->ready.load(std::memory_order_acquire))
    throw std::runtime_error("Spec185 drainAsync notification pump deadline expired");
  std::lock_guard<std::mutex> lock(state->mutex);
  return state->value.value_or(false);
}

ProviderConfig
preparedProviderConfig(const std::string& serviceName,
                       const std::vector<std::string>& roles)
{
  std::vector<std::string> values{
    "provider", "--provider", "/provider", "--group", "/group",
    "--controller", "/controller", "--trust-schema",
    std::filesystem::absolute("examples/trust-any.conf").string(),
    "--service", serviceName, "--workers", "1"};
  for (const auto& role : roles) {
    values.push_back("--role");
    values.push_back(role);
  }
  std::vector<const char*> argv;
  argv.reserve(values.size());
  for (const auto& value : values)
    argv.push_back(value.c_str());
  return ProviderConfig::fromCommandLine(static_cast<int>(argv.size()), argv.data());
}

std::shared_ptr<NativeModelRunnerFactory>
makePreparedServedProviderRunnerFactory(std::shared_ptr<std::atomic<unsigned>> runs)
{
  auto factory = std::make_shared<RegistryNativeModelRunnerFactory>();
  const auto creator = [runs] (const NativeModelRunnerSpec& spec) {
    const bool nativeMerge = spec.backend == "native-yolo-postprocess";
    ExecutionEvidence evidence;
    evidence.providerName = spec.metadata.at("provider");
    evidence.providerBootId = spec.metadata.at("boot");
    evidence.evidenceEpoch = 1;
    evidence.runnerKind = nativeMerge ? RunnerKind::NativeYoloPostprocess
                                      : RunnerKind::OnnxRuntimeCpu;
    evidence.realCompute = !nativeMerge;
    evidence.deviceKind = "cpu";
    evidence.deviceId = "0";
    evidence.deviceIds = {"0"};
    evidence.runtimeVersion = "spec185-prepared-served-provider";
    evidence.modelDigest = spec.metadata.at("artifact");
    evidence.planDigest = spec.metadata.at("plan");
    evidence.artifactDigests[spec.role] = spec.metadata.at("artifact");
    evidence.roles = {spec.role};
    evidence.loadCompleted = !nativeMerge;
    evidence.warmupCompleted = !nativeMerge;
    evidence.createdAtMs = 1;
    evidence.validate();
    return makeNativeModelRunner(
      [runs] (const RoleExecutionContext&) {
        runs->fetch_add(1, std::memory_order_relaxed);
        const std::string response = "spec185-provider-response";
        return std::map<std::string, TensorBundle>{
          {"final-response", TensorBundle{
            "final-response",
            std::vector<std::uint8_t>(response.begin(), response.end()),
            1, response.size()}}};
      },
      std::move(evidence));
  };
  factory->registerBackend("onnxruntime-cpu", creator);
  factory->registerBackend("native-yolo-postprocess", creator);
  factory->freeze();
  return factory;
}

NativeProviderHandlerConfig::RunnerPreparationFactory
makePreparedServedProviderPreparation()
{
  return [] (ndn_service_framework::ServiceProvider::CollaborationContext&,
             const NativeSelectionProjectionV3& projection,
             const std::shared_ptr<ProtectedRuntime>& protectedRuntime) {
    if (!protectedRuntime ||
        protectedRuntime->state() != ProtectedRuntimeState::GrantVerified)
      throw std::runtime_error("Spec185 served Provider did not verify grant");
    const auto& assembly = projection.assembly;
    NativeModelRunnerSpec spec;
    spec.role = assembly.selectedRole;
    spec.metadata["provider"] = projection.provider;
    spec.metadata["boot"] = protectedRuntime->binding().providerBootId;
    spec.metadata["plan"] = projection.planDigest;
    spec.metadata["artifact"] = assembly.artifactDigest;
    spec.metadata["fragmentDigest"] = assembly.artifactDigest;
    spec.metadata["recipeDigest"] = assembly.recipeDigest;
    if (assembly.mergeKind == "NATIVE_POSTPROCESS") {
      spec.kind = "native-yolo-postprocess";
      spec.backend = "native-yolo-postprocess";
      std::string shape;
      if (!assembly.expectedOutputs.empty()) {
        for (const auto& dimension : assembly.expectedOutputs.front().shape) {
          if (!shape.empty()) shape += ',';
          shape += std::holds_alternative<std::int64_t>(dimension)
            ? std::to_string(std::get<std::int64_t>(dimension))
            : std::get<std::string>(dimension);
        }
      }
      spec.metadata["mergeKind"] = assembly.mergeKind;
      spec.metadata["postprocessIdentity"] = assembly.postprocessIdentity;
      spec.metadata["postprocessOutputName"] = assembly.postprocessOutputName;
      spec.metadata["postprocessSort"] = assembly.postprocessSort;
      spec.metadata["expectedOutputShape"] = shape;
      spec.metadata["postprocessConfidenceThreshold"] =
        std::to_string(assembly.postprocessConfidenceThreshold);
    }
    else {
      spec.kind = "onnx-model";
      spec.backend = assembly.backend;
      spec.path = (std::filesystem::temp_directory_path() / "model.onnx").string();
      spec.metadata["modelManifestDigest"] = assembly.modelManifestDigest;
      spec.metadata["artifactProfileDigest"] = assembly.artifactProfileDigest;
      spec.metadata["graphDigest"] = assembly.graphDigest;
      spec.metadata["canonicalInitializerDigest"] = assembly.canonicalInitializerDigest;
      spec.metadata["adapterDescriptorDigest"] = assembly.adapterDescriptorDigest;
      spec.metadata["assemblerDescriptorDigest"] = assembly.assemblerDescriptorDigest;
      spec.metadata["backendAbi"] = assembly.backendAbi;
      spec.metadata["precision"] = assembly.precision;
      spec.metadata["quantization"] = assembly.quantization;
      spec.metadata["layout"] = assembly.layout;
      spec.metadata["padding"] = assembly.padding;
      spec.metadata["maxSourceBytes"] = std::to_string(assembly.maxSourceBytes);
      spec.metadata["maxAssembledBytes"] = std::to_string(assembly.maxAssembledBytes);
      spec.metadata["maxNodes"] = std::to_string(assembly.maxNodes);
    }
    return spec;
  };
}

} // namespace

namespace ndnsf::di {

struct Spec185PreparedModelTestAccess
{
  static std::shared_ptr<const PreparedModelPackage>
  package(const PreparedModel& model)
  {
    return model.m_package;
  }

  static std::weak_ptr<const NativeCanonicalSource>
  source(const PreparedModel& model)
  {
    return model.m_package->catalog.preparation->sourceLifetimeForTest(
      model.m_package->catalog.model.descriptor);
  }

  static PreparedModel
  bind(const PreparedModel& model,
       std::function<std::shared_ptr<NativeInferenceClient>(
         const std::shared_ptr<const PreparedModelPackage>&)> factory)
  {
    return PreparedModel(model.m_package, model.m_receipt, model.m_lease,
                         std::move(factory));
  }
};

} // namespace ndnsf::di

BOOST_AUTO_TEST_SUITE(Spec185PreparedRequest)

BOOST_AUTO_TEST_CASE(PreparedRequestsSharePackageButAllocateIndependentIds)
{
  RuntimeFixture fixture;
  InProcessRuntimeBinding binding;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  binding = bindInProcessRuntime(fixture, runtime);
  auto prepared = runtime->user().prepare();
  // This request goes through Runtime::prepare's production client factory;
  // the immediate cancellation keeps this identity probe independent of a
  // Provider response while still using the fixture-owned real Face.
  const auto bytes = Input::inlineBytes({0x01, 0x02, 0x03});
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto first = prepared.request(bytes, options);
  auto second = prepared.request(bytes, options);
  BOOST_REQUIRE(!first.id().empty());
  BOOST_REQUIRE(!second.id().empty());
  BOOST_CHECK(first.id() != second.id());
  BOOST_CHECK(first.status() == RequestStatus::Pending ||
              first.status() == RequestStatus::Failed);
  BOOST_CHECK(second.status() == RequestStatus::Pending ||
              second.status() == RequestStatus::Failed);
  RequestOptions unsupportedMode;
  unsupportedMode.timeout = std::chrono::milliseconds(500);
  unsupportedMode.ackTimeout = std::chrono::milliseconds(50);
  unsupportedMode.outputMode = "TOKEN_STREAMING";
  BOOST_CHECK_EXCEPTION(prepared.request(bytes, unsupportedMode), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY";
                        });
  first.cancel();
  second.cancel();
  BOOST_CHECK(first.status() == RequestStatus::Cancelled ||
              first.status() == RequestStatus::Failed);
  BOOST_CHECK(second.status() == RequestStatus::Cancelled ||
              second.status() == RequestStatus::Failed);
  runtime->close();
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, testDrainTimeout()));
}

BOOST_AUTO_TEST_CASE(StreamingRequestReaderPreservesTerminalEventAcrossCancel)
{
  RuntimeFixture fixture(true);
  InProcessRuntimeBinding binding;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  binding = bindInProcessRuntime(fixture, runtime);
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  options.stream = StreamOptions{true};
  auto handle = prepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  auto reader = handle.events();
  BOOST_CHECK_EXCEPTION(reader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "WAIT_TIMEOUT" &&
                                 error.boundary() == "wait";
                        });
  auto cancelledCallbacks = std::make_shared<std::atomic<unsigned>>(0);
  auto pendingRead = reader.nextAsync(
    std::chrono::seconds(2),
    [cancelledCallbacks] (std::exception_ptr, std::optional<Event>) {
      cancelledCallbacks->fetch_add(1, std::memory_order_relaxed);
    });
  BOOST_CHECK_EXCEPTION(
    reader.nextAsync(std::chrono::seconds(2),
      [] (std::exception_ptr, std::optional<Event>) {}),
    DiError,
    [] (const DiError& error) {
      return error.code() == "READ_IN_PROGRESS" && error.boundary() == "events";
    });
  pendingRead.unsubscribe();
  auto movedReader = std::move(reader);
  BOOST_CHECK_EXCEPTION(reader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "READER_CLOSED" &&
                                 error.boundary() == "events";
                        });
  handle.cancel();
  const auto terminal = movedReader.next(std::chrono::seconds(2));
  BOOST_REQUIRE(terminal);
  BOOST_CHECK(terminal->terminal);
  BOOST_CHECK_EQUAL(terminal->requestId, handle.id());
  BOOST_CHECK_EQUAL(terminal->sequence, 1U);
  BOOST_CHECK_EQUAL(cancelledCallbacks->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EXCEPTION(movedReader.next(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CANCELLED" &&
                                 error.boundary() == "request";
                        });
  movedReader.close();
  // Release the reader and request state before draining the owning Runtime.
  // Keeping either handle alive here retains the factory-created native client
  // and its external Face while later tests run in the same process.
  pendingRead = Subscription{};
  movedReader = EventReader{};
  handle = RequestHandle{};
  runtime->close();
  // Runtime close includes the native client and Face cleanup boundary used
  // by C-04; use the qualification budget instead of a shorter fixture-only
  // bound so a valid cancellation drain cannot be rejected under load.
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, testDrainTimeout()));
}

BOOST_AUTO_TEST_CASE(VerifiedStreamingDefaultsMaterializeWithoutExplicitOptions)
{
  RuntimeFixture fixture(true);
  InProcessRuntimeBinding binding;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  binding = bindInProcessRuntime(fixture, runtime);
  auto prepared = runtime->user().prepare();
  BOOST_REQUIRE(prepared.capabilities().streaming);
  BOOST_CHECK(std::find(prepared.capabilities().outputModes.begin(),
                        prepared.capabilities().outputModes.end(),
                        "TOKEN_STREAMING") != prepared.capabilities().outputModes.end());
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  // The verified task contract supplies stream and generation defaults; an
  // ordinary caller need not duplicate those operator-owned fields.
  auto handle = prepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  BOOST_REQUIRE(!handle.id().empty());
  handle.cancel();
  BOOST_CHECK(handle.status() == RequestStatus::Cancelled ||
              handle.status() == RequestStatus::Failed);
  runtime->close();
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, testDrainTimeout()));
}

BOOST_AUTO_TEST_CASE(PreparedRequestCompletesThroughCoreCollaborationFixture)
{
  RuntimeFixture fixture;
  ndn_service_framework::test::BootstrapProfile profile;
  // Match the operator-pinned Runtime identity/group so the production
  // factory's frozen request contract and the fixture ServiceUser are the
  // same principal after the internal transport override.
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/t005/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/t005/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/t005/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  // Construct the fixture before Runtime so the borrowed test transport
  // remains alive until the Runtime owner has completed its drain fence.
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  const auto package = ndnsf::di::Spec185PreparedModelTestAccess::package(prepared);

  const auto serviceName = environment.profile().serviceName.toUri();
  const auto requesterName = environment.user().getName().toUri();
  const auto providerName = environment.provider().getName().toUri();
  const auto model = package->catalog.model.descriptor;
  const auto candidates = package->catalog.splitter->enumerate(
    model, package->catalog.model.graph, NativeCandidateBudget{1, 1000, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto roles = candidates.front().executionPlan.roles;
  BOOST_REQUIRE(!roles.empty());

  const auto offerKey = deterministicEd25519Key(0x31);
  const auto offerKeyId = nativePlanningDigest(rawPublicKey(offerKey));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + environment.provider().getProviderBootEpoch();
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = roles;
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.hasModel = true;
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return signDigest(offerKey, value);
  };
  const auto candidatePolicyDigest = nativePlanningDigest("spec185-t005-provider-policy");
  const auto policy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "spec185-t005"}, {"candidateDigest", candidatePolicyDigest},
    {"trustSchema", "/spec185/t005/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider().getSigningCertificateName().toUri()}}})}});
  auto admission = std::make_shared<NativeOfferAdmission>(
    policy, std::map<std::string, std::string>{{offerKeyId, publicKeyPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = deterministicEd25519Key(0x41);
  const auto authorityKey = deterministicEd25519Key(0x51);
  const auto recipientKey = deterministicEd25519Key(0x61);
  const auto registration = nativeParseJson(package->registration->configurationJson);
  const auto protectionEpoch = registration.at("grant").at(
    "protection_epoch").get<std::string>();
  const auto authorityName = environment.profile().attributeAuthority.toUri();
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = authorityName;
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-t005-grant-key";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {package->catalog.model.modelManifestDigest};
  // The requester grants against the request-scoped published root digest;
  // authorize that root to resolve back to the package's immutable source.
  issuerConfig.publicationSources.emplace(
    package->catalog.model.modelManifestDigest,
    NativeGrantPublicationSource{
      package->catalog.model.descriptor.modelName,
      package->catalog.model.descriptor.contentDigest,
      package->catalog.model.canonicalSourceDigest,
      package->catalog.model.canonicalInitializerObjectDigest,
      nativePlanningDigest("fixture-profile")});
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  NativeAuthenticatedGrantClient::Issue issue = [grantIssuer] (
    const NativeSignedGrantRequest& request, const std::string& publishedManifest,
    std::uint64_t expiresAtMs, const NativeGrantControl& control) {
    control.check();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return grantIssuer->issue(request, nowMs, expiresAtMs, publishedManifest);
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, authorityName, rawPublicKey(authorityKey),
    protectionEpoch, std::move(issue), std::move(publish));

  auto ackCount = std::make_shared<std::atomic<unsigned>>(0);
  auto responseCount = std::make_shared<std::atomic<unsigned>>(0);
  auto wrongDigestRejected = std::make_shared<std::atomic<bool>>(false);
  auto observationMutex = std::make_shared<std::mutex>();
  auto ackRequestIds = std::make_shared<std::set<std::string>>();
  auto selectionByRequest = std::make_shared<std::map<std::string, std::string>>();
  auto grantRequestIds = std::make_shared<std::map<std::string, std::set<std::string>>>();
  const auto acceptedRepositoryDigest = nativePlanningDigest("spec185-t005-repository-ciphertext");
  environment.provider().addCollaborationHandler(
    ndn::Name(serviceName),
    [offerConfig, acceptedRepositoryDigest, wrongDigestRejected, ackCount,
     observationMutex, ackRequestIds] (
      const ndn_service_framework::RequestMessage& request) {
      ackCount->fetch_add(1, std::memory_order_relaxed);
      const auto payload = request.getPayload();
      try {
        const auto root = nativeParseJson(std::string(payload.begin(), payload.end()));
        const auto requestId = root.value("request_id", std::string{});
        if (root.value("input_transport", std::string{}) == "REPO_REF") {
          const auto reference = root.value("input_reference", NativeJson::object());
          if (reference.value("ciphertextDigest", std::string{}) != acceptedRepositoryDigest) {
            wrongDigestRejected->store(true, std::memory_order_release);
            ndn_service_framework::ServiceProvider::AckDecision decision;
            decision.message = "DI_INPUT_DIGEST_REJECTED";
            return decision;
          }
        }
        if (!requestId.empty()) {
          std::lock_guard<std::mutex> lock(*observationMutex);
          ackRequestIds->insert(requestId);
        }
      }
      catch (...) {
        // The production offer parser below remains authoritative.
      }
      const auto issued = issueNativeProviderOfferV3(
        std::vector<std::uint8_t>(payload.begin(), payload.end()), offerConfig,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      ndn_service_framework::ServiceProvider::AckDecision decision;
      if (!issued || !issued->status) {
        decision.message = issued ? issued->message : "DI_NATIVE_REQUEST_NOT_V3";
        return decision;
      }
      decision.status = true;
      decision.message = issued->message;
      decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
      decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      return decision;
    },
    [responseCount, observationMutex, selectionByRequest, grantRequestIds]
      (ndn_service_framework::ServiceProvider::CollaborationContext& context,
       const ndn_service_framework::RequestMessage&) {
      const auto assignment = context.assignment().assignmentPayload;
      const auto root = nativeParseJson(std::string(assignment.begin(), assignment.end()));
      const auto requestId = root.value("request_id", std::string{});
      const auto selectionDigest = context.assignment().selectionDigest;
      const auto grant = root.value("grant_binding", NativeJson::object());
      const auto grantRequestId = grant.value("request_id", std::string{});
      bool firstResponseForRequest = false;
      {
        std::lock_guard<std::mutex> lock(*observationMutex);
        firstResponseForRequest = selectionByRequest->emplace(requestId, selectionDigest).second;
        (*grantRequestIds)[requestId].insert(grantRequestId);
      }
      const std::string responseText = "spec185-provider-response";
      const ndn::Buffer payload(reinterpret_cast<const std::uint8_t*>(responseText.data()),
                                responseText.size());
      context.publishFinalResponse(payload);
      if (firstResponseForRequest)
        responseCount->fetch_add(1, std::memory_order_relaxed);
    });

  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  // Keep the Provider fixture's transport and grant owner, but invoke the
  // production Runtime::makeRuntimeClient factory through the prepared
  // package's frozen clientFactory.  This test-only override is internal and
  // never changes the public Runtime/PreparedModel API or its owner registry.
  auto environmentUser = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    runtime, environmentUser, grants, admission);
  auto providerPrepared = prepared;

  RequestOptions options;
  // Two concurrent selections externalize three assignment payloads each;
  // keep the fixture's ACK budget above that observed transport work while
  // retaining a bounded request deadline for the negative paths below.
  options.timeout = std::chrono::seconds(10);
  options.ackTimeout = std::chrono::seconds(5);
  auto first = providerPrepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  auto second = providerPrepared.request(Input::inlineBytes({0x04, 0x05, 0x06}), options);
  BOOST_REQUIRE_NE(first.id(), second.id());
  const auto firstId = first.id();
  BOOST_REQUIRE(first.status() == RequestStatus::Pending);
  BOOST_CHECK_EXCEPTION(first.events(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "events";
                        });
  BOOST_CHECK_EXCEPTION(first.result(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "LOCAL_WAIT_TIMEOUT" &&
                                 error.boundary() == "wait";
                        });

  auto observedTerminals = std::make_shared<std::atomic<unsigned>>(0);
  auto cancelledObservations = std::make_shared<std::atomic<unsigned>>(0);
  auto observation = first.observe(
    [observedTerminals, firstId] (const Event& event) {
      if (event.terminal && event.requestId == firstId)
        observedTerminals->fetch_add(1, std::memory_order_relaxed);
    });
  auto cancelledObservation = first.observe(
    [cancelledObservations] (const Event&) {
      cancelledObservations->fetch_add(1, std::memory_order_relaxed);
    });
  cancelledObservation.unsubscribe();
  auto completionPromise = std::make_shared<std::promise<bool>>();
  auto completionSet = std::make_shared<std::atomic<bool>>(false);
  auto completionSubscription = first.onCompletion(
    [completionPromise, completionSet, firstId]
    (std::exception_ptr error, std::optional<Result> result) {
      if (!completionSet->exchange(true, std::memory_order_acq_rel))
        completionPromise->set_value(!error && result && result->requestId == firstId);
    });
  auto resultAsyncPromise = std::make_shared<std::promise<bool>>();
  auto resultAsyncSet = std::make_shared<std::atomic<bool>>(false);
  auto resultAsyncSubscription = first.resultAsync(
    std::chrono::seconds(10),
    [resultAsyncPromise, resultAsyncSet, firstId]
    (std::exception_ptr error, std::optional<Result> result) {
      if (!resultAsyncSet->exchange(true, std::memory_order_acq_rel))
        resultAsyncPromise->set_value(!error && result && result->requestId == firstId);
    });
  auto timedWaitPromise = std::make_shared<std::promise<bool>>();
  auto timedWaitSet = std::make_shared<std::atomic<bool>>(false);
  auto timedWaitSubscription = second.resultAsync(
    std::chrono::milliseconds(1),
    [timedWaitPromise, timedWaitSet]
    (std::exception_ptr error, std::optional<Result> result) {
      if (timedWaitSet->exchange(true, std::memory_order_acq_rel))
        return;
      bool timedOut = !result;
      try {
        if (error) std::rethrow_exception(error);
      }
      catch (const DiError& value) {
        timedOut = timedOut && value.code() == "WAIT_TIMEOUT" &&
          value.boundary() == "wait";
      }
      catch (...) {
        timedOut = false;
      }
      timedWaitPromise->set_value(timedOut);
    });
  // Completion and timed-result waiters share one bounded native slot pool.
  // Release one slot and prove that a new subscription can reuse it.
  std::vector<Subscription> extraSubscriptions;
  extraSubscriptions.reserve(61);
  for (unsigned index = 0; index < 61; ++index) {
    extraSubscriptions.push_back(first.onCompletion(
      [] (std::exception_ptr, std::optional<Result>) {}));
  }
  BOOST_CHECK_EXCEPTION(
    first.onCompletion([] (std::exception_ptr, std::optional<Result>) {}), DiError,
    [] (const DiError& error) {
      return error.code() == "SUBSCRIPTION_LIMIT" && error.boundary() == "completion";
    });
  extraSubscriptions.back().unsubscribe();
  auto recycledSubscription = first.onCompletion(
    [] (std::exception_ptr, std::optional<Result>) {});

  auto firstObservation = startConversationResultObservation(first, std::chrono::seconds(10));
  auto secondObservation = startConversationResultObservation(second, std::chrono::seconds(10));
  // pumpUntil is intentionally bounded so setup-only callers cannot spin
  // forever.  A deferred collaboration's ACK window may expire at the end of
  // one bounded pump, so continue with another bounded pump before joining
  // the workers.  This keeps the test driver alive through the real Face
  // scheduler boundary without relying on std::future's shared state.
  pumpUntilConversationResultsReady(environment, firstObservation, secondObservation,
                                    std::chrono::seconds(10));
  const auto firstResult = firstObservation->get();
  const auto secondResult = secondObservation->get();
  auto completionReady = completionPromise->get_future();
  auto resultAsyncReady = resultAsyncPromise->get_future();
  auto timedWaitReady = timedWaitPromise->get_future();
  BOOST_REQUIRE(completionReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_REQUIRE(resultAsyncReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_REQUIRE(timedWaitReady.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
  BOOST_CHECK(completionReady.get());
  BOOST_CHECK(resultAsyncReady.get());
  BOOST_CHECK(timedWaitReady.get());
  BOOST_CHECK_EQUAL(observedTerminals->load(std::memory_order_relaxed), 1U);
  BOOST_CHECK_EQUAL(cancelledObservations->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EQUAL(std::string(firstResult.payload.begin(), firstResult.payload.end()),
                    "spec185-provider-response");
  BOOST_CHECK_EQUAL(firstResult.requestId, firstId);
  BOOST_CHECK_EQUAL(firstResult.modelDigest, model.intentDigest());
  BOOST_CHECK(!firstResult.planDigest.empty());
  BOOST_CHECK_EQUAL(std::string(secondResult.payload.begin(), secondResult.payload.end()),
                    "spec185-provider-response");
  BOOST_CHECK_EQUAL(secondResult.modelDigest, model.intentDigest());
  BOOST_CHECK(!secondResult.planDigest.empty());
  BOOST_CHECK_EQUAL(ackCount->load(std::memory_order_relaxed), 2U);
  BOOST_CHECK_EQUAL(responseCount->load(std::memory_order_relaxed), 2U);
  {
    std::lock_guard<std::mutex> lock(*observationMutex);
    BOOST_CHECK_EQUAL(ackRequestIds->size(), 2U);
    BOOST_CHECK_EQUAL(selectionByRequest->size(), 2U);
    BOOST_CHECK_EQUAL(grantRequestIds->size(), 2U);
    for (const auto& item : *grantRequestIds) {
      BOOST_CHECK_EQUAL(item.second.size(), 1U);
      BOOST_CHECK_EQUAL(*item.second.begin(), item.first);
      BOOST_CHECK(!selectionByRequest->at(item.first).empty());
    }
  }

  // The public blocking API must traverse the same production requester,
  // authenticated ACK/Selection, Provider execution, response decoding and
  // Runtime-owned client lifetime as request().  Run it in a C++ worker while
  // this fixture pumps the real Face; a local fake client would miss the
  // scheduling and transport boundaries that this regression protects.
  auto runObservation = startPreparedRunObservation(
    providerPrepared, Input::inlineBytes({0x0a, 0x0b, 0x0c}), options);
  pumpUntilPreparedRunReady(environment, runObservation, std::chrono::seconds(10));
  const auto runResult = runObservation->get();
  BOOST_CHECK_EQUAL(std::string(runResult.payload.begin(), runResult.payload.end()),
                    "spec185-provider-response");
  BOOST_REQUIRE(!runResult.requestId.empty());
  BOOST_CHECK_EQUAL(runResult.modelDigest, model.intentDigest());
  BOOST_CHECK(!runResult.planDigest.empty());
  BOOST_CHECK_EQUAL(ackCount->load(std::memory_order_relaxed), 3U);
  BOOST_CHECK_EQUAL(responseCount->load(std::memory_order_relaxed), 3U);
  {
    std::lock_guard<std::mutex> lock(*observationMutex);
    BOOST_CHECK_EQUAL(ackRequestIds->size(), 3U);
    BOOST_CHECK_EQUAL(selectionByRequest->size(), 3U);
    BOOST_CHECK_EQUAL(grantRequestIds->size(), 3U);
    BOOST_CHECK(grantRequestIds->count(runResult.requestId) == 1U);
    BOOST_CHECK(selectionByRequest->count(runResult.requestId) == 1U);
  }

  // A repository reference keeps its complete canonical metadata on the
  // request wire.  The Provider fixture applies the same authenticated
  // content-digest gate used by its data owner and must reject a hot request
  // whose ciphertext digest does not match the object bound to this service.
  const NativeJson wrongReference = {
    {"authorizationScope", "/group/di"},
    {"ciphertextDigest", "sha256:" + std::string(64, 'b')},
    {"dataName", "/group/data/spec185-t005"}, {"encrypted", true},
    {"manifestDigest", "sha256:" + std::string(64, 'c')},
    {"plaintextSize", 3}, {"protectionEpoch", "fixture-epoch"} };
  auto wrongDigestRequest = providerPrepared.request(
    Input::repository(DataRef::fromPublishedMetadata(nativeCanonicalJson(wrongReference))), options);
  auto wrongDigestObservation = startConversationResultObservation(
    wrongDigestRequest, std::chrono::seconds(5));
  pumpUntilConversationResultReady(environment, wrongDigestObservation,
                                   std::chrono::seconds(5));
  bool wrongDigestFailed = false;
  try {
    (void)wrongDigestObservation->get();
  }
  catch (const std::exception&) {
    wrongDigestFailed = true;
  }
  BOOST_CHECK(wrongDigestFailed);
  BOOST_CHECK(wrongDigestRejected->load(std::memory_order_acquire));

  // Advance the installed Controller status after the grant/response keys
  // are already hot.  A subsequent request must fail closed instead of
  // reusing the cached authorization material for a revoked identity.
  const auto now = static_cast<std::uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count());
  const auto currentVersion = environment.user().getControllerVersion();
  const auto nextEpoch = currentVersion ? currentVersion->controllerEpoch + 1 : 2;
  ndn_service_framework::PolicyStatusData revokedStatus;
  revokedStatus.setServiceName(ndn::Name(serviceName));
  revokedStatus.setControllerVersion(ndn_service_framework::ControllerVersion{now, nextEpoch});
  revokedStatus.setValidity(now - 1000, now + 120000);
  revokedStatus.setPolicyDigest("sha256:" + std::string(64, '0'));
  revokedStatus.setControllerCertificate(ndn::Name("/spec185/t005/controller"));
  ndn_service_framework::RevocationTarget revokedUser;
  revokedUser.kind = ndn_service_framework::RevocationKind::IDENTITY;
  revokedUser.targetIdentity = ndn::Name(requesterName);
  revokedStatus.addRevocation(revokedUser);
  BOOST_REQUIRE(environment.user().installControllerStatus(revokedStatus));

  auto revokedRequest = providerPrepared.request(Input::inlineBytes({0x06, 0x07, 0x08}), options);
  auto revokedObservation = startConversationResultObservation(
    revokedRequest, std::chrono::seconds(5));
  pumpUntilConversationResultReady(environment, revokedObservation,
                                   std::chrono::seconds(5));
  bool revokedFailed = false;
  try {
    (void)revokedObservation->get();
  }
  catch (const std::exception&) {
    revokedFailed = true;
  }
  BOOST_CHECK(revokedFailed);
  BOOST_CHECK(revokedRequest.status() == RequestStatus::Failed ||
              revokedRequest.status() == RequestStatus::Cancelled);

  // The status advance intentionally re-arms the fixture User's NAC DKEY
  // fetch.  Finish that real AA/User exchange before destroying the borrowed
  // Face; otherwise SegmentFetcher retains pending interests into the
  // sanitizer exit path even though the request itself is terminal.
  bool userReadyAfterRevocation = false;
  environment.pumpUntilWithAttributeAuthority([&] {
    userReadyAfterRevocation = environment.user().isNacConsumerReadyForTest();
    return userReadyAfterRevocation;
  });
  BOOST_CHECK(userReadyAfterRevocation);

  runtime->close();
  // This test-only binding borrows the integration environment's Face.  Run
  // the Runtime drain concurrently while pumping that borrowed transport so
  // terminal collaboration cleanup reaches the same external I/O owner.
  auto drainObservation = startBooleanResultObservation([runtime] {
    return runtime->drain(std::chrono::seconds(2));
  });
  pumpUntilBooleanResultReady(environment, drainObservation, std::chrono::seconds(2));
  BOOST_CHECK(drainObservation->get());
}

BOOST_AUTO_TEST_CASE(PreparedRequestCompletesThroughServedProvider)
{
  // This is the production-chain counterpart to the Core collaboration
  // fixture above.  The ACK strategy only supplies the authenticated V3 offer;
  // the selected request is handled by Provider::serve and its native
  // assembly/runner pipeline.
  RuntimeFixture fixture(false, true);
  ndn_service_framework::test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/served-provider/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/served-provider/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/served-provider/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  profile.providerRoles = {"/LLM/Pipeline/Stage/0"};
  profile.deferBridgeDelivery = true;
  profile.providerFacesHaveDedicatedIoWorkers = true;
  auto environment = std::make_shared<
    ndn_service_framework::test::NdnsfIntegrationEnvironment>(profile);
  environment->bootstrap();

  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  const auto package = ndnsf::di::Spec185PreparedModelTestAccess::package(prepared);
  const auto model = package->catalog.model.descriptor;
  const auto candidates = package->catalog.splitter->enumerate(
    model, package->catalog.model.graph, NativeCandidateBudget{1, 1000, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto roles = candidates.front().executionPlan.roles;
  BOOST_REQUIRE(!roles.empty());
  BOOST_REQUIRE(std::all_of(roles.begin(), roles.end(), [] (const std::string& role) {
    return !role.empty() && role.front() == '/';
  }));

  const auto serviceName = environment->profile().serviceName.toUri();
  const auto requesterName = environment->user().getName().toUri();
  const auto providerName = environment->provider().getName().toUri();
  const auto providerBootId = environment->provider().getProviderBootEpoch();
  const auto offerKey = deterministicEd25519Key(0x31);
  const auto offerKeyId = nativePlanningDigest(rawPublicKey(offerKey));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + providerBootId;
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = roles;
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.hasModel = true;
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return signDigest(offerKey, value);
  };
  const auto candidatePolicyDigest = nativePlanningDigest("spec185-served-provider-policy");
  const auto policy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "spec185-served-provider"},
    {"candidateDigest", candidatePolicyDigest},
    {"trustSchema", "/group/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment->provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment->provider().getSigningCertificateName().toUri()}}})}});
  auto admission = std::make_shared<NativeOfferAdmission>(
    policy, std::map<std::string, std::string>{{offerKeyId, publicKeyPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = deterministicEd25519Key(0x41);
  const auto authorityKey = deterministicEd25519Key(0x51);
  const auto recipientKey = deterministicEd25519Key(0x61);
  const auto registration = nativeParseJson(package->registration->configurationJson);
  const auto protectionEpoch = registration.at("grant").at(
    "protection_epoch").get<std::string>();
  const auto authorityName = environment->profile().attributeAuthority.toUri();
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = authorityName;
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-served-provider-grant";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {package->catalog.model.modelManifestDigest};
  issuerConfig.publicationSources.emplace(
    package->catalog.model.modelManifestDigest,
    NativeGrantPublicationSource{
      package->catalog.model.descriptor.modelName,
      package->catalog.model.descriptor.contentDigest,
      package->catalog.model.canonicalSourceDigest,
      package->catalog.model.canonicalInitializerObjectDigest,
      nativePlanningDigest("fixture-profile")});
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  auto issuedGrants = std::make_shared<std::map<std::string, NativeKeyGrant>>();
  auto issuedGrantsMutex = std::make_shared<std::mutex>();
  NativeAuthenticatedGrantClient::Issue issue = [grantIssuer, issuedGrants,
                                                   issuedGrantsMutex] (
    const NativeSignedGrantRequest& request, const std::string& publishedManifest,
    std::uint64_t expiresAtMs, const NativeGrantControl& control) {
    control.check();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    auto grant = grantIssuer->issue(request, nowMs, expiresAtMs, publishedManifest);
    {
      std::lock_guard<std::mutex> lock(*issuedGrantsMutex);
      (*issuedGrants)[grant.grantDigest] = grant;
    }
    return grant;
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, authorityName, rawPublicKey(authorityKey),
    protectionEpoch, std::move(issue), std::move(publish));

  const auto providerIdentity = environment->keyChain().getPib().getIdentity(
    environment->provider().getName());
  const auto providerCertificate = providerIdentity.getDefaultKey().getDefaultCertificate();
  const auto authorityIdentity = environment->keyChain().getPib().getIdentity(
    environment->profile().attributeAuthority);
  const auto authorityCertificate = authorityIdentity.getDefaultKey().getDefaultCertificate();
  const auto providerConfig = preparedProviderConfig(serviceName, roles);
  auto runs = std::make_shared<std::atomic<unsigned>>(0);
  auto preparation = makePreparedServedProviderPreparation();
  auto protectedFactory = [issuedGrants, issuedGrantsMutex, authorityName,
                           authorityKey, recipientKey, providerBootId] (
    ndn_service_framework::ServiceProvider::CollaborationContext&,
    const NativeSelectionProjectionV3& projection,
    const std::shared_ptr<ProviderGroupCoordinator>&) {
    NativeKeyGrant grant;
    {
      std::lock_guard<std::mutex> lock(*issuedGrantsMutex);
      const auto found = issuedGrants->find(projection.grantDigest);
      if (found == issuedGrants->end())
        throw std::runtime_error("Spec185 served Provider grant publication missing");
      grant = found->second;
    }
    ProtectedRuntimeBindingV1 binding;
    binding.provider = projection.provider;
    binding.role = projection.executionRole.roleId;
    binding.requestId = projection.requestId;
    binding.attempt = projection.attempt;
    binding.planCoreDigest = projection.planCoreDigest;
    binding.planDigest = projection.planDigest;
    binding.securityPolicySnapshotDigest = projection.securityPolicySnapshotDigest;
    binding.protectionEpoch = projection.selectedRole.protectionEpoch;
    binding.grantName = projection.grantName;
    binding.grantDigest = projection.grantDigest;
    binding.providerBootId = providerBootId;
    binding.fencingToken = nativeProtectedFencingToken(projection, providerBootId, {});
    binding.expiresAtMs = projection.deadlineMs;
    for (const auto& endpoint : projection.dataflow.mayPublish) {
      binding.mayPublishEndpointDigests.insert(endpoint.endpointDigest);
      binding.mayPublishConsumerByEndpoint[endpoint.endpointDigest] = endpoint.consumerRole;
    }
    for (const auto& endpoint : projection.dataflow.mustFetch) {
      if (endpoint.sourceKind == "APPLICATION_INPUT" || endpoint.operation == "APPLICATION_INPUT")
        continue;
      binding.mustFetchEndpointDigests.insert(endpoint.endpointDigest);
      binding.mustFetchProducerByEndpoint[endpoint.endpointDigest] = endpoint.producerRole;
    }
    NativeProtectedGrantConfig grantConfig;
    grantConfig.authorityIdentity = authorityName;
    grantConfig.authorityPublicKeyRaw = rawPublicKey(authorityKey);
    grantConfig.recipientKey = {NativeRecipientKey::Kind::Ed25519Seed, std::string(32, 0x61)};
    grantConfig.modelManifestDigest = projection.selectedRole.modelManifestDigest;
    grantConfig.fetchGrant = [wire = grant.wireJson] (const std::string&) {
      return wire;
    };
    auto runtime = std::make_shared<ProtectedRuntime>(binding, std::move(grantConfig));
    runtime->verifyGrant(binding, static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
    return runtime;
  };
  auto ackHandler = [offerConfig] (const ndn_service_framework::RequestMessage& request) {
    const auto payload = request.getPayload();
    const auto issued = issueNativeProviderOfferV3(
      std::vector<std::uint8_t>(payload.begin(), payload.end()), offerConfig,
      static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()));
    ndn_service_framework::ServiceProvider::AckDecision decision;
    if (!issued || !issued->status) {
      decision.message = issued ? issued->message : "DI_NATIVE_REQUEST_NOT_V3";
      return decision;
    }
    decision.status = true;
    decision.message = issued->message;
    decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
    decision.pendingStateTtlMs = issued->pendingStateTtlMs;
    return decision;
  };
  auto facade = Provider::fromServiceProviderForTest(
    environment->providerFace(), environment->provider(), environment->keyChain(),
    providerCertificate, authorityCertificate, providerConfig,
    makePreparedServedProviderRunnerFactory(runs), std::move(preparation),
    std::move(protectedFactory), std::move(ackHandler));
  environment->enableProductionIngressForTest();
  auto served = facade.serve({serviceName, roles});
  BOOST_REQUIRE(served.valid());
  environment->provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment->provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment->provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment->user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment->user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment->user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment->provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);

  auto environmentUser = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment->user(), [] (ndn_service_framework::ServiceUser*) {});
  InProcessRuntimeBinding binding;
  binding.environment = environment;
  binding.user = environmentUser;
  binding.grants = grants;
  binding.admission = admission;
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(
    runtime, environmentUser, grants, admission);

  RequestOptions options;
  options.timeout = std::chrono::seconds(10);
  options.ackTimeout = std::chrono::seconds(5);
  auto handle = prepared.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  auto observation = startConversationResultObservation(handle, std::chrono::seconds(10));
  pumpUntilConversationResultReady(*environment, observation, std::chrono::seconds(10));
  const auto result = observation->get();
  BOOST_CHECK_EQUAL(std::string(result.payload.begin(), result.payload.end()),
                    "spec185-provider-response");
  BOOST_CHECK_EQUAL(result.modelDigest, model.intentDigest());
  BOOST_CHECK(!result.planDigest.empty());
  BOOST_CHECK_EQUAL(runs->load(std::memory_order_relaxed), 1U);
  const auto counters = facade.counters();
  BOOST_CHECK_EQUAL(counters.assemblies, 1U);
  BOOST_CHECK_EQUAL(counters.runnersCreated, 1U);

  runtime->close();
  BOOST_REQUIRE(drainWithInProcessPump(binding, runtime, testDrainTimeout()));
  served.close();
  facade.stop();
  BOOST_CHECK(facade.drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedConversationCommitsTwoNativeTurns)
{
  // Conversation exercises the native Qwen generation contract, including
  // sealed state tensors.  Keep the ordinary streaming probes on the compact
  // YOLO fixture, but use the source-bound Qwen fixture for real turns.
  RuntimeFixture fixture(true, true);
  // Deliberately pin a stale model default. The native conversation boundary
  // must rebind it to each freshly allocated continuation generation identity
  // after defaults are merged, rather than rejecting or replaying the old ID.
  {
    NativeJson config;
    std::ifstream input(fixture.configPath);
    input >> config;
    config["request"]["generation_defaults"]["generationId"] = std::string(32, '0');
    std::ofstream output(fixture.configPath);
    output << nativeCanonicalJson(config);
  }
  ndn_service_framework::test::BootstrapProfile profile;
  profile.groupPrefix = ndn::Name("/group");
  profile.syncPrefix = ndn::Name("/ndnsf/spec185/t007/sync");
  profile.userNode = ndn::Name("/ndnsf/spec185/t007/user");
  profile.providerNode = ndn::Name("/ndnsf/spec185/t007/provider");
  profile.userIdentity = ndn::Name("/user");
  profile.providerIdentity = ndn::Name("/provider");
  profile.attributeAuthority = ndn::Name("/aa");
  profile.serviceName = ndn::Name("/Inference");
  profile.deferBridgeDelivery = true;
  ndn_service_framework::test::NdnsfIntegrationEnvironment environment(profile);
  environment.bootstrap();
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  const auto package = ndnsf::di::Spec185PreparedModelTestAccess::package(prepared);
  const auto serviceName = environment.profile().serviceName.toUri();
  const auto requesterName = environment.user().getName().toUri();
  const auto providerName = environment.provider().getName().toUri();
  const auto model = package->catalog.model.descriptor;
  const auto candidates = package->catalog.splitter->enumerate(
    model, package->catalog.model.graph, NativeCandidateBudget{1, 1000, 1});
  BOOST_REQUIRE_EQUAL(candidates.size(), 1U);
  const auto roles = candidates.front().executionPlan.roles;
  BOOST_REQUIRE(!roles.empty());
  const auto terminalRole = candidates.front().resultEgressRole;
  BOOST_REQUIRE(!terminalRole.empty());

  const auto offerKey = deterministicEd25519Key(0x71);
  const auto offerKeyId = nativePlanningDigest(rawPublicKey(offerKey));
  NativeProviderOfferV3Config offerConfig;
  offerConfig.provider = providerName;
  offerConfig.service = serviceName;
  offerConfig.bootEpoch = providerName + ":" + environment.provider().getProviderBootEpoch();
  offerConfig.signerKeyId = offerKeyId;
  offerConfig.acceptedRoles = roles;
  offerConfig.backends = {"onnxruntime-cpu"};
  offerConfig.hasModel = true;
  offerConfig.signDigest = [offerKey] (const std::string& value) {
    return signDigest(offerKey, value);
  };
  const auto candidatePolicyDigest = nativePlanningDigest("spec185-t007-provider-policy");
  const auto policy = nativeCanonicalJson(NativeJson{
    {"schema", "spec180-provider-offer-trust-v1"},
    {"candidateId", "spec185-t007"}, {"candidateDigest", candidatePolicyDigest},
    {"trustSchema", "/spec185/t007/trust"},
    {"entries", NativeJson::array({NativeJson{
      {"provider", providerName}, {"service", serviceName},
      {"keyLocatorPrefix", environment.provider().getSigningKeyName().toUri()},
      {"signerKeyId", offerKeyId},
      {"certificateName", environment.provider().getSigningCertificateName().toUri()}}})}});
  auto admission = std::make_shared<NativeOfferAdmission>(
    policy, std::map<std::string, std::string>{{offerKeyId, publicKeyPem(offerKey)}},
    candidatePolicyDigest);

  const auto requesterKey = deterministicEd25519Key(0x81);
  const auto authorityKey = deterministicEd25519Key(0x91);
  const auto recipientKey = deterministicEd25519Key(0xa1);
  const auto registration = nativeParseJson(package->registration->configurationJson);
  const auto protectionEpoch = registration.at("grant").at("protection_epoch").get<std::string>();
  const auto policyDigest = registration.at("request").at("security_policy_digest").get<std::string>();
  NativeGrantIssuerConfig issuerConfig;
  issuerConfig.authorityIdentity = "/aa";
  issuerConfig.requesterIdentity = requesterName;
  issuerConfig.protectionEpoch = protectionEpoch;
  issuerConfig.keyId = "spec185-t007-grant-key";
  issuerConfig.authorityPrivateKey = authorityKey;
  issuerConfig.requesterPublicKey = requesterKey;
  issuerConfig.allowedModelManifests = {package->catalog.model.modelManifestDigest};
  issuerConfig.publicationSources.emplace(package->catalog.model.modelManifestDigest,
    NativeGrantPublicationSource{
      package->catalog.model.descriptor.modelName,
      package->catalog.model.descriptor.contentDigest,
      package->catalog.model.canonicalSourceDigest,
      package->catalog.model.canonicalInitializerObjectDigest,
      nativePlanningDigest("fixture-profile")});
  issuerConfig.recipientPublicKeys = {{providerName, recipientKey}};
  issuerConfig.contentKey = [] (const auto&, const auto&) {
    return std::vector<std::uint8_t>(32, 0x77);
  };
  auto grantIssuer = std::make_shared<NativeArtifactGrantIssuer>(std::move(issuerConfig));
  NativeAuthenticatedGrantClient::Issue issue = [grantIssuer] (
    const NativeSignedGrantRequest& request, const std::string& publishedManifest,
    std::uint64_t expiresAtMs, const NativeGrantControl& control) {
    control.check();
    const auto nowMs = static_cast<std::uint64_t>(std::chrono::duration_cast<
      std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    return grantIssuer->issue(request, nowMs, expiresAtMs, publishedManifest);
  };
  NativeAuthenticatedGrantClient::Publish publish = [] (
    const std::string& name, const std::string&, const NativeGrantControl& control) {
    control.check();
    return name;
  };
  auto grants = std::make_shared<NativeAuthenticatedGrantClient>(
    requesterName, requesterKey, "/aa", rawPublicKey(authorityKey),
    protectionEpoch, std::move(issue), std::move(publish));

  auto ackCount = std::make_shared<std::atomic<unsigned>>(0);
  auto completedTurns = std::make_shared<std::atomic<unsigned>>(0);
  environment.provider().addCollaborationHandler(
    ndn::Name(serviceName),
    [offerConfig, ackCount] (const ndn_service_framework::RequestMessage& request) {
      ackCount->fetch_add(1, std::memory_order_relaxed);
      const auto payload = request.getPayload();
      const auto issued = issueNativeProviderOfferV3(
        std::vector<std::uint8_t>(payload.begin(), payload.end()), offerConfig,
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count()));
      ndn_service_framework::ServiceProvider::AckDecision decision;
      if (!issued || !issued->status) {
        decision.message = issued ? issued->message : "DI_NATIVE_REQUEST_NOT_V3";
        return decision;
      }
      decision.status = true;
      decision.message = issued->message;
      decision.payload = ndn::Buffer(issued->payload.begin(), issued->payload.end());
      decision.pendingStateTtlMs = issued->pendingStateTtlMs;
      return decision;
    },
    [model, serviceName, providerName, policyDigest, terminalRole, completedTurns] (
      ndn_service_framework::ServiceProvider::CollaborationContext& context,
      const ndn_service_framework::RequestMessage&) {
      try {
        const auto assignment = context.assignment().assignmentPayload;
        std::istringstream input(std::string(assignment.begin(), assignment.end()));
        const auto projection = nativeSelectionProjectionV3FromJson(input, context.role());
        if (!context.isStreamed() || !projection.conversationTurnBinding)
          throw std::runtime_error("Spec185 T007 conversation stream projection missing");
        const auto binding = *projection.conversationTurnBinding;
        const bool terminal = context.role() == terminalRole;
        const auto publishToken = [&context, &projection] (std::int64_t token,
                                                            std::uint64_t epoch,
                                                            const std::string& prefix,
                                                            const std::string& delta,
                                                            const std::string& hint) {
          const auto wire = nativeCanonicalJson(NativeJson{
            {"schema", "GenerationTokenEventV1"}, {"tokenId", token},
            {"tokenEpoch", epoch}, {"acceptedPrefixDigest", nativePlanningDigest(prefix)},
            {"textDelta", delta}, {"finishHint", hint},
            {"generationId", projection.generationContract.generationId},
            {"samplingDigest", projection.generationContract.samplingDigest}});
          if (!context.publishStreamEvent(ndn::Buffer(wire.begin(), wire.end())))
            throw std::runtime_error("Spec185 T007 token publication failed");
        };
        const bool append = binding.parentContextEpoch != 0;
        const auto nextToken = static_cast<std::int64_t>(binding.parentContextEpoch + 2);
        if (terminal) {
          if (append) {
            const auto token = std::to_string(nextToken);
            publishToken(nextToken, 1, token, "c", "EOS");
          }
          else {
            publishToken(1, 1, "1", "a", "NONE");
            publishToken(2, 2, "1,2", "b", "EOS");
          }
          const auto final = nativeCanonicalJson(NativeJson{
            {"schema", "NDNSF-DI-FINAL-V1"},
            {"tokenIds", append ? NativeJson::array({nextToken}) : NativeJson::array({1, 2})},
            {"text", append ? "c" : "ab"}, {"finishHint", "EOS"},
            {"finishReason", "eos"},
            {"generationId", projection.generationContract.generationId}});
          if (!context.finishStream(ndn::Buffer(final.begin(), final.end()),
                                    ndn_service_framework::StreamFinishReason::ApplicationComplete))
            throw std::runtime_error("Spec185 T007 final publication failed");
        }
        ProviderConversationStateReceiptV1 receipt;
        receipt.conversationId = binding.conversationId;
        receipt.parentContextEpoch = binding.parentContextEpoch;
        receipt.successorContextEpoch = binding.successorContextEpoch;
        receipt.originRequestId = context.sessionId();
        receipt.originGenerationId = projection.generationContract.generationId;
        receipt.serviceName = binding.serviceName;
        receipt.requesterIdentity = context.requesterName().toUri();
        receipt.securityDomainDigest = policyDigest;
        receipt.modelDigest = model.intentDigest();
        receipt.graphSemanticDigest = model.semanticsDigest;
        receipt.adapterDigest = model.adapter.descriptorDigest();
        receipt.roleName = context.role();
        receipt.roleSplitDigest = projection.selectedRole.recipeDigest;
        receipt.layoutDigest = projection.selectedRole.artifactProfileDigest;
        receipt.planRoleMapDigest = binding.planRoleMapDigest;
        receipt.providerIdentity = context.localProvider().toUri();
        receipt.providerBootId = providerName + "-boot";
        receipt.cacheEpoch = binding.successorContextEpoch;
        // The fixture adapter derives one canonical token per inline input
        // byte. Include those adapter-owned prompt tokens before the
        // deterministic streamed output in the committed successor prefix.
        std::vector<std::int64_t> fullTokens;
        if (!append)
          fullTokens = {1, 2, 3, 1, 2};
        else if (binding.parentContextEpoch == 1)
          fullTokens = {1, 2, 3, 1, 2, 4, 5, 6, 3};
        else
          fullTokens = {1, 2, 3, 1, 2, 4, 5, 6, 3, 7, 8, 9, 4};
        receipt.prefixDigest = nativeConversationPrefixDigest(fullTokens);
        receipt.prefixTokenCount = static_cast<std::uint32_t>(fullTokens.size());
        receipt.positionDigest = nativePlanningDigest("spec185-t007-position");
        receipt.stateSchemaDigest = nativePlanningDigest("spec185-t007-state-schema");
        receipt.stateComponentDigests = {nativePlanningDigest("spec185-t007-state")};
        receipt.expiresAtMs = binding.retentionDeadlineMs;
        receipt.validate();
        const auto receiptJson = receipt.toJson();
        context.publish("ndnsf-di-conversation-state-v1",
                        ndn::Name("/ndnsf-di/conversation/receipt").append(context.role()),
                        ndn::Buffer(receiptJson.begin(), receiptJson.end()));
        const auto controlTopic = ndn::Name("/ndnsf-di/conversation/control");
        const auto commitTopic = ndn::Name("/ndnsf-di/conversation/commit");
        for (int rounds = 0; rounds < 30; ++rounds) {
          const auto controls = context.waitFor("ndnsf-di-conversation-state-v1",
                                                controlTopic, 1, 1000);
          for (const auto& controlData : controls) {
            if (controlData.producer != context.requesterName() ||
                controlData.producerRole != "user-control-v1") continue;
            const auto control = nativeParseJson(std::string(
              controlData.payload.begin(), controlData.payload.end()));
            if (control.value("conversationId", std::string{}) != binding.conversationId ||
                control.value("successorContextEpoch", std::uint64_t{0}) != binding.successorContextEpoch ||
                control.value("roleName", std::string{}) != context.role()) continue;
            const auto action = control.value("action", std::string{});
            if (action == "FINALIZE" || action == "ROLLBACK") return;
            if (action != "COMMIT") continue;
            const auto ack = nativeCanonicalJson(NativeJson{
              {"schema", "ndnsf-di-provider-conversation-commit-ack-v1"},
              {"requestId", context.sessionId()}, {"attemptEpoch", projection.attempt},
              {"generationId", projection.generationContract.generationId},
              {"planDigest", projection.planDigest}, {"conversationId", binding.conversationId},
              {"parentContextEpoch", binding.parentContextEpoch},
              {"successorContextEpoch", binding.successorContextEpoch},
              {"serviceName", binding.serviceName}, {"planRoleMapDigest", binding.planRoleMapDigest},
              {"roleName", context.role()}, {"receiptDigest", receipt.computedDigest()},
              {"checkpointDigest", control.value("checkpointDigest", std::string{})},
              {"providerIdentity", context.localProvider().toUri()},
              {"providerBootId", providerName + "-boot"}, {"cacheEpoch", receipt.cacheEpoch},
              {"committed", true}});
            context.publish("ndnsf-di-conversation-state-v1", commitTopic,
                            ndn::Buffer(ack.begin(), ack.end()));
            if (terminal) completedTurns->fetch_add(1, std::memory_order_relaxed);
            else if (!context.completeRole())
              throw std::runtime_error("Spec185 T007 non-terminal role completion failed");
            return;
          }
        }
        throw std::runtime_error("Spec185 T007 conversation control timeout");
      }
      catch (const std::exception& error) {
        context.fail(std::string("Spec185 T007 handler failure: ") + error.what());
      }
    });
  environment.enableProductionIngressForTest();
  environment.provider().markHybridResponseKeyWrappedForTest(serviceName);
  const auto ackKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "ACK");
  const auto responseKey = environment.provider().prepareHybridSendKeyForTest(serviceName, "RESPONSE");
  environment.user().cacheHybridReceiveKeyForTest(ackKey.keyId, ackKey.epochId, ackKey.key);
  environment.user().cacheHybridReceiveKeyForTest(responseKey.keyId, responseKey.epochId, responseKey.key);
  const auto selectionKey = environment.user().prepareHybridSendKeyForTest(serviceName, "SELECTION");
  environment.provider().cacheHybridReceiveKeyForTest(selectionKey.keyId, selectionKey.epochId, selectionKey.key);
  auto environmentUser = std::shared_ptr<ndn_service_framework::ServiceUser>(
    &environment.user(), [] (ndn_service_framework::ServiceUser*) {});
  ndnsf::di::detail::RuntimeTestAccess::bindProviderFixture(runtime, environmentUser, grants, admission);

  auto conversation = prepared.openConversation();
  RequestOptions options;
  // The three-turn recovery path includes a fresh authenticated preparation
  // and state promotion.  Keep a bounded margin between the request deadline
  // and each blocking observation so normal VM scheduling cannot turn a valid
  // turn into an observation-timeout failure.
  // The authenticated three-turn path is sensitive to VM scheduling in both
  // normal and instrumented builds.  Keep one explicit bounded contract so a
  // valid turn is not made dependent on compiler-specific sanitizer macros.
  constexpr auto conversationTimeout = std::chrono::seconds(180);
  constexpr auto conversationResultWait = std::chrono::seconds(120);
  options.timeout = conversationTimeout;
  options.ackTimeout = std::chrono::seconds(3);
  options.generation = GenerationOptions{8};
  BOOST_CHECK_EXCEPTION(conversation.checkpoint(), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CHECKPOINT_NOT_READY";
                        });
  // Occupy the native operation worker in a public completion callback. The
  // callback is deliberately unrelated to the Conversation, so this test
  // does not add a production test seam or alter the installed API.
  DelayedConversationCompletion failureCompletion;
  NativeJson blockerConfig;
  {
    std::ifstream input(fixture.configPath);
    input >> blockerConfig;
  }
  auto blockerDefaultsJson = blockerConfig.at("request").at("generation_defaults");
  blockerDefaultsJson.erase("generationId");
  blockerDefaultsJson.erase("generation_id");
  const auto blockerDefaults = nativeCanonicalJson(blockerDefaultsJson);
  RequestOptions blockerOptions = options;
  blockerOptions.generation.reset();
  const std::vector<std::uint8_t> blockerApplicationOptions(
    blockerDefaults.begin(), blockerDefaults.end());
  auto blocker = prepared.request(
    Input::inlineBytes({0x0a, 0x0b, 0x0c}, blockerApplicationOptions), blockerOptions);
  auto blockerCompletion = blocker.onCompletion(failureCompletion.callback());
  blocker.cancel();
  BOOST_REQUIRE(failureCompletion.wait(std::chrono::seconds(5)));

  // Cancelled turns must release admission before the Conversation's
  // asynchronous completion observer runs. Submit the replacement while the
  // failed result is already visible and the observer worker is still gated.
  auto failed = conversation.request(Input::inlineBytes({0x09, 0x09, 0x09}), options);
  failed.cancel();
  BOOST_CHECK_EXCEPTION(failed.result(std::chrono::milliseconds(0)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CANCELLED";
                        });

  // The retry is submitted before the failed turn's delayed observer is
  // released. The native terminal hook must make this request admissible.
  RequestHandle first;
  try {
    first = conversation.request(Input::inlineBytes({0x01, 0x02, 0x03}), options);
  }
  catch (...) {
    failureCompletion.release();
    throw;
  }
  failureCompletion.release();
  // The first request cannot complete before the test pumps the borrowed Face.
  // Keep the active-turn negative case in a small helper frame so the
  // sanitizer observes the production exception boundary without coupling it
  // to this integration test's large stack frame.
  BOOST_CHECK(conversationRejectsBusyTurn(conversation, options));
  pumpUntilRequestTerminal(environment, first, conversationTimeout);
  Result firstResult;
  firstResult = first.result(std::chrono::milliseconds(0));
  BOOST_CHECK(!firstResult.payload.empty());
  // A successful result is already visible to the caller here. Submit the
  // next turn immediately, before any explicit Face pump, and let the normal
  // two-turn path below verify that it commits and remains usable.
  auto second = conversation.request(Input::inlineBytes({0x04, 0x05, 0x06}), options);
  auto checkpoint = conversation.checkpoint();
  const auto checkpointBytes = checkpoint.bytes();
  BOOST_REQUIRE(!checkpointBytes.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 1U);
  const auto exportPath = fixture.root / "checkpoint.json";
  conversation.exportCheckpoint(exportPath);
  {
    std::ifstream exported(exportPath, std::ios::binary);
    const std::string exportedWire((std::istreambuf_iterator<char>(exported)),
                                   std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(exportedWire,
                      std::string(checkpointBytes.begin(), checkpointBytes.end()));
  }
  const auto exportedCheckpoint = [&] {
    std::ifstream input(exportPath, std::ios::binary);
    const std::string wire((std::istreambuf_iterator<char>(input)),
                           std::istreambuf_iterator<char>());
    return ConversationCheckpoint::fromBytes(
      std::vector<std::uint8_t>(wire.begin(), wire.end()));
  }();
  BOOST_CHECK(exportedCheckpoint.bytes() == checkpointBytes);

  auto secondObservation = startConversationResultObservation(second, conversationResultWait);
  pumpUntilConversationResultReady(environment, secondObservation, conversationTimeout);
  const auto secondResult = secondObservation->get();
  BOOST_CHECK(!secondResult.payload.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 2U);
  BOOST_CHECK(ackCount->load(std::memory_order_relaxed) >= 2U);
  const auto secondCheckpoint = conversation.checkpoint();
  const auto secondCheckpointBytes = secondCheckpoint.bytes();
  const auto recoveryExportPath = fixture.root / "recovery-checkpoint.json";
  conversation.exportCheckpoint(recoveryExportPath);
  const auto failedExportPath = fixture.root / "failed-checkpoint.json";
  {
    std::ofstream old(failedExportPath, std::ios::binary);
    old << "old-checkpoint";
  }
  const auto symlinkPath = fixture.root / "failed-checkpoint-link.json";
  std::error_code symlinkError;
  std::filesystem::create_symlink(failedExportPath, symlinkPath, symlinkError);
  BOOST_REQUIRE(!symlinkError);
  BOOST_CHECK_EXCEPTION(conversation.exportCheckpoint(symlinkPath), DiError,
                        [] (const DiError& error) {
                          return error.code() == "CHECKPOINT_EXPORT_FAILED" &&
                                 error.boundary() == "export";
                        });
  {
    std::ifstream old(failedExportPath, std::ios::binary);
    const std::string oldWire((std::istreambuf_iterator<char>(old)),
                              std::istreambuf_iterator<char>());
    BOOST_CHECK_EQUAL(oldWire, "old-checkpoint");
  }

  ConversationOptions restoredOptions;
  std::ifstream recoveryInput(recoveryExportPath, std::ios::binary);
  const std::string recoveryWire((std::istreambuf_iterator<char>(recoveryInput)),
                                 std::istreambuf_iterator<char>());
  const auto recoveredCheckpoint = ConversationCheckpoint::fromBytes(
    std::vector<std::uint8_t>(recoveryWire.begin(), recoveryWire.end()));
  BOOST_CHECK(recoveredCheckpoint.bytes() == secondCheckpointBytes);
  restoredOptions.conversationId = nativeParseJson(recoveryWire).at("conversationId").get<std::string>();
  restoredOptions.checkpoint = recoveredCheckpoint;
  auto restored = prepared.openConversation(restoredOptions);
  auto third = restored.request(Input::inlineBytes({0x07, 0x08, 0x09}), options);
  auto thirdObservation = startConversationResultObservation(third, conversationResultWait);
  pumpUntilConversationResultReady(environment, thirdObservation, conversationTimeout);
  BOOST_CHECK(!thirdObservation->get().payload.empty());
  BOOST_CHECK_EQUAL(completedTurns->load(std::memory_order_relaxed), 3U);
  restored.close();
  conversation.close();
  runtime->close();
  auto drainObservation = startBooleanResultObservation([runtime] {
    return runtime->drain(std::chrono::seconds(3));
  });
  pumpUntilBooleanResultReady(environment, drainObservation, std::chrono::seconds(3));
  BOOST_CHECK(drainObservation->get());
}

BOOST_AUTO_TEST_CASE(PlacementHandlesAreRuntimeBound)
{
  RuntimeFixture fixture;
  auto firstRuntime = Runtime::open(runtimeConfig(fixture));
  auto secondRuntime = Runtime::open(runtimeConfig(fixture));
  auto prepared = firstRuntime->user().prepare();
  auto placement = secondRuntime->placementStrategy("native-pre-split-first");
  auto materializations = std::make_shared<std::atomic<unsigned>>(0);
  auto instrumented = ndnsf::di::Spec185PreparedModelTestAccess::bind(
    prepared, [materializations] (const std::shared_ptr<const PreparedModelPackage>&) {
      materializations->fetch_add(1, std::memory_order_relaxed);
      return std::shared_ptr<NativeInferenceClient>{};
    });
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  options.placement = placement;
  BOOST_CHECK_EXCEPTION(instrumented.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "STRATEGY_NOT_FOUND" &&
                                 error.boundary() == "request";
                        });
  // Placement rejection is before client materialization, so no request or
  // Selection can be published through a factory that would otherwise run.
  BOOST_CHECK_EQUAL(materializations->load(std::memory_order_relaxed), 0U);
  BOOST_CHECK_EXCEPTION(firstRuntime->placementStrategy("missing"), DiError,
                        [] (const DiError& error) {
                          return error.code() == "STRATEGY_NOT_FOUND" &&
                                 error.boundary() == "placement";
                        });
  firstRuntime->close();
  secondRuntime->close();
  BOOST_CHECK(firstRuntime->drain(std::chrono::seconds(2)));
  BOOST_CHECK(secondRuntime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestFactoryErrorsMapAtPublicBoundary)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  auto throwing = ndnsf::di::Spec185PreparedModelTestAccess::bind(
    prepared, [] (const std::shared_ptr<const PreparedModelPackage>&) ->
      std::shared_ptr<NativeInferenceClient> {
      throw std::runtime_error("injected client factory failure");
    });
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  BOOST_CHECK_EXCEPTION(throwing.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "REQUEST_FAILED" &&
                                 error.domain() == "runtime" &&
                                 error.boundary() == "request";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncIncludesNativeClientWork)
{
  RuntimeFixture fixture;
  InProcessRuntimeBinding binding;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  binding = bindInProcessRuntime(fixture, runtime);
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto handle = prepared.request(Input::inlineBytes({0x01}), options);
  runtime->close();
  const auto drainTimeout = testDrainTimeout();
  Subscription subscription;
  BOOST_CHECK(waitForInProcessDrainNotification(
    binding, runtime, drainTimeout, subscription));
  BOOST_CHECK(handle.status() == RequestStatus::Failed ||
              handle.status() == RequestStatus::Cancelled);
  subscription.cancel();
  // Repeat the terminal fence after the asynchronous callback has been
  // retired.  This mirrors the multi-client cases below and lets the Runtime
  // release its final native client before the fixture Face is destroyed.
  runtime->close();
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, drainTimeout));
}

BOOST_AUTO_TEST_CASE(RuntimeClientDoesNotKeepEvictedSourceAlive)
{
  RuntimeFixture fixture;
  // Change only the frozen request budget so the second registration has a
  // distinct preparation identity while reusing the same valid ONNX fixture.
  NativeJson secondaryConfig;
  {
    std::ifstream input(fixture.configPath);
    input >> secondaryConfig;
  }
  secondaryConfig["request"]["max_candidates"] = 2;
  const auto secondaryPath = fixture.write(
    "secondary-requester.json", nativeCanonicalJson(secondaryConfig));

  InProcessRuntimeBinding binding;
  auto config = runtimeConfig(fixture);
  config.maxPreparedEntries = 1;
  config.models.push_back({"secondary", secondaryPath.string()});
  auto runtime = Runtime::open(config);
  binding = bindInProcessRuntime(fixture, runtime);

  std::optional<PreparedModel> first;
  first.emplace(runtime->user().prepare("default"));
  auto source = ndnsf::di::Spec185PreparedModelTestAccess::source(*first);
  BOOST_REQUIRE(!source.expired());
  {
    RequestOptions options;
    options.timeout = std::chrono::milliseconds(500);
    options.ackTimeout = std::chrono::milliseconds(50);
    auto handle = first->request(Input::inlineBytes({0x01}), options);
    handle.cancel();
    binding.environment->pumpUntil([] { return false; });
  }
  // The application still owns the prepared view, so the source remains live
  // even if the Runtime cache later has to retire this package.
  BOOST_REQUIRE(!source.expired());
  first.reset(); // release the view and its lazily cached client

  auto second = runtime->user().prepare("secondary");
  BOOST_CHECK(source.expired());
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncTracksMultiplePreparedClients)
{
  RuntimeFixture fixture;
  InProcessRuntimeBinding binding;
  auto config = runtimeConfig(fixture);
  config.models.push_back({"secondary", fixture.configPath.string()});
  auto runtime = Runtime::open(config);
  binding = bindInProcessRuntime(fixture, runtime);
  auto primary = runtime->user().prepare("default");
  auto secondary = runtime->user().prepare("secondary");
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(500);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto primaryHandle = primary.request(Input::inlineBytes({0x01}), options);
  auto secondaryHandle = secondary.request(Input::inlineBytes({0x02}), options);
  BOOST_REQUIRE(!primaryHandle.id().empty());
  BOOST_REQUIRE(!secondaryHandle.id().empty());

  // Two distinct prepared registrations materialize two Runtime clients. The
  // non-closing Core drain must inspect both through the immutable DI snapshot
  // without taking the DI state mutex under the Core worker lock.
  const auto drainTimeout = testDrainTimeout();
  Subscription subscription;
  // Register while both clients still own active request work, then drive the
  // terminal transitions. This exercises the notifier wakeup path as well as
  // the multi-client snapshot through the borrowed fixture Face.
  auto notification = std::make_shared<DrainNotificationState>();
  subscription = runtime->drainAsync(
    drainTimeout, [notification] (std::exception_ptr error, bool drained) {
      {
        std::lock_guard<std::mutex> lock(notification->mutex);
        notification->value = !error && drained;
      }
      notification->ready.store(true, std::memory_order_release);
    });
  primaryHandle.cancel();
  secondaryHandle.cancel();
  const auto deadline = std::chrono::steady_clock::now() + drainTimeout;
  while (!notification->ready.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    binding.environment->pumpUntil([] { return false; });
  }
  BOOST_REQUIRE(notification->ready.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(notification->mutex);
    BOOST_CHECK(notification->value.value_or(false));
  }
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, drainTimeout));
}

BOOST_AUTO_TEST_CASE(RuntimeDrainAsyncWakesAfterLastClientTimerRetires)
{
  RuntimeFixture fixture;
  InProcessRuntimeBinding binding;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  binding = bindInProcessRuntime(fixture, runtime);
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.timeout = std::chrono::milliseconds(300);
  options.ackTimeout = std::chrono::milliseconds(50);
  auto handle = prepared.request(Input::inlineBytes({0x01}), options);

  // Keep Runtime open: this exercises the non-closing drainAsync path and
  // requires the client-owned request deadline timer to release its final
  // ticket before the outer Core waiter is notified.
  const auto drainTimeout = testDrainTimeout();
  Subscription subscription;
  BOOST_CHECK(waitForInProcessDrainNotification(
    binding, runtime, drainTimeout, subscription));
  BOOST_CHECK(handle.status() == RequestStatus::Failed ||
              handle.status() == RequestStatus::Cancelled);
  // The open-runtime notification is not a shutdown operation.
  BOOST_CHECK_NO_THROW(runtime->user());
  subscription.cancel();
  runtime->close();
  BOOST_CHECK(drainWithInProcessPump(binding, runtime, drainTimeout));
}

BOOST_AUTO_TEST_CASE(PreparedRequestRejectsUnsupportedTextBeforeNativeSubmission)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  BOOST_CHECK_EXCEPTION(prepared.request(Input::text("hello")), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "input";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestRejectsUnsupportedGenerationContract)
{
  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  RequestOptions options;
  options.stream = StreamOptions{};
  options.generation = GenerationOptions{4};
  BOOST_CHECK_EXCEPTION(prepared.request(Input::inlineBytes({0x01}), options), DiError,
                        [] (const DiError& error) {
                          return error.code() == "UNSUPPORTED_CAPABILITY" &&
                                 error.boundary() == "request";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_CASE(PreparedRequestValidatesProtectedReferenceMetadata)
{
  const NativeJson reference = {
    {"authorizationScope", "/group/di"}, {"ciphertextDigest", "sha256:" + std::string(64, 'b')},
    {"dataName", "/group/data/model"}, {"encrypted", true},
    {"manifestDigest", "sha256:" + std::string(64, 'c')},
    {"plaintextSize", 3}, {"protectionEpoch", "epoch-1"} };
  auto data = DataRef::fromPublishedMetadata(nativeCanonicalJson(reference));
  BOOST_CHECK_EQUAL(data.canonicalMetadata(), nativeCanonicalJson(reference));
  auto malformed = reference;
  malformed["plaintextSize"] = 0;
  BOOST_CHECK_EXCEPTION(DataRef::fromPublishedMetadata(nativeCanonicalJson(malformed)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_DATA_REFERENCE" &&
                                 error.boundary() == "input";
                        });

  RuntimeFixture fixture;
  auto runtime = Runtime::open(runtimeConfig(fixture));
  auto prepared = runtime->user().prepare();
  auto oversized = reference;
  oversized["plaintextSize"] = 33; // fixture adapter max_payload_bytes is 32
  const auto oversizedRef = DataRef::fromPublishedMetadata(nativeCanonicalJson(oversized));
  BOOST_CHECK_EXCEPTION(prepared.request(Input::repository(oversizedRef)), DiError,
                        [] (const DiError& error) {
                          return error.code() == "INVALID_DATA_REFERENCE" &&
                                 error.boundary() == "input";
                        });
  runtime->close();
  BOOST_CHECK(runtime->drain(std::chrono::seconds(2)));
}

BOOST_AUTO_TEST_SUITE_END()

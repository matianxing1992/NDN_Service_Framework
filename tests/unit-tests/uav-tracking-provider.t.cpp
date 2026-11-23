#include "tests/fixtures/uav-tracking-core-flow.hpp"
#include "tests/fixtures/uav-tracking-provider-access.hpp"
#include "tests/fixtures/uav-person-profile-golden.hpp"
#include "tests/fixtures/test-resource-paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <csignal>
#include <cstring>
#include <limits>
#include <iterator>
#include <future>
#include <regex>
#include <set>
#include <sstream>
#include <boost/scope_exit.hpp>

#include "NDNSF-UAV-APP/shared/UavObservationSource.hpp"
#include "NDNSF-UAV-APP/shared/UavWindowBinding.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-onnx.hpp"
#include "NDNSF-UAV-APP/shared/uav-person-published-frame.hpp"
#include "NDNSF-UAV-APP/shared/UavTrackingWorkerTransaction.hpp"
#include "NDNSF-UAV-APP/tracking/uav-tracking-provider.hpp"
#include "NDNSF-UAV-APP/tracking/UavTrackingCoordinator.hpp"

#include <sys/inotify.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace ndnsf::test {
using namespace ndnsf::examples::uav;

struct RecordingObservationSource final : IObservationSource
{
  void start(IObservationSink&) override { started = true; }
  void stop() noexcept override { stopped = true; }
  bool started = false;
  bool stopped = false;
};
struct SingleFrameObservationSource final : IObservationSource
{
  CapturedFrame frame;
  void start(IObservationSink& sink) override
  {
    if (!sink.submitFrame(std::move(frame)).accepted()) {
      throw std::runtime_error("fixture frame was not admitted");
    }
  }
  void stop() noexcept override {}
};

namespace {

using namespace tracking_flow;

const std::string CALIBRATION_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"calibration","payload":{"schema":"uav-calibration-v2","cameraId":"UAV1","revision":"3","width":"640","height":"480","fx":"500","fy":"500","cx":"320","cy":"240","distortion":["0","0","0","0","0"],"imageTransform":"raw","validFromUs":"0","validUntilUs":"10000000"}})";
const std::string FRAME_EVENT =
  R"({"schema":"uav-source-event-v2","kind":"frame","payload":{"key":{"sourceId":"camera-source-1","cameraId":"UAV1","epoch":"1","sequence":"1"},"deviceTimeUs":"1000","clockBinding":{"revision":"1","clockId":"sim-clock","clockEpoch":"2","a":"1","bUs":"0","validFromUs":"0","validUntilUs":"10000000","uncertaintyUs":"0"},"width":"640","height":"480","encoding":"image/jpeg","calibrationRevision":"3","imageBase64":"/9j/2Q=="}})";

struct ProviderFixture : JobFlowFixture
{
  ProviderFixture()
    : JobFlowFixture(true)
  {
    std::filesystem::create_directory(root / "streams");
    std::ofstream(root / "streams/window-0.jsonl") << CALIBRATION_EVENT << '\n' << FRAME_EVENT << '\n';
    providerConfig.producerIdentity = definition.sources[0].producerIdentity;
    providerConfig.droneId = "actual-source-drone";
    providerConfig.ownedWorkspace = root / "provider-work";
    TrackingProviderRunDefinition run;
    run.runId = definition.runId;
    run.mode = TrackingMode::VEHICLE;
    run.role = TrackingProviderRole::SOURCE;
    run.cameraId = "UAV1";
    for (size_t i = 0; i < 3; ++i) run.sourceProducers[i] = definition.sources[i].producerIdentity;
    run.computeProducer = definition.computeIdentity;
    run.windowCount = 1;
    run.sourceGenerations = definition.sources[0].sourceManifest;
    run.observationStreamDirectory = root / "streams";
    run.worldFrameId = "synthetic-local";
    run.acquisitionKind = "simulated";
    run.deliveryMode = "replay";
    providerConfig.runs.push_back(std::move(run));
  }

  ~ProviderFixture()
  {
    stopAndDrainJob();
    if (capability) {
      capability->stopAdmission();
      capability->cancelActiveWork();
      environment->pumpUntilWithAttributeAuthority([this] { return capability->isDrained(); });
      BOOST_REQUIRE(capability->isDrained());
      capability.reset();
    }
    for (auto& source : sourceCapabilities) {
      source->stopAdmission();
      source->cancelActiveWork();
      environment->pumpUntilWithAttributeAuthority([&source] { return source->isDrained(); });
      BOOST_REQUIRE(source->isDrained());
    }
    sourceCapabilities.clear();
    if (writer >= 0) ::close(writer);
    if (watch >= 0) ::close(watch);
  }

  void checkNativePersonWindow(size_t missingSources)
  {
    if (missingSources == 0) {
      // The successful result uses the maintained segmented-response path.
      environment->user().fetchPermissionsFromController(environment->profile().attributeAuthority);
      BOOST_REQUIRE(environment->user().hasRequestScopedDefault());
    }
    const auto* assetRoot = std::getenv("NDNSF_SPEC226_PERSON_ASSETS");
    BOOST_REQUIRE_MESSAGE(assetRoot != nullptr, "set the frozen person asset root for this opt-in flow");
    const std::filesystem::path assets(assetRoot);
    BOOST_REQUIRE(assets.is_absolute());
    const auto asset = [] (const std::filesystem::path& path) {
      const auto canonical = std::filesystem::canonical(path);
      std::ifstream input(canonical, std::ios::binary);
      BOOST_REQUIRE(input.good());
      const std::string bytes{std::istreambuf_iterator<char>(input), {}};
      return TrackingAssetReference{canonical, digestOf(bytes)};
    };
    const auto model = asset(assets / "yolov8n.onnx");
    const auto profile = asset(assets / "profile-native.json");
    const auto reid = asset(assets / "osnet_x1_0_market1501_256x128.onnx");
    const auto computationPath = root / "person-computation.json";
    std::ofstream(computationPath) << describeUavPersonNativeComputation(
      model.path, profile.path, reid.path);
    const auto computation = asset(computationPath);
    config["mode"] = "person";
    config["profile"] = {{"path", profile.path.string()}, {"digest", profile.digest}};
    config["computationManifest"] = {{"path", computation.path.string()},
                                     {"digest", computation.digest}};
    config["deadlineMs"] = 30000;
    config["requestTimeoutMs"] = 20000;
    definition = load();
    const auto base = providerConfig;
    for (size_t i = 0; i < 3; ++i) {
      auto sourceConfig = base;
      sourceConfig.producerIdentity = definition.sources[i].producerIdentity;
      sourceConfig.ownedWorkspace = root / ("source-work-" + std::to_string(i));
      auto& run = sourceConfig.runs.front();
      run.mode = TrackingMode::PERSON;
      run.cameraId = definition.sources[i].cameraId;
      std::shared_ptr<IObservationSource> source;
      if (i < missingSources) source = std::make_shared<RecordingObservationSource>();
      else {
        auto one = std::make_shared<SingleFrameObservationSource>();
        one->frame.key = {definition.sources[i].sourceId, run.cameraId, 1, 1};
        one->frame.deviceTimeUs = 1000;
        one->frame.clockBinding = {1, "fixture-clock", 1, 1.0, 0.0, 0, 10000000, 0};
        one->frame.width = 640;
        one->frame.height = 480;
        one->frame.encoding = "image/jpeg";
        one->frame.imageBytes = tracking_fixture::trackingFixtureJpeg();
        source = one;
      }
      auto owner = std::make_unique<UavTrackingProvider>(sourceConfig, source);
      owner->registerServices(environment->provider(i));
      sourceCapabilities.push_back(std::move(owner));
    }
    providerConfig.producerIdentity = definition.computeIdentity;
    auto& run = providerConfig.runs.front();
    run.mode = TrackingMode::PERSON;
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    run.model = model;
    run.profile = profile;
    run.reidModel = reid;
    run.computationManifest = computation;
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider(3));
    environment->enableProductionIngressForTest();
    int savedOut = -1;
    int savedErr = -1;
    int capture = -1;
    BOOST_SCOPE_EXIT_ALL(&) {
      std::cout.flush();
      std::cerr.flush();
      if (savedOut >= 0) { ::dup2(savedOut, STDOUT_FILENO); ::close(savedOut); }
      if (savedErr >= 0) { ::dup2(savedErr, STDERR_FILENO); ::close(savedErr); }
      if (capture >= 0) {
        ::close(capture);
        // Preserve the first failure boundary before the fixture removes its scratch tree.
        std::ifstream diagnostic(root / "person-flow.log");
        if (diagnostic.good()) std::cerr << diagnostic.rdbuf();
      }
    };
    std::cout.flush();
    std::cerr.flush();
    savedOut = ::dup(STDOUT_FILENO);
    BOOST_REQUIRE(savedOut >= 0);
    savedErr = ::dup(STDERR_FILENO);
    BOOST_REQUIRE(savedErr >= 0);
    capture = ::open((root / "person-flow.log").c_str(),
                     O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    BOOST_REQUIRE(capture >= 0);
    BOOST_REQUIRE(::dup2(capture, STDOUT_FILENO) >= 0);
    BOOST_REQUIRE(::dup2(capture, STDERR_FILENO) >= 0);
    BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
    pumpUntilWithin([this] { return job->isDrained(); }, std::chrono::seconds(30));
    BOOST_REQUIRE(job->isDrained());
    BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
    BOOST_CHECK_EQUAL(latest.selectedProviders.size(), 4);
    if (missingSources == 0) {
      BOOST_CHECK(latest.state == TrackingJobState::COMPLETED);
      BOOST_CHECK_EQUAL(latest.completedWindows, 1);
      BOOST_CHECK(latest.latestResult);
      BOOST_REQUIRE(latest.presentation);
      BOOST_CHECK_EQUAL(latest.presentation->frames.size(), 3);
    }
    else {
      BOOST_CHECK(latest.state == TrackingJobState::FAILED);
      BOOST_CHECK_EQUAL(latest.completedWindows, 0);
      BOOST_CHECK(!latest.latestResult);
      BOOST_CHECK(latest.reason.find(
        "source UAV1: injected source produced no frame for the window") != std::string::npos);
    }
    capability->stopAdmission();
    capability->cancelActiveWork();
    for (auto& source : sourceCapabilities) {
      source->stopAdmission();
      source->cancelActiveWork();
    }
    const auto providersDrained = [this] {
      return capability->isDrained() && std::all_of(sourceCapabilities.begin(),
        sourceCapabilities.end(), [] (const auto& source) { return source->isDrained(); });
    };
    pumpUntilWithin(providersDrained, std::chrono::seconds(10));
    BOOST_REQUIRE(providersDrained());
    std::cout.flush();
    std::cerr.flush();
    std::ifstream captured(root / "person-flow.log");
    const std::string output{std::istreambuf_iterator<char>(captured), {}};
    const auto publications = [&output] (const std::string& marker) {
      std::multiset<std::string> cameras;
      const std::regex pattern(marker + " camera=(UAV[123]) window=window-0 name=");
      for (auto it = std::sregex_iterator(output.begin(), output.end(), pattern);
           it != std::sregex_iterator(); ++it) cameras.insert((*it)[1].str());
      return cameras;
    };
    std::multiset<std::string> expectedFailures;
    std::multiset<std::string> expectedReady;
    for (size_t i = 0; i < 3; ++i) {
      (i < missingSources ? expectedFailures : expectedReady).insert("UAV" + std::to_string(i + 1));
    }
    BOOST_CHECK(publications("SPEC226_WINDOW_FAILURE_PUBLISHED") == expectedFailures);
    BOOST_CHECK(publications("SPEC226_WINDOW_BINDING_PUBLISHED") == expectedReady);
    const bool nativeBackendInvoked =
      output.find("SPEC226_NATIVE_BACKEND_INVOKED") != std::string::npos;
    BOOST_CHECK_EQUAL(nativeBackendInvoked, missingSources == 0);
    BOOST_CHECK(output.find("SPEC210_TRACKING_WORKER_INVOKED") == std::string::npos);
    if (missingSources != 0 && std::filesystem::exists(providerConfig.ownedWorkspace)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(providerConfig.ownedWorkspace)) {
        BOOST_CHECK_MESSAGE(entry.path().filename() != "manifest.json" ||
          entry.path().parent_path().filename() != "window-0", "failed window committed " << entry.path());
      }
    }
  }

  void startProvider()
  {
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider());
    install(false, false, false, false, {}, 1);
  }

  void startFailingCompute()
  {
    providerConfig.producerIdentity = definition.computeIdentity;
    auto& run = providerConfig.runs[0];
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    const auto asset = root / "invalid-detector.onnx";
    // The digest is valid, but ORT must reject the malformed detector.
    std::ofstream(asset) << "abc";
    run.model = {asset, digestOf("abc")};
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    capability->registerServices(environment->provider(3));
    install(false, false, false, false, {}, 0, 3);
  }

  void holdSource()
  {
    const auto path = root / "streams/window-0.jsonl";
    std::filesystem::remove(path);
    BOOST_REQUIRE_EQUAL(::mkfifo(path.c_str(), 0600), 0);
    writer = ::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    BOOST_REQUIRE(writer >= 0);
    watch = ::inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    BOOST_REQUIRE(watch >= 0);
    BOOST_REQUIRE(::inotify_add_watch(watch, path.c_str(), IN_OPEN) >= 0);
  }

  void startBlockingCompute(bool successfulCandidate = false)
  {
    // Real native results cross the Core's 6000-byte boundary. Configure the
    // maintained Controller default, never raise the inline threshold.
    if (successfulCandidate) {
      environment->user().fetchPermissionsFromController(environment->profile().attributeAuthority);
      BOOST_REQUIRE(environment->user().hasRequestScopedDefault());
    }
    for (size_t i = 0; i < 3; ++i) {
      auto config = providerConfig;
      config.producerIdentity = definition.sources[i].producerIdentity;
      config.droneId = "source-drone-" + std::to_string(i);
      config.ownedWorkspace = root / ("native-source-" + std::to_string(i));
      auto& run = config.runs[0];
      run.cameraId = "UAV" + std::to_string(i + 1);
      run.observationStreamDirectory = root / ("source-input-" + std::to_string(i));
      std::filesystem::create_directory(run.observationStreamDirectory);
      auto calibration = Json::parse(CALIBRATION_EVENT);
      calibration["payload"]["cameraId"] = run.cameraId;
      auto frame = Json::parse(FRAME_EVENT);
      frame["payload"]["key"]["cameraId"] = run.cameraId;
      frame["payload"]["key"]["sourceId"] = definition.sources[i].sourceId;
      std::ofstream(run.observationStreamDirectory / "window-0.jsonl")
        << calibration.dump() << '\n' << frame.dump() << '\n';
      auto source = std::make_unique<UavTrackingProvider>(config);
      source->registerServices(environment->provider(i));
      sourceCapabilities.push_back(std::move(source));
    }
    const auto fifo = root / "compute-release";
    BOOST_REQUIRE_EQUAL(::mkfifo(fifo.c_str(), 0600), 0);
    writer = ::open(fifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    BOOST_REQUIRE(writer >= 0);
    Json frames = Json::array();
    Json cameras = Json::object();
    for (const auto* camera : {"UAV1", "UAV2", "UAV3"}) {
      frames.push_back({{"cameraId", camera}, {"width", uint64_t(640)}, {"height", uint64_t(480)},
        {"detections", Json::array({{{"box", {10, 20, 50, 60}}, {"confidence", 0.9},
                                    {"class", "car"}, {"byteTrackId", nullptr}}})}});
      cameras[camera] = {{"frameId", 1}, {"idHighWatermark", 0},
                         {"tracked", Json::array()}, {"lost", Json::array()},
                         {"removedIds", Json::array()}};
    }
    const auto candidate = Json{{"schema", VEHICLE_NATIVE_BACKEND_WINDOW_SCHEMA},
      {"runId", definition.runId}, {"missionId", "tracking-" + definition.runId},
      {"windowId", "window-0"}, {"sequence", uint64_t(1)}, {"actualDevice", "cpu"},
      {"frames", frames}, {"backendState", {{"schema", VEHICLE_NATIVE_BACKEND_STATE_SCHEMA},
                                            {"cameras", cameras}}}};
    std::ofstream(root / "controlled-candidate.json") << candidate.dump();
    const auto model = root / "controlled-model.onnx";
    std::ofstream(model) << "controlled-model\n";
    providerConfig.producerIdentity = definition.computeIdentity;
    auto& run = providerConfig.runs[0];
    run.role = TrackingProviderRole::COMPUTE;
    run.cameraId.clear();
    run.observationStreamDirectory.clear();
    run.model = {model, digestOf("controlled-model\n")};
    capability = std::make_unique<UavTrackingProvider>(providerConfig);
    UavTrackingProviderTestAccess::setVehicleBackend(*capability, definition.runId,
      [this, successfulCandidate] (const auto& window, const auto&, const auto&,
                                  const auto& authorized) {
        std::ofstream(root / "compute-entered").close();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (authorized() && std::chrono::steady_clock::now() < deadline) {
          char signal[32];
          if (::read(writer, signal, sizeof(signal)) > 0) {
            if (!successfulCandidate) throw std::runtime_error("fixture detector failed");
            std::ifstream input(root / "controlled-candidate.json");
            std::string wire{std::istreambuf_iterator<char>(input), {}};
            return parseUavVehicleBackendWindow(wire, window);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        throw std::runtime_error("fixture detector cancelled or timed out");
      });
    capability->registerServices(environment->provider(3));
    environment->provider(3).addService(SERVICE_TRACKING_SOURCE,
      [] (const nsf::RequestMessage&) {
        nsf::ServiceProvider::AckDecision decision;
        decision.status = true;
        return decision;
      },
      [] (const ndn::Name&, const ndn::Name&, const ndn::Name&, const ndn::Name&,
          const nsf::RequestMessage&) {
        nsf::ResponseMessage response;
        response.setStatus(true);
        return response;
      }, nsf::ServiceProvider::ServiceInvocationMode::NormalAndTargeted);
    install(false, false, false, false, {}, 3, 3);
  }

  template<typename Predicate>
  void pumpUntilWithin(Predicate done, std::chrono::milliseconds timeout)
  {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    const auto finished = [&] {
      return done() || std::chrono::steady_clock::now() >= deadline;
    };
    while (!finished()) environment->pumpUntilWithAttributeAuthority(finished);
  }

  bool sourceOpened()
  {
    alignas(inotify_event) char events[4096];
    const auto count = ::read(watch, events, sizeof(events));
    return count > 0;
  }

  nsf::RequestMessage frameRequest() const
  {
    UavPersonFrameReadBinding binding;
    binding.runId = definition.runId;
    binding.missionId = "tracking-" + definition.runId;
    binding.windowId = "window-0";
    binding.cameraId = "UAV1";
    binding.sourceId = definition.sources[0].sourceId;
    binding.sourceEpoch = 1;
    binding.sequence = 1;
    binding.frameName = makeUavObservationDataName(definition.sources[0].producerIdentity,
      "UAV1", 1, 1, "FRAME", 1).toUri();
    const std::string jpeg("\xff\xd8\xff\xd9", 4);
    binding.inputDigest = digestOf(jpeg);
    auto bytes = asFields(makeUavPersonFrameReadFields(binding));
    nsf::RequestMessage request;
    request.setPayload(bytes, bytes.size());
    return request;
  }

  UavTrackingProviderConfig providerConfig;
  std::unique_ptr<UavTrackingProvider> capability;
  std::vector<std::unique_ptr<UavTrackingProvider>> sourceCapabilities;
  int writer = -1;
  int watch = -1;
};

} // namespace

BOOST_AUTO_TEST_SUITE(UavTrackingProviderLifecycle)

BOOST_AUTO_TEST_CASE(PersonReidCropMatchesStockPillowInput)
{
  // Frozen by make_person_golden.py --stock-crops with Pillow 10.4.0,
  // torchvision 0.19.1 and Torch 2.4.1. RGB(x,y,c)=(17*x+13*y+73*c) mod 256.
  struct Case
  {
    std::array<double, 4> observation;
    std::array<int, 4> box;
    const char* tensorDigest;
  };
  const Case cases[] = {
    {{{5, 7, 20, 39}}, {{5, 7, 20, 39}}, nullptr},
    {{{5, 7, 21, 38}}, {{5, 7, 21, 38}}, nullptr},
    {{{5, 7, 21, 39}}, {{5, 7, 21, 39}},
      "de4512d0b11c1ae0acb86e3e949dd1a1613deb7f6586ccfbbf5f92a5403dd23e"},
    {{{2.5, 3.5, 83.5, 146.5}}, {{2, 4, 84, 146}},
      "05b4fa96da5d9f02cd49c15a33bc37112809236ee48fd3d5beaaa0f81eb2458a"},
    {{{5, 7, 133, 263}}, {{5, 7, 133, 263}},
      "35f3fdf5fb4510a09dbc56c656dc2ba2d8014e8518883252014b114d7d2e8fe8"},
    {{{0, 0, 511, 777}}, {{0, 0, 511, 777}},
      "0732d520dafdc62f03f9b1fdc6a5ba510aaf8e93c5131550d7a59f6ee7a7d483"},
    {{{1, 3, 65, 600}}, {{1, 3, 65, 600}},
      "b3ef7478d06dfb9766c371ee4777c5d40dad11fd16dfc6d691e74446da53f459"},
    {{{-4.5, -7.5, 510.5, 776.5}}, {{0, 0, 510, 776}},
      "9ee30c5b6aecb104eaea0b1e3fe1762290e7c06e8d29cd3813a846882f1ef5af"},
    {{{1.5, 2.5, 128.5, 259.5}}, {{2, 2, 128, 260}},
      "cd0311d5be165521654a26c8a24fb8aa8e4dba296da043fae967179b2ad14f13"},
  };
  UavRgbImage image{511, 777, std::vector<uint8_t>(511 * 777 * 3)};
  for (uint32_t y = 0; y < image.height; ++y) {
    for (uint32_t x = 0; x < image.width; ++x) {
      for (uint32_t channel = 0; channel < 3; ++channel) {
        image.rgb[(static_cast<size_t>(y) * image.width + x) * 3 + channel] =
          static_cast<uint8_t>((17 * x + 13 * y + 73 * channel) % 256);
      }
    }
  }
  for (const auto& expected : cases) {
    const auto box = integerPersonBox(expected.observation, image.width, image.height);
    BOOST_REQUIRE_EQUAL_COLLECTIONS(box.begin(), box.end(),
                                    expected.box.begin(), expected.box.end());
    const auto tensor = reidPersonCrop(image, box);
    if (!expected.tensorDigest) {
      BOOST_CHECK(tensor.empty());
      continue;
    }
    BOOST_REQUIRE_EQUAL(tensor.size(), 3 * 128 * 256);
    static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559);
    std::string bytes;
    bytes.reserve(tensor.size() * 4);
    for (const auto value : tensor) {
      uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      for (int byte = 0; byte < 4; ++byte) {
        bytes.push_back(static_cast<char>((bits >> (byte * 8)) & 255));
      }
    }
    ndn::util::Sha256 hash;
    hash << std::string_view(bytes);
    BOOST_CHECK_EQUAL(canonicalSha256Hex(hash.toString()), expected.tensorDigest);
  }
  BOOST_CHECK_EXCEPTION(integerPersonBox({0, 0, NAN, 40}, 511, 777),
    std::invalid_argument, [] (const auto& e) {
      return std::string(e.what()) == "non-finite person box";
    });
  BOOST_CHECK_EXCEPTION(reidPersonCrop(image, {0, 0, 512, 32}),
    std::invalid_argument, [] (const auto& e) {
      return std::string(e.what()) == "invalid admitted ReID crop";
    });
  image.rgb.pop_back();
  BOOST_CHECK_EXCEPTION(reidPersonCrop(image, {0, 0, 16, 32}),
    std::invalid_argument, [] (const auto& e) {
      return std::string(e.what()) == "invalid admitted ReID crop";
    });
}

BOOST_AUTO_TEST_CASE(PersonReidMatchesIndependentStockCheckpoint,
                     *boost::unit_test::disabled())
{
  const auto* configured = std::getenv("NDNSF_PERSON_CROP_GOLDEN");
  BOOST_REQUIRE_MESSAGE(configured, "set the explicitly prepared stock crop golden root");
  const std::filesystem::path root(configured);
  BOOST_REQUIRE(root.is_absolute());
  std::ifstream receiptFile(root / "golden.json");
  BOOST_REQUIRE(receiptFile.good());
  person_json::Json receipt;
  receiptFile >> receipt;
  BOOST_REQUIRE_EQUAL(receipt.at("schema"), "ndnsf-person-stock-crops-v1");
  std::ifstream exportFile(root / "onnx-export.json");
  BOOST_REQUIRE(exportFile.good());
  person_json::Json exported;
  exportFile >> exported;
  BOOST_REQUIRE_EQUAL(exported.at("checkpoint").at("sha256"),
                      receipt.at("checkpoint").at("sha256"));
  UavPersonReidSession session(exported.at("onnx").at("path").get<std::string>());
  BOOST_REQUIRE_EQUAL(session.modelDigest(),
                      "sha256:" + exported.at("onnx").at("sha256").get<std::string>());
  UavRgbImage image;
  image.width = receipt.at("image").at("width").get<uint32_t>();
  image.height = receipt.at("image").at("height").get<uint32_t>();
  std::ifstream rgbFile(root / "image.rgb", std::ios::binary);
  BOOST_REQUIRE(rgbFile.good());
  image.rgb.assign(std::istreambuf_iterator<char>(rgbFile), {});
  BOOST_REQUIRE_EQUAL(image.rgb.size(), static_cast<size_t>(image.width) * image.height * 3);
  const auto imagePath = receipt.at("image").at("path").get<std::string>();
  std::ifstream imageFile(imagePath, std::ios::binary);
  BOOST_REQUIRE(imageFile.good());
  const ndn::Buffer imageBytes(std::istreambuf_iterator<char>(imageFile), {});
  ndn::util::Sha256 imageHash;
  imageHash << std::string_view(reinterpret_cast<const char*>(imageBytes.data()),
                               imageBytes.size());
  BOOST_REQUIRE_EQUAL(canonicalSha256Hex(imageHash.toString()),
                      receipt.at("image").at("sha256"));
  if (imageBytes.size() >= 2 && imageBytes[0] == 0xff && imageBytes[1] == 0xd8) {
    const auto decoded = decodePersonJpeg(imageBytes);
    BOOST_REQUIRE_EQUAL(decoded.width, image.width);
    BOOST_REQUIRE_EQUAL(decoded.height, image.height);
    BOOST_REQUIRE(decoded.rgb == image.rgb);
  }
  const size_t count = receipt.at("cropCount").get<size_t>();
  BOOST_REQUIRE(count > 0 && count <= 64);
  const auto readFloats = [] (const std::filesystem::path& path, size_t count) {
    std::ifstream file(path, std::ios::binary);
    BOOST_REQUIRE(file.good());
    const std::vector<uint8_t> bytes(std::istreambuf_iterator<char>(file), {});
    BOOST_REQUIRE_EQUAL(bytes.size(), count * 4);
    std::vector<float> values(count);
    for (size_t i = 0; i < count; ++i) {
      uint32_t bits = 0;
      for (size_t b = 0; b < 4; ++b) bits |= uint32_t{bytes[i * 4 + b]} << (8 * b);
      std::memcpy(&values[i], &bits, sizeof(bits));
    }
    BOOST_REQUIRE(std::all_of(values.begin(), values.end(),
                              [] (float value) { return std::isfinite(value); }));
    return values;
  };
  constexpr size_t stride = 3 * 256 * 128;
  const auto expectedInput = readFloats(root / "reid-input.f32", count * stride);
  const auto expectedOutput = readFloats(root / "reid-output.f32", count * 512);
  std::vector<float> input;
  input.reserve(expectedInput.size());
  for (const auto& row : receipt.at("boxes")) {
    const auto box = integerPersonBox(row.at("observationBox").get<std::array<double, 4>>(),
                                      image.width, image.height);
    const auto expectedBox = row.at("box").get<std::array<int, 4>>();
    BOOST_REQUIRE(box == expectedBox);
    const auto crop = reidPersonCrop(image, box);
    if (row.at("tensorIndex").is_null()) {
      BOOST_REQUIRE(crop.empty());
    }
    else {
      BOOST_REQUIRE_EQUAL(row.at("tensorIndex").get<size_t>(), input.size() / stride);
      BOOST_REQUIRE_EQUAL(crop.size(), stride);
      input.insert(input.end(), crop.begin(), crop.end());
    }
  }
  BOOST_REQUIRE_EQUAL(input.size(), expectedInput.size());
  size_t different = 0;
  for (size_t i = 0; i < input.size(); ++i) {
    if (std::memcmp(&input[i], &expectedInput[i], sizeof(float)) != 0) ++different;
  }
  BOOST_REQUIRE_EQUAL(different, 0U);
  double maximumError = 0;
  for (size_t offset = 0; offset < count; offset += 32) {
    const size_t batch = std::min<size_t>(32, count - offset);
    const std::vector<float> values(input.begin() + offset * stride,
                                    input.begin() + (offset + batch) * stride);
    const auto embedded = session.embed(values, batch);
    BOOST_REQUIRE_EQUAL(embedded.size(), batch * 512);
    for (size_t i = 0; i < embedded.size(); ++i) {
      maximumError = std::max(maximumError,
        std::abs(static_cast<double>(embedded[i]) - expectedOutput[offset * 512 + i]));
    }
  }
  BOOST_CHECK_LE(maximumError, 1e-4);
}

BOOST_AUTO_TEST_CASE(StockPersonHeadsPreserveShapeAndClassAdmission)
{
  // ONNX opset 17: three fixed raw candidates followed by zero padding to the
  // input's stride-8/16/32 anchor count. Class index is [car, person].
  static constexpr char modelData[] =
    "\x08\x08\x3a\xaf\x08\x0a\x1b\x0a\x06\x69\x6d\x61\x67\x65\x73\x12\x0a\x69"
    "\x6e\x70\x75\x74\x53\x68\x61\x70\x65\x22\x05\x53\x68\x61\x70\x65\x0a\x2c"
    "\x0a\x0a\x69\x6e\x70\x75\x74\x53\x68\x61\x70\x65\x0a\x06\x68\x49\x6e\x64"
    "\x65\x78\x12\x01\x68\x22\x06\x47\x61\x74\x68\x65\x72\x2a\x0b\x0a\x04\x61"
    "\x78\x69\x73\x18\x00\xa0\x01\x02\x0a\x2c\x0a\x0a\x69\x6e\x70\x75\x74\x53"
    "\x68\x61\x70\x65\x0a\x06\x77\x49\x6e\x64\x65\x78\x12\x01\x77\x22\x06\x47"
    "\x61\x74\x68\x65\x72\x2a\x0b\x0a\x04\x61\x78\x69\x73\x18\x00\xa0\x01\x02"
    "\x0a\x15\x0a\x01\x68\x0a\x07\x73\x74\x72\x69\x64\x65\x38\x12\x02\x68\x38"
    "\x22\x03\x44\x69\x76\x0a\x15\x0a\x01\x77\x0a\x07\x73\x74\x72\x69\x64\x65"
    "\x38\x12\x02\x77\x38\x22\x03\x44\x69\x76\x0a\x11\x0a\x02\x68\x38\x0a\x02"
    "\x77\x38\x12\x02\x6e\x38\x22\x03\x4d\x75\x6c\x0a\x17\x0a\x01\x68\x0a\x08"
    "\x73\x74\x72\x69\x64\x65\x31\x36\x12\x03\x68\x31\x36\x22\x03\x44\x69\x76"
    "\x0a\x17\x0a\x01\x77\x0a\x08\x73\x74\x72\x69\x64\x65\x31\x36\x12\x03\x77"
    "\x31\x36\x22\x03\x44\x69\x76\x0a\x14\x0a\x03\x68\x31\x36\x0a\x03\x77\x31"
    "\x36\x12\x03\x6e\x31\x36\x22\x03\x4d\x75\x6c\x0a\x17\x0a\x01\x68\x0a\x08"
    "\x73\x74\x72\x69\x64\x65\x33\x32\x12\x03\x68\x33\x32\x22\x03\x44\x69\x76"
    "\x0a\x17\x0a\x01\x77\x0a\x08\x73\x74\x72\x69\x64\x65\x33\x32\x12\x03\x77"
    "\x33\x32\x22\x03\x44\x69\x76\x0a\x14\x0a\x03\x68\x33\x32\x0a\x03\x77\x33"
    "\x32\x12\x03\x6e\x33\x32\x22\x03\x4d\x75\x6c\x0a\x14\x0a\x02\x6e\x38\x0a"
    "\x03\x6e\x31\x36\x12\x04\x6e\x53\x75\x6d\x22\x03\x41\x64\x64\x0a\x19\x0a"
    "\x04\x6e\x53\x75\x6d\x0a\x03\x6e\x33\x32\x12\x07\x61\x6e\x63\x68\x6f\x72"
    "\x73\x22\x03\x41\x64\x64\x0a\x22\x0a\x07\x61\x6e\x63\x68\x6f\x72\x73\x0a"
    "\x05\x74\x68\x72\x65\x65\x12\x0b\x70\x61\x64\x64\x69\x6e\x67\x53\x69\x7a"
    "\x65\x22\x03\x53\x75\x62\x0a\x2d\x0a\x0b\x70\x61\x64\x64\x69\x6e\x67\x53"
    "\x69\x7a\x65\x0a\x04\x61\x78\x65\x73\x12\x0d\x70\x61\x64\x64\x69\x6e\x67"
    "\x56\x65\x63\x74\x6f\x72\x22\x09\x55\x6e\x73\x71\x75\x65\x65\x7a\x65\x0a"
    "\x41\x0a\x03\x6f\x6e\x65\x0a\x08\x63\x68\x61\x6e\x6e\x65\x6c\x73\x0a\x0d"
    "\x70\x61\x64\x64\x69\x6e\x67\x56\x65\x63\x74\x6f\x72\x12\x0c\x70\x61\x64"
    "\x64\x69\x6e\x67\x53\x68\x61\x70\x65\x22\x06\x43\x6f\x6e\x63\x61\x74\x2a"
    "\x0b\x0a\x04\x61\x78\x69\x73\x18\x00\xa0\x01\x02\x0a\x46\x0a\x0c\x70\x61"
    "\x64\x64\x69\x6e\x67\x53\x68\x61\x70\x65\x12\x07\x70\x61\x64\x64\x69\x6e"
    "\x67\x22\x0f\x43\x6f\x6e\x73\x74\x61\x6e\x74\x4f\x66\x53\x68\x61\x70\x65"
    "\x2a\x1c\x0a\x05\x76\x61\x6c\x75\x65\x2a\x10\x08\x01\x10\x01\x22\x04\x00"
    "\x00\x00\x00\x42\x04\x7a\x65\x72\x6f\xa0\x01\x04\x0a\x33\x0a\x0b\x70\x72"
    "\x65\x64\x69\x63\x74\x69\x6f\x6e\x73\x0a\x07\x70\x61\x64\x64\x69\x6e\x67"
    "\x12\x06\x6f\x75\x74\x70\x75\x74\x22\x06\x43\x6f\x6e\x63\x61\x74\x2a\x0b"
    "\x0a\x04\x61\x78\x69\x73\x18\x02\xa0\x01\x02\x12\x15\x73\x74\x6f\x63\x6b"
    "\x2d\x70\x65\x72\x73\x6f\x6e\x2d\x72\x61\x77\x2d\x68\x65\x61\x64\x2a\x13"
    "\x08\x01\x10\x07\x42\x03\x6f\x6e\x65\x4a\x08\x01\x00\x00\x00\x00\x00\x00"
    "\x00\x2a\x18\x08\x01\x10\x07\x42\x08\x63\x68\x61\x6e\x6e\x65\x6c\x73\x4a"
    "\x08\x06\x00\x00\x00\x00\x00\x00\x00\x2a\x13\x10\x07\x42\x05\x74\x68\x72"
    "\x65\x65\x4a\x08\x03\x00\x00\x00\x00\x00\x00\x00\x2a\x14\x10\x07\x42\x06"
    "\x68\x49\x6e\x64\x65\x78\x4a\x08\x02\x00\x00\x00\x00\x00\x00\x00\x2a\x14"
    "\x10\x07\x42\x06\x77\x49\x6e\x64\x65\x78\x4a\x08\x03\x00\x00\x00\x00\x00"
    "\x00\x00\x2a\x15\x10\x07\x42\x07\x73\x74\x72\x69\x64\x65\x38\x4a\x08\x08"
    "\x00\x00\x00\x00\x00\x00\x00\x2a\x16\x10\x07\x42\x08\x73\x74\x72\x69\x64"
    "\x65\x31\x36\x4a\x08\x10\x00\x00\x00\x00\x00\x00\x00\x2a\x16\x10\x07\x42"
    "\x08\x73\x74\x72\x69\x64\x65\x33\x32\x4a\x08\x20\x00\x00\x00\x00\x00\x00"
    "\x00\x2a\x14\x08\x01\x10\x07\x42\x04\x61\x78\x65\x73\x4a\x08\x00\x00\x00"
    "\x00\x00\x00\x00\x00\x2a\x5f\x08\x01\x08\x06\x08\x03\x10\x01\x42\x0b\x70"
    "\x72\x65\x64\x69\x63\x74\x69\x6f\x6e\x73\x4a\x48\x00\x00\xa0\x40\x00\x00"
    "\xa0\x41\x00\x00\xa0\x41\x00\x00\xa0\x40\x00\x00\xa0\x41\x00\x00\xa0\x41"
    "\x00\x00\x00\x40\x00\x00\x00\x41\x00\x00\x00\x41\x00\x00\x00\x40\x00\x00"
    "\x20\x41\x00\x00\x20\x41\xa4\x70\x7d\x3f\xcd\xcc\xcc\x3d\xcd\xcc\x4c\x3e"
    "\x66\x66\x66\x3f\xcd\xcc\x4c\x3f\x33\x33\x33\x3f\x5a\x30\x0a\x06\x69\x6d"
    "\x61\x67\x65\x73\x12\x26\x0a\x24\x08\x01\x12\x20\x0a\x07\x12\x05\x62\x61"
    "\x74\x63\x68\x0a\x02\x08\x03\x0a\x08\x12\x06\x68\x65\x69\x67\x68\x74\x0a"
    "\x07\x12\x05\x77\x69\x64\x74\x68\x62\x28\x0a\x06\x6f\x75\x74\x70\x75\x74"
    "\x12\x1e\x0a\x1c\x08\x01\x12\x18\x0a\x07\x12\x05\x62\x61\x74\x63\x68\x0a"
    "\x02\x08\x06\x0a\x09\x12\x07\x61\x6e\x63\x68\x6f\x72\x73\x42\x04\x0a\x00"
    "\x10\x11\x72\x2c\x0a\x18\x6e\x64\x6e\x73\x66\x2e\x75\x61\x76\x2e\x63\x6c"
    "\x61\x73\x73\x2d\x6e\x61\x6d\x65\x73\x2e\x76\x31\x12\x10\x5b\x22\x63\x61"
    "\x72\x22\x2c\x22\x70\x65\x72\x73\x6f\x6e\x22\x5d";
  auto pattern = makeTestScratchTemplate("person-raw-head");
  const auto* directory = ::mkdtemp(pattern.data());
  BOOST_REQUIRE(directory);
  const std::filesystem::path root(directory);
  BOOST_SCOPE_EXIT(&root) {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  } BOOST_SCOPE_EXIT_END
  const auto modelPath = root / "model.onnx";
  {
    std::ofstream model(modelPath, std::ios::binary);
    model.write(modelData, sizeof(modelData) - 1);
    BOOST_REQUIRE(model.good());
  }
  for (const size_t maximum : {1U, 3U}) {
    UavPersonDetectorSession detector(modelPath, 0.25F, 0.7F, maximum,
                                      UavPersonDetectorSession::ModelFormat::PERSON_STOCK);
    for (const size_t width : {32U, 64U}) {
      UavRectLetterboxedImage input;
      input.tensorWidth = static_cast<int>(width);
      input.tensorHeight = 32;
      input.image.tensor.resize(3 * width * 32);
      const auto selected = detector.detect(input);
      BOOST_REQUIRE_EQUAL(selected.size(), 1U);
      BOOST_CHECK_EQUAL(selected.front().confidence, 0.8F);
      const std::array<float, 4> expected{16, 15, 24, 25};
      BOOST_CHECK(selected.front().box == expected);
      BOOST_CHECK_EXCEPTION(detector.detect(input, [] { return false; }),
        std::runtime_error, [] (const auto& error) {
          return std::string(error.what()) == "person detector cancelled before inference";
        });
    }
  }
  // The output axes are data-dependent: zero input returns [1,300,6],
  // while a first value of one returns [1,301,7] and must fail after the run.
  static constexpr char dynamicModelData[] =
    "\x08\x08\x3a\xae\x05\x0a\x24\x0a\x06\x69\x6d\x61\x67\x65\x73\x12\x04\x66"
    "\x6c\x61\x74\x22\x07\x46\x6c\x61\x74\x74\x65\x6e\x2a\x0b\x0a\x04\x61\x78"
    "\x69\x73\x18\x00\xa0\x01\x02\x0a\x22\x0a\x04\x66\x6c\x61\x74\x0a\x09\x66"
    "\x6c\x61\x74\x53\x68\x61\x70\x65\x12\x06\x70\x69\x78\x65\x6c\x73\x22\x07"
    "\x52\x65\x73\x68\x61\x70\x65\x0a\x2b\x0a\x06\x70\x69\x78\x65\x6c\x73\x0a"
    "\x05\x66\x69\x72\x73\x74\x12\x05\x70\x69\x78\x65\x6c\x22\x06\x47\x61\x74"
    "\x68\x65\x72\x2a\x0b\x0a\x04\x61\x78\x69\x73\x18\x00\xa0\x01\x02\x0a\x21"
    "\x0a\x05\x70\x69\x78\x65\x6c\x12\x07\x69\x6e\x74\x65\x67\x65\x72\x22\x04"
    "\x43\x61\x73\x74\x2a\x09\x0a\x02\x74\x6f\x18\x07\xa0\x01\x02\x0a\x1e\x0a"
    "\x07\x69\x6e\x74\x65\x67\x65\x72\x0a\x04\x72\x6f\x77\x73\x12\x08\x72\x6f"
    "\x77\x43\x6f\x75\x6e\x74\x22\x03\x41\x64\x64\x0a\x24\x0a\x07\x69\x6e\x74"
    "\x65\x67\x65\x72\x0a\x07\x63\x6f\x6c\x75\x6d\x6e\x73\x12\x0b\x63\x6f\x6c"
    "\x75\x6d\x6e\x43\x6f\x75\x6e\x74\x22\x03\x41\x64\x64\x0a\x26\x0a\x08\x72"
    "\x6f\x77\x43\x6f\x75\x6e\x74\x0a\x04\x61\x78\x69\x73\x12\x09\x72\x6f\x77"
    "\x56\x65\x63\x74\x6f\x72\x22\x09\x55\x6e\x73\x71\x75\x65\x65\x7a\x65\x0a"
    "\x2c\x0a\x0b\x63\x6f\x6c\x75\x6d\x6e\x43\x6f\x75\x6e\x74\x0a\x04\x61\x78"
    "\x69\x73\x12\x0c\x63\x6f\x6c\x75\x6d\x6e\x56\x65\x63\x74\x6f\x72\x22\x09"
    "\x55\x6e\x73\x71\x75\x65\x65\x7a\x65\x0a\x3a\x0a\x03\x6f\x6e\x65\x0a\x09"
    "\x72\x6f\x77\x56\x65\x63\x74\x6f\x72\x0a\x0c\x63\x6f\x6c\x75\x6d\x6e\x56"
    "\x65\x63\x74\x6f\x72\x12\x05\x73\x68\x61\x70\x65\x22\x06\x43\x6f\x6e\x63"
    "\x61\x74\x2a\x0b\x0a\x04\x61\x78\x69\x73\x18\x00\xa0\x01\x02\x0a\x3e\x0a"
    "\x05\x73\x68\x61\x70\x65\x12\x06\x6f\x75\x74\x70\x75\x74\x22\x0f\x43\x6f"
    "\x6e\x73\x74\x61\x6e\x74\x4f\x66\x53\x68\x61\x70\x65\x2a\x1c\x0a\x05\x76"
    "\x61\x6c\x75\x65\x2a\x10\x08\x01\x10\x01\x22\x04\x00\x00\x00\x00\x42\x04"
    "\x7a\x65\x72\x6f\xa0\x01\x04\x12\x19\x64\x79\x6e\x61\x6d\x69\x63\x2d\x70"
    "\x65\x72\x73\x6f\x6e\x2d\x65\x6e\x64\x2d\x74\x6f\x2d\x65\x6e\x64\x2a\x13"
    "\x10\x07\x42\x05\x66\x69\x72\x73\x74\x4a\x08\x00\x00\x00\x00\x00\x00\x00"
    "\x00\x2a\x13\x08\x01\x10\x07\x42\x03\x6f\x6e\x65\x4a\x08\x01\x00\x00\x00"
    "\x00\x00\x00\x00\x2a\x12\x10\x07\x42\x04\x72\x6f\x77\x73\x4a\x08\x2c\x01"
    "\x00\x00\x00\x00\x00\x00\x2a\x15\x10\x07\x42\x07\x63\x6f\x6c\x75\x6d\x6e"
    "\x73\x4a\x08\x06\x00\x00\x00\x00\x00\x00\x00\x2a\x14\x08\x01\x10\x07\x42"
    "\x04\x61\x78\x69\x73\x4a\x08\x00\x00\x00\x00\x00\x00\x00\x00\x2a\x19\x08"
    "\x01\x10\x07\x42\x09\x66\x6c\x61\x74\x53\x68\x61\x70\x65\x4a\x08\xff\xff"
    "\xff\xff\xff\xff\xff\xff\x5a\x30\x0a\x06\x69\x6d\x61\x67\x65\x73\x12\x26"
    "\x0a\x24\x08\x01\x12\x20\x0a\x07\x12\x05\x62\x61\x74\x63\x68\x0a\x02\x08"
    "\x03\x0a\x08\x12\x06\x68\x65\x69\x67\x68\x74\x0a\x07\x12\x05\x77\x69\x64"
    "\x74\x68\x62\x27\x0a\x06\x6f\x75\x74\x70\x75\x74\x12\x1d\x0a\x1b\x08\x01"
    "\x12\x17\x0a\x02\x08\x01\x0a\x06\x12\x04\x72\x6f\x77\x73\x0a\x09\x12\x07"
    "\x63\x6f\x6c\x75\x6d\x6e\x73\x42\x04\x0a\x00\x10\x11";
  const auto dynamicPath = root / "dynamic.onnx";
  {
    std::ofstream model(dynamicPath, std::ios::binary);
    model.write(dynamicModelData, sizeof(dynamicModelData) - 1);
    BOOST_REQUIRE(model.good());
  }
  UavPersonDetectorSession dynamic(dynamicPath, 0.25F, 0.7F, 3,
                                   UavPersonDetectorSession::ModelFormat::PERSON_STOCK);
  UavRectLetterboxedImage input;
  input.tensorWidth = 32;
  input.tensorHeight = 32;
  input.image.tensor.resize(3 * 32 * 32);
  BOOST_CHECK(dynamic.detect(input).empty());
  input.image.tensor[0] = 1;
  BOOST_CHECK_EXCEPTION(dynamic.detect(input), std::runtime_error,
    [] (const auto& error) {
      return std::string(error.what()) ==
             "end-to-end person detector returned an unexpected tensor";
    });
}

BOOST_AUTO_TEST_CASE(DetectorLetterboxPreservesSquareAndStrideAlignedRectangle)
{
  UavRgbImage image;
  image.width = 30;
  image.height = 40;
  image.rgb.assign(3 * image.width * image.height, 127);

  const auto square = letterboxPersonImage(image);
  BOOST_REQUIRE_EQUAL(square.tensor.size(), 3 * 640 * 640);
  BOOST_CHECK_EQUAL(square.padX, 80);
  BOOST_CHECK_EQUAL(square.tensor.front(), 114.0F / 255.0F);

#ifdef NDNSF_UAV_TEST_HAS_OPENCV
  const auto rectangular = letterboxRectangularImage(image);
  BOOST_REQUIRE_EQUAL(rectangular.image.tensor.size(), 3 * 480 * 640);
  BOOST_CHECK_EQUAL(rectangular.tensorWidth, 480);
  BOOST_CHECK_EQUAL(rectangular.tensorHeight, 640);
  BOOST_CHECK_EQUAL(rectangular.image.padX, 0);
  BOOST_CHECK_EQUAL(rectangular.image.padY, 0);
  BOOST_CHECK_EQUAL(rectangular.image.tensor.front(), 127.0F / 255.0F);
#else
  BOOST_CHECK_THROW(letterboxRectangularImage(image), std::runtime_error);
#endif
}

BOOST_FIXTURE_TEST_CASE(InjectedSourceStartsAndStopsWithProvider, ProviderFixture)
{
  auto source = std::make_shared<RecordingObservationSource>();
  auto provider = std::make_unique<UavTrackingProvider>(providerConfig, source);
  BOOST_CHECK(source->started);
  BOOST_CHECK(!source->stopped);
  provider->stopAdmission();
  BOOST_CHECK(source->stopped);
  provider->cancelActiveWork();
  environment->pumpUntilWithAttributeAuthority([&] { return provider->isDrained(); });
  BOOST_REQUIRE(provider->isDrained());
  provider.reset();
}

BOOST_FIXTURE_TEST_CASE(NoncanonicalWindowsCannotAliasAnAdmittedSourceWindow, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  const auto decision = [&] {
    nsf::RequestMessage request;
    auto payload = asFields(fields);
    request.setPayload(payload, payload.size());
    return UavTrackingProviderTestAccess::acknowledge(*capability, request);
  };
  BOOST_REQUIRE(decision().status);
  for (const auto* invalid : {"window-00", "window-0suffix", "window-+0", "window--0"}) {
    fields["window_id"] = invalid;
    BOOST_CHECK(!decision().status);
  }
}

BOOST_FIXTURE_TEST_CASE(ProtectedAckReadsDiscoveryButCannotReplaceFormalInput, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  nsf::RequestCapabilities caps;
  caps.setField("RequestScopedConfidentialityV1", "required");
  caps.setField("UavTrackingDiscoveryV1", "required");
  for (const auto& [key, value] : fields) caps.setField("UavTracking." + key, value);
  nsf::RequestMessage request;
  request.setRequestCapabilities(caps);
  BOOST_CHECK(UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
  BOOST_CHECK_THROW(validateUavTrackingDiscoveryBinding(request), std::invalid_argument);
  caps.setField("UavTracking.source_epoch", "2");
  request.setRequestCapabilities(caps);
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
  caps.setField("UavTracking.source_epoch", "1");
  caps.setField("UavTracking.run_id", "undeclared-run");
  request.setRequestCapabilities(caps);
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*capability, request).status);
}

BOOST_FIXTURE_TEST_CASE(RequestBudgetControlsAckRetentionAndRejectsUnpreparedWork, ProviderFixture)
{
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"},
    {"tracking_request_timeout_ms", "10000"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  const auto decide = [&] {
    nsf::RequestMessage request;
    auto payload = asFields(fields);
    request.setPayload(payload, payload.size());
    return UavTrackingProviderTestAccess::acknowledge(*capability, request);
  };
  BOOST_REQUIRE(decide().status);
  BOOST_CHECK_EQUAL(decide().pendingStateTtlMs, 10000);
  for (const auto* bad : {"0", "1000", "010001", "+10001", "10001tail", "30001", "1200001"}) {
    fields["tracking_request_timeout_ms"] = bad;
    BOOST_CHECK(!decide().status);
  }
  fields.erase("tracking_request_timeout_ms");
  BOOST_CHECK(!decide().status);
  auto overHost = providerConfig;
  overHost.runs[0].requestTimeoutMs = 30001;
  auto notReady = std::make_unique<UavTrackingProvider>(overHost);
  fields["tracking_request_timeout_ms"] = "30001";
  nsf::RequestMessage request;
  auto bytes = asFields(fields);
  request.setPayload(bytes, bytes.size());
  BOOST_CHECK(!UavTrackingProviderTestAccess::acknowledge(*notReady, request).status);
}

BOOST_FIXTURE_TEST_CASE(ExplicitHostAndPreparedBudgetPreserveLongerAckHorizon, ProviderFixture)
{
  const auto* configured = std::getenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS");
  const auto previous = configured ? std::optional<std::string>(configured) : std::nullopt;
  BOOST_SCOPE_EXIT_ALL(&, previous) {
    if (previous) ::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", previous->c_str(), 1);
    else ::unsetenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS");
  };
  BOOST_REQUIRE_EQUAL(::setenv("NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS", "60000", 1), 0);
  providerConfig.runs[0].requestTimeoutMs = 60000;
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"run_id", definition.runId},
    {"mission_id", "tracking-" + definition.runId}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "1"},
    {"tracking_request_timeout_ms", "60000"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  auto bytes = asFields(fields);
  nsf::RequestMessage request;
  request.setPayload(bytes, bytes.size());
  const auto decision = UavTrackingProviderTestAccess::acknowledge(*capability, request);
  BOOST_REQUIRE(decision.status);
  BOOST_CHECK_EQUAL(decision.pendingStateTtlMs, 60000);
}

BOOST_FIXTURE_TEST_CASE(CommitAuthorityUsesTheCompleteDispatchFence, ProviderFixture)
{
  for (const auto* scenario : {"valid", "expired", "not-current", "missing", "throw", "revoked"}) {
    nsf::ServiceProvider::CollaborationAssignment assignment;
    assignment.role = "Tracking-Compute";
    assignment.service = SERVICE_TRACKING_WINDOW;
    assignment.workDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    assignment.workCurrent = [] { return true; };
    assignment.scopeKeys["test-scope"] = ndn::Buffer(32, 0x42);
    const std::string name(scenario);
    if (name == "expired") {
      assignment.workDeadline = std::chrono::steady_clock::now() - std::chrono::milliseconds(1);
    }
    if (name == "not-current") assignment.workCurrent = [] { return false; };
    if (name == "missing") assignment.workCurrent = {};
    if (name == "throw") {
      assignment.workCurrent = [] () -> bool { throw std::runtime_error("authority unavailable"); };
    }
    nsf::ServiceProvider::CollaborationContext context(environment->provider(3),
      environment->profile().userIdentity, ndn::Name("/test/authority").append(name),
      nsf::RequestMessage{}, assignment);
    if (name == "revoked") BOOST_CHECK(context.revokeCurrentWorkAuthority());
    const bool valid = name == "valid";
    BOOST_CHECK_EQUAL(context.hasCurrentWorkAuthority(), valid);
    UavTrackingWorkerTransaction transaction(root / ("authority-" + name));
    const bool committed = transaction.commit("window-0", tracking_fixture::ABC_DIGEST,
      asBuffer("result"), asBuffer("state"), [&context] { return context.hasCurrentWorkAuthority(); });
    BOOST_CHECK_EQUAL(committed, valid);
    BOOST_CHECK_EQUAL(transaction.committedStatePath("window-0").has_value(), valid);
    const auto dataName = ndn::Name(environment->provider(3).getName())
      .append("Result").append(name).appendVersion(1);
    if (name == "throw") {
      BOOST_CHECK_THROW(context.publishReusableLargeNamed("test-scope", dataName,
        asBuffer("result")), std::runtime_error);
    }
    else if (!valid) {
      BOOST_CHECK(context.publishReusableLargeNamed("test-scope", dataName,
        asBuffer("result")).empty());
    }
    else {
      auto publication = std::async(std::launch::async, [&] {
        return context.publishReusableLargeNamed("test-scope", dataName, asBuffer("result"));
      });
      environment->pumpUntilWithAttributeAuthority([&] {
        return publication.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready;
      });
      BOOST_CHECK(publication.get() == dataName);
    }
  }
}

BOOST_FIXTURE_TEST_CASE(InitialAssetAndHandlerInspectionHonorsTheSynchronousAuthorityPredicate,
                        ProviderFixture)
{
  startFailingCompute();
  const auto cancelled = [] (const std::exception& error) {
    return std::string(error.what()).find("inspection cancelled") != std::string::npos;
  };
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [] { return false; }), std::runtime_error, cancelled);
  auto assetChecks = std::make_shared<unsigned>(0);
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [assetChecks] { return (*assetChecks)++ == 0; }),
    std::runtime_error, cancelled);
  BOOST_CHECK(*assetChecks >= 2);
  BOOST_CHECK_NO_THROW(UavTrackingProviderTestAccess::verifyAssets(
    *capability, definition.runId, [] { return true; }));
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [] { return false; }), std::runtime_error, cancelled);
  auto handlerChecks = std::make_shared<unsigned>(0);
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [handlerChecks] { return (*handlerChecks)++ == 0; }),
    std::runtime_error, cancelled);
  BOOST_CHECK(*handlerChecks >= 2);
  // Positive authority reaches the real model loader.
  BOOST_CHECK_EXCEPTION(UavTrackingProviderTestAccess::initializeHandlers(
    *capability, definition.runId, [] { return true; }), std::exception,
    [] (const std::exception& error) {
      return std::string(error.what()).find("invalid-detector.onnx") != std::string::npos;
    });
}

BOOST_FIXTURE_TEST_CASE(FirstHandlerInitializationFailurePublishesWorkDrained, ProviderFixture)
{
  startFailingCompute();
  const auto payload = asFields({{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"},
    {"run_id", definition.runId}, {"mission_id", "tracking-" + definition.runId},
    {"window_id", "window-0"}, {"observation_schema", "uav-observation-v2"},
    {"source_epoch", "1"}, {"source_uav1_id", definition.sources[0].sourceId},
    {"source_uav2_id", definition.sources[1].sourceId},
    {"source_uav3_id", definition.sources[2].sourceId}});
  std::vector<ndn::Name> sources;
  for (const auto& source : definition.sources) sources.push_back(source.producerIdentity);
  auto committed = std::make_shared<bool>(false);
  auto selectedCount = std::make_shared<size_t>(0);
  auto responseReceived = std::make_shared<bool>(false);
  UavTrackingCoordinatorCallbacks callbacks;
  callbacks.onPlanCommitted = [committed, selectedCount] (const auto&, const auto& selected) {
    *committed = true;
    *selectedCount = selected.size();
  };
  callbacks.onFailure = [] (const auto& reason) { BOOST_FAIL(reason); };
  callbacks.onTimeout = [this, committed, selectedCount] (const auto&) {
    BOOST_FAIL("initialization failure response missing; plan_committed=" << *committed
      << " selected_count=" << *selectedCount
      << " synthetic_acknowledgements=" << syntheticAcknowledgements.load()
      << " source_handler_executions=" << executions);
  };
  callbacks.onResponse = [responseReceived] (const nsf::ResponseMessage& response) {
    BOOST_CHECK(!response.getStatus());
    const auto bytes = response.getPayload();
    const auto fields = decodeFields(std::string(bytes.begin(), bytes.end()));
    BOOST_CHECK(fieldOr(fields, "failure_reason", "").find("invalid-detector.onnx") !=
                std::string::npos);
    *responseReceived = true;
  };
  const auto request = UavTrackingCoordinator{}.begin(environment->user(), SERVICE_TRACKING_WINDOW,
    payload, UavTrackingCoordinator::makePlan(sources, definition.computeIdentity, payload, 1000, 15000),
    std::move(callbacks));
  BOOST_REQUIRE(!request.empty());
  const auto responseDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(16);
  const auto responseDone = [&] {
    return *responseReceived || std::chrono::steady_clock::now() >= responseDeadline;
  };
  while (!responseDone()) environment->pumpUntilWithAttributeAuthority(responseDone);
  BOOST_REQUIRE(*committed);
  BOOST_REQUIRE(*responseReceived);
  const auto hasWorkDrainedProof = [&] {
    const auto available = environment->user().waitForVerifiedCollaborationData(request,
      TRACKING_INPUT_KEY_SCOPE, TRACKING_WORK_DRAINED_TOPIC, 1, 1, false);
    return std::any_of(available.begin(), available.end(), [this] (const auto& record) {
      return record.producer == definition.computeIdentity && record.producerRole == "Tracking-Compute";
    });
  };
  const auto proofDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  const auto proofDone = [&] {
    return hasWorkDrainedProof() || std::chrono::steady_clock::now() >= proofDeadline;
  };
  while (!proofDone()) environment->pumpUntilWithAttributeAuthority(proofDone);
  const auto records = environment->user().waitForVerifiedCollaborationData(request,
    TRACKING_INPUT_KEY_SCOPE, TRACKING_WORK_DRAINED_TOPIC, 1, 1, false);
  const auto proof = std::find_if(records.begin(), records.end(), [this] (const auto& record) {
    return record.producer == definition.computeIdentity && record.producerRole == "Tracking-Compute";
  });
  BOOST_REQUIRE(proof != records.end());
  const auto fields = decodeFields(std::string(proof->payload.begin(), proof->payload.end()));
  BOOST_CHECK_EQUAL(fieldOr(fields, "work_drained", ""), "true");
  BOOST_CHECK_EQUAL(fieldOr(fields, "request_id", ""), request.toUri());
  BOOST_CHECK_EQUAL(fieldOr(fields, "run_id", ""), definition.runId);
  BOOST_CHECK_EQUAL(fieldOr(fields, "mission_id", ""), "tracking-" + definition.runId);
  BOOST_CHECK_EQUAL(fieldOr(fields, "source_epoch", ""), "1");
  environment->user().CancelCollaboration(request);
}

BOOST_FIXTURE_TEST_CASE(NativePersonCompletesWithoutWorkerAssets, ProviderFixture,
                       *boost::unit_test::disabled())
{
  checkNativePersonWindow(0);
}

BOOST_FIXTURE_TEST_CASE(OnePersonSourceFailureDrainsWithoutInference, ProviderFixture,
                       *boost::unit_test::disabled())
{
  checkNativePersonWindow(1);
}

BOOST_FIXTURE_TEST_CASE(AllPersonSourceFailuresDrainWithoutInference, ProviderFixture,
                       *boost::unit_test::disabled())
{
  checkNativePersonWindow(3);
}

BOOST_FIXTURE_TEST_CASE(InitializationFailureLeavesTheJobFailedButClean, ProviderFixture)
{
  startFailingCompute();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  const auto failedAndClean = [this] {
    return latest.state == TrackingJobState::FAILED && latest.cleanupState == "CLEAN";
  };
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(17);
  while (!failedAndClean() && std::chrono::steady_clock::now() < deadline) {
    environment->pumpUntilWithAttributeAuthority(failedAndClean);
  }
  BOOST_REQUIRE(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
}

BOOST_FIXTURE_TEST_CASE(SourceUsesInjectedProducerAndServesOnlyItsPublishedFrame, ProviderFixture)
{
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  auto done = std::make_shared<bool>(false);
  auto received = std::make_shared<ndn::Buffer>();
  auto success = std::make_shared<bool>(false);
  const auto request = environment->user().RequestServiceTargeted(definition.sources[0].producerIdentity,
    SERVICE_TRACKING_SOURCE, frameRequest(), 3000,
    [done] (const ndn::Name&) { *done = true; },
    [done, received, success] (const nsf::ResponseMessage& response) {
      *success = response.getStatus();
      *received = response.getPayload();
      *done = true;
    });
  BOOST_REQUIRE(!request.empty());
  environment->pumpUntilWithAttributeAuthority([done] { return *done; });
  BOOST_REQUIRE(*done);
  BOOST_REQUIRE(*success);
  const ndn::Buffer expected{0xff, 0xd8, 0xff, 0xd9};
  BOOST_CHECK_EQUAL_COLLECTIONS(received->begin(), received->end(), expected.begin(), expected.end());

  auto infoDone = std::make_shared<bool>(false);
  auto infoPayload = std::make_shared<std::string>();
  auto infoSuccess = std::make_shared<bool>(false);
  const auto infoRequest = renderUavSourceInfoRequest(
    UavSourceInfoRequest{"UAV1", "", 0});
  nsf::RequestMessage infoMessage;
  auto infoBuffer = ndn::Buffer(reinterpret_cast<const uint8_t*>(infoRequest.data()),
                                infoRequest.size());
  infoMessage.setPayload(infoBuffer, infoBuffer.size());
  const auto infoId = environment->user().RequestServiceTargeted(
    definition.sources[0].producerIdentity, SERVICE_TRACKING_SOURCE, infoMessage, 3000,
    [infoDone] (const ndn::Name&) { *infoDone = true; },
    [infoDone, infoPayload, infoSuccess] (const nsf::ResponseMessage& response) {
      *infoSuccess = response.getStatus();
      const auto& payload = response.getPayload();
      infoPayload->assign(reinterpret_cast<const char*>(payload.data()), payload.size());
      *infoDone = true;
    });
  BOOST_REQUIRE(!infoId.empty());
  environment->pumpUntilWithAttributeAuthority([infoDone] { return *infoDone; });
  BOOST_REQUIRE(*infoSuccess);
  const auto info = parseUavSourceInfoResponse(*infoPayload);
  BOOST_CHECK_EQUAL(info.descriptor.cameraId, "UAV1");
  BOOST_CHECK_EQUAL(info.descriptor.sourceId, definition.sources[0].sourceId);
  BOOST_CHECK_EQUAL(info.descriptor.sourceEpoch, definition.sources[0].sourceEpoch);
  BOOST_CHECK_EQUAL(info.digest, uavSourceDescriptorDigest(info.descriptor));

  const auto expectRejected = [&] (const std::string& requestText) {
    auto done = std::make_shared<bool>(false);
    auto success = std::make_shared<bool>(true);
    nsf::RequestMessage message;
    auto buffer = ndn::Buffer(reinterpret_cast<const uint8_t*>(requestText.data()),
                              requestText.size());
    message.setPayload(buffer, buffer.size());
    const auto id = environment->user().RequestServiceTargeted(
      definition.sources[0].producerIdentity, SERVICE_TRACKING_SOURCE, message, 3000,
      [done] (const ndn::Name&) { *done = true; },
      [done, success] (const nsf::ResponseMessage& response) {
        *success = response.getStatus();
        *done = true;
      });
    BOOST_REQUIRE(!id.empty());
    environment->pumpUntilWithAttributeAuthority([done] { return *done; });
    return *success;
  };
  BOOST_CHECK(!expectRejected(renderUavSourceInfoRequest(
    UavSourceInfoRequest{"UAV1", "different-source", 0})));
  BOOST_CHECK(!expectRejected(renderUavSourceInfoRequest(
    UavSourceInfoRequest{"UAV3", "", 0})));
  BOOST_CHECK(!expectRejected(renderUavSourceInfoRequest(
    UavSourceInfoRequest{"UAV1", "", 999})));
}

BOOST_FIXTURE_TEST_CASE(ShutdownCancelsAnOwnedSourceReaderAndPreservesTheHostFace, ProviderFixture)
{
  holdSource();
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  bool opened = false;
  environment->pumpUntilWithAttributeAuthority([&] { return opened = sourceOpened(); });
  BOOST_REQUIRE(opened);
  capability->stopAdmission();
  capability->cancelActiveWork();
  environment->pumpUntilWithAttributeAuthority([this] { return capability->isDrained(); });
  BOOST_REQUIRE(capability->isDrained());
  auto alive = std::make_shared<bool>(false);
  boost::asio::post(environment->userFace().getIoContext(), [alive] { *alive = true; });
  environment->pumpUntilWithAttributeAuthority([alive] { return *alive; });
  BOOST_CHECK(*alive);
}

BOOST_FIXTURE_TEST_CASE(OrdinaryServiceRepliesWhileNativeComputeOwnsItsCandidate, ProviderFixture)
{
  startBlockingCompute();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  pumpUntilWithin([this] {
    return std::filesystem::exists(root / "compute-entered");
  }, std::chrono::seconds(12));
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  BOOST_REQUIRE(latest.state == TrackingJobState::RUNNING);
  BOOST_CHECK(!capability->isDrained());
  auto done = std::make_shared<bool>(false);
  auto replied = std::make_shared<bool>(false);
  nsf::RequestMessage request;
  auto payload = asBuffer("ordinary-service-isolation");
  request.setPayload(payload, payload.size());
  const auto requestId = environment->user().RequestServiceTargeted(definition.computeIdentity,
    SERVICE_TRACKING_SOURCE, request, 2000,
    [done] (const ndn::Name&) { *done = true; },
    [done, replied] (const nsf::ResponseMessage& response) {
      *replied = response.getStatus();
      *done = true;
    });
  BOOST_REQUIRE(!requestId.empty());
  environment->pumpUntilWithAttributeAuthority([done] { return *done; });
  BOOST_REQUIRE(*replied);
  BOOST_CHECK(latest.state == TrackingJobState::RUNNING);
  BOOST_CHECK(!capability->isDrained());
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()), static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] {
    return latest.state == TrackingJobState::FAILED && latest.cleanupState == "CLEAN";
  });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.cleanupState, "CLEAN");
}

BOOST_FIXTURE_TEST_CASE(NativeComputeCommitsOnlyCppDecisionsAndExportsTheSameMeasurements, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  pumpUntilWithin([this] {
    return std::filesystem::exists(root / "compute-entered");
  }, std::chrono::seconds(12));
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()), static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  BOOST_REQUIRE(latest.latestResult);
  const auto result = Json::parse(*latest.latestResult);
  BOOST_CHECK_EQUAL(result["decisionSchema"], "uav-vehicle-native-decision-v1");
  BOOST_CHECK_EQUAL(latest.actualDevice, "cpu");
  BOOST_CHECK_EQUAL(latest.sourcePtsUs, 1000);
  for (size_t i = 0; i < 3; ++i) {
    BOOST_CHECK_EQUAL(result["frames"][i]["tracks"][0]["localId"], 1);
    BOOST_CHECK_EQUAL(result["frames"][i]["tracks"][0]["globalId"], i + 1);
    BOOST_CHECK_EQUAL(result["frames"][i]["inputDigest"], digestOf(std::string("\xff\xd8\xff\xd9", 4)));
  }
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  const auto committed = transactions.committedStatePath("window-0");
  BOOST_REQUIRE(committed);
  std::ifstream stateFile(*committed);
  const auto state = Json::parse(stateFile);
  BOOST_CHECK_EQUAL(state["schema"], "uav-vehicle-native-state-v1");
  BOOST_CHECK_EQUAL(state["nativeDecisionDigest"], result["nativeDecisionDigest"]);
  std::ifstream csv(worker / "annotated/motion-measurements-000001.csv");
  BOOST_REQUIRE(csv);
  const std::string text((std::istreambuf_iterator<char>(csv)), {});
  BOOST_CHECK(text.find("schema,sourceId,cameraId,sourceEpoch,sequence,localTrackId,trackGeneration,globalId,") == 0);
  BOOST_CHECK(text.find("camera-source-1,UAV1,1,1,1,1,1,") != std::string::npos);
  BOOST_CHECK(text.find(",unknown,unknown,unknown,") != std::string::npos);
}

BOOST_FIXTURE_TEST_CASE(CancelledNativeCandidateNeverPromotesATransactionMarker, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  pumpUntilWithin([this] {
    return std::filesystem::exists(root / "compute-entered");
  }, std::chrono::seconds(12));
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  BOOST_REQUIRE(job->requestCancel(definition.jobId) == TrackingCancelResult::REQUESTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::CANCELLED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  BOOST_CHECK(!transactions.committedStatePath("window-0"));
}

BOOST_FIXTURE_TEST_CASE(BackendCannotInjectGlobalDecisionsIntoCommittedState, ProviderFixture)
{
  startBlockingCompute(true);
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  pumpUntilWithin([this] {
    return std::filesystem::exists(root / "compute-entered");
  }, std::chrono::seconds(12));
  BOOST_REQUIRE(std::filesystem::exists(root / "compute-entered"));
  const auto path = root / "controlled-candidate.json";
  std::ifstream input(path);
  auto candidate = Json::parse(input);
  input.close();
  candidate["frames"][0]["detections"][0]["globalId"] = 999;
  std::ofstream(path) << candidate.dump();
  const std::string release = "release\n";
  BOOST_REQUIRE_EQUAL(::write(writer, release.data(), release.size()),
                      static_cast<ssize_t>(release.size()));
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(latest.completedWindows, 0);
  const auto worker = providerConfig.ownedWorkspace / "vehicle/native-default" / definition.runId;
  UavTrackingWorkerTransaction transactions(
    worker / "transactions" / digestOf("tracking-" + definition.runId).substr(7));
  BOOST_CHECK(!transactions.committedStatePath("window-0"));
}

BOOST_FIXTURE_TEST_CASE(HeavySourceRejectsAnotherRunAndNextPreparedRunExecutes, ProviderFixture)
{
  auto nextGeneration = Json::parse(tracking_fixture::SOURCE_MANIFEST);
  nextGeneration["windowCount"] = 1;
  nextGeneration["generations"][0]["sourceEpoch"] = 2;
  const auto manifest = nextGeneration.dump();
  std::ofstream(root / "next-generation") << manifest;
  auto nextRun = providerConfig.runs[0];
  nextRun.runId = "run-2";
  nextRun.sourceGenerations = {root / "next-generation", digestOf(manifest)};
  nextRun.observationStreamDirectory = root / "next-streams";
  std::filesystem::create_directory(nextRun.observationStreamDirectory);
  auto nextFrame = Json::parse(FRAME_EVENT);
  nextFrame["payload"]["key"]["epoch"] = "2";
  std::ofstream(nextRun.observationStreamDirectory / "window-0.jsonl")
    << CALIBRATION_EVENT << '\n' << nextFrame.dump() << '\n';
  providerConfig.runs.push_back(nextRun);
  holdSource();
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  bool opened = false;
  environment->pumpUntilWithAttributeAuthority([&] { return opened = sourceOpened(); });
  BOOST_REQUIRE(opened);
  std::vector<ndn::Name> sources;
  for (const auto& source : definition.sources) sources.push_back(source.producerIdentity);
  const auto payload = asFields({{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"}, {"run_id", "run-2"},
    {"mission_id", "tracking-run-2"}, {"window_id", "window-0"},
    {"observation_schema", "uav-observation-v2"}, {"source_epoch", "2"},
    {"source_uav1_id", definition.sources[0].sourceId},
    {"source_uav2_id", definition.sources[1].sourceId},
    {"source_uav3_id", definition.sources[2].sourceId}});
  nsf::RequestMessage busyRequest;
  auto busyPayload = payload;
  busyRequest.setPayload(busyPayload, busyPayload.size());
  // Invoke the same native ACK decision used by the registered Core wrapper.
  // A short ACK collection timer alone could reject for network latency and
  // would not prove that this specific capability rejected heavy admission.
  const auto decision = UavTrackingProviderTestAccess::acknowledge(*capability, busyRequest);
  BOOST_CHECK(!decision.status);
  BOOST_CHECK_EQUAL(decision.message, "TRACKING_BUSY");
  BOOST_REQUIRE(latest.state == TrackingJobState::RUNNING);
  const auto stream = CALIBRATION_EVENT + '\n' + FRAME_EVENT + '\n';
  BOOST_REQUIRE_EQUAL(::write(writer, stream.data(), stream.size()), static_cast<ssize_t>(stream.size()));
  ::close(writer);
  writer = -1;
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_REQUIRE(latest.state == TrackingJobState::COMPLETED);
  const auto previousPresentation = latest.presentation;
  definition.runId = "run-2";
  definition.jobId = "job-2";
  definition.outputDirectory = root / "output-run-2";
  for (auto& source : definition.sources) {
    source.sourceEpoch = 2;
    source.sourceManifest = nextRun.sourceGenerations;
  }
  std::string admissionReason;
  BOOST_REQUIRE_MESSAGE(job->start(definition, &admissionReason) == TrackingAdmission::ACCEPTED,
                        admissionReason);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.cleanupState == "CLEAN"; });
  BOOST_CHECK(latest.state == TrackingJobState::COMPLETED);
  BOOST_REQUIRE(latest.presentation);
  BOOST_CHECK(latest.presentation != previousPresentation);
  BOOST_CHECK_EQUAL(latest.presentation->runId, "run-2");
  BOOST_CHECK_EQUAL(latest.presentation->generation, 2);
  for (const auto& image : latest.presentation->frames) BOOST_CHECK_EQUAL(image.sourceEpoch, 2);
}

BOOST_FIXTURE_TEST_CASE(UnsupportedPersonProfileIsNotReadyBeforeAnyModelExecution, ProviderFixture)
{
  const auto asset = [&] (const char* name, const std::string& bytes) {
    const auto path = root / name;
    std::ofstream(path) << bytes;
    return TrackingAssetReference{path, digestOf(bytes)};
  };
  auto profileWire = person_json::Json::parse(PERSON_PROFILE_GOLDEN);
  profileWire["requestedDevice"] = "cuda:0";
  profileWire["detectorDigest"] = digestOf("abc");
  profileWire["reidDigest"] = digestOf("abc");
  const auto profile = parseUavPersonTrackingProfile(profileWire.dump());
  const auto manifest = makeUavPersonNativeComputationManifest(profile,
    "bytetrack-ultralytics-v1", "onnxruntime-fixture",
    {{"executable:/fixture/provider", digestOf("abc")}});
  providerConfig.producerIdentity = definition.computeIdentity;
  auto& run = providerConfig.runs.front();
  run.mode = TrackingMode::PERSON;
  run.role = TrackingProviderRole::COMPUTE;
  run.cameraId.clear();
  run.observationStreamDirectory.clear();
  run.model = asset("model.onnx", "abc");
  run.reidModel = run.model;
  run.profile = asset("profile.json", profile.canonicalJson);
  run.computationManifest = asset("computation.json", manifest.canonicalJson);
  capability = std::make_unique<UavTrackingProvider>(providerConfig);
  Fields fields{{"schema", "spec191-window-request-v1"}, {"tracking_request_timeout_ms", "10000"},
    {"run_id", definition.runId}, {"mission_id", "tracking-" + definition.runId},
    {"window_id", "window-0"}, {"observation_schema", "uav-observation-v2"},
    {"source_epoch", "1"}};
  for (size_t i = 0; i < 3; ++i) {
    fields["source_uav" + std::to_string(i + 1) + "_id"] = definition.sources[i].sourceId;
  }
  nsf::RequestMessage request;
  auto bytes = asFields(fields);
  request.setPayload(bytes, bytes.size());
  const auto decision = UavTrackingProviderTestAccess::acknowledge(*capability, request);
  BOOST_CHECK(!decision.status);
  BOOST_CHECK(decision.message.find("CPU only") != std::string::npos);
  BOOST_CHECK(!std::filesystem::exists(providerConfig.ownedWorkspace / "person"));
}

BOOST_FIXTURE_TEST_CASE(BadAssetIsNotReadyWhileItsHostStillProcessesCallbacks, ProviderFixture)
{
  providerConfig.runs[0].sourceGenerations.digest = "sha256:" + std::string(64, '0');
  startProvider();
  BOOST_REQUIRE(job->start(definition) == TrackingAdmission::ACCEPTED);
  environment->pumpUntilWithAttributeAuthority([this] { return latest.state == TrackingJobState::FAILED; });
  BOOST_CHECK(latest.state == TrackingJobState::FAILED);
  BOOST_CHECK_EQUAL(executions.load(), 0);
}

BOOST_FIXTURE_TEST_CASE(RejectsConflictingProducerAndReusedEpochCatalogs, ProviderFixture)
{
  auto wrong = providerConfig;
  wrong.runs[0].sourceProducers[0] = wrong.runs[0].computeProducer;
  BOOST_CHECK_THROW(static_cast<void>(UavTrackingProvider{wrong}),
                    std::invalid_argument);
  auto reused = providerConfig.runs[0];
  reused.runId = "another-run";
  providerConfig.runs.push_back(std::move(reused));
  BOOST_CHECK_THROW(static_cast<void>(UavTrackingProvider{providerConfig}),
                    std::invalid_argument);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::test

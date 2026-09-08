# UAV API 参考

声明从源码语法树提取。保留准确类型、参数、默认值、限定符和原始注释；注释不代替运行证据。中文语义契约见开发者指南。protected 扩展点、测试 helper、应用内部接口各自标注。

## NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp

源码 SHA-256：`54273acb7cbd9526c17b668594455cae5b70a4ea13a6bc50ff65440a4c979381`。

### API-df138ab3554f · FlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4)

```cpp
class FlightControllerBackend
```

### API-97acf51de495 · FlightControllerBackend::~FlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L7)

```cpp
virtual ~FlightControllerBackend() = default;
```

### API-b1e46a3def66 · FlightControllerBackend::sendMavlink

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L8)

```cpp
virtual Fields sendMavlink(const std::vector<uint8_t>& frame,
                             const std::string& commandName) = 0;
```

### API-3e54f6ace058 · FlightControllerBackend::latestTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L10)

```cpp
virtual Fields latestTelemetry() = 0;
```

### API-b87aa863be9a · FlightControllerBackend::parameterSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L11)

```cpp
virtual VehicleParameterSnapshot parameterSnapshot() = 0;
```

### API-ff4941a7533b · FlightControllerBackend::editParameter

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L12)

```cpp
virtual VehicleParameterEditResult editParameter(const VehicleParameterEditRequest& request)
```

### API-f1cb47b44480 · FlightControllerBackend::executeMissionWaypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L27)

```cpp
virtual Fields executeMissionWaypoints(const std::vector<std::pair<std::string, std::string>>& waypoints,
                                         const Fields& missionFields)
```

### API-2eab9b2a98ee · FlightControllerBackend::description

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L62)

```cpp
virtual std::string description() const = 0;
```

### API-744f43859471 · MockFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L65)

```cpp
class MockFlightControllerBackend : public FlightControllerBackend
```

### API-74295fa0f9ec · MockFlightControllerBackend::MockFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L68)

```cpp
explicit MockFlightControllerBackend(std::string droneId)
```

### API-c4f6835e7d67 · MockFlightControllerBackend::sendMavlink

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L79)

```cpp
Fields
  sendMavlink(const std::vector<uint8_t>& frame,
              const std::string& commandName) override
```

### API-4542f4333661 · MockFlightControllerBackend::description

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L140)

```cpp
std::string
  description() const override
```

### API-bc34476fc124 · MockFlightControllerBackend::latestTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L146)

```cpp
Fields
  latestTelemetry() override
```

### API-7c20dffd21be · MockFlightControllerBackend::parameterSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L202)

```cpp
VehicleParameterSnapshot
  parameterSnapshot() override
```

### API-24dba5522f41 · MockFlightControllerBackend::editParameter

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L219)

```cpp
VehicleParameterEditResult
  editParameter(const VehicleParameterEditRequest& request) override
```

### API-772d741df584 · UdpFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L277)

```cpp
class UdpFlightControllerBackend : public FlightControllerBackend
```

### API-437470013f19 · UdpFlightControllerBackend::UdpFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L280)

```cpp
UdpFlightControllerBackend(std::string droneId, std::string host, std::string port,
                             std::string listenPort, bool configurePx4SitlDemoParams)
```

### API-98eba030eea8 · UdpFlightControllerBackend::UdpFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L291)

```cpp
UdpFlightControllerBackend(std::string droneId, std::string serialDevice,
                             std::string serialBaud)
```

### API-fbc0e7447f77 · UdpFlightControllerBackend::~UdpFlightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L300)

```cpp
~UdpFlightControllerBackend()
```

### API-76b58451421c · UdpFlightControllerBackend::sendMavlink

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L314)

```cpp
Fields
  sendMavlink(const std::vector<uint8_t>& frame,
              const std::string& commandName) override
```

### API-946c5d95d3d6 · UdpFlightControllerBackend::description

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L371)

```cpp
std::string
  description() const override
```

### API-b1fbde358fd1 · UdpFlightControllerBackend::executeMissionWaypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L380)

```cpp
Fields
  executeMissionWaypoints(const std::vector<std::pair<std::string, std::string>>& waypoints,
                          const Fields& missionFields) override
```

### API-1b92c67ebc57 · UdpFlightControllerBackend::latestTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L502)

```cpp
Fields
  latestTelemetry() override
```

### API-da6c4dee5dea · UdpFlightControllerBackend::parameterSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L518)

```cpp
VehicleParameterSnapshot
  parameterSnapshot() override
```

### API-fdc4cf1634eb · VideoPublisher

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1426)

```cpp
class VideoPublisher
```

### API-db1c7c082b64 · VideoPublisher::CameraRuntimeOptions

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1429)

```cpp
struct CameraRuntimeOptions
```

### API-afa005c9f779 · VideoPublisher::CameraRuntimeOptions::captureOnStart

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1431)

```cpp
bool captureOnStart = false;
```

### API-3654dd615ccb · VideoPublisher::CameraRuntimeOptions::recordToLocalRepo

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1432)

```cpp
bool recordToLocalRepo = false;
```

### API-803758dfffb1 · VideoPublisher::CameraRuntimeOptions::recordRepoPath

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1433)

```cpp
std::string recordRepoPath;
```

### API-14c551613cd3 · VideoPublisher::CameraRuntimeOptions::recordObjectPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1434)

```cpp
std::string recordObjectPrefix;
```

### API-fd5433f7b923 · VideoPublisher::CameraRuntimeOptions::recordPacketLimit

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1435)

```cpp
uint64_t recordPacketLimit = 0;
```

### API-f36acb3068b5 · VideoPublisher::CameraRuntimeOptions::v4l2InputFormat

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1436)

```cpp
std::string v4l2InputFormat = "auto";
```

### API-efef70422032 · VideoPublisher::CameraRuntimeOptions::v4l2InputSize

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1437)

```cpp
std::string v4l2InputSize = "auto";
```

### API-382ed4365b77 · VideoPublisher::CameraRuntimeOptions::v4l2InputFps

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1438)

```cpp
uint64_t v4l2InputFps = 0;
```

### API-6177e5ae3a12 · VideoPublisher::VideoPublisher

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1441)

```cpp
VideoPublisher(ndn_service_framework::ServiceProvider& serviceProvider,
                 ndn::Face& face, ndn::KeyChain& keyChain,
                 ndn_service_framework::LocalServiceRegistry& localRegistry,
                 ndn::Name localArchivedPacketServiceName,
                 UavRuntimeConfig config, std::string droneId, std::string videoPath,
                 CameraRuntimeOptions options)
```

### API-0a243bee88c0 · VideoPublisher::~VideoPublisher

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1535)

```cpp
~VideoPublisher()
```

### API-7321f0da7d7e · VideoPublisher::shutdown

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1540)

```cpp
void
  shutdown()
```

### API-22708ed2b701 · VideoPublisher::randomBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1564)

```cpp
static ndn::Buffer
  randomBytes(size_t size)
```

### API-1f0a060d4ead · VideoPublisher::lowerHex

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1574)

```cpp
static std::string
  lowerHex(const ndn::Buffer& value)
```

### API-46dbcbac402b · VideoPublisher::randomNonZeroUint64

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1587)

```cpp
static uint64_t
  randomNonZeroUint64()
```

### API-3650958dd7c3 · VideoPublisher::ensureFutureSampleAnnouncementsLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1598)

```cpp
void
  ensureFutureSampleAnnouncementsLocked(uint64_t throughSampleId)
```

### API-3aeb76c29a78 · VideoPublisher::initializeProtectedSessionLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1629)

```cpp
void
  initializeProtectedSessionLocked()
```

### API-fa5ab3680868 · VideoPublisher::clearProtectedSessionLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1894)

```cpp
void
  clearProtectedSessionLocked()
```

### API-e68d86de95e2 · VideoPublisher::fieldAsUint64

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1916)

```cpp
static uint64_t
  fieldAsUint64(const Fields& fields, const std::string& key, uint64_t fallback)
```

### API-c3039535e11f · VideoPublisher::startFingerprintLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1927)

```cpp
std::string
  startFingerprintLocked() const
```

### API-785613741bbf · VideoPublisher::refreshDescriptorSnapshotLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1936)

```cpp
void
  refreshDescriptorSnapshotLocked()
```

### API-0dcbfd17cf22 · VideoPublisher::makeStartFailureFieldsLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1980)

```cpp
Fields
  makeStartFailureFieldsLocked(const std::string& reason) const
```

### API-d189d7d62d8c · VideoPublisher::makeStartResponseFieldsLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L1994)

```cpp
Fields
  makeStartResponseFieldsLocked() const
```

### API-f9d95ad48b9d · VideoPublisher::start

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2032)

```cpp
Fields
  start(const Fields& requestFields)
```

### API-cb3d3497a5e3 · VideoPublisher::stop

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2155)

```cpp
Fields
  stop()
```

### API-98178957d67b · VideoPublisher::stopWithReason

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2161)

```cpp
Fields
  stopWithReason(const std::string& reason)
```

### API-7f2a9de1df6d · VideoPublisher::isStreaming

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2226)

```cpp
bool
  isStreaming() const
```

### API-36fc1ead3d58 · VideoPublisher::isCapturing

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2232)

```cpp
bool
  isCapturing() const
```

### API-f6e15fdc9c87 · VideoPublisher::isRecording

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2238)

```cpp
bool
  isRecording() const
```

### API-1c8b93f9f65d · VideoPublisher::isRecordingEnabled

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2245)

```cpp
bool
  isRecordingEnabled() const
```

### API-f4ef12a4a3a4 · VideoPublisher::isRepoOpen

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2251)

```cpp
bool
  isRepoOpen() const
```

### API-4edf61de496e · VideoPublisher::recordingRepoPath

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2257)

```cpp
std::string
  recordingRepoPath() const
```

### API-8b9eafae2501 · VideoPublisher::recordingObjectPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2263)

```cpp
std::string
  recordingObjectPrefix() const
```

### API-d138cfbbb320 · VideoPublisher::cameraAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2269)

```cpp
bool
  cameraAvailable() const
```

### API-4dd07e5822db · VideoPublisher::cameraSource

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2278)

```cpp
std::string
  cameraSource() const
```

### API-1956a8e92248 · VideoPublisher::cameraReason

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2284)

```cpp
std::string
  cameraReason() const
```

### API-2a82821f38af · VideoPublisher::streamPacketsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2302)

```cpp
uint64_t
  streamPacketsPublished() const
```

### API-3b5f7be1a111 · VideoPublisher::fecGroupsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2308)

```cpp
uint64_t
  fecGroupsPublished() const
```

### API-9602e8fc6dd9 · VideoPublisher::recordingChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2314)

```cpp
uint64_t
  recordingChunks() const
```

### API-b0bffc8d4713 · VideoPublisher::recordingBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2320)

```cpp
uint64_t
  recordingBytes() const
```

### API-5409a9f553f1 · VideoPublisher::recordingLastUpdateMs

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2326)

```cpp
uint64_t
  recordingLastUpdateMs() const
```

### API-d15353244f19 · VideoPublisher::recordingPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2332)

```cpp
std::string
  recordingPrefix() const
```

### API-57b07fa75ae3 · VideoPublisher::recordingManifestFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2338)

```cpp
Fields
  recordingManifestFields() const
```

### API-4f7e65eba705 · VideoPublisher::recordingManifestFieldsFor

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2394)

```cpp
Fields
  recordingManifestFieldsFor(const ndn::Name& requesterIdentity) const
```

### API-f8c006385b44 · VideoPublisher::startRetention

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2438)

```cpp
Fields
  startRetention()
```

### API-e5a524bfd89f · VideoPublisher::finalizeRetention

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2533)

```cpp
Fields
  finalizeRetention()
```

### API-9ade87fbb036 · VideoPublisher::repoStatusFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2615)

```cpp
Fields
  repoStatusFields() const
```

### API-ed8452e06a90 · VideoPublisher::recordingCatalogFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2634)

```cpp
Fields
  recordingCatalogFields() const
```

### API-65944647aa36 · VideoPublisher::archivedPacketWire

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2671)

```cpp
std::vector<uint8_t>
  archivedPacketWire(const std::string& objectName) const
```

### API-d7494186ca64 · VideoPublisher::streamPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L2711)

```cpp
ndn::Name
  streamPrefix() const
```

### API-06a0b62b94e4 · DroneServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4176)

```cpp
class DroneServiceContainer
```

### API-030548a3ff30 · DroneServiceContainer::DroneServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4231)

```cpp
DroneServiceContainer(std::string droneId, bool available, bool serveCertificates,
               UavRuntimeConfig config,
               std::string videoPath, std::string flightControllerBackend,
               std::string mavlinkUdpHost, std::string mavlinkUdpPort,
               std::string mavlinkUdpListenPort, std::string mavlinkSerialDevice,
               std::string mavlinkSerialBaud,
               bool configurePx4SitlDemoParams,
               VideoPublisher::CameraRuntimeOptions cameraOptions)
```

### API-4f47ff27c162 · DroneServiceContainer::~DroneServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4279)

```cpp
~DroneServiceContainer()
```

### API-7e7968aca747 · DroneServiceContainer::start

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4291)

```cpp
void
  start()
```

### API-8ccaa146e656 · DroneServiceContainer::waitUntilReady

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4361)

```cpp
bool
  waitUntilReady(std::chrono::seconds timeout)
```

### API-5dbd3bf38291 · DroneServiceContainer::ndnsfContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4377)

```cpp
ndn_service_framework::ServiceContainer&
  ndnsfContainer()
```

### API-ed5290903e95 · DroneServiceContainer::localRegistry

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4383)

```cpp
ndn_service_framework::LocalServiceRegistry&
  localRegistry()
```

### API-202b3601a0bc · DroneServiceContainer::setStatusCallback

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4389)

```cpp
void
  setStatusCallback(std::function<void(std::string)> callback)
```

### API-fe2910dfff4b · DroneServiceContainer::isStreaming

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4395)

```cpp
bool
  isStreaming() const
```

### API-050ad7daf99c · DroneServiceContainer::isCapturing

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4402)

```cpp
bool
  isCapturing() const
```

### API-8a5e2a47f637 · DroneServiceContainer::isRecording

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4409)

```cpp
bool
  isRecording() const
```

### API-139f88cedd75 · DroneServiceContainer::cameraStatusFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4416)

```cpp
Fields
  cameraStatusFields() const
```

### API-ffcbad72d270 · DroneServiceContainer::streamPacketsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4429)

```cpp
uint64_t
  streamPacketsPublished() const
```

### API-6fc09df7181d · DroneServiceContainer::fecGroupsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4436)

```cpp
uint64_t
  fecGroupsPublished() const
```

### API-a52b7d6e469b · DroneServiceContainer::recordingChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4443)

```cpp
uint64_t
  recordingChunks() const
```

### API-504d1649f06b · DroneServiceContainer::recordingBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4450)

```cpp
uint64_t
  recordingBytes() const
```

### API-65b48300a539 · DroneServiceContainer::recordingManifestFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4457)

```cpp
Fields
  recordingManifestFields() const
```

### API-adc5a7abf788 · DroneServiceContainer::recordingManifestFieldsFor

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4473)

```cpp
Fields
  recordingManifestFieldsFor(const ndn::Name& requesterIdentity) const
```

### API-4109f7cd100c · DroneServiceContainer::repoStatusFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4485)

```cpp
Fields
  repoStatusFields() const
```

### API-179de6d621b1 · DroneServiceContainer::recordingCatalogFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4509)

```cpp
Fields
  recordingCatalogFields() const
```

### API-0b02c0681a61 · DroneServiceContainer::archivedPacketWire

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4527)

```cpp
std::vector<uint8_t>
  archivedPacketWire(const std::string& objectName) const
```

### API-87c50746fb9f · DroneServiceContainer::latestTelemetryState

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4535)

```cpp
TelemetryState
  latestTelemetryState() const
```

### API-6c5e5aae4d24 · DroneServiceContainer::latestReadinessState

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4560)

```cpp
ReadinessState
  latestReadinessState() const
```

### API-78e598b2e6c3 · DroneServiceContainer::latestVideoState

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4566)

```cpp
VideoState
  latestVideoState() const
```

### API-caf19a591add · DroneServiceContainer::preflightChecklistFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4572)

```cpp
Fields
  preflightChecklistFields()
```

### API-477b05d5d378 · DroneServiceContainer::analyzeSnapshotFields

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4633)

```cpp
Fields
  analyzeSnapshotFields(const MissionState& mission)
```

### API-dfef62bba254 · DroneServiceContainer::identityUri

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp#L4676)

```cpp
std::string
  identityUri() const
```

## NDNSF-UAV-APP/drone/DroneWindow.inc.hpp

源码 SHA-256：`45fa715a74c8c1cb142b48b49352ef441151463f269c0604305a87517c2fe2e9`。

### API-7adadd0a50fa · DroneWindow

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneWindow.inc.hpp#L4)

```cpp
class DroneWindow : public Gtk::Window
```

### API-def68f256187 · DroneWindow::DroneWindow

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/DroneWindow.inc.hpp#L7)

```cpp
explicit DroneWindow(DroneServiceContainer& runtime, std::string flightControllerStatusFile)
```

## NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp

源码 SHA-256：`777f60e7a9ee0f193b8d843b48988d5be0ac0fcfd7b55bdcb49364ec2a602d59`。

### API-70f3003d3510 · ndnsf::examples::uav::UavCollaborationParticipant

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L10)

```cpp
class UavCollaborationParticipant
```

### API-f19d676a7691 · ndnsf::examples::uav::UavCollaborationParticipant::UavCollaborationParticipant

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L13)

```cpp
UavCollaborationParticipant(ndn::Name providerIdentity,
                             std::string role,
                             std::optional<UavDetectorProvider> detector = std::nullopt);
```

### API-363bb3f298af · ndnsf::examples::uav::UavCollaborationParticipant::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L17)

```cpp
const ndn::Name& providerIdentity() const noexcept
```

### API-fe1b4c47b25b · ndnsf::examples::uav::UavCollaborationParticipant::role

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L18)

```cpp
const std::string& role() const noexcept
```

### API-c671628e72d5 · ndnsf::examples::uav::UavCollaborationParticipant::freezeEvidence

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L21)

```cpp
std::optional<UavEvidenceReference>
  freezeEvidence(const std::string& missionId, const std::string& incidentId,
                 const std::string& evidenceId, uint64_t version,
                 const std::string& streamId, uint64_t streamSessionEpoch,
                 uint64_t firstSequence, uint64_t lastSequence,
                 uint64_t windowStartMs, uint64_t windowEndMs,
                 const ndn::Buffer& bytes, uint64_t retentionDeadlineMs,
                 std::string contentType = "application/octet-stream",
                 std::string* reason = nullptr) const;
```

原始接口说明：

```text
/** Freeze a bounded producer-owned evidence object; bytes remain outside the job payload. */
```

### API-c66908f0e604 · ndnsf::examples::uav::UavCollaborationParticipant::validateAssignment

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L31)

```cpp
bool validateAssignment(const UavCollaborationJobRecord& job,
                          const UavRoleAssignment& assignment,
                          std::string* reason = nullptr) const;
```

### API-42c024a0852c · ndnsf::examples::uav::UavCollaborationParticipant::executeDetector

public / application-internal；[源码](../../NDNSF-UAV-APP/drone/UavCollaborationParticipant.hpp#L35)

```cpp
std::optional<UavTerminalReport>
  executeDetector(const UavCollaborationJobRecord& job,
                  const UavVerifiedEvidence& evidence,
                  uint64_t reportVersion = 1,
                  std::string* reason = nullptr) const;
```

## NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp

源码 SHA-256：`16a050a024f5f657a909d2aae03fb599269239575ed71c3e13ae4e3341ce27e7`。

### API-1612b4a04934 · ndnsf::examples::uav::RuntimeAvailability

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L16)

```cpp
enum class RuntimeAvailability
```

### API-ce7266172331 · ndnsf::examples::uav::RuntimeAvailability::Unknown

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L18)

```cpp
Unknown
```

### API-f48b897343a8 · ndnsf::examples::uav::RuntimeAvailability::Available

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L19)

```cpp
Available
```

### API-f5cd15e0d410 · ndnsf::examples::uav::RuntimeAvailability::Unavailable

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L20)

```cpp
Unavailable
```

### API-56f2ed7bfb6f · ndnsf::examples::uav::CommandLifecycle

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L23)

```cpp
enum class CommandLifecycle
```

### API-321c4e61de99 · ndnsf::examples::uav::CommandLifecycle::Idle

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L25)

```cpp
Idle
```

### API-39548eecf073 · ndnsf::examples::uav::CommandLifecycle::Sending

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L26)

```cpp
Sending
```

### API-a51c04f47e27 · ndnsf::examples::uav::CommandLifecycle::AckWait

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L27)

```cpp
AckWait
```

### API-25e6f10a2c78 · ndnsf::examples::uav::CommandLifecycle::Running

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L28)

```cpp
Running
```

### API-04139985fc04 · ndnsf::examples::uav::CommandLifecycle::Success

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L29)

```cpp
Success
```

### API-31177b9111d9 · ndnsf::examples::uav::CommandLifecycle::Timeout

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L30)

```cpp
Timeout
```

### API-e3b5b448a8d3 · ndnsf::examples::uav::CommandLifecycle::Failed

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L31)

```cpp
Failed
```

### API-8d7fa6c4f95e · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L34)

```cpp
inline const char*
to_string(CommandLifecycle lifecycle)
```

### API-fcb9aa027f83 · ndnsf::examples::uav::RuntimeConnectionState

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L57)

```cpp
enum class RuntimeConnectionState
```

### API-2cc525c38298 · ndnsf::examples::uav::RuntimeConnectionState::Unknown

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L59)

```cpp
Unknown
```

### API-d60928127e0a · ndnsf::examples::uav::RuntimeConnectionState::Online

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L60)

```cpp
Online
```

### API-969a0cd97638 · ndnsf::examples::uav::RuntimeConnectionState::Stale

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L61)

```cpp
Stale
```

### API-5051d116b900 · ndnsf::examples::uav::RuntimeConnectionState::Offline

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L62)

```cpp
Offline
```

### API-b40bb613ad22 · ndnsf::examples::uav::NotReadyReason

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L65)

```cpp
enum class NotReadyReason
```

### API-9992f47c3532 · ndnsf::examples::uav::NotReadyReason::Certificate

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L67)

```cpp
Certificate
```

### API-18255d01f468 · ndnsf::examples::uav::NotReadyReason::FlightController

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L68)

```cpp
FlightController
```

### API-9f8f9a5959e7 · ndnsf::examples::uav::NotReadyReason::Camera

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L69)

```cpp
Camera
```

### API-065bda4e2573 · ndnsf::examples::uav::NotReadyReason::Repo

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L70)

```cpp
Repo
```

### API-eaa921f47c72 · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L73)

```cpp
inline const char*
to_string(NotReadyReason reason)
```

### API-f428a3fdae48 · ndnsf::examples::uav::RuntimeCommandSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L90)

```cpp
struct RuntimeCommandSnapshot
```

### API-e2b136386b58 · ndnsf::examples::uav::RuntimeCommandSnapshot::command

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L92)

```cpp
std::string command = "none";
```

### API-4e5d033646f5 · ndnsf::examples::uav::RuntimeCommandSnapshot::lifecycle

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L93)

```cpp
CommandLifecycle lifecycle = CommandLifecycle::Idle;
```

### API-2c4e475b1a93 · ndnsf::examples::uav::RuntimeCommandSnapshot::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L94)

```cpp
std::string detail = "idle";
```

### API-7595082a01b0 · ndnsf::examples::uav::RuntimeCommandSnapshot::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L95)

```cpp
uint64_t updatedMs = 0;
```

### API-25e730b6cf84 · ndnsf::examples::uav::RuntimeCommandSnapshot::rttMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L96)

```cpp
uint64_t rttMs = 0;
```

### API-125209c5db38 · ndnsf::examples::uav::RuntimeCommandSnapshot::timeoutMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L97)

```cpp
uint64_t timeoutMs = 0;
```

### API-6ce057ef02a0 · ndnsf::examples::uav::OperatorAuthorityAlert

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L100)

```cpp
struct OperatorAuthorityAlert
```

### API-0a699f4600bf · ndnsf::examples::uav::OperatorAuthorityAlert::type

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L102)

```cpp
std::string type = "unknown";
```

### API-cedcc1bb043a · ndnsf::examples::uav::OperatorAuthorityAlert::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L103)

```cpp
std::string reason = "unknown";
```

### API-dbec5e61bca7 · ndnsf::examples::uav::OperatorAuthorityAlert::leaseId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L104)

```cpp
std::string leaseId = "none";
```

### API-5cfc94f3012f · ndnsf::examples::uav::OperatorAuthorityAlert::revokedOperator

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L105)

```cpp
std::string revokedOperator = "unknown";
```

### API-770ad5ef36e0 · ndnsf::examples::uav::OperatorAuthorityAlert::revokerOperator

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L106)

```cpp
std::string revokerOperator = "unknown";
```

### API-0506e604b689 · ndnsf::examples::uav::OperatorAuthorityAlert::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L107)

```cpp
std::string droneId = "unknown";
```

### API-136d63a6f6f2 · ndnsf::examples::uav::OperatorAuthorityAlert::scope

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L108)

```cpp
std::string scope = "unknown";
```

### API-71a08d0dc31a · ndnsf::examples::uav::OperatorAuthorityAlert::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L109)

```cpp
uint64_t updatedMs = 0;
```

### API-fd6388108426 · ndnsf::examples::uav::OperatorAuthorityAlert::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L111)

```cpp
std::string
  statusLine() const
```

### API-93b6163abd32 · ndnsf::examples::uav::VehicleRuntimeState

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L126)

```cpp
struct VehicleRuntimeState
```

### API-6676441b1e1e · ndnsf::examples::uav::VehicleRuntimeState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L128)

```cpp
std::string droneId = "unknown";
```

### API-3e8bbfaa373b · ndnsf::examples::uav::VehicleRuntimeState::connection

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L129)

```cpp
RuntimeConnectionState connection = RuntimeConnectionState::Unknown;
```

### API-f91837c78d03 · ndnsf::examples::uav::VehicleRuntimeState::telemetryReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L130)

```cpp
RuntimeAvailability telemetryReady = RuntimeAvailability::Unknown;
```

### API-a5d5825b8db9 · ndnsf::examples::uav::VehicleRuntimeState::videoReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L131)

```cpp
RuntimeAvailability videoReady = RuntimeAvailability::Unknown;
```

### API-a8e8aad3ba6c · ndnsf::examples::uav::VehicleRuntimeState::cameraReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L132)

```cpp
RuntimeAvailability cameraReady = RuntimeAvailability::Unknown;
```

### API-73aeff5ab7fc · ndnsf::examples::uav::VehicleRuntimeState::flightControllerReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L133)

```cpp
RuntimeAvailability flightControllerReady = RuntimeAvailability::Unknown;
```

### API-2ada978f1969 · ndnsf::examples::uav::VehicleRuntimeState::missionReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L134)

```cpp
RuntimeAvailability missionReady = RuntimeAvailability::Unknown;
```

### API-4d93a77850bf · ndnsf::examples::uav::VehicleRuntimeState::repoReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L135)

```cpp
RuntimeAvailability repoReady = RuntimeAvailability::Unknown;
```

### API-38034da8fc7d · ndnsf::examples::uav::VehicleRuntimeState::commandStates

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L136)

```cpp
std::map<std::string, RuntimeCommandSnapshot> commandStates;
```

### API-2fb38bcbdf92 · ndnsf::examples::uav::VehicleRuntimeState::commandHistory

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L137)

```cpp
std::vector<RuntimeCommandSnapshot> commandHistory;
```

### API-93d6871031d4 · ndnsf::examples::uav::VehicleRuntimeState::telemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L139)

```cpp
std::optional<TelemetryState> telemetry;
```

### API-46289ee08564 · ndnsf::examples::uav::VehicleRuntimeState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L140)

```cpp
std::optional<ReadinessState> readiness;
```

### API-10ffe5d4e56b · ndnsf::examples::uav::VehicleRuntimeState::lastFlightCommand

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L141)

```cpp
std::optional<FlightCommandState> lastFlightCommand;
```

### API-22797747accc · ndnsf::examples::uav::VehicleRuntimeState::safety

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L142)

```cpp
std::optional<SafetyState> safety;
```

### API-826fa1e87890 · ndnsf::examples::uav::VehicleRuntimeState::video

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L143)

```cpp
std::optional<VideoState> video;
```

### API-ea299cc2520c · ndnsf::examples::uav::VehicleRuntimeState::videoAdaptive

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L144)

```cpp
std::optional<VideoAdaptiveState> videoAdaptive;
```

### API-847cf2df702e · ndnsf::examples::uav::VehicleRuntimeState::mission

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L145)

```cpp
std::optional<MissionState> mission;
```

### API-5eadf1211dab · ndnsf::examples::uav::VehicleRuntimeState::missionProgress

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L146)

```cpp
std::optional<MissionProgressState> missionProgress;
```

### API-594d06cf1a6b · ndnsf::examples::uav::VehicleRuntimeState::notReadyReasons

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L147)

```cpp
std::vector<NotReadyReason> notReadyReasons;
```

### API-f84b5cfb4675 · ndnsf::examples::uav::VehicleRuntimeState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L148)

```cpp
uint64_t updatedMs = 0;
```

### API-ec206d4c18b9 · ndnsf::examples::uav::VehicleRuntimeState::clearNotReadyReasons

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L150)

```cpp
void
  clearNotReadyReasons()
```

### API-73c4b1b023ed · ndnsf::examples::uav::VehicleRuntimeState::appendNotReadyReason

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L156)

```cpp
void
  appendNotReadyReason(NotReadyReason reason)
```

### API-e03025264cf8 · ndnsf::examples::uav::VehicleRuntimeState::ready

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L164)

```cpp
bool
  ready() const
```

### API-0bfaf9aeb9d1 · ndnsf::examples::uav::VehicleRuntimeState::appendCommandHistory

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L170)

```cpp
void
  appendCommandHistory(RuntimeCommandSnapshot state, size_t maxEntries = 10)
```

### API-69cf549de2b6 · ndnsf::examples::uav::VehicleRuntimeState::notReadyReasonText

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L180)

```cpp
std::string
  notReadyReasonText() const
```

### API-e242fee5560d · ndnsf::examples::uav::GroundStationRuntimeState

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L197)

```cpp
struct GroundStationRuntimeState
```

### API-b93c80ef115d · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L199)

```cpp
struct CollaborationView
```

### API-108479dee617 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L201)

```cpp
std::string missionId;
```

### API-d4565e61bab5 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::incidentId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L202)

```cpp
std::string incidentId;
```

### API-a576f5e971eb · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::attemptId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L203)

```cpp
std::string attemptId;
```

### API-826fd3da9008 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L204)

```cpp
std::string requestId;
```

### API-5a86f89770af · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::stage

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L205)

```cpp
std::string stage = "idle";
```

### API-6f36c1383912 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::selectedProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L206)

```cpp
std::string selectedProvider;
```

### API-fe7eafd28c11 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::terminalOwner

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L207)

```cpp
std::string terminalOwner;
```

### API-067a83db6dfb · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::failureStage

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L208)

```cpp
std::string failureStage;
```

### API-3674eee12d07 · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::fallbackMode

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L209)

```cpp
std::string fallbackMode = "disabled";
```

### API-e8d0a3e959cf · ndnsf::examples::uav::GroundStationRuntimeState::CollaborationView::deadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L210)

```cpp
uint64_t deadlineMs = 0;
```

### API-c41a0c45727c · ndnsf::examples::uav::GroundStationRuntimeState::selectedDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L213)

```cpp
std::string selectedDroneId = "unknown";
```

### API-3a3a6aff7133 · ndnsf::examples::uav::GroundStationRuntimeState::selectedDroneLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L214)

```cpp
bool selectedDroneLocked = false;
```

### API-f1066d9fdf14 · ndnsf::examples::uav::GroundStationRuntimeState::drones

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L215)

```cpp
std::map<std::string, VehicleRuntimeState> drones;
```

### API-3ac6ed0081d3 · ndnsf::examples::uav::GroundStationRuntimeState::missionPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L216)

```cpp
std::optional<MissionPlan> missionPlan;
```

### API-1528961cb6d0 · ndnsf::examples::uav::GroundStationRuntimeState::missionProgress

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L217)

```cpp
std::optional<MissionProgressState> missionProgress;
```

### API-25217fc51030 · ndnsf::examples::uav::GroundStationRuntimeState::operatorAuthorityAlerts

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L218)

```cpp
std::vector<OperatorAuthorityAlert> operatorAuthorityAlerts;
```

### API-c5cd5a6a1e6d · ndnsf::examples::uav::GroundStationRuntimeState::collaboration

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L219)

```cpp
CollaborationView collaboration;
```

### API-0089c1d75bbb · ndnsf::examples::uav::GroundStationRuntimeState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L220)

```cpp
uint64_t updatedMs = 0;
```

### API-ca873c63702e · ndnsf::examples::uav::GroundStationRuntimeState::findDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L222)

```cpp
const VehicleRuntimeState*
  findDrone(const std::string& droneId) const
```

### API-4456f241ba90 · ndnsf::examples::uav::GroundStationRuntimeState::ensureDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationRuntimeState.hpp#L229)

```cpp
VehicleRuntimeState&
  ensureDrone(const std::string& droneId)
```

## NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp

源码 SHA-256：`c76301414a80e6ccd79dda53e4353cfc3e56bef22526db3f089b06855f87101a`。

### API-adf1c45dbd76 · GroundStationServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L4)

```cpp
class GroundStationServiceContainer
```

### API-1a30cf4bd08a · GroundStationServiceContainer::GroundStationServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L7)

```cpp
GroundStationServiceContainer(bool serveCertificates, int ackTimeoutMs, int timeoutMs,
                       UavRuntimeConfig config,
                       std::string targetDroneId, uint64_t videoBitrateKbps,
                       uint64_t videoFps, uint64_t videoFrameWidth,
                       uint64_t videoFecParityShards,
                       std::string liveStreamPrefetchPolicy = "mapped-pressure",
                       std::vector<std::string> patrolDroneIds = {},
                       std::string yoloModel = "yolo26n.pt",
                       std::string yoloScript = "NDNSF-UAV-APP/tools/yolo_detect_once.py",
                       std::string yoloWorkerScript = "NDNSF-UAV-APP/tools/yolo_detect_worker.py",
                       uint64_t linkStaleMs = 3500,
                       uint64_t linkLostMs = 8000,
                       std::string lostLinkAction = "notify",
                       std::string videoBitratePolicy = "manual",
                       uint64_t videoBitrateAutoPressureMs = 2500,
                       std::string missionPlanFilePath = "",
                       std::string operatorId = "",
                       std::string operatorLeaseDrone = "all",
                       std::string operatorLeaseScope = "control",
                       uint64_t operatorLeaseTtlMs = 0,
                       std::string operatorAuthorityStateFile = "",
                       std::string operatorAdminIds = "",
                       uint64_t operatorAuthorityRefreshIntervalMs = 0)
```

### API-bc46b119b103 · GroundStationServiceContainer::~GroundStationServiceContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L104)

```cpp
~GroundStationServiceContainer()
```

### API-09336b752174 · GroundStationServiceContainer::shutdownRuntime

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L109)

```cpp
void
  shutdownRuntime()
```

### API-438080467b95 · GroundStationServiceContainer::start

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L144)

```cpp
void
  start()
```

### API-5a2c4b8ba967 · GroundStationServiceContainer::waitUntilReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L187)

```cpp
bool
  waitUntilReady(std::chrono::seconds timeout)
```

### API-28e6a6c4e18a · GroundStationServiceContainer::ndnsfContainer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L203)

```cpp
ndn_service_framework::ServiceContainer&
  ndnsfContainer()
```

### API-aa6005ecf5e4 · GroundStationServiceContainer::localRegistry

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L209)

```cpp
ndn_service_framework::LocalServiceRegistry&
  localRegistry()
```

### API-d6347413db1c · GroundStationServiceContainer::config

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L215)

```cpp
const UavRuntimeConfig&
  config() const
```

### API-bd58ec11e67a · GroundStationServiceContainer::setOperatorAuthorityLease

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L221)

```cpp
void
  setOperatorAuthorityLease(OperatorAuthorityLease lease)
```

### API-fcd2886e192d · GroundStationServiceContainer::operatorAuthorityLease

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L233)

```cpp
OperatorAuthorityLease
  operatorAuthorityLease() const
```

### API-188e16318322 · GroundStationServiceContainer::refreshOperatorAuthorityLeaseFromIssuer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L240)

```cpp
bool
  refreshOperatorAuthorityLeaseFromIssuer(const ndn::Name& issuerIdentity,
                                          std::chrono::seconds timeout,
                                          std::string& reason,
                                          Fields* revocationFields = nullptr)
```

### API-63e38787b89f · GroundStationServiceContainer::operatorAuthorityRefreshIntervalMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L299)

```cpp
uint64_t
  operatorAuthorityRefreshIntervalMs() const
```

### API-c46b48f6988f · GroundStationServiceContainer::operatorAuthorityAlertsSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L305)

```cpp
std::vector<OperatorAuthorityAlert>
  operatorAuthorityAlertsSnapshot() const
```

### API-e79770ea2fd1 · GroundStationServiceContainer::setStatusCallback

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L312)

```cpp
void
  setStatusCallback(std::function<void(std::string)> callback)
```

### API-c979f9f55b87 · GroundStationServiceContainer::setFrameCallback

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L318)

```cpp
void
  setFrameCallback(std::function<void(UavVideoFrame, uint64_t, std::string, uint64_t)> callback)
```

### API-1366d5328ad1 · GroundStationServiceContainer::activeVideoStreamId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L324)

```cpp
std::string
  activeVideoStreamId() const
```

### API-5947f5a395ee · GroundStationServiceContainer::videoStreamSessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L331)

```cpp
uint64_t
  videoStreamSessionEpoch() const
```

### API-b79d40e62441 · GroundStationServiceContainer::isCurrentStreamSession

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L337)

```cpp
bool
  isCurrentStreamSession(uint64_t streamSessionEpoch) const
```

### API-a2b616c5da4a · GroundStationServiceContainer::makeVideoSessionId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L343)

```cpp
std::string
  makeVideoSessionId(const std::string& tag, const std::string& droneId)
```

### API-52f2f93a4bee · GroundStationServiceContainer::allocateStreamSessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L350)

```cpp
uint64_t
  allocateStreamSessionEpoch(std::string streamId)
```

### API-72f512075666 · GroundStationServiceContainer::allocateStreamSessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L361)

```cpp
uint64_t
  allocateStreamSessionEpoch(std::string streamId, uint64_t streamSessionEpoch)
```

### API-8b04b5458b38 · GroundStationServiceContainer::syncStreamSessionFromActiveId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L388)

```cpp
void
  syncStreamSessionFromActiveId()
```

### API-1025920ca97a · GroundStationServiceContainer::streamSessionEpochForStreamId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L398)

```cpp
uint64_t
  streamSessionEpochForStreamId(const std::string& streamId) const
```

### API-1259f376300e · GroundStationServiceContainer::isCurrentVideoSessionPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L406)

```cpp
bool
  isCurrentVideoSessionPacket(const std::string& streamId, uint64_t streamSessionEpoch) const
```

### API-786064d9cd38 · GroundStationServiceContainer::isCurrentSessionStreamId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L415)

```cpp
bool
  isCurrentSessionStreamId(const std::string& streamId) const
```

### API-512c99728387 · GroundStationServiceContainer::setActiveStreamSession

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L422)

```cpp
void
  setActiveStreamSession(std::string streamId)
```

### API-a147dd6c5353 · GroundStationServiceContainer::isCurrentLiveVideoStream

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L430)

```cpp
bool
  isCurrentLiveVideoStream(const std::string& streamId) const
```

### API-90221924a484 · GroundStationServiceContainer::targetDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L437)

```cpp
std::string
  targetDroneId() const
```

### API-567982c77a3c · GroundStationServiceContainer::groundStationIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L444)

```cpp
const ndn::Name&
  groundStationIdentity() const
```

### API-3fa921749358 · GroundStationServiceContainer::setTargetDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L450)

```cpp
void
  setTargetDroneId(std::string droneId)
```

### API-3bd6b7768249 · GroundStationServiceContainer::TargetSelectionSource

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L456)

```cpp
enum class TargetSelectionSource
```

### API-9899e508500b · GroundStationServiceContainer::TargetSelectionSource::User

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L458)

```cpp
User
```

### API-38ed109425f3 · GroundStationServiceContainer::TargetSelectionSource::Auto

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L459)

```cpp
Auto
```

### API-bd04f5ecc871 · GroundStationServiceContainer::TargetSelectionSource::Internal

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L460)

```cpp
Internal
```

### API-d0a04829f438 · GroundStationServiceContainer::setTargetDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L463)

```cpp
void
  setTargetDroneId(std::string droneId, TargetSelectionSource source)
```

### API-3a4844a2f46b · GroundStationServiceContainer::isTargetDroneLocked

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L488)

```cpp
bool
  isTargetDroneLocked() const
```

### API-47d300a78ca8 · GroundStationServiceContainer::missionReadyDrones

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L495)

```cpp
std::vector<std::string>
  missionReadyDrones() const
```

### API-e391921bd929 · GroundStationServiceContainer::missionStartableDrones

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L502)

```cpp
std::vector<std::string>
  missionStartableDrones() const
```

### API-38b5f86edd35 · GroundStationServiceContainer::injectMissionStateForTest

public / test-helper；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L522)

```cpp
void
  injectMissionStateForTest(MissionState mission)
```

### API-00002c0fe7c5 · GroundStationServiceContainer::injectMissionProgressForTest

public / test-helper；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L543)

```cpp
void
  injectMissionProgressForTest(MissionProgressState progress)
```

### API-a25fbc2b631f · GroundStationServiceContainer::serviceCatalogForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L552)

```cpp
std::string
  serviceCatalogForDrone(const std::string& droneId) const
```

### API-09631da1ef54 · GroundStationServiceContainer::logServiceCatalogForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L579)

```cpp
void
  logServiceCatalogForDrone(const std::string& droneId) const
```

### API-6e8faf84ca4b · GroundStationServiceContainer::requestTelemetryStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L591)

```cpp
void
  requestTelemetryStatus()
```

### API-8e81d3b1e091 · GroundStationServiceContainer::requestTelemetryStatusForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L597)

```cpp
void
  requestTelemetryStatusForDrone(const std::string& droneId)
```

### API-cc3476a25e29 · GroundStationServiceContainer::telemetryForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L643)

```cpp
std::optional<TelemetryState>
  telemetryForDrone(const std::string& droneId) const
```

### API-0e8c06cba16a · GroundStationServiceContainer::missionForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L654)

```cpp
std::optional<MissionState>
  missionForDrone(const std::string& droneId) const
```

### API-11197f6f8414 · GroundStationServiceContainer::missionProgressSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L665)

```cpp
std::optional<MissionProgressState>
  missionProgressSnapshot() const
```

### API-dd4c1f27619d · GroundStationServiceContainer::missionPlanSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L675)

```cpp
std::optional<MissionPlan>
  missionPlanSnapshot() const
```

### API-f704d484ee9f · GroundStationServiceContainer::missionPlanFilePath

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L685)

```cpp
std::string
  missionPlanFilePath() const
```

### API-b9fd499de412 · GroundStationServiceContainer::setMissionPlanFilePath

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L692)

```cpp
void
  setMissionPlanFilePath(std::string path)
```

### API-d01f7c8835e1 · GroundStationServiceContainer::saveMissionPlanToFile

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L699)

```cpp
bool
  saveMissionPlanToFile(const MissionPlan& plan, const std::string& path,
                        std::string* detail = nullptr) const
```

### API-27944f2b0f1f · GroundStationServiceContainer::saveCurrentMissionPlanToFile

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L731)

```cpp
bool
  saveCurrentMissionPlanToFile(const std::string& path, std::string* detail = nullptr) const
```

### API-af3feb5137d2 · GroundStationServiceContainer::loadMissionPlanFromFile

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L744)

```cpp
bool
  loadMissionPlanFromFile(const std::string& path, std::string* detail = nullptr)
```

### API-73eac96c518d · GroundStationServiceContainer::runtimeSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L776)

```cpp
GroundStationRuntimeState
  runtimeSnapshot() const
```

### API-1b1a6bafcec5 · GroundStationServiceContainer::missionPartForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L964)

```cpp
std::optional<MissionPart>
  missionPartForDrone(const std::string& droneId) const
```

### API-b500ac2b3492 · GroundStationServiceContainer::functionalitySnapshotForSelectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L979)

```cpp
UavFunctionalityState
  functionalitySnapshotForSelectedDrone() const
```

### API-83b699190042 · GroundStationServiceContainer::practicalitySnapshotForSelectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1007)

```cpp
UavPracticalityState
  practicalitySnapshotForSelectedDrone() const
```

### API-fd253d08f45a · GroundStationServiceContainer::stabilitySnapshotForSelectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1018)

```cpp
UavStabilityState
  stabilitySnapshotForSelectedDrone() const
```

### API-5587ba37d364 · GroundStationServiceContainer::readinessForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1031)

```cpp
std::optional<ReadinessState>
  readinessForDrone(const std::string& droneId) const
```

### API-7c1ea77c10a8 · GroundStationServiceContainer::injectReadinessStateForTest

public / test-helper；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1042)

```cpp
void
  injectReadinessStateForTest(ReadinessState readiness)
```

### API-3b67d79954b4 · GroundStationServiceContainer::videoForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1055)

```cpp
std::optional<VideoState>
  videoForDrone(const std::string& droneId) const
```

### API-cb8ec48e6010 · GroundStationServiceContainer::videoAdaptiveForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1066)

```cpp
std::optional<VideoAdaptiveState>
  videoAdaptiveForDrone(const std::string& droneId) const
```

### API-0e755fff6425 · GroundStationServiceContainer::injectVideoAdaptivePressureForTest

public / test-helper；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1077)

```cpp
void
  injectVideoAdaptivePressureForTest(const std::string& profile,
                                     uint64_t timeoutPressure,
                                     uint64_t probePressure,
                                     uint64_t duplicatePressure,
                                     uint64_t decoderPendingChunks,
                                     uint64_t receivedChunks,
                                     uint64_t timeouts,
                                     uint64_t nacks)
```

### API-f8ba240da2d0 · GroundStationServiceContainer::commandForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1102)

```cpp
std::optional<FlightCommandState>
  commandForDrone(const std::string& droneId) const
```

### API-5a9a9cda84a2 · GroundStationServiceContainer::safetyForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1113)

```cpp
std::optional<SafetyState>
  safetyForDrone(const std::string& droneId) const
```

### API-5a484bf03e52 · GroundStationServiceContainer::telemetrySnapshots

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1136)

```cpp
std::vector<TelemetryState>
  telemetrySnapshots() const
```

### API-46c766e3d4dc · GroundStationServiceContainer::startVideo

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1148)

```cpp
void
  startVideo()
```

### API-258fd2088ccd · GroundStationServiceContainer::applySuggestedVideoBitrate

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1168)

```cpp
bool
  applySuggestedVideoBitrate()
```

### API-cd8049923655 · GroundStationServiceContainer::videoBitratePolicy

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1198)

```cpp
std::string
  videoBitratePolicy() const
```

### API-ec92c615b533 · GroundStationServiceContainer::stopVideo

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1204)

```cpp
void
  stopVideo()
```

### API-9ff1d78a045d · GroundStationServiceContainer::isStreaming

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1247)

```cpp
bool
  isStreaming() const
```

### API-437bd0dd7dac · GroundStationServiceContainer::isStreamingForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1253)

```cpp
bool
  isStreamingForDrone(const std::string& droneId) const
```

### API-eb411fc0244f · GroundStationServiceContainer::isVideoDisplayActiveForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1259)

```cpp
bool
  isVideoDisplayActiveForDrone(const std::string& droneId) const
```

### API-7a39953f7199 · GroundStationServiceContainer::activeVideoDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1266)

```cpp
std::string
  activeVideoDroneId() const
```

### API-449f6f9627ca · GroundStationServiceContainer::activeRecordingPlaybackDroneId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1273)

```cpp
std::string
  activeRecordingPlaybackDroneId() const
```

### API-a2f91edd2a72 · GroundStationServiceContainer::activeRecordingPlaybackStreamId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1280)

```cpp
std::string
  activeRecordingPlaybackStreamId() const
```

### API-bf4b8da59c3a · GroundStationServiceContainer::requestRecordingManifest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1287)

```cpp
void
  requestRecordingManifest()
```

### API-a64edac3b628 · GroundStationServiceContainer::requestRecordingRetentionAction

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1293)

```cpp
void
  requestRecordingRetentionAction(const std::string& action)
```

### API-768da69ea166 · GroundStationServiceContainer::requestRepoCatalog

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1315)

```cpp
void
  requestRepoCatalog()
```

### API-48fcff169459 · GroundStationServiceContainer::requestVehicleParameters

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1321)

```cpp
void
  requestVehicleParameters(std::function<void(std::optional<VehicleParameterSnapshot>)> onDone = {})
```

### API-87255a180957 · GroundStationServiceContainer::requestVehicleParameterEdit

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1327)

```cpp
void
  requestVehicleParameterEdit(VehicleParameterEditRequest request,
                              std::function<void(std::optional<VehicleParameterEditResult>)> onDone = {})
```

### API-a85f67374b9d · GroundStationServiceContainer::requestPreflightChecklist

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1334)

```cpp
void
  requestPreflightChecklist(std::function<void(std::vector<PreflightCheckItem>)> onDone = {})
```

### API-9d985c6f9c31 · GroundStationServiceContainer::requestAnalyzeSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1340)

```cpp
void
  requestAnalyzeSnapshot(std::function<void(std::optional<UavAnalyzeSnapshot>)> onDone = {})
```

### API-9bd7c01745df · GroundStationServiceContainer::catalogForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1346)

```cpp
std::optional<UavDataProductCatalogState>
  catalogForDrone(const std::string& droneId) const
```

### API-b587f8f6804b · GroundStationServiceContainer::parameterSnapshotForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1357)

```cpp
std::optional<VehicleParameterSnapshot>
  parameterSnapshotForDrone(const std::string& droneId) const
```

### API-ba722e940808 · GroundStationServiceContainer::preflightChecklistForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1368)

```cpp
std::vector<PreflightCheckItem>
  preflightChecklistForDrone(const std::string& droneId) const
```

### API-c27486e4b0a8 · GroundStationServiceContainer::analyzeSnapshotForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1379)

```cpp
std::optional<UavAnalyzeSnapshot>
  analyzeSnapshotForDrone(const std::string& droneId) const
```

### API-72289b362150 · GroundStationServiceContainer::operatorDashboardSnapshotForDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1390)

```cpp
UavOperatorDashboardSnapshot
  operatorDashboardSnapshotForDrone(const std::string& droneId) const
```

### API-0e53dc89cb02 · GroundStationServiceContainer::playLatestRecording

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1460)

```cpp
void
  playLatestRecording()
```

### API-2a33101bf799 · GroundStationServiceContainer::sendMavlinkCommand

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1466)

```cpp
bool
  sendMavlinkCommand(const std::string& commandName, Fields params = {})
```

### API-6b8314632902 · GroundStationServiceContainer::sendMavlinkCommandToDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1472)

```cpp
bool
  sendMavlinkCommandToDrone(const std::string& droneId, const std::string& commandName, Fields params = {})
```

### API-e04d3fa99b3a · GroundStationServiceContainer::sendMavlinkCommandToDroneSync

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1615)

```cpp
bool
  sendMavlinkCommandToDroneSync(const std::string& droneId, const std::string& commandName,
                                Fields params, std::chrono::milliseconds timeout)
```

### API-fe19277ed58d · GroundStationServiceContainer::requestTelemetryStatusForDroneSync

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1717)

```cpp
Fields
  requestTelemetryStatusForDroneSync(const std::string& droneId,
                                     std::chrono::milliseconds timeout)
```

### API-a76fd21be563 · GroundStationServiceContainer::runTelemetryLiveTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1776)

```cpp
bool
  runTelemetryLiveTest(std::chrono::seconds timeout, bool requireSensorDetails)
```

### API-94e44185c8e3 · GroundStationServiceContainer::runLinkStateAgingTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1933)

```cpp
bool
  runLinkStateAgingTest(std::chrono::seconds timeout)
```

### API-0c3cab7f7a42 · GroundStationServiceContainer::runSingleDroneMissionUploadTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L1995)

```cpp
bool
  runSingleDroneMissionUploadTest(std::chrono::seconds timeout, bool startMission)
```

### API-3f4b06eac13a · GroundStationServiceContainer::runAutoPatrolCompensationDemo

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2136)

```cpp
bool
  runAutoPatrolCompensationDemo(std::chrono::seconds timeout)
```

### API-755e8f15b42e · GroundStationServiceContainer::runSpec176SitlAcceptance

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2147)

```cpp
bool
  runSpec176SitlAcceptance(std::chrono::seconds timeout)
```

原始接口说明：

```text
/**
   * Execute the bounded PX4/SITL acceptance sequence on one running Ground
   * Station. This composes existing application-owned paths and adds no Core
   * wire mode or patrol-wide collaboration request.
   */
```

### API-204211f2951f · GroundStationServiceContainer::runLoadedMissionPlanUploadTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2225)

```cpp
bool
  runLoadedMissionPlanUploadTest(std::chrono::seconds timeout, std::string path)
```

### API-13ff9ca62573 · GroundStationServiceContainer::runRepoCatalogBrowseTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2270)

```cpp
bool
  runRepoCatalogBrowseTest(std::chrono::seconds timeout)
```

### API-f2a325abe6fc · GroundStationServiceContainer::runParameterCacheTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2293)

```cpp
bool
  runParameterCacheTest(std::chrono::seconds timeout)
```

### API-2afc75b4546f · GroundStationServiceContainer::runParameterEditTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2318)

```cpp
bool
  runParameterEditTest(std::chrono::seconds timeout)
```

### API-d073a1b6c063 · GroundStationServiceContainer::runPreflightChecklistTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2368)

```cpp
bool
  runPreflightChecklistTest(std::chrono::seconds timeout)
```

### API-fa4db051969b · GroundStationServiceContainer::runAnalyzeSnapshotTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2397)

```cpp
bool
  runAnalyzeSnapshotTest(std::chrono::seconds timeout)
```

### API-301fcf444bf6 · GroundStationServiceContainer::runOperatorDashboardSnapshotTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2425)

```cpp
bool
  runOperatorDashboardSnapshotTest(std::chrono::seconds timeout)
```

### API-af5f4931fb48 · GroundStationServiceContainer::runAuthorityLeaseGateTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2476)

```cpp
bool
  runAuthorityLeaseGateTest(std::chrono::seconds)
```

### API-4cb41f75eca4 · GroundStationServiceContainer::runConfiguredAuthorityLeaseTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2520)

```cpp
bool
  runConfiguredAuthorityLeaseTest(std::chrono::seconds)
```

### API-47f1c1d8feef · GroundStationServiceContainer::runAuthorityLeaseIssuerTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2548)

```cpp
bool
  runAuthorityLeaseIssuerTest(std::chrono::seconds timeout)
```

### API-0fa53fa18b5e · GroundStationServiceContainer::runAuthorityLeaseArbitrationTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2598)

```cpp
bool
  runAuthorityLeaseArbitrationTest(std::chrono::seconds timeout)
```

### API-08b98904e9e3 · GroundStationServiceContainer::runAuthorityLeasePersistenceTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2672)

```cpp
bool
  runAuthorityLeasePersistenceTest(std::chrono::seconds timeout)
```

### API-5b0aaad3e29d · GroundStationServiceContainer::runAuthorityRevocationLookupTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2756)

```cpp
bool
  runAuthorityRevocationLookupTest(std::chrono::seconds timeout)
```

### API-13d501cb66dc · GroundStationServiceContainer::runAuthorityLeaseRefreshTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2827)

```cpp
bool
  runAuthorityLeaseRefreshTest(std::chrono::seconds timeout)
```

### API-498354be4b20 · GroundStationServiceContainer::runAuthorityLeaseRefreshTimerTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2904)

```cpp
bool
  runAuthorityLeaseRefreshTimerTest(std::chrono::seconds timeout)
```

### API-0030a73e39db · GroundStationServiceContainer::runAuthorityAlertHistoryTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L2987)

```cpp
bool
  runAuthorityAlertHistoryTest(std::chrono::seconds timeout)
```

### API-63e63dcca5ce · GroundStationServiceContainer::runAuthorityAuditQueryTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3097)

```cpp
bool
  runAuthorityAuditQueryTest(std::chrono::seconds timeout)
```

### API-09762f53fc70 · GroundStationServiceContainer::uploadMissionPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3303)

```cpp
bool
  uploadMissionPlan(MissionPlan plan, std::chrono::seconds timeout)
```

### API-9f5fb33ba377 · GroundStationServiceContainer::cancelCurrentPatrolMission

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3618)

```cpp
bool
  cancelCurrentPatrolMission()
```

### API-27b232ea1e9b · GroundStationServiceContainer::cancelPatrolMission

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3630)

```cpp
bool
  cancelPatrolMission(const std::string& taskId)
```

### API-6352b4de34ba · GroundStationServiceContainer::activePatrolMissionId

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3642)

```cpp
std::optional<std::string>
  activePatrolMissionId() const
```

### API-3185d5f36578 · GroundStationServiceContainer::missionSessionState

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3652)

```cpp
std::optional<UavMissionSessionState>
  missionSessionState() const
```

### API-ff6424729681 · GroundStationServiceContainer::missionSessionSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3660)

```cpp
std::optional<std::string>
  missionSessionSnapshot() const
```

### API-1dbd13a711a8 · GroundStationServiceContainer::refetchNamedEvidenceFromSecondConsumer

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3675)

```cpp
bool
  refetchNamedEvidenceFromSecondConsumer(const UavEvidenceReference& evidence,
                                         std::string* reason = nullptr)
```

原始接口说明：

```text
/**
   * Re-fetch one immutable producer object through an independent consumer
   * path.  This deliberately uses an exact Interest and explicit producer
   * certificate verification; it does not reuse the collaboration callback,
   * a process endpoint, or a transport address.  The method is bounded so a
   * missing object cannot stall the MissionSession.
   */
```

### API-d13997415779 · GroundStationServiceContainer::runIncidentCollaborationTest

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3763)

```cpp
bool
  runIncidentCollaborationTest(std::chrono::seconds timeout,
                               std::string failureCase = {},
                               bool reuseMissionSession = false)
```

原始接口说明：

```text
/**
   * Exercise the complete named-evidence collaboration over MiniNDN.  The
   * evidence descriptor is deterministic and the source Drone publishes its
   * own immutable Data during the EvidenceSource role; the detector must
   * retrieve that Data by exact Interest before producing the terminal report.
   */
```

### API-7abc23455c47 · GroundStationServiceContainer::beginIncidentAnalysis

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L3977)

```cpp
bool
  beginIncidentAnalysis(UavIncidentRecord incident, std::string* reason = nullptr)
```

原始接口说明：

```text
/** Start one finite, data-centric incident collaboration on the existing User. */
```

### API-2a3bec3244e9 · GroundStationServiceContainer::runPatrolCompensationTask

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L4358)

```cpp
bool
  runPatrolCompensationTask(std::chrono::seconds timeout, double centerLat, double centerLon,
                            double sideMeters, bool simulateFirstPartMissing,
                            const std::vector<std::pair<double, double>>& routeWaypoints = {})
```

### API-5fd9e6e1c2bc · serveObjectDetection

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp#L9457)

```cpp
int
serveObjectDetection(ndn::Face& face, ndn::KeyChain& keyChain,
                     const ndn::security::Certificate& gsCert,
                     const ndn::security::Certificate& controllerCert,
                     const UavRuntimeConfig& config,
                     bool serveCertificates)
```

## NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp

源码 SHA-256：`7b1e9be4c4980717b9a7fdeeeb632db3f64143e34e53ff70ac1110c8d039a214`。

### API-c80c21192833 · GroundStationWindow

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp#L4)

```cpp
class GroundStationWindow : public Gtk::Window
```

### API-6a98f1274a72 · GroundStationWindow::GroundStationWindow

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp#L7)

```cpp
GroundStationWindow(GroundStationServiceContainer& runtime, bool autoStart,
                      int autoStopSeconds, int autoStartDelayMs,
                      bool autoMavlinkTest, bool autoKeyboardTest,
                      bool autoManualControlTest,
                      bool autoTwoDroneSwitchTest,
                      bool autoVideoSelectionTest,
                      bool autoMissionControlsTest,
                      bool autoFlightControlsTest,
                      bool autoRecordingPlaybackTest,
                      bool autoDashboardPanelTest,
                      bool autoDashboardDetailPanelTest,
                      bool autoDashboardRefreshButtonsTest,
                      bool autoParameterEditPanelTest,
                      bool autoApplyBitrateTest,
                      bool autoVideoPressureProfileTest,
                      bool autoRepeatStopTest,
                      std::vector<std::string> droneIds,
                      double groundStationMapLat,
                      double groundStationMapLon,
                      uint64_t operatorAuthorityRefreshIntervalMs)
```

### API-79f18fddf7b8 · GroundStationWindow::~GroundStationWindow

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/GroundStationWindow.inc.hpp#L1508)

```cpp
~GroundStationWindow() override
```

## NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp

源码 SHA-256：`22b373872d1789f7625860f9ea67eff34e3e1ba7d14651a242085d034f1f0cb5`。

### API-7438ebc3dd55 · ndnsf::examples::uav::UavIncidentCoordinatorConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L14)

```cpp
struct UavIncidentCoordinatorConfig
```

### API-e80746e09efa · ndnsf::examples::uav::UavIncidentCoordinatorConfig::ackTimeoutMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L16)

```cpp
uint64_t ackTimeoutMs = 1000;
```

### API-cd49556c22a4 · ndnsf::examples::uav::UavIncidentCoordinatorConfig::globalDeadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L17)

```cpp
uint64_t globalDeadlineMs = 5000;
```

### API-ad9901f2b740 · ndnsf::examples::uav::UavIncidentCoordinatorConfig::maxAssignments

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L18)

```cpp
std::size_t maxAssignments = 8;
```

### API-93ec4882d6e5 · ndnsf::examples::uav::UavIncidentCoordinatorConfig::maxEvidence

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L19)

```cpp
std::size_t maxEvidence = 8;
```

### API-6c38f29933cc · ndnsf::examples::uav::UavIncidentCoordinatorConfig::maxAnalysisRetries

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L20)

```cpp
std::size_t maxAnalysisRetries = 1;
```

### API-315b38223718 · ndnsf::examples::uav::UavIncidentCoordinatorConfig::explicitGroundFallback

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L21)

```cpp
ndn::Name explicitGroundFallback;
```

### API-3d685f6237c8 · ndnsf::examples::uav::UavCollaborationFailureStage

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L24)

```cpp
enum class UavCollaborationFailureStage
```

### API-3ea8d832ac49 · ndnsf::examples::uav::UavCollaborationFailureStage::Discovery

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L26)

```cpp
Discovery
```

### API-967b03984a34 · ndnsf::examples::uav::UavCollaborationFailureStage::AckClosure

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L27)

```cpp
AckClosure
```

### API-f3804a4f23c6 · ndnsf::examples::uav::UavCollaborationFailureStage::Plan

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L28)

```cpp
Plan
```

### API-e4b50e65439f · ndnsf::examples::uav::UavCollaborationFailureStage::Selection

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L29)

```cpp
Selection
```

### API-516741a3cdfd · ndnsf::examples::uav::UavCollaborationFailureStage::Evidence

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L30)

```cpp
Evidence
```

### API-35c5d19ebcea · ndnsf::examples::uav::UavCollaborationFailureStage::Execution

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L31)

```cpp
Execution
```

### API-8a46589e83ee · ndnsf::examples::uav::UavCollaborationFailureStage::Report

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L32)

```cpp
Report
```

### API-923fa6f1ad37 · ndnsf::examples::uav::UavCollaborationFailureStage::Delivery

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L33)

```cpp
Delivery
```

### API-986eaec0dd04 · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L36)

```cpp
const char* to_string(UavCollaborationFailureStage stage) noexcept;
```

### API-7634a03ae91a · ndnsf::examples::uav::isIdempotentAnalysisFailure

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L38)

```cpp
bool isIdempotentAnalysisFailure(UavCollaborationFailureStage stage) noexcept;
```

### API-699c37d839e9 · ndnsf::examples::uav::UavIncidentCoordinator

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L45)

```cpp
class UavIncidentCoordinator
```

### API-76df0d4bea7f · ndnsf::examples::uav::UavIncidentCoordinator::UavIncidentCoordinator

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L48)

```cpp
UavIncidentCoordinator(UavMissionSession& mission,
                         UavIncidentRecord incident,
                         UavIncidentCoordinatorConfig config = {});
```

### API-e5bc9178dedd · ndnsf::examples::uav::UavIncidentCoordinator::begin

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L52)

```cpp
bool begin(const ndn::Name& requestId, uint64_t nowMs,
             std::string* reason = nullptr);
```

### API-09bba93738dd · ndnsf::examples::uav::UavIncidentCoordinator::closeAcks

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L54)

```cpp
bool closeAcks(uint64_t nowMs, std::string* reason = nullptr);
```

### API-e533052607b3 · ndnsf::examples::uav::UavIncidentCoordinator::commitPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L55)

```cpp
bool commitPlan(std::vector<UavRoleAssignment> assignments,
                  ndn::Name terminalOwner, std::string planDigest,
                  std::string* reason = nullptr);
```

### API-bf150eef96dd · ndnsf::examples::uav::UavIncidentCoordinator::selectProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L58)

```cpp
bool selectProvider(const ndn::Name& provider, std::string* reason = nullptr);
```

### API-d6ba406ca3db · ndnsf::examples::uav::UavIncidentCoordinator::markEvidenceReady

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L59)

```cpp
bool markEvidenceReady(std::string* reason = nullptr);
```

### API-9e03b54ecea3 · ndnsf::examples::uav::UavIncidentCoordinator::markExecuting

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L60)

```cpp
bool markExecuting(std::string* reason = nullptr);
```

### API-fbc3cddb0840 · ndnsf::examples::uav::UavIncidentCoordinator::markReporting

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L61)

```cpp
bool markReporting(std::string* reason = nullptr);
```

### API-f10389b9d21d · ndnsf::examples::uav::UavIncidentCoordinator::acceptReport

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L62)

```cpp
bool acceptReport(const UavTerminalReport& report, uint64_t nowMs,
                    std::string* reason = nullptr);
```

### API-e74ad266703d · ndnsf::examples::uav::UavIncidentCoordinator::beginMultiViewJob

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L64)

```cpp
bool beginMultiViewJob(const MultiViewRecognitionJob& job, uint64_t nowMs,
                         std::string* reason = nullptr);
```

### API-01be92408d26 · ndnsf::examples::uav::UavIncidentCoordinator::acceptMultiViewResult

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L66)

```cpp
bool acceptMultiViewResult(const FusedRecognitionResult& result, uint64_t nowMs,
                             std::string* reason = nullptr);
```

### API-91cc67167cf9 · ndnsf::examples::uav::UavIncidentCoordinator::fail

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L68)

```cpp
bool fail(const std::string& stage, const std::string& detail,
            bool timedOut = false, std::string* reason = nullptr);
```

### API-1f56eb70038c · ndnsf::examples::uav::UavIncidentCoordinator::retryAnalysis

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L70)

```cpp
bool retryAnalysis(UavCollaborationFailureStage stage,
                     const ndn::Name& newRequestId,
                     const std::string& newAttemptId,
                     uint64_t nowMs, std::string* reason = nullptr);
```

### API-3228f5ccf4b0 · ndnsf::examples::uav::UavIncidentCoordinator::retriesUsed

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L75)

```cpp
std::size_t retriesUsed() const noexcept
```

### API-8a6ea335ad76 · ndnsf::examples::uav::UavIncidentCoordinator::job

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L77)

```cpp
const UavCollaborationJobRecord& job() const noexcept
```

### API-c34af7c409ce · ndnsf::examples::uav::UavIncidentCoordinator::incident

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L78)

```cpp
const UavIncidentRecord& incident() const noexcept
```

### API-eb24d6c210df · ndnsf::examples::uav::UavIncidentCoordinator::selectedProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/ground-station/UavIncidentCoordinator.hpp#L79)

```cpp
const ndn::Name& selectedProvider() const noexcept
```

## NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp

源码 SHA-256：`10ad006907b9da3b86f5e5546f1b47521c223a65e07eb9ad4bfaf67bd6c33fd1`。

### API-c655023f1f7d · ndnsf::examples::uav::UavDetectorRequirement

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L13)

```cpp
struct UavDetectorRequirement
```

### API-3cc685086ca8 · ndnsf::examples::uav::UavDetectorRequirement::modelId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L15)

```cpp
std::string modelId;
```

### API-3fdb7bef08b2 · ndnsf::examples::uav::UavDetectorRequirement::qualityProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L16)

```cpp
std::string qualityProfile;
```

### API-f937045b9fc1 · ndnsf::examples::uav::UavDetectorRequirement::preferredDeviceClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L17)

```cpp
std::string preferredDeviceClass;
```

### API-dc10a03e7b22 · ndnsf::examples::uav::UavDetectorRequirement::maxAckAgeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L18)

```cpp
uint64_t maxAckAgeMs = 5000;
```

### API-092e958ba185 · ndnsf::examples::uav::UavDetectorRequirement::maxQueueDepth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L19)

```cpp
uint64_t maxQueueDepth = 64;
```

### API-b236f19f288e · ndnsf::examples::uav::UavDetectorCandidate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L22)

```cpp
struct UavDetectorCandidate
```

### API-fd0ba9abc4fb · ndnsf::examples::uav::UavDetectorCandidate::capability

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L24)

```cpp
UavProviderCapabilitySnapshot capability;
```

### API-ffd9b6cd6625 · ndnsf::examples::uav::UavDetectorCandidate::ackVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L29)

```cpp
bool ackVerified = false;
```

原始接口说明：

```text
// This is a provenance assertion supplied by the NDNSF ServiceUser after
// its ACK validation path. The policy never treats a provider name or a
// payload parse as cryptographic verification; callers must leave it false
// unless the candidate came from the accepted ACK_CLOSED snapshot.
```

### API-897a4db09a74 · ndnsf::examples::uav::UavDetectorCandidate::explicitFallback

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L30)

```cpp
bool explicitFallback = false;
```

### API-abc518c27df2 · ndnsf::examples::uav::UavSelectionDecision

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L33)

```cpp
struct UavSelectionDecision
```

### API-9233eb71432a · ndnsf::examples::uav::UavSelectionDecision::selected

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L35)

```cpp
bool selected = false;
```

### API-8199e4e2a021 · ndnsf::examples::uav::UavSelectionDecision::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L36)

```cpp
ndn::Name providerIdentity;
```

### API-8e45b3cf82ef · ndnsf::examples::uav::UavSelectionDecision::fallbackUsed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L37)

```cpp
bool fallbackUsed = false;
```

### API-8ce2cbf82ba2 · ndnsf::examples::uav::UavSelectionDecision::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L38)

```cpp
std::string reason;
```

### API-534adb4c3847 · ndnsf::examples::uav::selectUavDetector

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavCollaborationPolicy.hpp#L46)

```cpp
UavSelectionDecision
selectUavDetector(const UavDetectorRequirement& requirement,
                  const std::vector<UavDetectorCandidate>& candidates,
                  uint64_t nowMs,
                  const ndn::Name& explicitFallback = {},
                  const std::optional<UavDetectorCandidate>& fallbackCandidate = std::nullopt);
```

原始接口说明：

```text
/**
 * Select one detector using request-scoped, verified capability metadata.
 * Provider identities are NDN names; this function has no transport-address
 * inputs and never infers capability from a name.
 */
```

## NDNSF-UAV-APP/shared/UavDetectorProvider.hpp

源码 SHA-256：`07ec628b4813ebcf89e0de0548f486409bb80e21e082cbdad02cfec0401ea801`。

### API-2e578398a45f · ndnsf::examples::uav::UavDetectorProviderConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L16)

```cpp
struct UavDetectorProviderConfig
```

### API-61233cbde5cc · ndnsf::examples::uav::UavDetectorProviderConfig::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L18)

```cpp
ndn::Name providerIdentity;
```

### API-4216b0e37fc2 · ndnsf::examples::uav::UavDetectorProviderConfig::modelId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L19)

```cpp
std::string modelId;
```

### API-76f35012411d · ndnsf::examples::uav::UavDetectorProviderConfig::modelDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L20)

```cpp
std::string modelDigest;
```

### API-0e91599d326f · ndnsf::examples::uav::UavDetectorProviderConfig::qualityProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L21)

```cpp
std::string qualityProfile;
```

### API-d2ffc1067cca · ndnsf::examples::uav::UavDetectorProviderConfig::deviceClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L22)

```cpp
std::string deviceClass;
```

### API-987bc02d47f4 · ndnsf::examples::uav::UavDetectorProviderConfig::ready

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L23)

```cpp
bool ready = false;
```

### API-a700bb16bb28 · ndnsf::examples::uav::UavDetectorExecution

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L26)

```cpp
struct UavDetectorExecution
```

### API-2d9c14e1ab9e · ndnsf::examples::uav::UavDetectorExecution::success

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L28)

```cpp
bool success = false;
```

### API-762a584b3e41 · ndnsf::examples::uav::UavDetectorExecution::resultDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L29)

```cpp
std::string resultDigest;
```

### API-3d3885e15695 · ndnsf::examples::uav::UavDetectorExecution::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L30)

```cpp
std::string detail;
```

### API-460113a30a78 · ndnsf::examples::uav::UavVerifiedEvidence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L40)

```cpp
struct UavVerifiedEvidence
```

### API-4c0feafec7e8 · ndnsf::examples::uav::UavVerifiedEvidence::reference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L42)

```cpp
UavEvidenceReference reference;
```

### API-2bbb839bcb04 · ndnsf::examples::uav::UavVerifiedEvidence::content

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L43)

```cpp
ndn::Buffer content;
```

### API-76a9b44f7a9a · ndnsf::examples::uav::UavVerifiedEvidence::signerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L44)

```cpp
ndn::Name signerIdentity;
```

### API-0cc59cf58faa · ndnsf::examples::uav::UavVerifiedEvidence::nameVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L45)

```cpp
bool nameVerified = false;
```

### API-1a6283f004ba · ndnsf::examples::uav::UavVerifiedEvidence::signatureVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L46)

```cpp
bool signatureVerified = false;
```

### API-a59a5aceb574 · ndnsf::examples::uav::UavVerifiedEvidence::digestVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L47)

```cpp
bool digestVerified = false;
```

### API-1b5c39730be3 · ndnsf::examples::uav::UavVerifiedEvidence::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L49)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-c895f8bcb92a · ndnsf::examples::uav::UavDetectorProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L57)

```cpp
class UavDetectorProvider
```

### API-23d95f827c91 · ndnsf::examples::uav::UavDetectorProvider::UavDetectorProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L60)

```cpp
explicit UavDetectorProvider(UavDetectorProviderConfig config);
```

### API-bacf26148833 · ndnsf::examples::uav::UavDetectorProvider::config

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L62)

```cpp
const UavDetectorProviderConfig& config() const noexcept
```

### API-ae29ac38f279 · ndnsf::examples::uav::UavDetectorProvider::ready

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L63)

```cpp
bool ready() const noexcept
```

### API-d66dc165c529 · ndnsf::examples::uav::UavDetectorProvider::capability

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L65)

```cpp
UavProviderCapabilitySnapshot
  capability(uint64_t nowMs, uint64_t queueDepth = 0,
             bool evidenceAccess = true) const;
```

### API-957dea2292bf · ndnsf::examples::uav::UavDetectorProvider::execute

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L69)

```cpp
UavDetectorExecution
  execute(const UavVerifiedEvidence& evidence) const;
```

### API-c9f60448addf · ndnsf::examples::uav::UavDetectorProvider::verifyFetchedData

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L79)

```cpp
std::optional<UavVerifiedEvidence>
  verifyFetchedData(const UavEvidenceReference& evidence,
                    const ndn::Data& fetchedData,
                    const ndn::security::Certificate& signerCertificate,
                    std::string* reason = nullptr) const;
```

原始接口说明：

```text
/**
   * Verify the exact Data name, producer identity, signature, and content
   * digest before execution. The certificate is supplied by the caller because
   * one detector may consume evidence from more than one producer. The
   * returned UavVerifiedEvidence is the only execution input accepted by the
   * detector adapter.
   */
```

### API-1c2e3006c599 · ndnsf::examples::uav::UavDetectorProvider::acceptValidatedContent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L91)

```cpp
std::optional<UavVerifiedEvidence>
  acceptValidatedContent(const UavEvidenceReference& evidence,
                         ndn::Buffer content,
                         const ndn::Name& verifiedSigner,
                         std::string* reason = nullptr) const;
```

原始接口说明：

```text
/**
   * Admit content returned by CollaborationContext::fetchSignedExactData().
   * That Core API has already performed exact-name, configured-validator, and
   * expected-producer checks; this adapter rebinds its returned bytes to the
   * same typed evidence boundary and verifies the declared digest again.
   */
```

### API-80a227973aa8 · ndnsf::examples::uav::UavDetectorProvider::execute

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L97)

```cpp
UavDetectorExecution
  execute(const UavEvidenceReference& evidence,
          const ndn::Data& fetchedData,
          const ndn::security::Certificate& signerCertificate,
          std::string* reason = nullptr) const;
```

### API-6e2fb1b906a7 · ndnsf::examples::uav::UavDetectorProvider::executeMultiView

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDetectorProvider.hpp#L105)

```cpp
MultiViewExecutionResult
  executeMultiView(const MultiViewRecognitionJob& job,
                   const MultiViewModelProfile& profile,
                   const std::vector<UavVerifiedEvidence>& evidence,
                   const ndn::Name& terminalOwner,
                   const IMultiViewRecognitionAlgorithm& algorithm =
                     DeterministicMultiViewAlgorithm{}) const;
```

原始接口说明：

```text
/** Execute the registered multi-view algorithm after every input has crossed
   * the same signed-Data acceptance boundary as the single-view path. */
```

## NDNSF-UAV-APP/shared/UavDiagnostics.hpp

源码 SHA-256：`37e9afa819d47d6f6d7f5045a3ec71630837aa0cbc2d40666b1e5541fe8e34f5`。

### API-ece0c842ba1c · ndnsf::examples::uav::UavTraceStage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L16)

```cpp
enum class UavTraceStage
```

### API-93b69380e080 · ndnsf::examples::uav::UavTraceStage::MissionTrigger

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L18)

```cpp
MissionTrigger
```

### API-e841d62e34b3 · ndnsf::examples::uav::UavTraceStage::RequestPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L19)

```cpp
RequestPublished
```

### API-40ea13a9cd6e · ndnsf::examples::uav::UavTraceStage::AckClosed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L20)

```cpp
AckClosed
```

### API-16db3312edcc · ndnsf::examples::uav::UavTraceStage::PlanCommitted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L21)

```cpp
PlanCommitted
```

### API-40e13349b520 · ndnsf::examples::uav::UavTraceStage::Selection

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L22)

```cpp
Selection
```

### API-4a2a6b20122f · ndnsf::examples::uav::UavTraceStage::EvidenceInterest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L23)

```cpp
EvidenceInterest
```

### API-a6df5182c8d9 · ndnsf::examples::uav::UavTraceStage::EvidenceDataVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L24)

```cpp
EvidenceDataVerified
```

### API-0625610a09e8 · ndnsf::examples::uav::UavTraceStage::Execution

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L25)

```cpp
Execution
```

### API-9dabc3d27521 · ndnsf::examples::uav::UavTraceStage::ReportPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L26)

```cpp
ReportPublished
```

### API-adea625166a8 · ndnsf::examples::uav::UavTraceStage::TerminalAccepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L27)

```cpp
TerminalAccepted
```

### API-ec97c815f4ee · ndnsf::examples::uav::UavTraceStage::Failure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L28)

```cpp
Failure
```

### API-f3472447b37e · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L31)

```cpp
const char* to_string(UavTraceStage stage) noexcept;
```

### API-334363738b73 · ndnsf::examples::uav::UavTraceEvent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L33)

```cpp
struct UavTraceEvent
```

### API-d3d4ecb502d0 · ndnsf::examples::uav::UavTraceEvent::timestampMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L35)

```cpp
uint64_t timestampMs = 0;
```

### API-c43be838370e · ndnsf::examples::uav::UavTraceEvent::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L36)

```cpp
std::string missionId;
```

### API-43c69c333ff5 · ndnsf::examples::uav::UavTraceEvent::incidentId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L37)

```cpp
std::string incidentId;
```

### API-7c96851df22a · ndnsf::examples::uav::UavTraceEvent::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L38)

```cpp
ndn::Name requestId;
```

### API-3b9f007e684e · ndnsf::examples::uav::UavTraceEvent::stage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L39)

```cpp
UavTraceStage stage = UavTraceStage::MissionTrigger;
```

### API-152c7d83236f · ndnsf::examples::uav::UavTraceEvent::requestedDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L40)

```cpp
ndn::Name requestedDataName;
```

### API-840068e83fd0 · ndnsf::examples::uav::UavTraceEvent::returnedDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L41)

```cpp
ndn::Name returnedDataName;
```

### API-0ca52919cec0 · ndnsf::examples::uav::UavTraceEvent::producerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L42)

```cpp
ndn::Name producerIdentity;
```

### API-2a7ae622b46e · ndnsf::examples::uav::UavTraceEvent::signerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L43)

```cpp
ndn::Name signerIdentity;
```

### API-41f160a610cb · ndnsf::examples::uav::UavTraceEvent::version

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L44)

```cpp
uint64_t version = 0;
```

### API-28a733c39036 · ndnsf::examples::uav::UavTraceEvent::contentDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L45)

```cpp
std::string contentDigest;
```

### API-53bae2d486af · ndnsf::examples::uav::UavTraceEvent::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L46)

```cpp
std::string detail;
```

### API-6aec8156d886 · ndnsf::examples::uav::UavTraceRecorder

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L50)

```cpp
class UavTraceRecorder
```

### API-b89ff337567d · ndnsf::examples::uav::UavTraceRecorder::UavTraceRecorder

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L53)

```cpp
explicit UavTraceRecorder(std::size_t maxEvents = 4096,
                            uint32_t sampleRate = 1);
```

### API-275fb0a915db · ndnsf::examples::uav::UavTraceRecorder::registerIncident

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L56)

```cpp
void registerIncident(const std::string& incidentId);
```

### API-4b7533bee7fe · ndnsf::examples::uav::UavTraceRecorder::record

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L57)

```cpp
bool record(UavTraceEvent event, std::string* reason = nullptr);
```

### API-a3ca9482ff60 · ndnsf::examples::uav::UavTraceRecorder::events

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L58)

```cpp
const std::vector<UavTraceEvent>& events() const noexcept
```

### API-6eee833348dc · ndnsf::examples::uav::UavTraceRecorder::snapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L59)

```cpp
std::string snapshot() const;
```

### API-55578cd383cb · ndnsf::examples::uav::UavTraceRecorder::containsTransportEndpoint

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L61)

```cpp
static bool containsTransportEndpoint(const UavTraceEvent& event) noexcept;
```

### API-71d9c66cefe7 · ndnsf::examples::uav::UavTraceRecorder::hasCompleteLineage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L62)

```cpp
static bool hasCompleteLineage(const UavTraceEvent& event) noexcept;
```

### API-593fa68dc7ec · ndnsf::examples::uav::UavBoundedWorkQueue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L77)

```cpp
class UavBoundedWorkQueue
```

### API-d448903b4113 · ndnsf::examples::uav::UavBoundedWorkQueue::UavBoundedWorkQueue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L80)

```cpp
explicit UavBoundedWorkQueue(std::size_t capacity = 32)
```

### API-12bf49d10029 · ndnsf::examples::uav::UavBoundedWorkQueue::tryPush

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L85)

```cpp
bool tryPush(std::string work);
```

### API-f3c7dddde2ee · ndnsf::examples::uav::UavBoundedWorkQueue::tryPop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L86)

```cpp
std::optional<std::string> tryPop();
```

### API-a0e4d0abe14e · ndnsf::examples::uav::UavBoundedWorkQueue::size

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L87)

```cpp
std::size_t size() const noexcept
```

### API-0ca396e1a0f1 · ndnsf::examples::uav::UavBoundedWorkQueue::capacity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L88)

```cpp
std::size_t capacity() const noexcept
```

### API-89560066a0af · ndnsf::examples::uav::UavBoundedWorkQueue::dropped

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L89)

```cpp
std::size_t dropped() const noexcept
```

### API-7d571c1e6ba0 · ndnsf::examples::uav::UavBoundedWorkQueue::overloaded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavDiagnostics.hpp#L90)

```cpp
bool overloaded() const noexcept
```

## NDNSF-UAV-APP/shared/UavMissionSession.hpp

源码 SHA-256：`8396a36dd4b640c84129d5ebd4b5d6d0888487b172c2d58eeebd869a4d577e47`。

### API-c5b7079ed30a · ndnsf::examples::uav::UavMissionSession

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L18)

```cpp
class UavMissionSession
```

### API-57890e8d70fc · ndnsf::examples::uav::UavMissionSession::UavMissionSession

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L21)

```cpp
explicit UavMissionSession(UavMissionSessionRecord record,
                             std::size_t maxHistory = 256);
```

### API-feec8bee2768 · ndnsf::examples::uav::UavMissionSession::create

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L24)

```cpp
static UavMissionSession create(std::string missionId,
                                  ndn::Name operatorIdentity,
                                  uint64_t deadlineMs,
                                  std::size_t maxHistory = 256);
```

### API-0eae7ebbc7e3 · ndnsf::examples::uav::UavMissionSession::record

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L29)

```cpp
const UavMissionSessionRecord& record() const noexcept
```

### API-4ac1ff68889e · ndnsf::examples::uav::UavMissionSession::transition

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L31)

```cpp
bool transition(UavMissionSessionState next, std::string* reason = nullptr);
```

### API-22221dab866b · ndnsf::examples::uav::UavMissionSession::start

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L32)

```cpp
bool start(std::string* reason = nullptr);
```

### API-18f2c532b580 · ndnsf::examples::uav::UavMissionSession::markRecovering

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L33)

```cpp
bool markRecovering(std::string* reason = nullptr);
```

### API-5c1b5f5f30c9 · ndnsf::examples::uav::UavMissionSession::reconcileVehicleAndStreams

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L34)

```cpp
bool reconcileVehicleAndStreams(std::string* reason = nullptr);
```

### API-2bde40653834 · ndnsf::examples::uav::UavMissionSession::cancel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L35)

```cpp
bool cancel(std::string* reason = nullptr);
```

### API-c01aa62785c2 · ndnsf::examples::uav::UavMissionSession::addPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L37)

```cpp
bool addPart(UavMissionPartRecord part, std::string* reason = nullptr);
```

### API-4121b243005f · ndnsf::examples::uav::UavMissionSession::assignPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L38)

```cpp
bool assignPart(const std::string& partId, const ndn::Name& provider,
                  const std::string& attemptId, std::string* reason = nullptr);
```

### API-63be7a1f8f62 · ndnsf::examples::uav::UavMissionSession::markPartExecuting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L40)

```cpp
bool markPartExecuting(const std::string& partId, std::string* reason = nullptr);
```

### API-c5276e4b2b1b · ndnsf::examples::uav::UavMissionSession::completePart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L41)

```cpp
bool completePart(const std::string& partId, const std::string& attemptId,
                    const std::string& responseDigest,
                    std::vector<uint64_t> completedWaypoints,
                    std::string* reason = nullptr);
```

### API-5888fe9c38f3 · ndnsf::examples::uav::UavMissionSession::completePart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L45)

```cpp
bool completePart(const std::string& partId, const std::string& responseDigest,
                    std::vector<uint64_t> completedWaypoints,
                    std::string* reason = nullptr);
```

### API-5edbd1e8e0d5 · ndnsf::examples::uav::UavMissionSession::markPartMissing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L48)

```cpp
bool markPartMissing(const std::string& partId, std::string* reason = nullptr);
```

### API-9439cf6844b2 · ndnsf::examples::uav::UavMissionSession::compensateMissingParts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L49)

```cpp
bool compensateMissingParts(std::string* reason = nullptr);
```

### API-9e0e5fb64313 · ndnsf::examples::uav::UavMissionSession::bindStream

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L51)

```cpp
bool bindStream(UavStreamBinding binding, std::string* reason = nullptr);
```

### API-c39263160c2a · ndnsf::examples::uav::UavMissionSession::recordIncident

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L52)

```cpp
bool recordIncident(UavIncidentRecord incident, std::string* reason = nullptr);
```

### API-f8a6155a7013 · ndnsf::examples::uav::UavMissionSession::beginIncidentAttempt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L53)

```cpp
bool beginIncidentAttempt(const std::string& incidentId,
                           const std::string& attemptId,
                           std::string* reason = nullptr);
```

### API-356ab1634530 · ndnsf::examples::uav::UavMissionSession::addJob

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L56)

```cpp
bool addJob(UavCollaborationJobRecord job, std::string* reason = nullptr);
```

### API-39dee3f69e8b · ndnsf::examples::uav::UavMissionSession::updateJob

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L57)

```cpp
bool updateJob(const ndn::Name& requestId, UavCollaborationJobState state,
                 std::string failureStage = {}, std::string failureReason = {},
                 std::string* reason = nullptr);
```

### API-8bc23c85a02b · ndnsf::examples::uav::UavMissionSession::acceptTerminalReport

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L60)

```cpp
bool acceptTerminalReport(const UavTerminalReport& report,
                            std::string* reason = nullptr);
```

### API-9190a14f8bb1 · ndnsf::examples::uav::UavMissionSession::canIssueFlightControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L63)

```cpp
bool canIssueFlightControl() const noexcept;
```

### API-03eb04aa22a0 · ndnsf::examples::uav::UavMissionSession::hasPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L64)

```cpp
bool hasPart(const std::string& partId) const noexcept;
```

### API-4076ceac2dcc · ndnsf::examples::uav::UavMissionSession::hasIncident

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L65)

```cpp
bool hasIncident(const std::string& incidentId) const noexcept;
```

### API-2b5a69ea9b1d · ndnsf::examples::uav::UavMissionSession::hasJob

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L66)

```cpp
bool hasJob(const ndn::Name& requestId) const noexcept;
```

### API-741e8e2893e0 · ndnsf::examples::uav::UavMissionSession::snapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L69)

```cpp
std::string snapshot() const;
```

原始接口说明：

```text
/** Versioned, bounded application snapshot. */
```

### API-ad86b0f74705 · ndnsf::examples::uav::UavMissionSession::restore

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMissionSession.hpp#L70)

```cpp
static std::optional<UavMissionSession>
  restore(const std::string& snapshot,
          std::size_t maxHistory = 256,
          std::string* reason = nullptr);
```

## NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp

源码 SHA-256：`9f8907fe74428e6d992521d8f304a883a459f23476749f4eddc6a8427fb3e97d`。

### API-31f5c4942229 · ndnsf::examples::uav::UAV_MULTIVIEW_MIN_VIEWS = 2

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L14)

```cpp
inline constexpr size_t UAV_MULTIVIEW_MIN_VIEWS = 2;
```

### API-c69cb4b679ec · ndnsf::examples::uav::UAV_MULTIVIEW_MAX_VIEWS = 6

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L15)

```cpp
inline constexpr size_t UAV_MULTIVIEW_MAX_VIEWS = 6;
```

### API-b93e70d93b3d · ndnsf::examples::uav::UAV_MULTIVIEW_MAX_WIRE_BYTES = 16384

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L16)

```cpp
inline constexpr size_t UAV_MULTIVIEW_MAX_WIRE_BYTES = 16384;
```

### API-9ffeecc9f789 · ndnsf::examples::uav::MultiViewTerminalStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L18)

```cpp
enum class MultiViewTerminalStatus
```

### API-b47c94087055 · ndnsf::examples::uav::MultiViewTerminalStatus::Completed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L20)

```cpp
Completed
```

### API-051293035e5d · ndnsf::examples::uav::MultiViewTerminalStatus::InsufficientViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L21)

```cpp
InsufficientViews
```

### API-a718261243c4 · ndnsf::examples::uav::MultiViewTerminalStatus::ValidationFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L22)

```cpp
ValidationFailed
```

### API-4fb814921f09 · ndnsf::examples::uav::MultiViewTerminalStatus::CorrelationFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L23)

```cpp
CorrelationFailed
```

### API-94147de0ccb3 · ndnsf::examples::uav::MultiViewTerminalStatus::UnsupportedModelProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L24)

```cpp
UnsupportedModelProfile
```

### API-8526ee425665 · ndnsf::examples::uav::MultiViewTerminalStatus::InferenceFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L25)

```cpp
InferenceFailed
```

### API-d7551b09906a · ndnsf::examples::uav::MultiViewTerminalStatus::AnnotationPublicationFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L26)

```cpp
AnnotationPublicationFailed
```

### API-261e66127ce7 · ndnsf::examples::uav::MultiViewTerminalStatus::DeliveryTimeout

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L27)

```cpp
DeliveryTimeout
```

### API-0819afc1c924 · ndnsf::examples::uav::MultiViewTerminalStatus::Cancelled

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L28)

```cpp
Cancelled
```

### API-832b439c81ab · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L31)

```cpp
const char* to_string(MultiViewTerminalStatus status) noexcept;
```

### API-1dd6b6178597 · ndnsf::examples::uav::parseMultiViewTerminalStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L32)

```cpp
std::optional<MultiViewTerminalStatus>
parseMultiViewTerminalStatus(const std::string& value);
```

### API-7e5c7cff5eb4 · ndnsf::examples::uav::ViewEvidenceReference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L35)

```cpp
struct ViewEvidenceReference
```

### API-bb4c5d8ee6d0 · ndnsf::examples::uav::ViewEvidenceReference::viewId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L37)

```cpp
std::string viewId;
```

### API-6702ef724a37 · ndnsf::examples::uav::ViewEvidenceReference::producerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L38)

```cpp
ndn::Name producerIdentity;
```

### API-bf2675f58ef8 · ndnsf::examples::uav::ViewEvidenceReference::exactDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L39)

```cpp
ndn::Name exactDataName;
```

### API-0258b5fa486b · ndnsf::examples::uav::ViewEvidenceReference::contentDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L40)

```cpp
std::string contentDigest;
```

### API-9ed59f74ea70 · ndnsf::examples::uav::ViewEvidenceReference::captureTimeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L41)

```cpp
uint64_t captureTimeMs = 0;
```

### API-3f533b5590ac · ndnsf::examples::uav::ViewEvidenceReference::targetId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L42)

```cpp
std::string targetId;
```

### API-09598208168e · ndnsf::examples::uav::ViewEvidenceReference::mediaType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L43)

```cpp
std::string mediaType = "image/png";
```

### API-8fe7241e4c7f · ndnsf::examples::uav::ViewEvidenceReference::viewpoint

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L44)

```cpp
std::string viewpoint;
```

### API-72a3e02e77e7 · ndnsf::examples::uav::ViewEvidenceReference::nominalDistanceM

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L45)

```cpp
std::optional<double> nominalDistanceM;
```

### API-9182684744f1 · ndnsf::examples::uav::ViewEvidenceReference::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L47)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-a8a87f752018 · ndnsf::examples::uav::ViewEvidenceReference::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L48)

```cpp
Fields toFields(const std::string& prefix = {}) const;
```

### API-d0a0dbdf4e09 · ndnsf::examples::uav::ViewEvidenceReference::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L49)

```cpp
static ViewEvidenceReference fromFields(const Fields& fields,
                                          const std::string& prefix = {});
```

### API-b8563a296fa9 · ndnsf::examples::uav::MultiViewModelProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L53)

```cpp
struct MultiViewModelProfile
```

### API-628712b01002 · ndnsf::examples::uav::MultiViewModelProfile::profileId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L55)

```cpp
std::string profileId;
```

### API-84935e900479 · ndnsf::examples::uav::MultiViewModelProfile::algorithmId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L56)

```cpp
std::string algorithmId;
```

### API-53e2bd57e165 · ndnsf::examples::uav::MultiViewModelProfile::modelId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L57)

```cpp
std::string modelId;
```

### API-1154276a0cce · ndnsf::examples::uav::MultiViewModelProfile::modelDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L58)

```cpp
std::string modelDigest;
```

### API-87e32fea22a5 · ndnsf::examples::uav::MultiViewModelProfile::preprocessingProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L59)

```cpp
std::string preprocessingProfile;
```

### API-f290bfa4f852 · ndnsf::examples::uav::MultiViewModelProfile::poolingOperator

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L60)

```cpp
std::string poolingOperator = "elementwise-max/v1";
```

### API-a8451e948863 · ndnsf::examples::uav::MultiViewModelProfile::deviceClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L61)

```cpp
std::string deviceClass = "cpu-or-cuda";
```

### API-f34c483a88f3 · ndnsf::examples::uav::MultiViewModelProfile::supportedMedia

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L62)

```cpp
std::vector<std::string> supportedMedia{"image/png", "image/jpeg"};
```

### API-61d544738309 · ndnsf::examples::uav::MultiViewModelProfile::minimumViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L63)

```cpp
size_t minimumViews = UAV_MULTIVIEW_MIN_VIEWS;
```

### API-b1bf6f87c450 · ndnsf::examples::uav::MultiViewModelProfile::maximumViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L64)

```cpp
size_t maximumViews = UAV_MULTIVIEW_MAX_VIEWS;
```

### API-9b40f7309bd4 · ndnsf::examples::uav::MultiViewModelProfile::minimumDistinctProducers

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L65)

```cpp
size_t minimumDistinctProducers = 2;
```

### API-b8a70216a449 · ndnsf::examples::uav::MultiViewModelProfile::requiresCalibration

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L66)

```cpp
bool requiresCalibration = false;
```

### API-4f3fd3ea256a · ndnsf::examples::uav::MultiViewModelProfile::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L68)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-38b893f85b4d · ndnsf::examples::uav::MultiViewRecognitionJob

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L71)

```cpp
struct MultiViewRecognitionJob
```

### API-b1543c6564cb · ndnsf::examples::uav::MultiViewRecognitionJob::schemaVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L73)

```cpp
uint64_t schemaVersion = 1;
```

### API-6be5a83cbae1 · ndnsf::examples::uav::MultiViewRecognitionJob::missionSessionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L74)

```cpp
std::string missionSessionId;
```

### API-8f026ae6fdeb · ndnsf::examples::uav::MultiViewRecognitionJob::jobId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L75)

```cpp
std::string jobId;
```

### API-5b2d80e4dfe0 · ndnsf::examples::uav::MultiViewRecognitionJob::attempt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L76)

```cpp
uint64_t attempt = 1;
```

### API-d1ec86952e43 · ndnsf::examples::uav::MultiViewRecognitionJob::targetId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L77)

```cpp
std::string targetId;
```

### API-abe796409331 · ndnsf::examples::uav::MultiViewRecognitionJob::captureWindowStartMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L78)

```cpp
uint64_t captureWindowStartMs = 0;
```

### API-c0d463544962 · ndnsf::examples::uav::MultiViewRecognitionJob::captureWindowEndMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L79)

```cpp
uint64_t captureWindowEndMs = 0;
```

### API-f0b97cd831d5 · ndnsf::examples::uav::MultiViewRecognitionJob::views

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L80)

```cpp
std::vector<ViewEvidenceReference> views;
```

### API-7036e2f382d1 · ndnsf::examples::uav::MultiViewRecognitionJob::minimumViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L81)

```cpp
size_t minimumViews = UAV_MULTIVIEW_MIN_VIEWS;
```

### API-51a8858b05ed · ndnsf::examples::uav::MultiViewRecognitionJob::minimumDistinctProducers

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L82)

```cpp
size_t minimumDistinctProducers = 2;
```

### API-c028f8af185d · ndnsf::examples::uav::MultiViewRecognitionJob::modelProfileId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L83)

```cpp
std::string modelProfileId;
```

### API-2048cb2e57b5 · ndnsf::examples::uav::MultiViewRecognitionJob::deadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L84)

```cpp
uint64_t deadlineMs = 5000;
```

### API-1379e665e45a · ndnsf::examples::uav::MultiViewRecognitionJob::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L86)

```cpp
bool isValid(const MultiViewModelProfile* profile = nullptr,
               std::string* reason = nullptr) const;
```

### API-b5a109f205d9 · ndnsf::examples::uav::MultiViewRecognitionJob::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L88)

```cpp
Fields toFields() const;
```

### API-8adcacee0264 · ndnsf::examples::uav::MultiViewRecognitionJob::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L89)

```cpp
static MultiViewRecognitionJob fromFields(const Fields& fields);
```

### API-d245039079c2 · ndnsf::examples::uav::MultiViewRecognitionJob::wireEncode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L90)

```cpp
ndn::Buffer wireEncode() const;
```

### API-831a02c1ce45 · ndnsf::examples::uav::MultiViewRecognitionJob::wireDecode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L91)

```cpp
static MultiViewRecognitionJob wireDecode(const ndn::Buffer& wire);
```

### API-dee54e55ecfc · ndnsf::examples::uav::AnnotatedViewReference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L94)

```cpp
struct AnnotatedViewReference
```

### API-1d13e958db14 · ndnsf::examples::uav::AnnotatedViewReference::viewId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L96)

```cpp
std::string viewId;
```

### API-4180e5dc7122 · ndnsf::examples::uav::AnnotatedViewReference::sourceDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L97)

```cpp
std::string sourceDigest;
```

### API-0d234fcd13a2 · ndnsf::examples::uav::AnnotatedViewReference::exactDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L98)

```cpp
ndn::Name exactDataName;
```

### API-29a1330767f8 · ndnsf::examples::uav::AnnotatedViewReference::contentDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L99)

```cpp
std::string contentDigest;
```

### API-52783aac8dc5 · ndnsf::examples::uav::AnnotatedViewReference::mediaType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L100)

```cpp
std::string mediaType = "image/png";
```

### API-22157d7e84ed · ndnsf::examples::uav::AnnotatedViewReference::signerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L101)

```cpp
ndn::Name signerIdentity;
```

### API-08a7da7812f0 · ndnsf::examples::uav::AnnotatedViewReference::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L103)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-c1f6669727af · ndnsf::examples::uav::AnnotatedViewReference::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L104)

```cpp
Fields toFields(const std::string& prefix = {}) const;
```

### API-65b845106a98 · ndnsf::examples::uav::AnnotatedViewReference::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L105)

```cpp
static AnnotatedViewReference fromFields(const Fields& fields,
                                           const std::string& prefix = {});
```

### API-1de22ea7af77 · ndnsf::examples::uav::FusedRecognitionResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L109)

```cpp
struct FusedRecognitionResult
```

### API-9a745d8b0254 · ndnsf::examples::uav::FusedRecognitionResult::schemaVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L111)

```cpp
uint64_t schemaVersion = 1;
```

### API-38ca36feaf72 · ndnsf::examples::uav::FusedRecognitionResult::missionSessionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L112)

```cpp
std::string missionSessionId;
```

### API-73500ddd552a · ndnsf::examples::uav::FusedRecognitionResult::jobId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L113)

```cpp
std::string jobId;
```

### API-ad91412d0d79 · ndnsf::examples::uav::FusedRecognitionResult::attempt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L114)

```cpp
uint64_t attempt = 1;
```

### API-c7e8e1c5f57d · ndnsf::examples::uav::FusedRecognitionResult::status

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L115)

```cpp
MultiViewTerminalStatus status = MultiViewTerminalStatus::InferenceFailed;
```

### API-cc4da6a723ae · ndnsf::examples::uav::FusedRecognitionResult::fusedLabel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L116)

```cpp
std::string fusedLabel;
```

### API-97462cdc0bbc · ndnsf::examples::uav::FusedRecognitionResult::confidence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L117)

```cpp
double confidence = 0.0;
```

### API-d98a38617900 · ndnsf::examples::uav::FusedRecognitionResult::profileId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L118)

```cpp
std::string profileId;
```

### API-cce8e226d0de · ndnsf::examples::uav::FusedRecognitionResult::algorithmId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L119)

```cpp
std::string algorithmId;
```

### API-af9f4167c6d3 · ndnsf::examples::uav::FusedRecognitionResult::modelDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L120)

```cpp
std::string modelDigest;
```

### API-bbbe73e315d3 · ndnsf::examples::uav::FusedRecognitionResult::contributingViewIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L121)

```cpp
std::vector<std::string> contributingViewIds;
```

### API-f1f76a53b76b · ndnsf::examples::uav::FusedRecognitionResult::rejectedViewIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L122)

```cpp
std::vector<std::string> rejectedViewIds;
```

### API-2c82599d19d8 · ndnsf::examples::uav::FusedRecognitionResult::poolingOperator

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L123)

```cpp
std::string poolingOperator;
```

### API-45334a7de1fb · ndnsf::examples::uav::FusedRecognitionResult::consumedViewCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L124)

```cpp
size_t consumedViewCount = 0;
```

### API-d8ac358087fc · ndnsf::examples::uav::FusedRecognitionResult::pooledFeatureDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L125)

```cpp
std::string pooledFeatureDigest;
```

### API-8c863bdf23a9 · ndnsf::examples::uav::FusedRecognitionResult::resultManifestName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L126)

```cpp
ndn::Name resultManifestName;
```

### API-f87607db0bf5 · ndnsf::examples::uav::FusedRecognitionResult::resultManifestDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L127)

```cpp
std::string resultManifestDigest;
```

### API-4eb80af08014 · ndnsf::examples::uav::FusedRecognitionResult::annotatedViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L128)

```cpp
std::vector<AnnotatedViewReference> annotatedViews;
```

### API-a5b199d0a2fe · ndnsf::examples::uav::FusedRecognitionResult::failureStage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L129)

```cpp
std::string failureStage;
```

### API-ed011ae49f43 · ndnsf::examples::uav::FusedRecognitionResult::failureReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L130)

```cpp
std::string failureReason;
```

### API-a60c707ae57f · ndnsf::examples::uav::FusedRecognitionResult::elapsedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L131)

```cpp
uint64_t elapsedMs = 0;
```

### API-0ee059eab8f6 · ndnsf::examples::uav::FusedRecognitionResult::bytesTransferred

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L132)

```cpp
uint64_t bytesTransferred = 0;
```

### API-d2823d7ac0ff · ndnsf::examples::uav::FusedRecognitionResult::terminalOwner

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L133)

```cpp
ndn::Name terminalOwner;
```

### API-97b5536002d0 · ndnsf::examples::uav::FusedRecognitionResult::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L135)

```cpp
bool isValid(const MultiViewRecognitionJob* job = nullptr,
               const MultiViewModelProfile* profile = nullptr,
               std::string* reason = nullptr) const;
```

### API-432a1365c648 · ndnsf::examples::uav::FusedRecognitionResult::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L138)

```cpp
Fields toFields() const;
```

### API-90229cfb4c78 · ndnsf::examples::uav::FusedRecognitionResult::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L139)

```cpp
static FusedRecognitionResult fromFields(const Fields& fields);
```

### API-78ae27e6555e · ndnsf::examples::uav::FusedRecognitionResult::wireEncode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L140)

```cpp
ndn::Buffer wireEncode() const;
```

### API-e8b8e4843930 · ndnsf::examples::uav::FusedRecognitionResult::wireDecode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L141)

```cpp
static FusedRecognitionResult wireDecode(const ndn::Buffer& wire);
```

### API-1dbf7bb3768f · ndnsf::examples::uav::VerifiedMultiViewInput

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L144)

```cpp
struct VerifiedMultiViewInput
```

### API-49fb71f17b46 · ndnsf::examples::uav::VerifiedMultiViewInput::reference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L146)

```cpp
ViewEvidenceReference reference;
```

### API-206aad5dda30 · ndnsf::examples::uav::VerifiedMultiViewInput::content

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L147)

```cpp
ndn::Buffer content;
```

### API-23df0c1e5b78 · ndnsf::examples::uav::VerifiedMultiViewInput::signerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L148)

```cpp
ndn::Name signerIdentity;
```

### API-d9cfb4e3264f · ndnsf::examples::uav::VerifiedMultiViewInput::nameVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L149)

```cpp
bool nameVerified = false;
```

### API-599b2712e29b · ndnsf::examples::uav::VerifiedMultiViewInput::signatureVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L150)

```cpp
bool signatureVerified = false;
```

### API-a7103025c468 · ndnsf::examples::uav::VerifiedMultiViewInput::digestVerified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L151)

```cpp
bool digestVerified = false;
```

### API-13f5ee6e9794 · ndnsf::examples::uav::VerifiedMultiViewInput::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L153)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-e04571ce8818 · ndnsf::examples::uav::MultiViewExecutionResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L156)

```cpp
struct MultiViewExecutionResult
```

### API-740265f2a010 · ndnsf::examples::uav::MultiViewExecutionResult::success

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L158)

```cpp
bool success = false;
```

### API-65e3a54c6041 · ndnsf::examples::uav::MultiViewExecutionResult::result

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L159)

```cpp
FusedRecognitionResult result;
```

### API-52886f997d9e · ndnsf::examples::uav::MultiViewExecutionResult::acceptedViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L160)

```cpp
std::vector<VerifiedMultiViewInput> acceptedViews;
```

### API-44f46bf26a0d · ndnsf::examples::uav::MultiViewExecutionResult::rejectedViewIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L161)

```cpp
std::vector<std::string> rejectedViewIds;
```

### API-21e12bdfc933 · ndnsf::examples::uav::MultiViewExecutionResult::failureStage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L162)

```cpp
std::string failureStage;
```

### API-9a1c5822f10c · ndnsf::examples::uav::MultiViewExecutionResult::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L163)

```cpp
std::string detail;
```

### API-526e412ad600 · ndnsf::examples::uav::IMultiViewRecognitionAlgorithm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L166)

```cpp
class IMultiViewRecognitionAlgorithm
```

### API-76b84b9d81d9 · ndnsf::examples::uav::IMultiViewRecognitionAlgorithm::~IMultiViewRecognitionAlgorithm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L169)

```cpp
virtual ~IMultiViewRecognitionAlgorithm() = default;
```

### API-7b491f91beaf · ndnsf::examples::uav::IMultiViewRecognitionAlgorithm::execute

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L170)

```cpp
virtual FusedRecognitionResult execute(
    const MultiViewRecognitionJob& job,
    const MultiViewModelProfile& profile,
    const std::vector<VerifiedMultiViewInput>& views,
    const ndn::Name& terminalOwner) const = 0;
```

### API-cbbbbca091b5 · ndnsf::examples::uav::DeterministicMultiViewAlgorithm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L178)

```cpp
class DeterministicMultiViewAlgorithm final : public IMultiViewRecognitionAlgorithm
```

### API-1114c2334e8a · ndnsf::examples::uav::DeterministicMultiViewAlgorithm::execute

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L181)

```cpp
FusedRecognitionResult execute(
    const MultiViewRecognitionJob& job,
    const MultiViewModelProfile& profile,
    const std::vector<VerifiedMultiViewInput>& views,
    const ndn::Name& terminalOwner) const override;
```

### API-1f99dfd63268 · ndnsf::examples::uav::validateMultiViewJobAndViews

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L188)

```cpp
bool
validateMultiViewJobAndViews(const MultiViewRecognitionJob& job,
                             const MultiViewModelProfile& profile,
                             const std::vector<VerifiedMultiViewInput>& views,
                             std::vector<std::string>* rejectedViewIds = nullptr,
                             std::string* reason = nullptr);
```

### API-1da9ec7348c6 · ndnsf::examples::uav::executeMultiViewJob

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavMultiViewRecognition.hpp#L195)

```cpp
MultiViewExecutionResult
executeMultiViewJob(const MultiViewRecognitionJob& job,
                    const MultiViewModelProfile& profile,
                    const std::vector<VerifiedMultiViewInput>& views,
                    const ndn::Name& terminalOwner,
                    const IMultiViewRecognitionAlgorithm& algorithm =
                      DeterministicMultiViewAlgorithm{});
```

## NDNSF-UAV-APP/shared/UavNames.hpp

源码 SHA-256：`9570babd94ac8df407c2d700fdd99c899c46d1eb02e66034732ab2fa38f35c68`。

### API-1fcc704e9755 · ndnsf::examples::uav::GROUP_PREFIX("/example/uav/group")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L11)

```cpp
inline const ndn::Name GROUP_PREFIX("/example/uav/group");
```

### API-af2536f324a6 · ndnsf::examples::uav::CONTROLLER_PREFIX("/example/uav/controller")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L12)

```cpp
inline const ndn::Name CONTROLLER_PREFIX("/example/uav/controller");
```

### API-23d56a363e35 · ndnsf::examples::uav::GROUND_STATION_IDENTITY("/example/uav/gs")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L13)

```cpp
inline const ndn::Name GROUND_STATION_IDENTITY("/example/uav/gs");
```

### API-fffd796bb3e7 · ndnsf::examples::uav::DRONE_IDENTITY_PREFIX("/example/uav/drone")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L14)

```cpp
inline const ndn::Name DRONE_IDENTITY_PREFIX("/example/uav/drone");
```

### API-a90ab957e965 · ndnsf::examples::uav::* TRUST_SCHEMA = "examples/trust-schema.conf"

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L15)

```cpp
inline const char* TRUST_SCHEMA = "examples/trust-schema.conf";
```

### API-4230d543631e · ndnsf::examples::uav::SERVICE_MAVLINK_EXECUTE("/UAV/MAVLink/Execute")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L17)

```cpp
inline const ndn::Name SERVICE_MAVLINK_EXECUTE("/UAV/MAVLink/Execute");
```

### API-2198f23b0d31 · ndnsf::examples::uav::SERVICE_MISSION_ASSIGN("/UAV/Mission/Assign")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L18)

```cpp
inline const ndn::Name SERVICE_MISSION_ASSIGN("/UAV/Mission/Assign");
```

### API-2d9a17eb9b36 · ndnsf::examples::uav::SERVICE_TELEMETRY_STATUS("/UAV/Telemetry/GetStatus")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L19)

```cpp
inline const ndn::Name SERVICE_TELEMETRY_STATUS("/UAV/Telemetry/GetStatus");
```

### API-7439b5449908 · ndnsf::examples::uav::SERVICE_CAMERA_FRAME("/UAV/Camera/GetFrame")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L20)

```cpp
inline const ndn::Name SERVICE_CAMERA_FRAME("/UAV/Camera/GetFrame");
```

### API-ebedaedf9d2c · ndnsf::examples::uav::SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX("/UAV/Camera/Video")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L21)

```cpp
inline const ndn::Name SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX("/UAV/Camera/Video");
```

### API-e8ccd3cbe3b2 · ndnsf::examples::uav::SERVICE_CAMERA_RECORDING_MANIFEST_SUFFIX("/UAV/Camera/Recording/Manifest")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L22)

```cpp
inline const ndn::Name SERVICE_CAMERA_RECORDING_MANIFEST_SUFFIX("/UAV/Camera/Recording/Manifest");
```

### API-9a9b99144cd6 · ndnsf::examples::uav::SERVICE_CAMERA_REPO_CATALOG_SUFFIX("/UAV/Camera/Repo/Catalog")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L23)

```cpp
inline const ndn::Name SERVICE_CAMERA_REPO_CATALOG_SUFFIX("/UAV/Camera/Repo/Catalog");
```

### API-cb076a4610f8 · ndnsf::examples::uav::SERVICE_MAVLINK_PARAMETERS_SUFFIX("/UAV/MAVLink/Parameters")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L24)

```cpp
inline const ndn::Name SERVICE_MAVLINK_PARAMETERS_SUFFIX("/UAV/MAVLink/Parameters");
```

### API-38fb7f299449 · ndnsf::examples::uav::SERVICE_MAVLINK_PARAMETER_EDIT_SUFFIX("/UAV/MAVLink/ParameterEdit")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L25)

```cpp
inline const ndn::Name SERVICE_MAVLINK_PARAMETER_EDIT_SUFFIX("/UAV/MAVLink/ParameterEdit");
```

### API-384297bfe441 · ndnsf::examples::uav::SERVICE_MAVLINK_ANALYZE_SNAPSHOT_SUFFIX("/UAV/MAVLink/AnalyzeSnapshot")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L26)

```cpp
inline const ndn::Name SERVICE_MAVLINK_ANALYZE_SNAPSHOT_SUFFIX("/UAV/MAVLink/AnalyzeSnapshot");
```

### API-463590a75386 · ndnsf::examples::uav::SERVICE_PREFLIGHT_CHECKLIST_SUFFIX("/UAV/Preflight/Checklist")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L27)

```cpp
inline const ndn::Name SERVICE_PREFLIGHT_CHECKLIST_SUFFIX("/UAV/Preflight/Checklist");
```

### API-196e19255793 · ndnsf::examples::uav::SERVICE_GS_OBJECT_DETECTION("/UAV/GS/ObjectDetection")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L28)

```cpp
inline const ndn::Name SERVICE_GS_OBJECT_DETECTION("/UAV/GS/ObjectDetection");
```

### API-b73c256ffbe0 · ndnsf::examples::uav::SERVICE_INCIDENT_ANALYZE("/UAV/Incident/Analyze")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L29)

```cpp
inline const ndn::Name SERVICE_INCIDENT_ANALYZE("/UAV/Incident/Analyze");
```

### API-a85cf10bc71a · ndnsf::examples::uav::SERVICE_GS_OPERATOR_AUTHORITY_LEASE("/UAV/GS/OperatorAuthority/Lease")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L30)

```cpp
inline const ndn::Name SERVICE_GS_OPERATOR_AUTHORITY_LEASE("/UAV/GS/OperatorAuthority/Lease");
```

### API-28ea98148952 · ndnsf::examples::uav::SERVICE_GS_OPERATOR_AUTHORITY_REVOCATION("/UAV/GS/OperatorAuthority/Revocation")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L31)

```cpp
inline const ndn::Name SERVICE_GS_OPERATOR_AUTHORITY_REVOCATION("/UAV/GS/OperatorAuthority/Revocation");
```

### API-dbaa21ec7340 · ndnsf::examples::uav::SERVICE_GS_OPERATOR_AUTHORITY_AUDIT("/UAV/GS/OperatorAuthority/Audit")

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L32)

```cpp
inline const ndn::Name SERVICE_GS_OPERATOR_AUTHORITY_AUDIT("/UAV/GS/OperatorAuthority/Audit");
```

### API-537529eef4ce · ndnsf::examples::uav::DEFAULT_GS_MAP_LAT = 35.1186

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L33)

```cpp
inline constexpr double DEFAULT_GS_MAP_LAT = 35.1186;
```

### API-5709db7f3f03 · ndnsf::examples::uav::DEFAULT_GS_MAP_LON = -89.9375

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L34)

```cpp
inline constexpr double DEFAULT_GS_MAP_LON = -89.9375;
```

### API-4253f3719a1c · ndnsf::examples::uav::UavRuntimeConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L36)

```cpp
struct UavRuntimeConfig
```

### API-728a1ee1981d · ndnsf::examples::uav::UavRuntimeConfig::groupPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L38)

```cpp
ndn::Name groupPrefix = GROUP_PREFIX;
```

### API-735a8f3591d9 · ndnsf::examples::uav::UavRuntimeConfig::controllerPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L39)

```cpp
ndn::Name controllerPrefix = CONTROLLER_PREFIX;
```

### API-c79ad5eb18bb · ndnsf::examples::uav::UavRuntimeConfig::groundStationIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L40)

```cpp
ndn::Name groundStationIdentity = GROUND_STATION_IDENTITY;
```

### API-27d6c882d2a9 · ndnsf::examples::uav::UavRuntimeConfig::droneIdentityPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L41)

```cpp
ndn::Name droneIdentityPrefix = DRONE_IDENTITY_PREFIX;
```

### API-49669809f675 · ndnsf::examples::uav::UavRuntimeConfig::trustSchema

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L42)

```cpp
std::string trustSchema = TRUST_SCHEMA;
```

### API-e4941f475aa9 · ndnsf::examples::uav::UavRuntimeConfig::serviceMavlinkExecute

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L43)

```cpp
ndn::Name serviceMavlinkExecute = SERVICE_MAVLINK_EXECUTE;
```

### API-d9614884f083 · ndnsf::examples::uav::UavRuntimeConfig::serviceMissionAssign

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L44)

```cpp
ndn::Name serviceMissionAssign = SERVICE_MISSION_ASSIGN;
```

### API-072f022bb88c · ndnsf::examples::uav::UavRuntimeConfig::serviceTelemetryStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L45)

```cpp
ndn::Name serviceTelemetryStatus = SERVICE_TELEMETRY_STATUS;
```

### API-d111e3be26b3 · ndnsf::examples::uav::UavRuntimeConfig::serviceCameraFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L46)

```cpp
ndn::Name serviceCameraFrame = SERVICE_CAMERA_FRAME;
```

### API-3cf3e407c368 · ndnsf::examples::uav::UavRuntimeConfig::serviceCameraVideoControlSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L47)

```cpp
ndn::Name serviceCameraVideoControlSuffix = SERVICE_CAMERA_VIDEO_CONTROL_SUFFIX;
```

### API-b86989b30720 · ndnsf::examples::uav::UavRuntimeConfig::serviceCameraRecordingManifestSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L48)

```cpp
ndn::Name serviceCameraRecordingManifestSuffix = SERVICE_CAMERA_RECORDING_MANIFEST_SUFFIX;
```

### API-4c698d659886 · ndnsf::examples::uav::UavRuntimeConfig::serviceCameraRepoCatalogSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L49)

```cpp
ndn::Name serviceCameraRepoCatalogSuffix = SERVICE_CAMERA_REPO_CATALOG_SUFFIX;
```

### API-e7061ff72921 · ndnsf::examples::uav::UavRuntimeConfig::serviceMavlinkParametersSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L50)

```cpp
ndn::Name serviceMavlinkParametersSuffix = SERVICE_MAVLINK_PARAMETERS_SUFFIX;
```

### API-09210698f09f · ndnsf::examples::uav::UavRuntimeConfig::serviceMavlinkParameterEditSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L51)

```cpp
ndn::Name serviceMavlinkParameterEditSuffix = SERVICE_MAVLINK_PARAMETER_EDIT_SUFFIX;
```

### API-70a2b5144984 · ndnsf::examples::uav::UavRuntimeConfig::serviceMavlinkAnalyzeSnapshotSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L52)

```cpp
ndn::Name serviceMavlinkAnalyzeSnapshotSuffix = SERVICE_MAVLINK_ANALYZE_SNAPSHOT_SUFFIX;
```

### API-69cac780b4bc · ndnsf::examples::uav::UavRuntimeConfig::servicePreflightChecklistSuffix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L53)

```cpp
ndn::Name servicePreflightChecklistSuffix = SERVICE_PREFLIGHT_CHECKLIST_SUFFIX;
```

### API-af0d7bc1585a · ndnsf::examples::uav::UavRuntimeConfig::serviceGsObjectDetection

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L54)

```cpp
ndn::Name serviceGsObjectDetection = SERVICE_GS_OBJECT_DETECTION;
```

### API-f7c7556fd125 · ndnsf::examples::uav::UavRuntimeConfig::serviceIncidentAnalyze

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L55)

```cpp
ndn::Name serviceIncidentAnalyze = SERVICE_INCIDENT_ANALYZE;
```

### API-ca02340534ae · ndnsf::examples::uav::UavRuntimeConfig::serviceGsOperatorAuthorityLease

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L56)

```cpp
ndn::Name serviceGsOperatorAuthorityLease = SERVICE_GS_OPERATOR_AUTHORITY_LEASE;
```

### API-6050a556f428 · ndnsf::examples::uav::UavRuntimeConfig::serviceGsOperatorAuthorityRevocation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L57)

```cpp
ndn::Name serviceGsOperatorAuthorityRevocation = SERVICE_GS_OPERATOR_AUTHORITY_REVOCATION;
```

### API-4d164acf8fc1 · ndnsf::examples::uav::UavRuntimeConfig::serviceGsOperatorAuthorityAudit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L58)

```cpp
ndn::Name serviceGsOperatorAuthorityAudit = SERVICE_GS_OPERATOR_AUTHORITY_AUDIT;
```

### API-b54a925fcade · ndnsf::examples::uav::UavRuntimeConfig::groundStationMapLat

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L59)

```cpp
double groundStationMapLat = std::numeric_limits<double>::quiet_NaN();
```

### API-6e0fcbff742d · ndnsf::examples::uav::UavRuntimeConfig::groundStationMapLon

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L60)

```cpp
double groundStationMapLon = std::numeric_limits<double>::quiet_NaN();
```

### API-dc3fad6463b8 · ndnsf::examples::uav::loadUavRuntimeConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L63)

```cpp
UavRuntimeConfig
loadUavRuntimeConfig(const std::string& path);
```

### API-85cc7dbcaa45 · ndnsf::examples::uav::makeUavEvidenceName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L66)

```cpp
ndn::Name
makeUavEvidenceName(const ndn::Name& producerIdentity,
                    const std::string& missionId,
                    const std::string& incidentId,
                    const std::string& evidenceId,
                    uint64_t version);
```

### API-8e1cf6b96893 · ndnsf::examples::uav::makeUavReportName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L73)

```cpp
ndn::Name
makeUavReportName(const ndn::Name& producerIdentity,
                  const std::string& missionId,
                  const std::string& incidentId,
                  const std::string& attemptId,
                  uint64_t version);
```

### API-9694304faa1c · ndnsf::examples::uav::makeUavMultiViewResultName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L81)

```cpp
ndn::Name
makeUavMultiViewResultName(const ndn::Name& providerIdentity,
                           const std::string& missionSessionId,
                           const std::string& jobId,
                           uint64_t attempt,
                           uint64_t version);
```

原始接口说明：

```text
/** Provider-owned immutable outputs for a bounded multi-view recognition job. */
```

### API-2525a3688382 · ndnsf::examples::uav::makeUavMultiViewAnnotationName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L88)

```cpp
ndn::Name
makeUavMultiViewAnnotationName(const ndn::Name& providerIdentity,
                               const std::string& missionSessionId,
                               const std::string& jobId,
                               uint64_t attempt,
                               const std::string& viewId,
                               uint64_t version);
```

### API-e5e74a609ad5 · ndnsf::examples::uav::isUavProviderMultiViewDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L96)

```cpp
bool
isUavProviderMultiViewDataName(const ndn::Name& providerIdentity,
                               const ndn::Name& objectName,
                               const std::string& objectKind,
                               const std::string& missionSessionId,
                               const std::string& jobId);
```

### API-461fa8669813 · ndnsf::examples::uav::isUavProducerDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L103)

```cpp
bool
isUavProducerDataName(const ndn::Name& producerIdentity,
                      const ndn::Name& objectName,
                      const std::string& objectKind);
```

### API-43e6b69d22dc · ndnsf::examples::uav::isUavProducerDataNameForContext

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L114)

```cpp
bool
isUavProducerDataNameForContext(const ndn::Name& producerIdentity,
                                const ndn::Name& objectName,
                                const std::string& objectKind,
                                const std::string& missionId,
                                const std::string& incidentId);
```

原始接口说明：

```text
/**
 * Validate a producer-owned object name and bind its mission/incident
 * components to the surrounding application record.  The generic helper
 * above checks only the object grammar; this contextual form prevents a
 * validly signed object from being attached to the wrong incident.
 */
```

### API-6bdd985c957e · ndnsf::examples::uav::droneIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L121)

```cpp
ndn::Name
droneIdentity(const std::string& droneId);
```

### API-590eee6fbd38 · ndnsf::examples::uav::droneIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L124)

```cpp
ndn::Name
droneIdentity(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-70894bf37f05 · ndnsf::examples::uav::droneVideoControlService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L127)

```cpp
ndn::Name
droneVideoControlService(const std::string& droneId);
```

### API-b2a7e1cecbca · ndnsf::examples::uav::droneVideoControlService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L130)

```cpp
ndn::Name
droneVideoControlService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-f363842c8d97 · ndnsf::examples::uav::droneCameraRecordingManifestService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L133)

```cpp
ndn::Name
droneCameraRecordingManifestService(const std::string& droneId);
```

### API-eedb27743047 · ndnsf::examples::uav::droneCameraRecordingManifestService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L136)

```cpp
ndn::Name
droneCameraRecordingManifestService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-019401b524be · ndnsf::examples::uav::droneCameraRepoCatalogService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L139)

```cpp
ndn::Name
droneCameraRepoCatalogService(const std::string& droneId);
```

### API-30003b89f002 · ndnsf::examples::uav::droneCameraRepoCatalogService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L142)

```cpp
ndn::Name
droneCameraRepoCatalogService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-0352281d9c2b · ndnsf::examples::uav::droneMavlinkParametersService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L145)

```cpp
ndn::Name
droneMavlinkParametersService(const std::string& droneId);
```

### API-47d802b59ec2 · ndnsf::examples::uav::droneMavlinkParametersService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L148)

```cpp
ndn::Name
droneMavlinkParametersService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-dc8969621049 · ndnsf::examples::uav::droneMavlinkParameterEditService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L151)

```cpp
ndn::Name
droneMavlinkParameterEditService(const std::string& droneId);
```

### API-86884ced351c · ndnsf::examples::uav::droneMavlinkParameterEditService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L154)

```cpp
ndn::Name
droneMavlinkParameterEditService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-799151ef03df · ndnsf::examples::uav::droneMavlinkAnalyzeSnapshotService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L157)

```cpp
ndn::Name
droneMavlinkAnalyzeSnapshotService(const std::string& droneId);
```

### API-2fb9dce943a5 · ndnsf::examples::uav::droneMavlinkAnalyzeSnapshotService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L160)

```cpp
ndn::Name
droneMavlinkAnalyzeSnapshotService(const UavRuntimeConfig& config, const std::string& droneId);
```

### API-b36255e4753f · ndnsf::examples::uav::dronePreflightChecklistService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L163)

```cpp
ndn::Name
dronePreflightChecklistService(const std::string& droneId);
```

### API-83d6bf4c8adf · ndnsf::examples::uav::dronePreflightChecklistService

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavNames.hpp#L166)

```cpp
ndn::Name
dronePreflightChecklistService(const UavRuntimeConfig& config, const std::string& droneId);
```

## NDNSF-UAV-APP/shared/UavProtocol.hpp

源码 SHA-256：`e54782a9abdfc6e20fd55fd1f9794c78c6a4c1e93df8969fc24b0cf00b9b850c`。

### API-f9c18d9b10f3 · ndnsf::examples::uav::Fields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L18)

```cpp
using Fields = std::map<std::string, std::string>;
```

### API-f23a28cf14f1 · ndnsf::examples::uav::UavMissionSessionState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L24)

```cpp
enum class UavMissionSessionState
```

### API-5ca2c184e581 · ndnsf::examples::uav::UavMissionSessionState::Planned

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L26)

```cpp
Planned
```

### API-51453388aa05 · ndnsf::examples::uav::UavMissionSessionState::Starting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L27)

```cpp
Starting
```

### API-5bf938483218 · ndnsf::examples::uav::UavMissionSessionState::Active

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L28)

```cpp
Active
```

### API-332655aa1a75 · ndnsf::examples::uav::UavMissionSessionState::Degraded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L29)

```cpp
Degraded
```

### API-0bf5278dd945 · ndnsf::examples::uav::UavMissionSessionState::Compensating

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L30)

```cpp
Compensating
```

### API-08770e545c76 · ndnsf::examples::uav::UavMissionSessionState::Cancelling

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L31)

```cpp
Cancelling
```

### API-47083295400c · ndnsf::examples::uav::UavMissionSessionState::Recovering

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L32)

```cpp
Recovering
```

### API-213224831261 · ndnsf::examples::uav::UavMissionSessionState::Completed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L33)

```cpp
Completed
```

### API-688d5623b79e · ndnsf::examples::uav::UavMissionSessionState::Cancelled

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L34)

```cpp
Cancelled
```

### API-708c3a6faca4 · ndnsf::examples::uav::UavMissionSessionState::Failed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L35)

```cpp
Failed
```

### API-45d301767f39 · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L38)

```cpp
const char*
to_string(UavMissionSessionState state) noexcept;
```

### API-379a21cc1189 · ndnsf::examples::uav::parseUavMissionSessionState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L41)

```cpp
std::optional<UavMissionSessionState>
parseUavMissionSessionState(const std::string& value);
```

### API-fd7ff39f4087 · ndnsf::examples::uav::UavMissionPartState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L44)

```cpp
enum class UavMissionPartState
```

### API-fa8d74aa4e08 · ndnsf::examples::uav::UavMissionPartState::Pending

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L46)

```cpp
Pending
```

### API-9892c088a08c · ndnsf::examples::uav::UavMissionPartState::Accepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L47)

```cpp
Accepted
```

### API-7b515a5e9f0b · ndnsf::examples::uav::UavMissionPartState::Executing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L48)

```cpp
Executing
```

### API-7d5880afbf1d · ndnsf::examples::uav::UavMissionPartState::Completed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L49)

```cpp
Completed
```

### API-cd5ef4dfe920 · ndnsf::examples::uav::UavMissionPartState::Missing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L50)

```cpp
Missing
```

### API-e5dffb55e8f6 · ndnsf::examples::uav::UavMissionPartState::Compensated

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L51)

```cpp
Compensated
```

### API-bb4587f3cacf · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L54)

```cpp
const char*
to_string(UavMissionPartState state) noexcept;
```

### API-884dbb1a1219 · ndnsf::examples::uav::parseUavMissionPartState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L57)

```cpp
std::optional<UavMissionPartState>
parseUavMissionPartState(const std::string& value);
```

### API-f456818bceb3 · ndnsf::examples::uav::UavCollaborationJobState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L60)

```cpp
enum class UavCollaborationJobState
```

### API-33d3df9ec8de · ndnsf::examples::uav::UavCollaborationJobState::Created

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L62)

```cpp
Created
```

### API-ab7c8ef93f11 · ndnsf::examples::uav::UavCollaborationJobState::AckCollecting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L63)

```cpp
AckCollecting
```

### API-341a7639a233 · ndnsf::examples::uav::UavCollaborationJobState::AckClosed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L64)

```cpp
AckClosed
```

### API-75d0bdb4b2b7 · ndnsf::examples::uav::UavCollaborationJobState::PlanCommitted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L65)

```cpp
PlanCommitted
```

### API-2c0616272ae7 · ndnsf::examples::uav::UavCollaborationJobState::Selected

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L66)

```cpp
Selected
```

### API-9669656481fd · ndnsf::examples::uav::UavCollaborationJobState::EvidenceReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L67)

```cpp
EvidenceReady
```

### API-6354c732236c · ndnsf::examples::uav::UavCollaborationJobState::Executing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L68)

```cpp
Executing
```

### API-038bd130004b · ndnsf::examples::uav::UavCollaborationJobState::Reporting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L69)

```cpp
Reporting
```

### API-7e6ebe83ba32 · ndnsf::examples::uav::UavCollaborationJobState::Succeeded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L70)

```cpp
Succeeded
```

### API-5d9bb6932218 · ndnsf::examples::uav::UavCollaborationJobState::Failed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L71)

```cpp
Failed
```

### API-d076ab0c8cf6 · ndnsf::examples::uav::UavCollaborationJobState::TimedOut

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L72)

```cpp
TimedOut
```

### API-22205735400b · ndnsf::examples::uav::UavCollaborationJobState::Cancelled

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L73)

```cpp
Cancelled
```

### API-8ff8e04a847a · ndnsf::examples::uav::to_string

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L76)

```cpp
const char*
to_string(UavCollaborationJobState state) noexcept;
```

### API-39897e1adbda · ndnsf::examples::uav::parseUavCollaborationJobState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L79)

```cpp
std::optional<UavCollaborationJobState>
parseUavCollaborationJobState(const std::string& value);
```

### API-c10721e6bb20 · ndnsf::examples::uav::UavStreamBinding

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L82)

```cpp
struct UavStreamBinding
```

### API-41649133b6dc · ndnsf::examples::uav::UavStreamBinding::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L84)

```cpp
std::string streamId;
```

### API-94d50ac51f4a · ndnsf::examples::uav::UavStreamBinding::producerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L85)

```cpp
ndn::Name producerIdentity;
```

### API-00d50ffc0e28 · ndnsf::examples::uav::UavStreamBinding::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L86)

```cpp
uint64_t sessionEpoch = 0;
```

### API-8be020897ed1 · ndnsf::examples::uav::UavStreamBinding::firstCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L87)

```cpp
uint64_t firstCursor = 0;
```

### API-f87a99e57e7b · ndnsf::examples::uav::UavStreamBinding::lastCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L88)

```cpp
uint64_t lastCursor = 0;
```

### API-967867d10830 · ndnsf::examples::uav::UavStreamBinding::active

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L89)

```cpp
bool active = false;
```

### API-075e517378df · ndnsf::examples::uav::UavEvidenceReference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L92)

```cpp
struct UavEvidenceReference
```

### API-7acc3f139098 · ndnsf::examples::uav::UavEvidenceReference::producerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L94)

```cpp
ndn::Name producerIdentity;
```

### API-a3365fd5372d · ndnsf::examples::uav::UavEvidenceReference::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L95)

```cpp
std::string streamId;
```

### API-94a48c8c55b7 · ndnsf::examples::uav::UavEvidenceReference::streamSessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L96)

```cpp
uint64_t streamSessionEpoch = 0;
```

### API-4e15e162076f · ndnsf::examples::uav::UavEvidenceReference::firstSequence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L97)

```cpp
uint64_t firstSequence = 0;
```

### API-d5183358579d · ndnsf::examples::uav::UavEvidenceReference::lastSequence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L98)

```cpp
uint64_t lastSequence = 0;
```

### API-dfb8964991b3 · ndnsf::examples::uav::UavEvidenceReference::windowStartMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L99)

```cpp
uint64_t windowStartMs = 0;
```

### API-940bc7150234 · ndnsf::examples::uav::UavEvidenceReference::windowEndMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L100)

```cpp
uint64_t windowEndMs = 0;
```

### API-8458c21a9dd0 · ndnsf::examples::uav::UavEvidenceReference::exactDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L101)

```cpp
ndn::Name exactDataName;
```

### API-15781f8ab376 · ndnsf::examples::uav::UavEvidenceReference::version

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L102)

```cpp
uint64_t version = 0;
```

### API-46d4f94f2a98 · ndnsf::examples::uav::UavEvidenceReference::contentDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L103)

```cpp
std::string contentDigest;
```

### API-af576cfe69b9 · ndnsf::examples::uav::UavEvidenceReference::contentType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L104)

```cpp
std::string contentType;
```

### API-6d2a60068bb9 · ndnsf::examples::uav::UavEvidenceReference::retentionDeadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L105)

```cpp
uint64_t retentionDeadlineMs = 0;
```

### API-7448258c3f1f · ndnsf::examples::uav::UavEvidenceReference::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L107)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-41cab5b89b26 · ndnsf::examples::uav::UavEvidenceReference::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L108)

```cpp
Fields toFields(const std::string& prefix = {}) const;
```

### API-9684caee0601 · ndnsf::examples::uav::makeUavIncidentEvidenceContent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L117)

```cpp
ndn::Buffer
makeUavIncidentEvidenceContent(const std::string& missionId,
                               const std::string& incidentId,
                               const UavEvidenceReference& evidence);
```

原始接口说明：

```text
/**
 * Build the bounded, deterministic manifest published by an EvidenceSource.
 * The manifest is application content carried in producer-owned signed Data;
 * its digest is stored in UavEvidenceReference and is intentionally not
 * repeated inside the content to avoid a self-referential hash.
 */
```

### API-7946ae0bb7b1 · ndnsf::examples::uav::UavMissionPartRecord

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L122)

```cpp
struct UavMissionPartRecord
```

### API-230955dd09d9 · ndnsf::examples::uav::UavMissionPartRecord::partId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L124)

```cpp
std::string partId;
```

### API-a40caecd1cd8 · ndnsf::examples::uav::UavMissionPartRecord::sector

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L125)

```cpp
std::string sector;
```

### API-04abb9bf986c · ndnsf::examples::uav::UavMissionPartRecord::waypointDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L126)

```cpp
std::string waypointDigest;
```

### API-afb6606a8021 · ndnsf::examples::uav::UavMissionPartRecord::attemptId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L127)

```cpp
std::string attemptId;
```

### API-c54b70632dba · ndnsf::examples::uav::UavMissionPartRecord::assignedProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L128)

```cpp
ndn::Name assignedProvider;
```

### API-294a1442fe7c · ndnsf::examples::uav::UavMissionPartRecord::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L129)

```cpp
UavMissionPartState state = UavMissionPartState::Pending;
```

### API-0c98d791d431 · ndnsf::examples::uav::UavMissionPartRecord::completedWaypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L130)

```cpp
std::vector<uint64_t> completedWaypoints;
```

### API-401e0b00f295 · ndnsf::examples::uav::UavMissionPartRecord::responseDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L131)

```cpp
std::string responseDigest;
```

### API-7c7ccdcc45a6 · ndnsf::examples::uav::UavMissionPartRecord::authoritativeVehicleState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L132)

```cpp
std::string authoritativeVehicleState;
```

### API-d9b7e0fb281b · ndnsf::examples::uav::UavIncidentRecord

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L135)

```cpp
struct UavIncidentRecord
```

### API-6d57cf951d2c · ndnsf::examples::uav::UavIncidentRecord::incidentId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L137)

```cpp
std::string incidentId;
```

### API-027f727ceed5 · ndnsf::examples::uav::UavIncidentRecord::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L138)

```cpp
std::string missionId;
```

### API-23710cbfbbc5 · ndnsf::examples::uav::UavIncidentRecord::triggerKind

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L139)

```cpp
std::string triggerKind;
```

### API-a82d1f78c36d · ndnsf::examples::uav::UavIncidentRecord::triggerTimeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L140)

```cpp
uint64_t triggerTimeMs = 0;
```

### API-864711d65417 · ndnsf::examples::uav::UavIncidentRecord::location

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L141)

```cpp
std::string location;
```

### API-6c89b293ec59 · ndnsf::examples::uav::UavIncidentRecord::requestedCapability

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L142)

```cpp
std::string requestedCapability;
```

### API-87347c8f15cf · ndnsf::examples::uav::UavIncidentRecord::evidence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L143)

```cpp
std::vector<UavEvidenceReference> evidence;
```

### API-15ab3204f976 · ndnsf::examples::uav::UavIncidentRecord::currentAttemptId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L144)

```cpp
std::string currentAttemptId;
```

### API-16145fead9c0 · ndnsf::examples::uav::UavIncidentRecord::acceptedTerminalReportDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L145)

```cpp
std::string acceptedTerminalReportDigest;
```

### API-e6407f1f8b63 · ndnsf::examples::uav::UavProviderCapabilitySnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L148)

```cpp
struct UavProviderCapabilitySnapshot
```

### API-4608ce356407 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L150)

```cpp
ndn::Name providerIdentity;
```

### API-da3a0604bb71 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::modelId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L151)

```cpp
std::string modelId;
```

### API-0774d1a57aff · ndnsf::examples::uav::UavProviderCapabilitySnapshot::modelDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L152)

```cpp
std::string modelDigest;
```

### API-c6d16a50d1f6 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::qualityProfile

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L153)

```cpp
std::string qualityProfile;
```

### API-c3f0957ccba8 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::deviceClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L154)

```cpp
std::string deviceClass;
```

### API-5445a5e3dec2 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::ready

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L155)

```cpp
bool ready = false;
```

### API-75f8721b2ba3 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::queueDepth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L156)

```cpp
uint64_t queueDepth = 0;
```

### API-b136b23ccfef · ndnsf::examples::uav::UavProviderCapabilitySnapshot::estimatedStartMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L157)

```cpp
uint64_t estimatedStartMs = 0;
```

### API-1dd627f40296 · ndnsf::examples::uav::UavProviderCapabilitySnapshot::evidenceAccess

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L158)

```cpp
bool evidenceAccess = false;
```

### API-220ed5e031ed · ndnsf::examples::uav::UavProviderCapabilitySnapshot::snapshotTimeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L159)

```cpp
uint64_t snapshotTimeMs = 0;
```

### API-74a7eb3b454c · ndnsf::examples::uav::UavRoleAssignment

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L162)

```cpp
struct UavRoleAssignment
```

### API-16c98bf6c68c · ndnsf::examples::uav::UavRoleAssignment::role

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L164)

```cpp
std::string role;
```

### API-adb4fe17fd2f · ndnsf::examples::uav::UavRoleAssignment::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L165)

```cpp
ndn::Name providerIdentity;
```

### API-ede6b8995f44 · ndnsf::examples::uav::UavRoleAssignment::terminalResponseOwner

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L166)

```cpp
bool terminalResponseOwner = false;
```

### API-4a4c54c6dea8 · ndnsf::examples::uav::UavRoleAssignment::evidence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L167)

```cpp
std::vector<UavEvidenceReference> evidence;
```

### API-d6e5f4d63638 · ndnsf::examples::uav::UavCollaborationJobRecord

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L170)

```cpp
struct UavCollaborationJobRecord
```

### API-58ea23344892 · ndnsf::examples::uav::UavCollaborationJobRecord::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L172)

```cpp
ndn::Name requestId;
```

### API-9079af95e460 · ndnsf::examples::uav::UavCollaborationJobRecord::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L173)

```cpp
std::string missionId;
```

### API-d690288e9515 · ndnsf::examples::uav::UavCollaborationJobRecord::incidentId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L174)

```cpp
std::string incidentId;
```

### API-c2d70db3db14 · ndnsf::examples::uav::UavCollaborationJobRecord::attemptId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L175)

```cpp
std::string attemptId;
```

### API-662c5960e031 · ndnsf::examples::uav::UavCollaborationJobRecord::ackDeadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L176)

```cpp
uint64_t ackDeadlineMs = 0;
```

### API-5baf96d539c2 · ndnsf::examples::uav::UavCollaborationJobRecord::globalDeadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L177)

```cpp
uint64_t globalDeadlineMs = 0;
```

### API-497e323c0d9a · ndnsf::examples::uav::UavCollaborationJobRecord::planDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L178)

```cpp
std::string planDigest;
```

### API-08c3a1cdebdb · ndnsf::examples::uav::UavCollaborationJobRecord::assignments

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L179)

```cpp
std::vector<UavRoleAssignment> assignments;
```

### API-04d9e4886418 · ndnsf::examples::uav::UavCollaborationJobRecord::terminalOwner

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L180)

```cpp
ndn::Name terminalOwner;
```

### API-2a2952c9b6f1 · ndnsf::examples::uav::UavCollaborationJobRecord::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L181)

```cpp
UavCollaborationJobState state = UavCollaborationJobState::Created;
```

### API-287b7f7e718e · ndnsf::examples::uav::UavCollaborationJobRecord::failureStage

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L182)

```cpp
std::string failureStage;
```

### API-5aada39a51fd · ndnsf::examples::uav::UavCollaborationJobRecord::failureReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L183)

```cpp
std::string failureReason;
```

### API-7be54c2564c0 · ndnsf::examples::uav::UavCollaborationJobRecord::fallbackMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L184)

```cpp
std::string fallbackMode = "disabled";
```

### API-483a2d0b5f27 · ndnsf::examples::uav::UavCollaborationJobRecord::terminalReportDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L185)

```cpp
std::string terminalReportDigest;
```

### API-898b162c816b · ndnsf::examples::uav::UavTerminalReport

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L188)

```cpp
struct UavTerminalReport
```

### API-a42fe6a24d70 · ndnsf::examples::uav::UavTerminalReport::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L190)

```cpp
std::string missionId;
```

### API-4b0a6efb4a4c · ndnsf::examples::uav::UavTerminalReport::incidentId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L191)

```cpp
std::string incidentId;
```

### API-e807b6de541d · ndnsf::examples::uav::UavTerminalReport::attemptId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L192)

```cpp
std::string attemptId;
```

### API-232e1d27600d · ndnsf::examples::uav::UavTerminalReport::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L193)

```cpp
ndn::Name requestId;
```

### API-faabcf1594b6 · ndnsf::examples::uav::UavTerminalReport::planDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L194)

```cpp
std::string planDigest;
```

### API-3da55a91efb1 · ndnsf::examples::uav::UavTerminalReport::terminalOwner

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L195)

```cpp
ndn::Name terminalOwner;
```

### API-b31eda6c9bfb · ndnsf::examples::uav::UavTerminalReport::selectedProvider

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L196)

```cpp
ndn::Name selectedProvider;
```

### API-fc87aeef94eb · ndnsf::examples::uav::UavTerminalReport::modelId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L197)

```cpp
std::string modelId;
```

### API-e53e70b74fc0 · ndnsf::examples::uav::UavTerminalReport::modelDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L198)

```cpp
std::string modelDigest;
```

### API-988bc26f79cd · ndnsf::examples::uav::UavTerminalReport::evidence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L199)

```cpp
std::vector<UavEvidenceReference> evidence;
```

### API-e95aad0d6ebd · ndnsf::examples::uav::UavTerminalReport::reportName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L200)

```cpp
ndn::Name reportName;
```

### API-4e41340e97b9 · ndnsf::examples::uav::UavTerminalReport::resultDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L201)

```cpp
std::string resultDigest;
```

### API-a7f4de1d9a72 · ndnsf::examples::uav::UavTerminalReport::status

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L202)

```cpp
std::string status;
```

### API-437ed6e034a9 · ndnsf::examples::uav::UavTerminalReport::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L204)

```cpp
bool isValid(std::string* reason = nullptr) const;
```

### API-093e05dd85de · ndnsf::examples::uav::UavMissionSessionRecord

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L207)

```cpp
struct UavMissionSessionRecord
```

### API-74d742258bf6 · ndnsf::examples::uav::UavMissionSessionRecord::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L209)

```cpp
std::string missionId;
```

### API-27b51cd0e53a · ndnsf::examples::uav::UavMissionSessionRecord::planDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L210)

```cpp
std::string planDigest;
```

### API-532198e2b46f · ndnsf::examples::uav::UavMissionSessionRecord::operatorIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L211)

```cpp
ndn::Name operatorIdentity;
```

### API-254711661a43 · ndnsf::examples::uav::UavMissionSessionRecord::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L212)

```cpp
UavMissionSessionState state = UavMissionSessionState::Planned;
```

### API-75dd43572517 · ndnsf::examples::uav::UavMissionSessionRecord::createdAtMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L213)

```cpp
uint64_t createdAtMs = 0;
```

### API-3d4037d7c6c7 · ndnsf::examples::uav::UavMissionSessionRecord::deadlineMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L214)

```cpp
uint64_t deadlineMs = 0;
```

### API-d2a57f34b616 · ndnsf::examples::uav::UavMissionSessionRecord::updatedAtMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L215)

```cpp
uint64_t updatedAtMs = 0;
```

### API-a57f14ea6588 · ndnsf::examples::uav::UavMissionSessionRecord::parts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L216)

```cpp
std::vector<UavMissionPartRecord> parts;
```

### API-ddd3fbbf66f7 · ndnsf::examples::uav::UavMissionSessionRecord::streams

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L217)

```cpp
std::vector<UavStreamBinding> streams;
```

### API-0b19978af6d2 · ndnsf::examples::uav::UavMissionSessionRecord::incidents

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L218)

```cpp
std::vector<UavIncidentRecord> incidents;
```

### API-f99baeadf4b5 · ndnsf::examples::uav::UavMissionSessionRecord::jobs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L219)

```cpp
std::vector<UavCollaborationJobRecord> jobs;
```

### API-1c7d42112877 · ndnsf::examples::uav::UAV_VIDEO_MAX_NAME_RESERVATIONS = 65536

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L222)

```cpp
inline constexpr size_t UAV_VIDEO_MAX_NAME_RESERVATIONS = 65536;
```

### API-eaa670723b5d · ndnsf::examples::uav::UAV_VIDEO_LIVE_RETENTION_MS = 10000

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L223)

```cpp
inline constexpr uint64_t UAV_VIDEO_LIVE_RETENTION_MS = 10000;
```

### API-986d9af04887 · ndnsf::examples::uav::UAV_VIDEO_MAX_RETAINED_ITEMS = 16384

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L224)

```cpp
inline constexpr size_t UAV_VIDEO_MAX_RETAINED_ITEMS = 16384;
```

### API-2b07d88e2ed0 · ndnsf::examples::uav::computeLiveVideoRetentionItems

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L227)

```cpp
size_t
computeLiveVideoRetentionItems(uint64_t fps,
                               uint64_t dataShards,
                               uint64_t parityShards,
                               uint64_t retentionMs = UAV_VIDEO_LIVE_RETENTION_MS);
```

原始接口说明：

```text
/** Convert a live-duration target into a bounded signed-Data item budget. */
```

### API-23fcc3471232 · ndnsf::examples::uav::VideoStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L240)

```cpp
struct VideoStreamDescriptor
```

### API-f14d0a5e3396 · ndnsf::examples::uav::VideoStreamDescriptor::contractVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L242)

```cpp
uint64_t contractVersion = 2;
```

### API-4f6a2039fa38 · ndnsf::examples::uav::VideoStreamDescriptor::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L243)

```cpp
std::string streamId;
```

### API-0995a36c9b13 · ndnsf::examples::uav::VideoStreamDescriptor::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L244)

```cpp
uint64_t sessionEpoch = 0;
```

### API-1c9ccec01d69 · ndnsf::examples::uav::VideoStreamDescriptor::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L245)

```cpp
ndn::Name providerIdentity;
```

### API-4dda995918e1 · ndnsf::examples::uav::VideoStreamDescriptor::serviceName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L246)

```cpp
ndn::Name serviceName;
```

### API-4b80602fc888 · ndnsf::examples::uav::VideoStreamDescriptor::dataPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L247)

```cpp
ndn::Name dataPrefix;
```

### API-df77278440f6 · ndnsf::examples::uav::VideoStreamDescriptor::mappingRoot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L248)

```cpp
ndn::Name mappingRoot;
```

### API-6e3613c23a7a · ndnsf::examples::uav::VideoStreamDescriptor::mappingVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L249)

```cpp
uint64_t mappingVersion = 0;
```

### API-ed9e6e5140d7 · ndnsf::examples::uav::VideoStreamDescriptor::mappingBlockCapacity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L250)

```cpp
uint64_t mappingBlockCapacity = 0;
```

### API-350066c72eab · ndnsf::examples::uav::VideoStreamDescriptor::maxNameReservations

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L251)

```cpp
size_t maxNameReservations = UAV_VIDEO_MAX_NAME_RESERVATIONS;
```

### API-b2615f8eaf87 · ndnsf::examples::uav::VideoStreamDescriptor::mappingAnchorBlock

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L252)

```cpp
uint64_t mappingAnchorBlock = 0;
```

### API-be31b17eb2c0 · ndnsf::examples::uav::VideoStreamDescriptor::mappingAnchorContentDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L253)

```cpp
ndn_service_framework::StreamContentDigest mappingAnchorContentDigest{};
```

### API-a9d627e10919 · ndnsf::examples::uav::VideoStreamDescriptor::sampleUnit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L254)

```cpp
std::string sampleUnit;
```

### API-261b8f377330 · ndnsf::examples::uav::VideoStreamDescriptor::samplePeriodMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L255)

```cpp
uint64_t samplePeriodMs = 0;
```

### API-2a03a94a61eb · ndnsf::examples::uav::VideoStreamDescriptor::frontiers

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L256)

```cpp
ndn_service_framework::StreamCursorFrontiers frontiers;
```

### API-b98ebdfddc57 · ndnsf::examples::uav::VideoStreamDescriptor::prefetchEligibility

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L257)

```cpp
std::string prefetchEligibility;
```

### API-770def8cb521 · ndnsf::examples::uav::VideoStreamDescriptor::cipher

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L258)

```cpp
std::string cipher;
```

### API-ca1dccdaa09d · ndnsf::examples::uav::VideoStreamDescriptor::keyEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L259)

```cpp
uint64_t keyEpoch = 0;
```

### API-8231ecefde1f · ndnsf::examples::uav::VideoStreamDescriptor::streamKey

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L260)

```cpp
ndn::Buffer streamKey;
```

### API-85ac3d3a797c · ndnsf::examples::uav::VideoStreamDescriptor::nonceSalt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L261)

```cpp
ndn::Buffer nonceSalt;
```

### API-261091bbce86 · ndnsf::examples::uav::VideoStreamDescriptor::extensions

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L262)

```cpp
Fields extensions;
```

### API-9245ff4a7444 · ndnsf::examples::uav::toCoreLiveStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L266)

```cpp
ndn_service_framework::LiveStreamDescriptor
toCoreLiveStreamDescriptor(const VideoStreamDescriptor& descriptor);
```

原始接口说明：

```text
/** Project the protected UAV descriptor onto the app-neutral Core transport. */
```

### API-e5c4bea28f51 · ndnsf::examples::uav::toCorePredictiveStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L270)

```cpp
ndn_service_framework::PredictiveStreamDescriptor
toCorePredictiveStreamDescriptor(const VideoStreamDescriptor& descriptor);
```

原始接口说明：

```text
/** Project a live protected UAV descriptor onto the predictive Core transport. */
```

### API-a6aa1f39e71f · ndnsf::examples::uav::applyCoreLiveStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L274)

```cpp
void
applyCoreLiveStreamDescriptor(VideoStreamDescriptor& descriptor,
                              const ndn_service_framework::LiveStreamDescriptor& core);
```

原始接口说明：

```text
/** Copy Core readiness/frontier evidence into the key-bearing UAV descriptor. */
```

### API-23ca282c6c5e · ndnsf::examples::uav::applyCorePredictiveStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L279)

```cpp
void
applyCorePredictiveStreamDescriptor(
  VideoStreamDescriptor& descriptor,
  const ndn_service_framework::PredictiveStreamDescriptor& core);
```

原始接口说明：

```text
/** Copy predictive Core readiness/frontier evidence into the UAV descriptor. */
```

### API-39b9c8926876 · ndnsf::examples::uav::applyCoreLiveStreamStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L285)

```cpp
void
applyCoreLiveStreamStatus(VideoStreamDescriptor& descriptor,
                          const ndn_service_framework::LiveStreamStatus& status,
                          ndn_service_framework::StreamCursor latestProduced);
```

原始接口说明：

```text
/** Refresh provisional publication frontiers after ahead Mapping reservations. */
```

### API-5f136d889323 · ndnsf::examples::uav::UavH264ReadinessTracker

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L298)

```cpp
class UavH264ReadinessTracker
```

### API-a933a817e86b · ndnsf::examples::uav::UavH264ReadinessTracker::UavH264ReadinessTracker

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L301)

```cpp
explicit UavH264ReadinessTracker(uint64_t minimumGroups = 3);
```

### API-286fbd9c8f07 · ndnsf::examples::uav::UavH264ReadinessTracker::reset

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L303)

```cpp
void reset();
```

### API-3273427b3c61 · ndnsf::examples::uav::UavH264ReadinessTracker::observePublicationGroup

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L304)

```cpp
void observePublicationGroup(uint64_t firstCursor,
                               uint64_t lastCursor,
                               uint64_t publishedMonotonicMs,
                               const std::vector<uint8_t>& annexBBytes);
```

### API-61f01c2fba10 · ndnsf::examples::uav::UavH264ReadinessTracker::ready

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L309)

```cpp
bool ready() const;
```

### API-03a0c5469790 · ndnsf::examples::uav::UavH264ReadinessTracker::completedGroups

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L310)

```cpp
uint64_t completedGroups() const;
```

### API-748696d83dfa · ndnsf::examples::uav::UavH264ReadinessTracker::samplePeriodMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L311)

```cpp
uint64_t samplePeriodMs() const;
```

### API-09a07734d2dd · ndnsf::examples::uav::UavH264ReadinessTracker::latestJoinCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L312)

```cpp
uint64_t latestJoinCursor() const;
```

### API-5060a3012d9c · ndnsf::examples::uav::UavH264ReadinessTracker::latestProducedCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L313)

```cpp
uint64_t latestProducedCursor() const;
```

### API-8742d3016af8 · ndnsf::examples::uav::UavH264ReadinessTracker::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L314)

```cpp
std::string reason() const;
```

### API-132acf78c486 · ndnsf::examples::uav::UavVideoDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L333)

```cpp
struct UavVideoDataName
```

### API-d65c7f742a7b · ndnsf::examples::uav::UavVideoDataName::name

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L335)

```cpp
ndn::Name name;
```

### API-87f6de8d8a36 · ndnsf::examples::uav::UavVideoDataName::finalBlockId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L336)

```cpp
ndn::name::Component finalBlockId;
```

### API-64471eedc854 · ndnsf::examples::uav::UavVideoDataName::cursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L337)

```cpp
ndn_service_framework::StreamCursor cursor = 0;
```

### API-2259143124d5 · ndnsf::examples::uav::UavVideoDataName::parity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L338)

```cpp
bool parity = false;
```

### API-1749ee115357 · ndnsf::examples::uav::sourceMediaSequenceForJoinCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L343)

```cpp
uint64_t
sourceMediaSequenceForJoinCursor(ndn_service_framework::StreamCursor cursor,
                                 uint32_t dataShards,
                                 uint32_t parityShards);
```

原始接口说明：

```text
/** Translate a publication join cursor (sources plus repairs) to the first
 * source-only media sequence that may enter the decoder. */
```

### API-c572cc0a3058 · ndnsf::examples::uav::VideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L348)

```cpp
struct VideoPacket
```

### API-ac1a1801ae40 · ndnsf::examples::uav::makeUavPredictiveSessionPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L354)

```cpp
ndn::Name
makeUavPredictiveSessionPrefix(const std::string& droneId, uint64_t epoch);
```

原始接口说明：

```text
// ── Predictive Stream Helpers (Spec 148) ──
/** Construct the predictive stream session prefix.
 *  /example/uav/drone/<droneId>/video/v=<epoch> */
```

### API-c164ad8377d6 · ndnsf::examples::uav::makeUavPredictiveMappingName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L359)

```cpp
ndn::Name
makeUavPredictiveMappingName(const ndn::Name& sessionPrefix,
                              uint64_t mappingVersion,
                              uint64_t cursor);
```

原始接口说明：

```text
/** Construct a Mapping name for a given cursor.
 *  <sessionPrefix>/v=<version>/SequenceNum=<cursor> */
```

### API-f626604574af · ndnsf::examples::uav::uavVideoPacketToSignedData

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L366)

```cpp
std::shared_ptr<ndn::Data>
uavVideoPacketToSignedData(const VideoPacket& packet,
                            const ndn::Name& sessionPrefix,
                            uint64_t mappingVersion,
                            ndn::KeyChain& keyChain,
                            const ndn::security::SigningInfo& signingInfo);
```

原始接口说明：

```text
/** Convert a VideoPacket to a signed NDN Data packet for push().
 *  The packet is named under the session prefix and signed by the drone KeyChain. */
```

### API-32fa61e88a76 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L374)

```cpp
enum : uint32_t
```

### API-ffdf02c69dc3 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L375)

```cpp
UavVideoAadType = 0xF700
```

### API-955c8936502c · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadVersionType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L376)

```cpp
UavVideoAadVersionType = 0xF701
```

### API-5f29e4d1e98b · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadExactDataNameType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L377)

```cpp
UavVideoAadExactDataNameType = 0xF702
```

### API-21fd44682e2f · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadProviderIdentityType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L378)

```cpp
UavVideoAadProviderIdentityType = 0xF703
```

### API-448c9b156007 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadServiceNameType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L379)

```cpp
UavVideoAadServiceNameType = 0xF704
```

### API-202dd1fd1d8c · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadStreamIdType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L380)

```cpp
UavVideoAadStreamIdType = 0xF705
```

### API-e922ff422ae4 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadSessionEpochType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L381)

```cpp
UavVideoAadSessionEpochType = 0xF706
```

### API-cf5c327ab221 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadMappingVersionType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L382)

```cpp
UavVideoAadMappingVersionType = 0xF707
```

### API-cbf70f3d20b1 · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadKeyEpochType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L383)

```cpp
UavVideoAadKeyEpochType = 0xF708
```

### API-a370c8e38c1a · ndnsf::examples::uav::uav_stream_tlv::(anonymous)::UavVideoAadCursorType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L384)

```cpp
UavVideoAadCursorType = 0xF709
```

### API-aba62c84051d · ndnsf::examples::uav::UavVideoAad

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L388)

```cpp
struct UavVideoAad
```

### API-3ddc58605c2f · ndnsf::examples::uav::UavVideoAad::version

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L390)

```cpp
uint64_t version = 1;
```

### API-c2f217b24b51 · ndnsf::examples::uav::UavVideoAad::exactDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L391)

```cpp
ndn::Name exactDataName;
```

### API-517f0019a5d4 · ndnsf::examples::uav::UavVideoAad::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L392)

```cpp
ndn::Name providerIdentity;
```

### API-9be070abcfb3 · ndnsf::examples::uav::UavVideoAad::serviceName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L393)

```cpp
ndn::Name serviceName;
```

### API-e05ad44d13bc · ndnsf::examples::uav::UavVideoAad::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L394)

```cpp
std::string streamId;
```

### API-9a838cb4b9df · ndnsf::examples::uav::UavVideoAad::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L395)

```cpp
uint64_t sessionEpoch = 0;
```

### API-b61399218d20 · ndnsf::examples::uav::UavVideoAad::mappingVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L396)

```cpp
uint64_t mappingVersion = 0;
```

### API-c6f89e4c6a18 · ndnsf::examples::uav::UavVideoAad::keyEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L397)

```cpp
uint64_t keyEpoch = 0;
```

### API-8b4f8b17bd5d · ndnsf::examples::uav::UavVideoAad::cursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L398)

```cpp
ndn_service_framework::StreamCursor cursor = 0;
```

### API-e9b9686a91d0 · ndnsf::examples::uav::UavVideoAad::wireEncode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L400)

```cpp
ndn::Block wireEncode() const;
```

### API-e81e87212362 · ndnsf::examples::uav::UavVideoAad::wireDecodeStrict

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L401)

```cpp
static UavVideoAad wireDecodeStrict(const ndn::Block& block);
```

### API-a2e87c649a20 · ndnsf::examples::uav::UavVideoNonceUseGuard

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L404)

```cpp
class UavVideoNonceUseGuard
```

### API-145313f90856 · ndnsf::examples::uav::UavVideoNonceUseGuard::UavVideoNonceUseGuard

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L407)

```cpp
explicit UavVideoNonceUseGuard(const VideoStreamDescriptor& descriptor);
```

### API-3844b8ca3d34 · ndnsf::examples::uav::UavVideoNonceUseGuard::reserve

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L409)

```cpp
void reserve(const VideoStreamDescriptor& descriptor,
               const UavVideoDataName& binding);
```

### API-28964d07d167 · ndnsf::examples::uav::UavVideoNonceUseGuard::closeForUncertainUse

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L411)

```cpp
void closeForUncertainUse();
```

### API-3a50433e6ffd · ndnsf::examples::uav::UavVideoNonceUseGuard::isClosed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L412)

```cpp
bool isClosed() const;
```

### API-c572cc0a3058 · ndnsf::examples::uav::VideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L435)

```cpp
struct VideoPacket
```

### API-80534cbc6596 · ndnsf::examples::uav::VideoPacket::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L437)

```cpp
std::string streamId;
```

### API-28d80146484a · ndnsf::examples::uav::VideoPacket::streamSessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L438)

```cpp
uint64_t streamSessionEpoch = 0;
```

### API-838a73aef7ef · ndnsf::examples::uav::VideoPacket::second

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L439)

```cpp
uint64_t second = 0;
```

### API-f124fb9dc241 · ndnsf::examples::uav::VideoPacket::packetSeq

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L440)

```cpp
uint64_t packetSeq = 0;
```

### API-cf6a4db5338b · ndnsf::examples::uav::VideoPacket::mediaSequence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L441)

```cpp
uint64_t mediaSequence = 0;
```

### API-8af23496da3f · ndnsf::examples::uav::VideoPacket::frameSeq

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L442)

```cpp
uint64_t frameSeq = 0;
```

### API-25709f57e629 · ndnsf::examples::uav::VideoPacket::captureMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L443)

```cpp
uint64_t captureMs = 0;
```

### API-52cecda66167 · ndnsf::examples::uav::VideoPacket::frameBindingVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L444)

```cpp
uint64_t frameBindingVersion = 0;
```

### API-105c912e716e · ndnsf::examples::uav::VideoPacket::sourceFrameId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L445)

```cpp
uint64_t sourceFrameId = 0;
```

### API-b02299ad211e · ndnsf::examples::uav::VideoPacket::captureOriginNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L446)

```cpp
uint64_t captureOriginNs = 0;
```

### API-64cd3b713968 · ndnsf::examples::uav::VideoPacket::captureClockId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L447)

```cpp
std::string captureClockId;
```

### API-9eea4d555289 · ndnsf::examples::uav::VideoPacket::codecPts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L448)

```cpp
int64_t codecPts = 0;
```

### API-a379b23427fb · ndnsf::examples::uav::VideoPacket::codecTimeBaseNum

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L449)

```cpp
uint32_t codecTimeBaseNum = 0;
```

### API-6a15216e8d93 · ndnsf::examples::uav::VideoPacket::codecTimeBaseDen

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L450)

```cpp
uint32_t codecTimeBaseDen = 0;
```

### API-ee215d6f4329 · ndnsf::examples::uav::VideoPacket::codecConfigEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L451)

```cpp
uint64_t codecConfigEpoch = 0;
```

### API-d5f0553f836d · ndnsf::examples::uav::VideoPacket::frameFirstPacketSeq

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L452)

```cpp
uint64_t frameFirstPacketSeq = 0;
```

### API-61287c7006d0 · ndnsf::examples::uav::VideoPacket::frameLastPacketSeq

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L453)

```cpp
uint64_t frameLastPacketSeq = 0;
```

### API-a05cf166bfe6 · ndnsf::examples::uav::VideoPacket::bucketPacketCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L454)

```cpp
uint64_t bucketPacketCount = 0;
```

### API-d908a655990b · ndnsf::examples::uav::VideoPacket::frameSegmentIndex

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L455)

```cpp
uint32_t frameSegmentIndex = 0;
```

### API-786fc8f9c2c9 · ndnsf::examples::uav::VideoPacket::frameSegmentCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L456)

```cpp
uint32_t frameSegmentCount = 0;
```

### API-98faefa430f5 · ndnsf::examples::uav::VideoPacket::keyFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L457)

```cpp
bool keyFrame = false;
```

### API-b72a25f67d04 · ndnsf::examples::uav::VideoPacket::encoding

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L458)

```cpp
std::string encoding;
```

### API-7aa7a5bc3960 · ndnsf::examples::uav::VideoPacket::fecDataShards

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L459)

```cpp
uint32_t fecDataShards = 0;
```

### API-b4e8fde73dfc · ndnsf::examples::uav::VideoPacket::fecParityShards

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L460)

```cpp
uint32_t fecParityShards = 0;
```

### API-351d54c196ca · ndnsf::examples::uav::VideoPacket::fecSymbolIndex

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L461)

```cpp
uint32_t fecSymbolIndex = 0;
```

### API-486c911e4866 · ndnsf::examples::uav::VideoPacket::fecSymbolCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L462)

```cpp
uint32_t fecSymbolCount = 0;
```

### API-c48d794c54d4 · ndnsf::examples::uav::VideoPacket::fecDataLengths

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L463)

```cpp
std::string fecDataLengths;
```

### API-35e249981697 · ndnsf::examples::uav::VideoPacket::payload

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L464)

```cpp
std::vector<uint8_t> payload;
```

### API-919b6ee13e02 · ndnsf::examples::uav::hasExactVideoFrameBinding

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L467)

```cpp
bool
hasExactVideoFrameBinding(const VideoPacket& packet);
```

### API-6de87c08ecb4 · ndnsf::examples::uav::validateVideoFrameBinding

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L470)

```cpp
void
validateVideoFrameBinding(const VideoPacket& packet,
                          uint64_t expectedSessionEpoch);
```

### API-e0c6f3d1994f · ndnsf::examples::uav::TelemetryState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L474)

```cpp
struct TelemetryState
```

### API-1bcd6dd9d5d5 · ndnsf::examples::uav::TelemetryState::telemetryFreshness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L476)

```cpp
std::string telemetryFreshness = "unknown";
```

### API-c48f2d10fdc1 · ndnsf::examples::uav::TelemetryState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L477)

```cpp
std::string droneId = "unknown";
```

### API-d4b1a6d3cf48 · ndnsf::examples::uav::TelemetryState::lat

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L478)

```cpp
std::string lat = "unknown";
```

### API-0f8634ee2bfa · ndnsf::examples::uav::TelemetryState::lon

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L479)

```cpp
std::string lon = "unknown";
```

### API-0b550f31f74f · ndnsf::examples::uav::TelemetryState::altitudeM

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L480)

```cpp
std::string altitudeM = "unknown";
```

### API-5a90f983982d · ndnsf::examples::uav::TelemetryState::groundspeedMps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L481)

```cpp
std::string groundspeedMps = "unknown";
```

### API-bf55ee598a7a · ndnsf::examples::uav::TelemetryState::batteryPercent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L482)

```cpp
std::string batteryPercent = "unknown";
```

### API-11322f2ac917 · ndnsf::examples::uav::TelemetryState::heartbeatSeen

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L483)

```cpp
std::string heartbeatSeen = "false";
```

### API-7c71127337ca · ndnsf::examples::uav::TelemetryState::flightControllerReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L484)

```cpp
std::string flightControllerReady = "unknown";
```

### API-56761ca31bdc · ndnsf::examples::uav::TelemetryState::gpsReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L485)

```cpp
std::string gpsReady = "unknown";
```

### API-8e2a7b68faa4 · ndnsf::examples::uav::TelemetryState::ekfReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L486)

```cpp
std::string ekfReady = "unknown";
```

### API-1b8d4d4b0b63 · ndnsf::examples::uav::TelemetryState::batteryReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L487)

```cpp
std::string batteryReady = "unknown";
```

### API-235b2e02566b · ndnsf::examples::uav::TelemetryState::armed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L488)

```cpp
std::string armed = "unknown";
```

### API-bc76c932423b · ndnsf::examples::uav::TelemetryState::gpsFixType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L489)

```cpp
std::string gpsFixType = "unknown";
```

### API-ed6520e74e1e · ndnsf::examples::uav::TelemetryState::gpsFixName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L490)

```cpp
std::string gpsFixName = "unknown";
```

### API-1e9f86a53643 · ndnsf::examples::uav::TelemetryState::gpsSatellitesVisible

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L491)

```cpp
std::string gpsSatellitesVisible = "unknown";
```

### API-2ef54fcad1d1 · ndnsf::examples::uav::TelemetryState::flightControllerBackend

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L492)

```cpp
std::string flightControllerBackend = "unknown";
```

### API-5fa3e9491b7c · ndnsf::examples::uav::TelemetryState::flightControllerAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L493)

```cpp
std::string flightControllerAvailable = "unknown";
```

### API-4656c43693ae · ndnsf::examples::uav::TelemetryState::flightControllerState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L494)

```cpp
std::string flightControllerState = "unknown";
```

### API-61184f961075 · ndnsf::examples::uav::TelemetryState::flightControllerReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L495)

```cpp
std::string flightControllerReason = "unknown";
```

### API-c0e8748495f1 · ndnsf::examples::uav::TelemetryState::systemStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L496)

```cpp
std::string systemStatus = "unknown";
```

### API-45ebed56757e · ndnsf::examples::uav::TelemetryState::systemStatusName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L497)

```cpp
std::string systemStatusName = "unknown";
```

### API-d0b48de30948 · ndnsf::examples::uav::TelemetryState::landedState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L498)

```cpp
std::string landedState = "unknown";
```

### API-cd3a213dbb03 · ndnsf::examples::uav::TelemetryState::landedStateName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L499)

```cpp
std::string landedStateName = "unknown";
```

### API-743ed6182aa0 · ndnsf::examples::uav::TelemetryState::vtolStateName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L500)

```cpp
std::string vtolStateName = "unknown";
```

### API-70c8ea830d15 · ndnsf::examples::uav::TelemetryState::batteryVoltageV

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L501)

```cpp
std::string batteryVoltageV = "unknown";
```

### API-a1cfdc2c4cd0 · ndnsf::examples::uav::TelemetryState::batteryCurrentA

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L502)

```cpp
std::string batteryCurrentA = "unknown";
```

### API-da167a26c709 · ndnsf::examples::uav::TelemetryState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L503)

```cpp
std::string readiness = "not-ready";
```

### API-e5f613b018f3 · ndnsf::examples::uav::TelemetryState::readinessReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L504)

```cpp
std::string readinessReason = "waiting-heartbeat";
```

### API-2f87cdb27ba4 · ndnsf::examples::uav::TelemetryState::video

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L505)

```cpp
std::string video = "unknown";
```

### API-430dbcf58813 · ndnsf::examples::uav::TelemetryState::capture

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L506)

```cpp
std::string capture = "unknown";
```

### API-24506ff3d80d · ndnsf::examples::uav::TelemetryState::recording

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L507)

```cpp
std::string recording = "unknown";
```

### API-627dcfe4398c · ndnsf::examples::uav::TelemetryState::cameraAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L508)

```cpp
std::string cameraAvailable = "unknown";
```

### API-dd5bd6cee110 · ndnsf::examples::uav::TelemetryState::cameraSource

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L509)

```cpp
std::string cameraSource = "unknown";
```

### API-68c95072d3a9 · ndnsf::examples::uav::TelemetryState::cameraReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L510)

```cpp
std::string cameraReason = "unknown";
```

### API-0a05591214e0 · ndnsf::examples::uav::TelemetryState::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L511)

```cpp
std::string linkState = "unknown";
```

### API-abe176e5b5bd · ndnsf::examples::uav::TelemetryState::manualControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L512)

```cpp
std::string manualControlState = "idle";
```

### API-1c82ef71f545 · ndnsf::examples::uav::TelemetryState::manualReplayActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L513)

```cpp
std::string manualReplayActive = "false";
```

### API-feb621e0c69e · ndnsf::examples::uav::TelemetryState::manualNeutralSent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L514)

```cpp
std::string manualNeutralSent = "true";
```

### API-cb1c1d9aa03a · ndnsf::examples::uav::TelemetryState::manualFreshForMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L515)

```cpp
std::string manualFreshForMs = "0";
```

### API-a7d0898f3ff8 · ndnsf::examples::uav::TelemetryState::manualReplayCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L516)

```cpp
std::string manualReplayCount = "0";
```

### API-d78243d726f8 · ndnsf::examples::uav::TelemetryState::safetyDetail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L517)

```cpp
std::string safetyDetail = "idle";
```

### API-c3c0cf0d0924 · ndnsf::examples::uav::TelemetryState::timestampMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L518)

```cpp
uint64_t timestampMs = 0;
```

### API-b20e6ca4f22b · ndnsf::examples::uav::TelemetryState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L520)

```cpp
static TelemetryState fromFields(const Fields& fields);
```

### API-eee838dbfbd0 · ndnsf::examples::uav::TelemetryState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L521)

```cpp
Fields toFields() const;
```

### API-5ed241d6d264 · ndnsf::examples::uav::TelemetryState::telemetryFreshnessLabel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L522)

```cpp
std::string telemetryFreshnessLabel() const;
```

### API-171c20e4efdc · ndnsf::examples::uav::TelemetryState::telemetryIsFresh

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L523)

```cpp
bool telemetryIsFresh() const;
```

### API-d76c42ee75db · ndnsf::examples::uav::TelemetryState::telemetryIsStale

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L524)

```cpp
bool telemetryIsStale() const;
```

### API-0bd7940ce4ea · ndnsf::examples::uav::TelemetryState::telemetryIsMissing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L525)

```cpp
bool telemetryIsMissing() const;
```

### API-02caa62e03cd · ndnsf::examples::uav::TelemetryState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L526)

```cpp
std::string statusLine() const;
```

### API-f132fdf2a226 · ndnsf::examples::uav::TelemetryState::mapSummary

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L527)

```cpp
std::string mapSummary(const std::string& selectedDrone) const;
```

### API-7230dfee9323 · ndnsf::examples::uav::ReadinessState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L530)

```cpp
struct ReadinessState
```

### API-db71e1af0e0b · ndnsf::examples::uav::ReadinessState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L532)

```cpp
std::string droneId = "unknown";
```

### API-7ddb965eacdd · ndnsf::examples::uav::ReadinessState::heartbeatSeen

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L533)

```cpp
std::string heartbeatSeen = "false";
```

### API-22f29ad312a6 · ndnsf::examples::uav::ReadinessState::flightControllerReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L534)

```cpp
std::string flightControllerReady = "unknown";
```

### API-ada90b243ce4 · ndnsf::examples::uav::ReadinessState::gpsReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L535)

```cpp
std::string gpsReady = "unknown";
```

### API-eb03a7a4040f · ndnsf::examples::uav::ReadinessState::ekfReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L536)

```cpp
std::string ekfReady = "unknown";
```

### API-6c78bc15a9a6 · ndnsf::examples::uav::ReadinessState::batteryReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L537)

```cpp
std::string batteryReady = "unknown";
```

### API-a115000baeaa · ndnsf::examples::uav::ReadinessState::armed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L538)

```cpp
std::string armed = "unknown";
```

### API-3e5fa6554d1b · ndnsf::examples::uav::ReadinessState::mode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L539)

```cpp
std::string mode = "unknown";
```

### API-300b517ccf45 · ndnsf::examples::uav::ReadinessState::landedStateName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L540)

```cpp
std::string landedStateName = "unknown";
```

### API-b32503c7c266 · ndnsf::examples::uav::ReadinessState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L541)

```cpp
std::string readiness = "not-ready";
```

### API-90a616173518 · ndnsf::examples::uav::ReadinessState::readinessReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L542)

```cpp
std::string readinessReason = "waiting-heartbeat";
```

### API-282f4d0f1eef · ndnsf::examples::uav::ReadinessState::timestampMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L543)

```cpp
uint64_t timestampMs = 0;
```

### API-72dde6a660dd · ndnsf::examples::uav::ReadinessState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L545)

```cpp
static ReadinessState fromFields(const Fields& fields);
```

### API-16d6e8ebb635 · ndnsf::examples::uav::ReadinessState::fromTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L546)

```cpp
static ReadinessState fromTelemetry(const TelemetryState& telemetry);
```

### API-b5ad2214a3eb · ndnsf::examples::uav::ReadinessState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L547)

```cpp
Fields toFields() const;
```

### API-c1e18cfca484 · ndnsf::examples::uav::ReadinessState::readyForArm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L548)

```cpp
bool readyForArm() const;
```

### API-ca2adb5e0454 · ndnsf::examples::uav::ReadinessState::landedForTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L549)

```cpp
bool landedForTakeoff() const;
```

### API-44679fa2abcb · ndnsf::examples::uav::ReadinessState::readyForTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L550)

```cpp
bool readyForTakeoff() const;
```

### API-cb521f2411e6 · ndnsf::examples::uav::ReadinessState::readyForLand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L551)

```cpp
bool readyForLand() const;
```

### API-3de038edcfad · ndnsf::examples::uav::ReadinessState::readyForManualControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L552)

```cpp
bool readyForManualControl() const;
```

### API-3552bdb934b9 · ndnsf::examples::uav::ReadinessState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L553)

```cpp
std::string statusLine() const;
```

### API-2bae43f82a7f · ndnsf::examples::uav::FlightCommandState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L556)

```cpp
struct FlightCommandState
```

### API-c6f23ac324c9 · ndnsf::examples::uav::FlightCommandState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L558)

```cpp
std::string droneId = "unknown";
```

### API-dfe10654799a · ndnsf::examples::uav::FlightCommandState::command

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L559)

```cpp
std::string command = "none";
```

### API-54c0530ae900 · ndnsf::examples::uav::FlightCommandState::accepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L560)

```cpp
std::string accepted = "unknown";
```

### API-addab5d4df2b · ndnsf::examples::uav::FlightCommandState::ackResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L561)

```cpp
std::string ackResult = "unknown";
```

### API-070611ea3d76 · ndnsf::examples::uav::FlightCommandState::flightControllerState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L562)

```cpp
std::string flightControllerState = "unknown";
```

### API-5e00f5c1ab75 · ndnsf::examples::uav::FlightCommandState::altitudeM

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L563)

```cpp
std::string altitudeM = "unknown";
```

### API-4ebd42800574 · ndnsf::examples::uav::FlightCommandState::groundspeedMps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L564)

```cpp
std::string groundspeedMps = "unknown";
```

### API-7ba3294dc77d · ndnsf::examples::uav::FlightCommandState::batteryPercent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L565)

```cpp
std::string batteryPercent = "unknown";
```

### API-5b22b6ce55f9 · ndnsf::examples::uav::FlightCommandState::forwardedBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L566)

```cpp
std::string forwardedBytes = "0";
```

### API-7364df42c5f6 · ndnsf::examples::uav::FlightCommandState::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L567)

```cpp
std::string detail = "idle";
```

### API-a6f6cd049cba · ndnsf::examples::uav::FlightCommandState::rttMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L568)

```cpp
uint64_t rttMs = 0;
```

### API-9abb34add295 · ndnsf::examples::uav::FlightCommandState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L569)

```cpp
uint64_t updatedMs = 0;
```

### API-1b2cd259854f · ndnsf::examples::uav::FlightCommandState::timeoutMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L570)

```cpp
uint64_t timeoutMs = 0;
```

### API-16fb329b7118 · ndnsf::examples::uav::FlightCommandState::makePending

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L572)

```cpp
static FlightCommandState makePending(const std::string& droneId,
                                        const std::string& command,
                                        uint64_t attemptMs,
                                        uint64_t timeoutMs);
```

### API-1b3a226fa035 · ndnsf::examples::uav::FlightCommandState::makeTimeout

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L576)

```cpp
static FlightCommandState makeTimeout(const std::string& droneId,
                                        const std::string& command,
                                        uint64_t attemptMs,
                                        uint64_t terminalMs,
                                        uint64_t timeoutMs);
```

### API-4b62d31dfd8e · ndnsf::examples::uav::FlightCommandState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L581)

```cpp
static FlightCommandState fromFields(const Fields& fields);
```

### API-ab9964ea5ce6 · ndnsf::examples::uav::FlightCommandState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L582)

```cpp
Fields toFields() const;
```

### API-18edcdefe210 · ndnsf::examples::uav::FlightCommandState::isAccepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L583)

```cpp
bool isAccepted() const;
```

### API-c879e4a5d838 · ndnsf::examples::uav::FlightCommandState::isTimeout

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L584)

```cpp
bool isTimeout() const;
```

### API-1563bd969f9f · ndnsf::examples::uav::FlightCommandState::isSafetyCritical

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L585)

```cpp
bool isSafetyCritical() const;
```

### API-90374d785702 · ndnsf::examples::uav::FlightCommandState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L586)

```cpp
std::string statusLine() const;
```

### API-443fb2030ee9 · ndnsf::examples::uav::SafetyState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L589)

```cpp
struct SafetyState
```

### API-64eca024e762 · ndnsf::examples::uav::SafetyState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L591)

```cpp
std::string droneId = "unknown";
```

### API-26e0ba3e27df · ndnsf::examples::uav::SafetyState::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L592)

```cpp
std::string linkState = "unknown";
```

### API-abd3ba9e24dc · ndnsf::examples::uav::SafetyState::manualControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L593)

```cpp
std::string manualControlState = "idle";
```

### API-8112ca9d62c6 · ndnsf::examples::uav::SafetyState::manualReplayActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L594)

```cpp
std::string manualReplayActive = "false";
```

### API-af5775305366 · ndnsf::examples::uav::SafetyState::manualNeutralSent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L595)

```cpp
std::string manualNeutralSent = "true";
```

### API-cf34a64082bb · ndnsf::examples::uav::SafetyState::manualFreshForMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L596)

```cpp
uint64_t manualFreshForMs = 0;
```

### API-81230a83ca1f · ndnsf::examples::uav::SafetyState::manualReplayCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L597)

```cpp
uint64_t manualReplayCount = 0;
```

### API-44e8f35ad221 · ndnsf::examples::uav::SafetyState::linkAgeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L598)

```cpp
uint64_t linkAgeMs = 0;
```

### API-a80ca6b5c000 · ndnsf::examples::uav::SafetyState::lostLinkAction

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L599)

```cpp
std::string lostLinkAction = "notify";
```

### API-06c67441aae8 · ndnsf::examples::uav::SafetyState::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L600)

```cpp
std::string detail = "idle";
```

### API-f0e365dfe1f1 · ndnsf::examples::uav::SafetyState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L601)

```cpp
uint64_t updatedMs = 0;
```

### API-b428a92df27a · ndnsf::examples::uav::SafetyState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L603)

```cpp
static SafetyState fromFields(const Fields& fields);
```

### API-204e39879c39 · ndnsf::examples::uav::SafetyState::fromTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L604)

```cpp
static SafetyState fromTelemetry(const TelemetryState& telemetry);
```

### API-cfc4b097f0a2 · ndnsf::examples::uav::SafetyState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L605)

```cpp
Fields toFields() const;
```

### API-c5aa39af85ac · ndnsf::examples::uav::SafetyState::manualControlFresh

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L606)

```cpp
bool manualControlFresh() const;
```

### API-df4cf5aad119 · ndnsf::examples::uav::SafetyState::needsOperatorAttention

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L607)

```cpp
bool needsOperatorAttention() const;
```

### API-07ab7ba6439e · ndnsf::examples::uav::SafetyState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L608)

```cpp
std::string statusLine() const;
```

### API-1a3751fa5d54 · ndnsf::examples::uav::FlightSafetyGateState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L611)

```cpp
struct FlightSafetyGateState
```

### API-d39ae10cfcc0 · ndnsf::examples::uav::FlightSafetyGateState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L613)

```cpp
std::string droneId = "unknown";
```

### API-f69eb9f68d1c · ndnsf::examples::uav::FlightSafetyGateState::hasReadiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L614)

```cpp
bool hasReadiness = false;
```

### API-af975efe6fdf · ndnsf::examples::uav::FlightSafetyGateState::hasSafety

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L615)

```cpp
bool hasSafety = false;
```

### API-68db9e91af4d · ndnsf::examples::uav::FlightSafetyGateState::operatorAttention

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L616)

```cpp
bool operatorAttention = false;
```

### API-e1084f639caf · ndnsf::examples::uav::FlightSafetyGateState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L617)

```cpp
std::string readiness = "unknown";
```

### API-2fa54012d5f9 · ndnsf::examples::uav::FlightSafetyGateState::readinessReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L618)

```cpp
std::string readinessReason = "no-telemetry";
```

### API-4f59b90aa6db · ndnsf::examples::uav::FlightSafetyGateState::armed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L619)

```cpp
std::string armed = "unknown";
```

### API-b9274dfd826d · ndnsf::examples::uav::FlightSafetyGateState::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L620)

```cpp
std::string linkState = "unknown";
```

### API-7807fe47e255 · ndnsf::examples::uav::FlightSafetyGateState::manualControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L621)

```cpp
std::string manualControlState = "unknown";
```

### API-32a327be6aa8 · ndnsf::examples::uav::FlightSafetyGateState::canArm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L622)

```cpp
bool canArm = false;
```

### API-8cbcd9762e1b · ndnsf::examples::uav::FlightSafetyGateState::canTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L623)

```cpp
bool canTakeoff = false;
```

### API-af11cf7abe58 · ndnsf::examples::uav::FlightSafetyGateState::canLand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L624)

```cpp
bool canLand = false;
```

### API-accba6fad419 · ndnsf::examples::uav::FlightSafetyGateState::canManualControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L625)

```cpp
bool canManualControl = false;
```

### API-3cc0babb656a · ndnsf::examples::uav::FlightSafetyGateState::canControlPanel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L626)

```cpp
bool canControlPanel = false;
```

### API-fe8a253eefad · ndnsf::examples::uav::FlightSafetyGateState::canEmergencyStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L627)

```cpp
bool canEmergencyStop = false;
```

### API-fd58006b8fb2 · ndnsf::examples::uav::FlightSafetyGateState::armReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L628)

```cpp
std::string armReason = "no-telemetry";
```

### API-36fd646a0638 · ndnsf::examples::uav::FlightSafetyGateState::takeoffReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L629)

```cpp
std::string takeoffReason = "no-telemetry";
```

### API-8904c021b819 · ndnsf::examples::uav::FlightSafetyGateState::landReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L630)

```cpp
std::string landReason = "no-telemetry";
```

### API-a8f3d6c9b546 · ndnsf::examples::uav::FlightSafetyGateState::manualControlReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L631)

```cpp
std::string manualControlReason = "no-telemetry";
```

### API-85b160209166 · ndnsf::examples::uav::FlightSafetyGateState::controlPanelReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L632)

```cpp
std::string controlPanelReason = "no-telemetry";
```

### API-cd5b53fb2a8d · ndnsf::examples::uav::FlightSafetyGateState::emergencyStopReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L633)

```cpp
std::string emergencyStopReason = "ok";
```

### API-23d65b4625c5 · ndnsf::examples::uav::FlightSafetyGateState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L635)

```cpp
static FlightSafetyGateState fromStates(const std::string& droneId,
                                          const std::optional<ReadinessState>& readiness,
                                          const std::optional<SafetyState>& safety);
```

### API-287b057d685c · ndnsf::examples::uav::FlightSafetyGateState::actionAllowed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L638)

```cpp
bool actionAllowed(const std::string& action, std::string& reason) const;
```

### API-e51ce04af7b0 · ndnsf::examples::uav::FlightSafetyGateState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L639)

```cpp
std::string statusLine() const;
```

### API-e64c7daf0f22 · ndnsf::examples::uav::FlightActionControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L642)

```cpp
struct FlightActionControlState
```

### API-e726f4f7fc5b · ndnsf::examples::uav::FlightActionControlState::selectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L644)

```cpp
std::string selectedDrone = "unknown";
```

### API-dad260e523d5 · ndnsf::examples::uav::FlightActionControlState::hasReadiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L645)

```cpp
bool hasReadiness = false;
```

### API-a7fa15a8a757 · ndnsf::examples::uav::FlightActionControlState::hasSafety

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L646)

```cpp
bool hasSafety = false;
```

### API-4b4997fdb418 · ndnsf::examples::uav::FlightActionControlState::operatorAttention

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L647)

```cpp
bool operatorAttention = false;
```

### API-3960688627a9 · ndnsf::examples::uav::FlightActionControlState::canArm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L648)

```cpp
bool canArm = false;
```

### API-fd999df7b5e3 · ndnsf::examples::uav::FlightActionControlState::canTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L649)

```cpp
bool canTakeoff = false;
```

### API-7605372ccba2 · ndnsf::examples::uav::FlightActionControlState::canLand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L650)

```cpp
bool canLand = false;
```

### API-3474c1903ee0 · ndnsf::examples::uav::FlightActionControlState::canManualControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L651)

```cpp
bool canManualControl = false;
```

### API-88374fb235f0 · ndnsf::examples::uav::FlightActionControlState::canControlPanel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L652)

```cpp
bool canControlPanel = false;
```

### API-13493ad73098 · ndnsf::examples::uav::FlightActionControlState::canEmergencyStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L653)

```cpp
bool canEmergencyStop = false;
```

### API-582b37a6d03e · ndnsf::examples::uav::FlightActionControlState::armReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L654)

```cpp
std::string armReason = "unknown";
```

### API-1fe9680c2c5e · ndnsf::examples::uav::FlightActionControlState::takeoffReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L655)

```cpp
std::string takeoffReason = "unknown";
```

### API-01a7c0af2d61 · ndnsf::examples::uav::FlightActionControlState::landReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L656)

```cpp
std::string landReason = "unknown";
```

### API-04c6f904bc7d · ndnsf::examples::uav::FlightActionControlState::manualControlReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L657)

```cpp
std::string manualControlReason = "unknown";
```

### API-6c3be0496bfe · ndnsf::examples::uav::FlightActionControlState::controlPanelReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L658)

```cpp
std::string controlPanelReason = "unknown";
```

### API-694582e198da · ndnsf::examples::uav::FlightActionControlState::emergencyStopReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L659)

```cpp
std::string emergencyStopReason = "unknown";
```

### API-cd9bd89ac36c · ndnsf::examples::uav::FlightActionControlState::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L660)

```cpp
std::string linkState = "unknown";
```

### API-909051a5dc6c · ndnsf::examples::uav::FlightActionControlState::manualControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L661)

```cpp
std::string manualControlState = "unknown";
```

### API-9605c20ce266 · ndnsf::examples::uav::FlightActionControlState::fromGate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L663)

```cpp
static FlightActionControlState fromGate(const FlightSafetyGateState& gate);
```

### API-196e55d8d337 · ndnsf::examples::uav::FlightActionControlState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L664)

```cpp
std::string statusLine() const;
```

### API-08dae9073fad · ndnsf::examples::uav::AutoControlSequenceStep

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L667)

```cpp
struct AutoControlSequenceStep
```

### API-5a9ffc15e3a7 · ndnsf::examples::uav::AutoControlSequenceStep::command

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L669)

```cpp
std::string command = "none";
```

### API-986d12ff4ab5 · ndnsf::examples::uav::AutoControlSequenceStep::prerequisite

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L670)

```cpp
std::string prerequisite = "none";
```

### API-1c53aa52bf4c · ndnsf::examples::uav::AutoControlSequenceStep::phase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L671)

```cpp
std::string phase = "idle";
```

### API-8a8a822160f0 · ndnsf::examples::uav::AutoControlSequenceStep::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L672)

```cpp
std::string reason = "pending";
```

### API-57d71b678e3b · ndnsf::examples::uav::AutoControlSequenceStep::startedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L673)

```cpp
uint64_t startedMs = 0;
```

### API-f79cc2303124 · ndnsf::examples::uav::AutoControlSequenceStep::finishedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L674)

```cpp
uint64_t finishedMs = 0;
```

### API-68e05b5e88f9 · ndnsf::examples::uav::AutoControlSequenceStep::dispatchCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L675)

```cpp
uint64_t dispatchCount = 0;
```

### API-4d0355ec5af2 · ndnsf::examples::uav::AutoControlSequenceStep::dispatched

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L676)

```cpp
bool dispatched = false;
```

### API-b587ecf3a674 · ndnsf::examples::uav::AutoControlSequenceStep::terminal

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L677)

```cpp
bool terminal = false;
```

### API-ba84b4807ea4 · ndnsf::examples::uav::AutoControlSequenceStep::beginWait

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L679)

```cpp
bool beginWait(std::string commandName, std::string prerequisiteName, uint64_t nowMs);
```

### API-b24b89f492c7 · ndnsf::examples::uav::AutoControlSequenceStep::satisfy

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L680)

```cpp
bool satisfy(std::string observedReason, uint64_t nowMs);
```

### API-19a6e9240f97 · ndnsf::examples::uav::AutoControlSequenceStep::expire

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L681)

```cpp
bool expire(std::string expiryReason, uint64_t nowMs);
```

### API-cb901f1a6ba2 · ndnsf::examples::uav::AutoControlSequenceStep::markDispatched

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L682)

```cpp
bool markDispatched(uint64_t nowMs);
```

### API-2e5e843bf3a9 · ndnsf::examples::uav::AutoControlSequenceStep::terminate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L683)

```cpp
bool terminate(std::string terminalReason, uint64_t nowMs);
```

### API-975377e5c943 · ndnsf::examples::uav::AutoControlSequenceStep::isTerminal

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L684)

```cpp
bool isTerminal() const;
```

### API-72d3098c241d · ndnsf::examples::uav::AutoControlSequenceStep::elapsedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L685)

```cpp
uint64_t elapsedMs(uint64_t nowMs) const;
```

### API-f0b6d8f4f5f2 · ndnsf::examples::uav::VideoState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L688)

```cpp
struct VideoState
```

### API-6e7a7d39a2e7 · ndnsf::examples::uav::VideoState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L690)

```cpp
std::string droneId = "unknown";
```

### API-3b883bf7a219 · ndnsf::examples::uav::VideoState::status

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L691)

```cpp
std::string status = "unknown";
```

### API-97a9d262add0 · ndnsf::examples::uav::VideoState::capture

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L692)

```cpp
std::string capture = "unknown";
```

### API-602ccbafed1c · ndnsf::examples::uav::VideoState::recording

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L693)

```cpp
std::string recording = "unknown";
```

### API-73d4c1b71b3c · ndnsf::examples::uav::VideoState::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L694)

```cpp
std::string streamId = "unknown";
```

### API-b728f975164a · ndnsf::examples::uav::VideoState::encoding

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L695)

```cpp
std::string encoding = "unknown";
```

### API-716003f1fea4 · ndnsf::examples::uav::VideoState::source

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L696)

```cpp
std::string source = "unknown";
```

### API-799def74eef3 · ndnsf::examples::uav::VideoState::cameraAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L697)

```cpp
std::string cameraAvailable = "unknown";
```

### API-906e4fc55f27 · ndnsf::examples::uav::VideoState::cameraReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L698)

```cpp
std::string cameraReason = "unknown";
```

### API-d71d14992a02 · ndnsf::examples::uav::VideoState::requestedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L699)

```cpp
uint64_t requestedBitrateKbps = 0;
```

### API-e30a0138c121 · ndnsf::examples::uav::VideoState::acceptedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L700)

```cpp
uint64_t acceptedBitrateKbps = 0;
```

### API-375ca0e3f3fd · ndnsf::examples::uav::VideoState::requestedFrameWidth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L701)

```cpp
uint64_t requestedFrameWidth = 0;
```

### API-010ea1324385 · ndnsf::examples::uav::VideoState::acceptedFrameWidth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L702)

```cpp
uint64_t acceptedFrameWidth = 0;
```

### API-4509bdc3ddd2 · ndnsf::examples::uav::VideoState::fps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L703)

```cpp
uint64_t fps = 0;
```

### API-d939ba291e4b · ndnsf::examples::uav::VideoState::streamPacketsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L704)

```cpp
uint64_t streamPacketsPublished = 0;
```

### API-c803c1f7638a · ndnsf::examples::uav::VideoState::framesPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L705)

```cpp
uint64_t framesPublished = 0;
```

### API-2f87d1abf16d · ndnsf::examples::uav::VideoState::fecGroupsPublished

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L706)

```cpp
uint64_t fecGroupsPublished = 0;
```

### API-187ffdd2591d · ndnsf::examples::uav::VideoState::recordingChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L707)

```cpp
uint64_t recordingChunks = 0;
```

### API-afde82e0b3fb · ndnsf::examples::uav::VideoState::recordingBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L708)

```cpp
uint64_t recordingBytes = 0;
```

### API-5fbb9b813aa3 · ndnsf::examples::uav::VideoState::rttMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L709)

```cpp
uint64_t rttMs = 0;
```

### API-b8f644666018 · ndnsf::examples::uav::VideoState::timeoutPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L710)

```cpp
uint64_t timeoutPressure = 0;
```

### API-f9b32a385d5a · ndnsf::examples::uav::VideoState::probePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L711)

```cpp
uint64_t probePressure = 0;
```

### API-7b7bbb2cdbcb · ndnsf::examples::uav::VideoState::backlogPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L712)

```cpp
uint64_t backlogPressure = 0;
```

### API-dd300307c5f4 · ndnsf::examples::uav::VideoState::decodedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L713)

```cpp
uint64_t decodedFrames = 0;
```

### API-db6ad3ddc0f0 · ndnsf::examples::uav::VideoState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L714)

```cpp
uint64_t updatedMs = 0;
```

### API-e021963a5415 · ndnsf::examples::uav::VideoState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L716)

```cpp
static VideoState fromFields(const Fields& fields);
```

### API-b5db2d50f774 · ndnsf::examples::uav::VideoState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L717)

```cpp
Fields toFields() const;
```

### API-4938534bcdc9 · ndnsf::examples::uav::VideoState::isStreaming

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L718)

```cpp
bool isStreaming() const;
```

### API-7fe6da6e9c7a · ndnsf::examples::uav::VideoState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L719)

```cpp
std::string statusLine() const;
```

### API-8d751f86cdc8 · ndnsf::examples::uav::VideoControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L722)

```cpp
struct VideoControlState
```

### API-cc496ddd97fd · ndnsf::examples::uav::VideoControlState::selectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L724)

```cpp
std::string selectedDrone = "unknown";
```

### API-90964227a9e9 · ndnsf::examples::uav::VideoControlState::remoteStreaming

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L725)

```cpp
bool remoteStreaming = false;
```

### API-d90255166a5b · ndnsf::examples::uav::VideoControlState::displayActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L726)

```cpp
bool displayActive = false;
```

### API-e204af95521f · ndnsf::examples::uav::VideoControlState::canStart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L727)

```cpp
bool canStart = true;
```

### API-a0d626227728 · ndnsf::examples::uav::VideoControlState::canStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L728)

```cpp
bool canStop = false;
```

### API-1915034cf18d · ndnsf::examples::uav::VideoControlState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L730)

```cpp
static VideoControlState fromStates(const std::string& selectedDrone,
                                      const std::optional<VideoState>& video,
                                      bool displayActive);
```

### API-ae31d374e40e · ndnsf::examples::uav::VideoControlState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L733)

```cpp
std::string statusLine() const;
```

### API-dcd2699b2575 · ndnsf::examples::uav::VideoAdaptiveState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L736)

```cpp
struct VideoAdaptiveState
```

### API-e10acdc09eb0 · ndnsf::examples::uav::VideoAdaptiveState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L738)

```cpp
std::string droneId = "unknown";
```

### API-60e00df041aa · ndnsf::examples::uav::VideoAdaptiveState::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L739)

```cpp
std::string state = "idle";
```

### API-f2c084500319 · ndnsf::examples::uav::VideoAdaptiveState::rttMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L740)

```cpp
uint64_t rttMs = 0;
```

### API-ebbef8d5c553 · ndnsf::examples::uav::VideoAdaptiveState::requestedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L741)

```cpp
uint64_t requestedBitrateKbps = 0;
```

### API-c3381fe68ffa · ndnsf::examples::uav::VideoAdaptiveState::acceptedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L742)

```cpp
uint64_t acceptedBitrateKbps = 0;
```

### API-e1f188536a75 · ndnsf::examples::uav::VideoAdaptiveState::suggestedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L743)

```cpp
uint64_t suggestedBitrateKbps = 0;
```

### API-9b30d31264dc · ndnsf::examples::uav::VideoAdaptiveState::bitrateAction

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L744)

```cpp
std::string bitrateAction = "hold";
```

### API-9d1565267c46 · ndnsf::examples::uav::VideoAdaptiveState::bitrateReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L745)

```cpp
std::string bitrateReason = "unknown";
```

### API-6ccd6839a4f3 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchDecisionAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L746)

```cpp
bool coreFetchDecisionAvailable = false;
```

### API-f79d86f8adc8 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchDecisionSource

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L747)

```cpp
std::string coreFetchDecisionSource = "unavailable";
```

### API-5dbacdc188d9 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchDecisionGeneration

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L748)

```cpp
uint64_t coreFetchDecisionGeneration = 0;
```

### API-5221dce52af4 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchDecisionObservedAtMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L749)

```cpp
uint64_t coreFetchDecisionObservedAtMs = 0;
```

### API-0c3e9158381d · ndnsf::examples::uav::VideoAdaptiveState::coreFetchPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L750)

```cpp
std::string coreFetchPhase = "INACTIVE";
```

### API-d5386faf761b · ndnsf::examples::uav::VideoAdaptiveState::coreFetchPolicyMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L751)

```cpp
std::string coreFetchPolicyMode = "none";
```

### API-310336a39c58 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchCapacityReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L752)

```cpp
std::string coreFetchCapacityReason = "unavailable";
```

### API-7c3a46c61dc9 · ndnsf::examples::uav::VideoAdaptiveState::coreFetchReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L753)

```cpp
std::string coreFetchReason = "unavailable";
```

### API-fa0fac706d8c · ndnsf::examples::uav::VideoAdaptiveState::window

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L754)

```cpp
uint64_t window = 0;
```

### API-a5b9659f3a5e · ndnsf::examples::uav::VideoAdaptiveState::lookahead

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L755)

```cpp
uint64_t lookahead = 0;
```

### API-0340d880663a · ndnsf::examples::uav::VideoAdaptiveState::futureProbeLimit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L756)

```cpp
uint64_t futureProbeLimit = 0;
```

### API-565a14e8bee3 · ndnsf::examples::uav::VideoAdaptiveState::futureProbeLimitSource

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L757)

```cpp
std::string futureProbeLimitSource = "uav-app-policy";
```

### API-76b552a00a0e · ndnsf::examples::uav::VideoAdaptiveState::interestLifetimeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L758)

```cpp
uint64_t interestLifetimeMs = 0;
```

### API-0aefe4cd3355 · ndnsf::examples::uav::VideoAdaptiveState::missingTimeoutMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L759)

```cpp
uint64_t missingTimeoutMs = 0;
```

### API-2353d0a10d9d · ndnsf::examples::uav::VideoAdaptiveState::timeoutPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L760)

```cpp
uint64_t timeoutPressure = 0;
```

### API-bbdce547048b · ndnsf::examples::uav::VideoAdaptiveState::probePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L761)

```cpp
uint64_t probePressure = 0;
```

### API-2165ad508621 · ndnsf::examples::uav::VideoAdaptiveState::duplicatePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L762)

```cpp
uint64_t duplicatePressure = 0;
```

### API-1c08d7bd1c8a · ndnsf::examples::uav::VideoAdaptiveState::lossPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L763)

```cpp
uint64_t lossPressure = 0;
```

### API-e23ceb7220ce · ndnsf::examples::uav::VideoAdaptiveState::backlogPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L764)

```cpp
uint64_t backlogPressure = 0;
```

### API-9c050afac6f3 · ndnsf::examples::uav::VideoAdaptiveState::primaryPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L765)

```cpp
std::string primaryPressure = "none";
```

### API-f0cb82847140 · ndnsf::examples::uav::VideoAdaptiveState::policyReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L766)

```cpp
std::string policyReason = "stable";
```

### API-6ae738002999 · ndnsf::examples::uav::VideoAdaptiveState::pendingChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L767)

```cpp
uint64_t pendingChunks = 0;
```

### API-920d8b4a69da · ndnsf::examples::uav::VideoAdaptiveState::maxReorderDepth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L768)

```cpp
uint64_t maxReorderDepth = 0;
```

### API-4afd665d718d · ndnsf::examples::uav::VideoAdaptiveState::pendingBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L769)

```cpp
uint64_t pendingBytes = 0;
```

### API-a336f888b0dd · ndnsf::examples::uav::VideoAdaptiveState::receivedChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L770)

```cpp
uint64_t receivedChunks = 0;
```

### API-bed72b66b651 · ndnsf::examples::uav::VideoAdaptiveState::fecRecoveredChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L771)

```cpp
uint64_t fecRecoveredChunks = 0;
```

### API-a1c87b0ec016 · ndnsf::examples::uav::VideoAdaptiveState::timeouts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L772)

```cpp
uint64_t timeouts = 0;
```

### API-ca2c15515a07 · ndnsf::examples::uav::VideoAdaptiveState::nacks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L773)

```cpp
uint64_t nacks = 0;
```

### API-2a5ba488368b · ndnsf::examples::uav::VideoAdaptiveState::duplicates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L774)

```cpp
uint64_t duplicates = 0;
```

### API-434d56d024c9 · ndnsf::examples::uav::VideoAdaptiveState::publishedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L775)

```cpp
uint64_t publishedFrames = 0;
```

### API-c5ba0599d2a6 · ndnsf::examples::uav::VideoAdaptiveState::decodedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L776)

```cpp
uint64_t decodedFrames = 0;
```

### API-210b4ab3f242 · ndnsf::examples::uav::VideoAdaptiveState::decodedFrameGap

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L777)

```cpp
uint64_t decodedFrameGap = 0;
```

### API-29024b262ca6 · ndnsf::examples::uav::VideoAdaptiveState::frameGapPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L778)

```cpp
uint64_t frameGapPressure = 0;
```

### API-a96833eed7ad · ndnsf::examples::uav::VideoAdaptiveState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L779)

```cpp
uint64_t updatedMs = 0;
```

### API-0c79e63e1e3b · ndnsf::examples::uav::VideoAdaptiveState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L781)

```cpp
static VideoAdaptiveState fromFields(const Fields& fields);
```

### API-16586d215a55 · ndnsf::examples::uav::VideoAdaptiveState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L782)

```cpp
Fields toFields() const;
```

### API-090c95537279 · ndnsf::examples::uav::VideoAdaptiveState::underPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L783)

```cpp
bool underPressure() const;
```

### API-643aa3114cbf · ndnsf::examples::uav::VideoAdaptiveState::maxPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L784)

```cpp
uint64_t maxPressure() const;
```

### API-c1f9b8b52536 · ndnsf::examples::uav::VideoAdaptiveState::toStreamHealth

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L785)

```cpp
ndn_service_framework::StreamHealth toStreamHealth(uint64_t streamSessionEpoch = 0,
                                                     const ndn::Name& streamPrefix = ndn::Name(),
                                                     uint64_t staleAfterMs = 3000,
                                                     uint64_t nowMs = 0) const;
```

### API-e7c05b6b2cb5 · ndnsf::examples::uav::VideoAdaptiveState::streamHealthSummary

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L789)

```cpp
std::string streamHealthSummary(uint64_t streamSessionEpoch = 0,
                                  const ndn::Name& streamPrefix = ndn::Name(),
                                  uint64_t staleAfterMs = 3000,
                                  uint64_t nowMs = 0) const;
```

### API-51df6e3546d3 · ndnsf::examples::uav::VideoAdaptiveState::compactSummary

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L793)

```cpp
std::string compactSummary() const;
```

### API-2024cddb95cd · ndnsf::examples::uav::VideoAdaptiveState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L794)

```cpp
std::string statusLine() const;
```

### API-14a60d1f6bea · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L803)

```cpp
struct VideoCoreFetchDecisionSnapshot
```

### API-1cc0a0fec7a6 · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::generation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L805)

```cpp
uint64_t generation = 0;
```

### API-05488a129e9f · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::observedAtMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L806)

```cpp
uint64_t observedAtMs = 0;
```

### API-967f2dc22488 · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::decision

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L807)

```cpp
std::optional<ndn_service_framework::StreamFetchDecision> decision;
```

### API-d7516998b268 · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::reset

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L809)

```cpp
void reset(uint64_t newGeneration);
```

### API-0dac8a106595 · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::observe

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L810)

```cpp
bool observe(uint64_t activeGeneration,
               uint64_t callbackGeneration,
               const ndn_service_framework::LiveStreamStatus& status,
               uint64_t nowMs);
```

### API-db06fe929d78 · ndnsf::examples::uav::VideoCoreFetchDecisionSnapshot::applyTo

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L814)

```cpp
void applyTo(VideoAdaptiveState& state) const;
```

### API-49dd3a20b4d9 · ndnsf::examples::uav::VideoAdaptivePolicyInput

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L817)

```cpp
struct VideoAdaptivePolicyInput
```

### API-40a894db5626 · ndnsf::examples::uav::VideoAdaptivePolicyInput::rttMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L819)

```cpp
uint64_t rttMs = 120;
```

### API-8db5a7b8e2f9 · ndnsf::examples::uav::VideoAdaptivePolicyInput::fps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L820)

```cpp
uint64_t fps = 30;
```

### API-258c1b8c1e05 · ndnsf::examples::uav::VideoAdaptivePolicyInput::deltaPacketsPerSecond

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L821)

```cpp
uint64_t deltaPacketsPerSecond = 160;
```

### API-fd24519ca6c1 · ndnsf::examples::uav::VideoAdaptivePolicyInput::timeoutBudgetMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L822)

```cpp
uint64_t timeoutBudgetMs = 2500;
```

### API-905a35343e31 · ndnsf::examples::uav::VideoAdaptivePolicyInput::dynamicWindowMax

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L823)

```cpp
uint64_t dynamicWindowMax = 128;
```

### API-864a6eec20ef · ndnsf::examples::uav::VideoAdaptivePolicyInput::dynamicLookaheadMax

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L824)

```cpp
uint64_t dynamicLookaheadMax = 64;
```

### API-de055209d717 · ndnsf::examples::uav::VideoAdaptivePolicyInput::decoderBacklogLimit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L825)

```cpp
uint64_t decoderBacklogLimit = 48;
```

### API-40e5ddc13027 · ndnsf::examples::uav::VideoAdaptivePolicyInput::decoderPendingChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L826)

```cpp
uint64_t decoderPendingChunks = 0;
```

### API-b5d79d34c6e9 · ndnsf::examples::uav::VideoAdaptivePolicyInput::receivedChunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L827)

```cpp
uint64_t receivedChunks = 0;
```

### API-f4c23c69073d · ndnsf::examples::uav::VideoAdaptivePolicyInput::timeouts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L828)

```cpp
uint64_t timeouts = 0;
```

### API-c433a9bd4cf6 · ndnsf::examples::uav::VideoAdaptivePolicyInput::nacks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L829)

```cpp
uint64_t nacks = 0;
```

### API-db3702c82431 · ndnsf::examples::uav::VideoAdaptivePolicyInput::timeoutPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L830)

```cpp
uint64_t timeoutPressure = 0;
```

### API-55b763de3447 · ndnsf::examples::uav::VideoAdaptivePolicyInput::probePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L831)

```cpp
uint64_t probePressure = 0;
```

### API-49fc19b12899 · ndnsf::examples::uav::VideoAdaptivePolicyInput::duplicatePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L832)

```cpp
uint64_t duplicatePressure = 0;
```

### API-4e4ea894ef6d · ndnsf::examples::uav::VideoAdaptivePolicyInput::publishedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L833)

```cpp
uint64_t publishedFrames = 0;
```

### API-0c8d64b45af1 · ndnsf::examples::uav::VideoAdaptivePolicyInput::decodedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L834)

```cpp
uint64_t decodedFrames = 0;
```

### API-99536090a5f4 · ndnsf::examples::uav::VideoAdaptivePolicyInput::requestedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L835)

```cpp
uint64_t requestedBitrateKbps = 8000;
```

### API-163cac9a9c20 · ndnsf::examples::uav::VideoAdaptivePolicyInput::acceptedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L836)

```cpp
uint64_t acceptedBitrateKbps = 8000;
```

### API-a93835df0326 · ndnsf::examples::uav::VideoAdaptivePolicyDecision

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L839)

```cpp
struct VideoAdaptivePolicyDecision
```

### API-5bfb73b44e73 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::window

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L841)

```cpp
uint64_t window = 0;
```

### API-e110529b52b1 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::lookahead

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L842)

```cpp
uint64_t lookahead = 0;
```

### API-eb7a489e3b80 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::futureProbeLimit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L843)

```cpp
uint64_t futureProbeLimit = 0;
```

### API-701498db201c · ndnsf::examples::uav::VideoAdaptivePolicyDecision::probeBackoffMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L844)

```cpp
uint64_t probeBackoffMs = 0;
```

### API-49b6fd31fad5 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::interestLifetimeMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L845)

```cpp
uint64_t interestLifetimeMs = 0;
```

### API-8152170be222 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::missingTimeoutMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L846)

```cpp
uint64_t missingTimeoutMs = 0;
```

### API-76e7c3176027 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::lossPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L847)

```cpp
uint64_t lossPressure = 0;
```

### API-a7e4be9953f6 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::congestionPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L848)

```cpp
uint64_t congestionPressure = 0;
```

### API-ba6545a8321a · ndnsf::examples::uav::VideoAdaptivePolicyDecision::probePressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L849)

```cpp
uint64_t probePressure = 0;
```

### API-77fe9994473e · ndnsf::examples::uav::VideoAdaptivePolicyDecision::backlogPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L850)

```cpp
uint64_t backlogPressure = 0;
```

### API-80f3d7f231c2 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::frameGapPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L851)

```cpp
uint64_t frameGapPressure = 0;
```

### API-e9f634987dab · ndnsf::examples::uav::VideoAdaptivePolicyDecision::primaryPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L852)

```cpp
std::string primaryPressure = "none";
```

### API-71f90a55b272 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::policyReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L853)

```cpp
std::string policyReason = "stable";
```

### API-b2e505373c11 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::suggestedBitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L854)

```cpp
uint64_t suggestedBitrateKbps = 0;
```

### API-1f746945d59f · ndnsf::examples::uav::VideoAdaptivePolicyDecision::bitrateAction

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L855)

```cpp
std::string bitrateAction = "hold";
```

### API-d458d0614c91 · ndnsf::examples::uav::VideoAdaptivePolicyDecision::bitrateReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L856)

```cpp
std::string bitrateReason = "stable";
```

### API-5ae2e6b64488 · ndnsf::examples::uav::computeVideoAdaptivePolicy

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L859)

```cpp
VideoAdaptivePolicyDecision
computeVideoAdaptivePolicy(const VideoAdaptivePolicyInput& input);
```

### API-36b0465d3184 · ndnsf::examples::uav::RecordingDataProductState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L862)

```cpp
struct RecordingDataProductState
```

### API-fc7cbdccb95b · ndnsf::examples::uav::RecordingDataProductState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L864)

```cpp
std::string droneId = "unknown";
```

### API-5113459eb9d7 · ndnsf::examples::uav::RecordingDataProductState::productType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L865)

```cpp
std::string productType = "camera-recording";
```

### API-aac3534ab6c7 · ndnsf::examples::uav::RecordingDataProductState::sessionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L866)

```cpp
std::string sessionId;
```

### API-e52c5cbd6b9d · ndnsf::examples::uav::RecordingDataProductState::objectPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L867)

```cpp
std::string objectPrefix;
```

### API-2687a7eff25b · ndnsf::examples::uav::RecordingDataProductState::chunks

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L868)

```cpp
uint64_t chunks = 0;
```

### API-9a665fecffd6 · ndnsf::examples::uav::RecordingDataProductState::bytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L869)

```cpp
uint64_t bytes = 0;
```

### API-f56b84f0e76a · ndnsf::examples::uav::RecordingDataProductState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L870)

```cpp
uint64_t updatedMs = 0;
```

### API-7dbe95a74d2d · ndnsf::examples::uav::RecordingDataProductState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L872)

```cpp
static RecordingDataProductState fromFields(const Fields& fields,
                                              const std::string& fallbackDroneId = "unknown");
```

### API-00b90917a03e · ndnsf::examples::uav::RecordingDataProductState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L874)

```cpp
Fields toFields() const;
```

### API-a5f4f2b3dca0 · ndnsf::examples::uav::RecordingDataProductState::isAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L875)

```cpp
bool isAvailable() const;
```

### API-95ceb3c662d6 · ndnsf::examples::uav::RecordingDataProductState::isPlayable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L876)

```cpp
bool isPlayable() const;
```

### API-59f2129d2cf1 · ndnsf::examples::uav::RecordingDataProductState::toDataProductReference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L877)

```cpp
ndn_service_framework::ServiceProvider::DataProductReference
  toDataProductReference(const ndn::Name& serviceName = ndn::Name(),
                         const ndn::Name& producerName = ndn::Name()) const;
```

### API-80a11a189436 · ndnsf::examples::uav::RecordingDataProductState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L880)

```cpp
std::string statusLine() const;
```

### API-cb1916bb8f21 · ndnsf::examples::uav::RetainedVideoPacketReference

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L883)

```cpp
struct RetainedVideoPacketReference
```

### API-6a2ce168f74d · ndnsf::examples::uav::RetainedVideoPacketReference::kind

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L885)

```cpp
std::string kind;
```

### API-117430403352 · ndnsf::examples::uav::RetainedVideoPacketReference::cursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L886)

```cpp
std::optional<uint64_t> cursor;
```

### API-7c8ed3def5d5 · ndnsf::examples::uav::RetainedVideoPacketReference::dataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L887)

```cpp
ndn::Name dataName;
```

### API-5575695295d7 · ndnsf::examples::uav::RetainedVideoPacketReference::wireDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L888)

```cpp
ndn_service_framework::StreamContentDigest wireDigest{};
```

### API-02230f466d53 · ndnsf::examples::uav::RetentionGap

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L891)

```cpp
struct RetentionGap
```

### API-95741452f011 · ndnsf::examples::uav::RetentionGap::firstCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L893)

```cpp
uint64_t firstCursor = 0;
```

### API-b7f2ac0bcf5d · ndnsf::examples::uav::RetentionGap::lastCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L894)

```cpp
uint64_t lastCursor = 0;
```

### API-bdda81100dbf · ndnsf::examples::uav::RetentionGap::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L895)

```cpp
std::string reason;
```

### API-299a990f60b5 · ndnsf::examples::uav::CanonicalVideoRecordingManifest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L899)

```cpp
struct CanonicalVideoRecordingManifest
```

### API-c1dbb96da215 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::contractVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L901)

```cpp
uint64_t contractVersion = 1;
```

### API-343ca40754a3 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::manifestVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L902)

```cpp
uint64_t manifestVersion = 1;
```

### API-6be81eb89159 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::recordingId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L903)

```cpp
std::string recordingId;
```

### API-ab29dd4e71b9 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L904)

```cpp
std::string streamId;
```

### API-8cf470738779 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L905)

```cpp
uint64_t sessionEpoch = 0;
```

### API-d62dbdd1feb9 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::mappingVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L906)

```cpp
uint64_t mappingVersion = 0;
```

### API-870b2c28c3b9 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::keyEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L907)

```cpp
uint64_t keyEpoch = 0;
```

### API-3bdfb489dff8 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L908)

```cpp
ndn::Name providerIdentity;
```

### API-b38b0224409a · ndnsf::examples::uav::CanonicalVideoRecordingManifest::serviceName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L909)

```cpp
ndn::Name serviceName;
```

### API-dd09e329405d · ndnsf::examples::uav::CanonicalVideoRecordingManifest::firstCommittedCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L910)

```cpp
uint64_t firstCommittedCursor = 0;
```

### API-6d2d877b2d84 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::lastCommittedCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L911)

```cpp
uint64_t lastCommittedCursor = 0;
```

### API-f72dcc492a3e · ndnsf::examples::uav::CanonicalVideoRecordingManifest::safeJoinCursor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L912)

```cpp
uint64_t safeJoinCursor = 0;
```

### API-ea4bf0b8e405 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::startedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L913)

```cpp
uint64_t startedMs = 0;
```

### API-8e889785507f · ndnsf::examples::uav::CanonicalVideoRecordingManifest::endedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L914)

```cpp
uint64_t endedMs = 0;
```

### API-d0b801a6c016 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::complete

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L915)

```cpp
bool complete = false;
```

### API-a9d6aa061f8b · ndnsf::examples::uav::CanonicalVideoRecordingManifest::signerCertificateName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L916)

```cpp
std::string signerCertificateName;
```

### API-e7f0ce43deda · ndnsf::examples::uav::CanonicalVideoRecordingManifest::signerCertificateDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L917)

```cpp
ndn_service_framework::StreamContentDigest signerCertificateDigest{};
```

### API-6cad895157f6 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::trustPolicyVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L918)

```cpp
std::string trustPolicyVersion;
```

### API-e6ec17e5c3c3 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::redactedStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L919)

```cpp
Fields redactedStreamDescriptor;
```

### API-9d13e73d66d7 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::archivedCertificateObjects

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L920)

```cpp
std::vector<ndn::Name> archivedCertificateObjects;
```

### API-054b6df1d871 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::keyAuthorizationObject

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L923)

```cpp
ndn::Name keyAuthorizationObject;
```

原始接口说明：

```text
/** Signed Repo object containing the epoch key wrapped to the Provider's
   * persistent RSA encryption certificate. It is never a plaintext key. */
```

### API-a1411e9a867b · ndnsf::examples::uav::CanonicalVideoRecordingManifest::packetCatalogPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L924)

```cpp
ndn::Name packetCatalogPrefix;
```

### API-453605ed1a97 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::packetCatalogEntries

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L925)

```cpp
uint64_t packetCatalogEntries = 0;
```

### API-b4bd7f78e7e4 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::packetCatalogHeadDigest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L926)

```cpp
ndn_service_framework::StreamContentDigest packetCatalogHeadDigest{};
```

### API-064b03a6569b · ndnsf::examples::uav::CanonicalVideoRecordingManifest::packets

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L927)

```cpp
std::vector<RetainedVideoPacketReference> packets;
```

### API-5bb694fee934 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::gaps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L928)

```cpp
std::vector<RetentionGap> gaps;
```

### API-9cdea77a9818 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::validate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L930)

```cpp
std::optional<std::string> validate() const;
```

### API-4244dc25982e · ndnsf::examples::uav::CanonicalVideoRecordingManifest::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L931)

```cpp
Fields toFields() const;
```

### API-fff4b024c218 · ndnsf::examples::uav::CanonicalVideoRecordingManifest::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L932)

```cpp
static CanonicalVideoRecordingManifest fromFields(const Fields& fields);
```

### API-9eadb98af64e · ndnsf::examples::uav::UavVideoContentKeyGrant

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L936)

```cpp
struct UavVideoContentKeyGrant
```

### API-0785c17ca34f · ndnsf::examples::uav::UavVideoContentKeyGrant::contractVersion

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L938)

```cpp
uint64_t contractVersion = 1;
```

### API-4c21337987e4 · ndnsf::examples::uav::UavVideoContentKeyGrant::recipientIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L939)

```cpp
std::string recipientIdentity;
```

### API-a7ecd5fcc637 · ndnsf::examples::uav::UavVideoContentKeyGrant::providerIdentity

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L940)

```cpp
ndn::Name providerIdentity;
```

### API-99489416401c · ndnsf::examples::uav::UavVideoContentKeyGrant::serviceName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L941)

```cpp
ndn::Name serviceName;
```

### API-04f88647a09b · ndnsf::examples::uav::UavVideoContentKeyGrant::permission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L942)

```cpp
std::string permission;
```

### API-63e9f58fb8c5 · ndnsf::examples::uav::UavVideoContentKeyGrant::streamId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L943)

```cpp
std::string streamId;
```

### API-701fd359d0aa · ndnsf::examples::uav::UavVideoContentKeyGrant::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L944)

```cpp
uint64_t sessionEpoch = 0;
```

### API-eaf44216e9b7 · ndnsf::examples::uav::UavVideoContentKeyGrant::keyEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L945)

```cpp
uint64_t keyEpoch = 0;
```

### API-ea1de4fffd07 · ndnsf::examples::uav::UavVideoContentKeyGrant::cipher

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L946)

```cpp
std::string cipher = "aes-256-gcm";
```

### API-68245d2cebd3 · ndnsf::examples::uav::UavVideoContentKeyGrant::protectedKeyMaterial

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L947)

```cpp
ndn::Buffer protectedKeyMaterial;
```

### API-e2041d5a8791 · ndnsf::examples::uav::UavVideoContentKeyGrant::protectedNonceSalt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L948)

```cpp
ndn::Buffer protectedNonceSalt;
```

### API-38219f80bc13 · ndnsf::examples::uav::UavVideoContentKeyGrant::issuedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L949)

```cpp
uint64_t issuedMs = 0;
```

### API-39b077d226b9 · ndnsf::examples::uav::UavVideoContentKeyGrant::expiresMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L950)

```cpp
uint64_t expiresMs = 0;
```

### API-b3e4a0cbdaee · ndnsf::examples::uav::UavVideoContentKeyGrant::validate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L952)

```cpp
std::optional<std::string> validate() const;
```

### API-42322b4ac37f · ndnsf::examples::uav::UavVideoContentKeyGrant::toProtectedFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L953)

```cpp
Fields toProtectedFields() const;
```

### API-501626aedbfb · ndnsf::examples::uav::MissionState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L956)

```cpp
struct MissionState
```

### API-ac922ba1acab · ndnsf::examples::uav::MissionState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L958)

```cpp
std::string droneId = "unknown";
```

### API-8461da9a0393 · ndnsf::examples::uav::MissionState::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L959)

```cpp
std::string missionId = "none";
```

### API-58e820f6ab70 · ndnsf::examples::uav::MissionState::partId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L960)

```cpp
std::string partId = "none";
```

### API-1d161ac615a4 · ndnsf::examples::uav::MissionState::phase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L961)

```cpp
std::string phase = "idle";
```

### API-ee7d550c9968 · ndnsf::examples::uav::MissionState::detail

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L962)

```cpp
std::string detail = "idle";
```

### API-940f63e8f980 · ndnsf::examples::uav::MissionState::ack

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L963)

```cpp
std::string ack = "unknown";
```

### API-e091a63b4f33 · ndnsf::examples::uav::MissionState::transport

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L964)

```cpp
std::string transport = "unknown";
```

### API-a5d0c80f0f01 · ndnsf::examples::uav::MissionState::waypointsForwarded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L965)

```cpp
std::string waypointsForwarded = "0";
```

### API-6f3e536ef023 · ndnsf::examples::uav::MissionState::waypointAcksAccepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L966)

```cpp
std::string waypointAcksAccepted = "0";
```

### API-149753b9b9d1 · ndnsf::examples::uav::MissionState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L967)

```cpp
uint64_t updatedMs = 0;
```

### API-874ce3be72dd · ndnsf::examples::uav::MissionState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L969)

```cpp
static MissionState fromFields(const Fields& fields);
```

### API-60a9ca613b6d · ndnsf::examples::uav::MissionState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L970)

```cpp
Fields toFields() const;
```

### API-3a3fc35044e8 · ndnsf::examples::uav::MissionState::isIdle

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L971)

```cpp
bool isIdle() const;
```

### API-9e1b0471b6a5 · ndnsf::examples::uav::MissionState::isUploading

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L972)

```cpp
bool isUploading() const;
```

### API-13c218b9a1b8 · ndnsf::examples::uav::MissionState::isUploaded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L973)

```cpp
bool isUploaded() const;
```

### API-55f6610a7b2e · ndnsf::examples::uav::MissionState::isExecuting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L974)

```cpp
bool isExecuting() const;
```

### API-662cd50f8b8e · ndnsf::examples::uav::MissionState::isStopping

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L975)

```cpp
bool isStopping() const;
```

### API-c151a6c21aec · ndnsf::examples::uav::MissionState::isCompleted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L976)

```cpp
bool isCompleted() const;
```

### API-358132f1946a · ndnsf::examples::uav::MissionState::isFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L977)

```cpp
bool isFailed() const;
```

### API-5f5d72bc133e · ndnsf::examples::uav::MissionState::isCancelled

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L978)

```cpp
bool isCancelled() const;
```

### API-f812268cf50b · ndnsf::examples::uav::MissionState::isTerminal

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L979)

```cpp
bool isTerminal() const;
```

### API-759ee03af3d2 · ndnsf::examples::uav::MissionState::isAssigned

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L980)

```cpp
bool isAssigned() const;
```

### API-9a41209cb726 · ndnsf::examples::uav::MissionState::isBusyForAssignment

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L981)

```cpp
bool isBusyForAssignment() const;
```

### API-428cb08676ce · ndnsf::examples::uav::MissionState::isStartable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L982)

```cpp
bool isStartable() const;
```

### API-4270f34fc9ca · ndnsf::examples::uav::MissionState::isStoppable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L983)

```cpp
bool isStoppable() const;
```

### API-b259ec940701 · ndnsf::examples::uav::MissionState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L984)

```cpp
std::string statusLine() const;
```

### API-33a1cba650e2 · ndnsf::examples::uav::MissionStartGateState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L987)

```cpp
struct MissionStartGateState
```

### API-9651bdbc1ad0 · ndnsf::examples::uav::MissionStartGateState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L989)

```cpp
std::string droneId = "unknown";
```

### API-881bea1cd22f · ndnsf::examples::uav::MissionStartGateState::hasMission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L990)

```cpp
bool hasMission = false;
```

### API-966818a98272 · ndnsf::examples::uav::MissionStartGateState::hasFlightGate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L991)

```cpp
bool hasFlightGate = false;
```

### API-057ddf044c90 · ndnsf::examples::uav::MissionStartGateState::missionUploaded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L992)

```cpp
bool missionUploaded = false;
```

### API-4af4f41183af · ndnsf::examples::uav::MissionStartGateState::canStart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L993)

```cpp
bool canStart = false;
```

### API-f73250da38d6 · ndnsf::examples::uav::MissionStartGateState::canStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L994)

```cpp
bool canStop = false;
```

### API-8962bb9a5576 · ndnsf::examples::uav::MissionStartGateState::missionPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L995)

```cpp
std::string missionPhase = "idle";
```

### API-6edf1422883d · ndnsf::examples::uav::MissionStartGateState::startReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L996)

```cpp
std::string startReason = "no-mission";
```

### API-9ab5978cb01b · ndnsf::examples::uav::MissionStartGateState::stopReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L997)

```cpp
std::string stopReason = "no-mission";
```

### API-410f70d7a6dc · ndnsf::examples::uav::MissionStartGateState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L999)

```cpp
static MissionStartGateState fromStates(const std::string& droneId,
                                          const std::optional<MissionState>& mission,
                                          const std::optional<FlightSafetyGateState>& flightGate);
```

### API-7d68bfd25822 · ndnsf::examples::uav::MissionStartGateState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1002)

```cpp
std::string statusLine() const;
```

### API-d43c39d3a734 · ndnsf::examples::uav::MissionProgressState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1005)

```cpp
struct MissionProgressState
```

### API-7ad7ec80fd15 · ndnsf::examples::uav::MissionProgressState::taskId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1007)

```cpp
std::string taskId = "none";
```

### API-7a9a0d3b0ea4 · ndnsf::examples::uav::MissionProgressState::phase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1008)

```cpp
std::string phase = "idle";
```

### API-377fd542c2b0 · ndnsf::examples::uav::MissionProgressState::assignment

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1009)

```cpp
std::string assignment = "unknown";
```

### API-87e545e78af4 · ndnsf::examples::uav::MissionProgressState::completionObjective

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1010)

```cpp
std::string completionObjective = "return-to-start";
```

### API-a698f69ac540 · ndnsf::examples::uav::MissionProgressState::drones

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1011)

```cpp
std::string drones = "none";
```

### API-bab58868b2d6 · ndnsf::examples::uav::MissionProgressState::attempts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1012)

```cpp
uint64_t attempts = 0;
```

### API-2db5c8c0fefc · ndnsf::examples::uav::MissionProgressState::totalParts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1013)

```cpp
uint64_t totalParts = 0;
```

### API-5483924bda54 · ndnsf::examples::uav::MissionProgressState::completedParts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1014)

```cpp
uint64_t completedParts = 0;
```

### API-e3cc38d06e52 · ndnsf::examples::uav::MissionProgressState::missingParts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1015)

```cpp
uint64_t missingParts = 0;
```

### API-493b19b5d283 · ndnsf::examples::uav::MissionProgressState::compensatedParts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1016)

```cpp
uint64_t compensatedParts = 0;
```

### API-29b6218123a6 · ndnsf::examples::uav::MissionProgressState::returnHomePlanned

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1017)

```cpp
bool returnHomePlanned = false;
```

### API-3dfc62babd9a · ndnsf::examples::uav::MissionProgressState::completedPartIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1018)

```cpp
std::string completedPartIds = "none";
```

### API-db3307f973fd · ndnsf::examples::uav::MissionProgressState::missingPartIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1019)

```cpp
std::string missingPartIds = "none";
```

### API-b3d5daf9d27b · ndnsf::examples::uav::MissionProgressState::compensatedPartIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1020)

```cpp
std::string compensatedPartIds = "none";
```

### API-98c279a213c8 · ndnsf::examples::uav::MissionProgressState::pendingPartIds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1021)

```cpp
std::string pendingPartIds = "none";
```

### API-e88fefb7197a · ndnsf::examples::uav::MissionProgressState::isActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1023)

```cpp
bool isActive() const;
```

### API-6f18d8522517 · ndnsf::examples::uav::MissionProgressState::needsCompensation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1024)

```cpp
bool needsCompensation() const;
```

### API-8d446a8c5241 · ndnsf::examples::uav::MissionProgressState::isComplete

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1025)

```cpp
bool isComplete() const;
```

### API-bb83f90876b0 · ndnsf::examples::uav::MissionProgressState::isFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1026)

```cpp
bool isFailed() const;
```

### API-13453d11269b · ndnsf::examples::uav::MissionProgressState::appliesToDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1027)

```cpp
bool appliesToDrone(const std::string& droneId) const;
```

### API-f455461edf82 · ndnsf::examples::uav::MissionProgressState::segmentStateForPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1028)

```cpp
std::string segmentStateForPart(const std::string& partId, const std::string& missionPhase = "idle") const;
```

### API-f7031afad7da · ndnsf::examples::uav::MissionProgressState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1029)

```cpp
std::string statusLine() const;
```

### API-1e5911eef8f8 · ndnsf::examples::uav::MissionWaypoint

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1032)

```cpp
struct MissionWaypoint
```

### API-6c76a56dbae4 · ndnsf::examples::uav::MissionWaypoint::lat

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1034)

```cpp
double lat = 0.0;
```

### API-d39b60360975 · ndnsf::examples::uav::MissionWaypoint::lon

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1035)

```cpp
double lon = 0.0;
```

### API-3cf211b3333b · ndnsf::examples::uav::MissionWaypoint::str

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1037)

```cpp
std::string str() const;
```

### API-28ac6c39a5a7 · ndnsf::examples::uav::MissionObject

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1040)

```cpp
struct MissionObject
```

### API-47b6acdffa31 · ndnsf::examples::uav::MissionObject::missionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1042)

```cpp
std::string missionId = "none";
```

### API-416aa7f28de3 · ndnsf::examples::uav::MissionObject::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1043)

```cpp
MissionState state;
```

### API-8fd52b7d5804 · ndnsf::examples::uav::MissionObject::waypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1044)

```cpp
std::vector<MissionWaypoint> waypoints;
```

### API-5d5b44f0721a · ndnsf::examples::uav::MissionObject::assignedDrones

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1045)

```cpp
std::vector<std::string> assignedDrones;
```

### API-32669b02b0b1 · ndnsf::examples::uav::MissionObject::progress

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1046)

```cpp
MissionProgressState progress;
```

### API-7bb2fc8f2cb1 · ndnsf::examples::uav::MissionObject::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1048)

```cpp
static MissionObject fromFields(const Fields& fields, const std::string& fallbackMissionId = "none");
```

### API-45c47c23d3ed · ndnsf::examples::uav::MissionObject::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1049)

```cpp
Fields toFields() const;
```

### API-a48262275635 · ndnsf::examples::uav::MissionObject::isKnown

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1050)

```cpp
bool isKnown() const;
```

### API-4a9af6d65766 · ndnsf::examples::uav::MissionObject::hasAssignment

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1051)

```cpp
bool hasAssignment(const std::string& droneId) const;
```

### API-4b1bda8bdd39 · ndnsf::examples::uav::MissionObject::waypointCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1052)

```cpp
size_t waypointCount() const;
```

### API-0d85e0d3a58a · ndnsf::examples::uav::MissionObject::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1053)

```cpp
std::string statusLine() const;
```

### API-c8cdad73191e · ndnsf::examples::uav::MissionControlState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1056)

```cpp
struct MissionControlState
```

### API-f5b5c41258c1 · ndnsf::examples::uav::MissionControlState::uploadPending

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1058)

```cpp
bool uploadPending = false;
```

### API-4b78087db8c9 · ndnsf::examples::uav::MissionControlState::startPending

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1059)

```cpp
bool startPending = false;
```

### API-5074051f1e8d · ndnsf::examples::uav::MissionControlState::stopPending

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1060)

```cpp
bool stopPending = false;
```

### API-fd84bd530a5c · ndnsf::examples::uav::MissionControlState::hasUploaded

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1061)

```cpp
bool hasUploaded = false;
```

### API-dd09cefd27ae · ndnsf::examples::uav::MissionControlState::hasExecuting

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1062)

```cpp
bool hasExecuting = false;
```

### API-531d18c7ef0f · ndnsf::examples::uav::MissionControlState::hasStopping

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1063)

```cpp
bool hasStopping = false;
```

### API-46a25cd9ef45 · ndnsf::examples::uav::MissionControlState::hasTerminal

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1064)

```cpp
bool hasTerminal = false;
```

### API-c79c6101db9f · ndnsf::examples::uav::MissionControlState::hasProgress

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1065)

```cpp
bool hasProgress = false;
```

### API-e6c733cd26f8 · ndnsf::examples::uav::MissionControlState::progressActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1066)

```cpp
bool progressActive = false;
```

### API-a18194688ebc · ndnsf::examples::uav::MissionControlState::progressNeedsCompensation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1067)

```cpp
bool progressNeedsCompensation = false;
```

### API-3eb8a8d5108c · ndnsf::examples::uav::MissionControlState::progressComplete

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1068)

```cpp
bool progressComplete = false;
```

### API-1039d55f2505 · ndnsf::examples::uav::MissionControlState::progressFailed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1069)

```cpp
bool progressFailed = false;
```

### API-2ac0032a5900 · ndnsf::examples::uav::MissionControlState::canUpload

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1070)

```cpp
bool canUpload = true;
```

### API-8655b79ef0ff · ndnsf::examples::uav::MissionControlState::canStart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1071)

```cpp
bool canStart = false;
```

### API-04a49abae08f · ndnsf::examples::uav::MissionControlState::canStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1072)

```cpp
bool canStop = false;
```

### API-f1e6f2771e99 · ndnsf::examples::uav::MissionControlState::startableCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1073)

```cpp
size_t startableCount = 0;
```

### API-18bb0d5770e8 · ndnsf::examples::uav::MissionControlState::startEligibleCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1074)

```cpp
size_t startEligibleCount = 0;
```

### API-1776adb98fac · ndnsf::examples::uav::MissionControlState::startBlockedCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1075)

```cpp
size_t startBlockedCount = 0;
```

### API-667972c8c542 · ndnsf::examples::uav::MissionControlState::progressPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1076)

```cpp
std::string progressPhase = "idle";
```

### API-3a5695fd9ec9 · ndnsf::examples::uav::MissionControlState::phases

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1077)

```cpp
std::string phases = "none";
```

### API-3cbe545a3d27 · ndnsf::examples::uav::MissionControlState::startEligible

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1078)

```cpp
std::string startEligible = "none";
```

### API-61d63b3efd02 · ndnsf::examples::uav::MissionControlState::startBlocked

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1079)

```cpp
std::string startBlocked = "none";
```

### API-01de98bd11a1 · ndnsf::examples::uav::MissionControlState::uploadReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1080)

```cpp
std::string uploadReason = "ok";
```

### API-b5857a2b6dbd · ndnsf::examples::uav::MissionControlState::startReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1081)

```cpp
std::string startReason = "no-uploaded-mission";
```

### API-0139b1c78e6e · ndnsf::examples::uav::MissionControlState::stopReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1082)

```cpp
std::string stopReason = "no-active-mission";
```

### API-118337abbd2b · ndnsf::examples::uav::MissionControlState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1084)

```cpp
static MissionControlState fromStates(const std::vector<MissionStartGateState>& missionGates,
                                        const std::optional<MissionProgressState>& progress,
                                        bool uploadPending,
                                        bool startPending,
                                        bool stopPending);
```

### API-05d8ee38d74e · ndnsf::examples::uav::MissionControlState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1089)

```cpp
std::string statusLine() const;
```

### API-c442ce4ab0d5 · ndnsf::examples::uav::SelectedActionState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1092)

```cpp
struct SelectedActionState
```

### API-e6df9338d1c7 · ndnsf::examples::uav::SelectedActionState::selectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1094)

```cpp
std::string selectedDrone = "unknown";
```

### API-c21d3c1742ec · ndnsf::examples::uav::SelectedActionState::flight

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1095)

```cpp
FlightActionControlState flight;
```

### API-2feea1343807 · ndnsf::examples::uav::SelectedActionState::mission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1096)

```cpp
MissionControlState mission;
```

### API-dc877dc5588c · ndnsf::examples::uav::SelectedActionState::manualMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1097)

```cpp
bool manualMode = false;
```

### API-609d51061515 · ndnsf::examples::uav::SelectedActionState::manualInputActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1098)

```cpp
bool manualInputActive = false;
```

### API-61cad8001ce0 · ndnsf::examples::uav::SelectedActionState::emergencyStopAvailable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1099)

```cpp
bool emergencyStopAvailable = false;
```

### API-2b71fda88a29 · ndnsf::examples::uav::SelectedActionState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1101)

```cpp
static SelectedActionState fromStates(const std::string& selectedDrone,
                                        const FlightActionControlState& flight,
                                        const MissionControlState& mission,
                                        bool manualMode,
                                        bool manualInputActive);
```

### API-a21ec573bf86 · ndnsf::examples::uav::SelectedActionState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1106)

```cpp
std::string statusLine() const;
```

### API-8acde6b6e1d6 · ndnsf::examples::uav::MissionPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1109)

```cpp
struct MissionPart
```

### API-161e14ef5ec3 · ndnsf::examples::uav::DroneListRowState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1111)

```cpp
struct DroneListRowState
```

### API-b8a4a036391a · ndnsf::examples::uav::DroneListRowState::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1113)

```cpp
std::string droneId;
```

### API-1a351fddc59d · ndnsf::examples::uav::DroneListRowState::selected

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1114)

```cpp
bool selected = false;
```

### API-992436092993 · ndnsf::examples::uav::DroneListRowState::hasTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1115)

```cpp
bool hasTelemetry = false;
```

### API-533ba21b2698 · ndnsf::examples::uav::DroneListRowState::hasReadiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1116)

```cpp
bool hasReadiness = false;
```

### API-d258c0cf4af6 · ndnsf::examples::uav::DroneListRowState::hasMission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1117)

```cpp
bool hasMission = false;
```

### API-581460fefafa · ndnsf::examples::uav::DroneListRowState::hasVideo

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1118)

```cpp
bool hasVideo = false;
```

### API-51c6f82f61e9 · ndnsf::examples::uav::DroneListRowState::hasCommand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1119)

```cpp
bool hasCommand = false;
```

### API-976ab5374ee5 · ndnsf::examples::uav::DroneListRowState::hasSafety

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1120)

```cpp
bool hasSafety = false;
```

### API-b474e998fad7 · ndnsf::examples::uav::DroneListRowState::hasMissionProgress

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1121)

```cpp
bool hasMissionProgress = false;
```

### API-ca1c5c818786 · ndnsf::examples::uav::DroneListRowState::hasVideoAdaptive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1122)

```cpp
bool hasVideoAdaptive = false;
```

### API-cd7f72e2d37c · ndnsf::examples::uav::DroneListRowState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1123)

```cpp
std::string readiness = "unknown";
```

### API-e4519cabdc1f · ndnsf::examples::uav::DroneListRowState::armed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1124)

```cpp
std::string armed = "unknown";
```

### API-c90a4ecd8612 · ndnsf::examples::uav::DroneListRowState::gps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1125)

```cpp
std::string gps = "unknown";
```

### API-e86871f62947 · ndnsf::examples::uav::DroneListRowState::battery

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1126)

```cpp
std::string battery = "unknown";
```

### API-7b257fbf0bd9 · ndnsf::examples::uav::DroneListRowState::mission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1127)

```cpp
std::string mission = "idle";
```

### API-2ea9f7da9023 · ndnsf::examples::uav::DroneListRowState::missionProgress

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1128)

```cpp
std::string missionProgress = "idle";
```

### API-48ee5126a1f2 · ndnsf::examples::uav::DroneListRowState::missionPartId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1129)

```cpp
std::string missionPartId = "none";
```

### API-ed10e528ab35 · ndnsf::examples::uav::DroneListRowState::missionSegmentState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1130)

```cpp
std::string missionSegmentState = "unknown";
```

### API-53862e2a9bed · ndnsf::examples::uav::DroneListRowState::video

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1131)

```cpp
std::string video = "unknown";
```

### API-04c55d62c798 · ndnsf::examples::uav::DroneListRowState::videoAdaptive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1132)

```cpp
std::string videoAdaptive = "unknown";
```

### API-0d1fccf33fbe · ndnsf::examples::uav::DroneListRowState::command

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1133)

```cpp
std::string command = "none";
```

### API-503183b054ba · ndnsf::examples::uav::DroneListRowState::safety

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1134)

```cpp
std::string safety = "unknown";
```

### API-c34afce02e8a · ndnsf::examples::uav::DroneListRowState::serviceCamera

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1135)

```cpp
std::string serviceCamera = "unknown";
```

### API-3c08157f8fa4 · ndnsf::examples::uav::DroneListRowState::serviceMavlink

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1136)

```cpp
std::string serviceMavlink = "unknown";
```

### API-01af0596a7f8 · ndnsf::examples::uav::DroneListRowState::serviceMission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1137)

```cpp
std::string serviceMission = "unknown";
```

### API-7a779a924204 · ndnsf::examples::uav::DroneListRowState::serviceRecording

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1138)

```cpp
std::string serviceRecording = "unknown";
```

### API-ce7b83013a7e · ndnsf::examples::uav::DroneListRowState::serviceRepo

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1139)

```cpp
std::string serviceRepo = "unknown";
```

### API-93a1ca9e27ce · ndnsf::examples::uav::DroneListRowState::rowText

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1140)

```cpp
std::string rowText;
```

### API-22c4f15acf9c · ndnsf::examples::uav::DroneListRowState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1142)

```cpp
static DroneListRowState fromStates(const std::string& droneId,
                                      bool selected,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<ReadinessState>& readiness,
                                      const std::optional<MissionState>& mission,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<FlightCommandState>& command,
                                      const std::optional<SafetyState>& safety,
                                      const std::optional<MissionProgressState>& progress,
                                      const std::optional<MissionPart>& missionPart,
                                      const std::string& cameraService = "unknown",
                                      const std::string& mavlinkService = "unknown",
                                      const std::string& missionService = "unknown",
                                      const std::string& recordingService = "unknown",
                                      const std::string& repoService = "unknown");
```

### API-257ed2d8e01b · ndnsf::examples::uav::DroneListRowState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1159)

```cpp
static DroneListRowState fromStates(const std::string& droneId,
                                      bool selected,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<ReadinessState>& readiness,
                                      const std::optional<MissionState>& mission,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<FlightCommandState>& command,
                                      const std::optional<SafetyState>& safety,
                                      const std::optional<MissionProgressState>& progress,
                                      const std::string& cameraService = "unknown",
                                      const std::string& mavlinkService = "unknown",
                                      const std::string& missionService = "unknown",
                                      const std::string& recordingService = "unknown",
                                      const std::string& repoService = "unknown");
```

### API-8acde6b6e1d6 · ndnsf::examples::uav::MissionPart

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1176)

```cpp
struct MissionPart
```

### API-c25bd0a4321c · ndnsf::examples::uav::MissionPart::id

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1178)

```cpp
std::string id;
```

### API-33937a6b3afa · ndnsf::examples::uav::MissionPart::role

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1179)

```cpp
std::string role;
```

### API-debbd7bd8b3a · ndnsf::examples::uav::MissionPart::assignedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1180)

```cpp
std::string assignedDrone;
```

### API-0273b5462eb1 · ndnsf::examples::uav::MissionPart::completedBy

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1181)

```cpp
std::string completedBy;
```

### API-893ff3f0ced3 · ndnsf::examples::uav::MissionPart::waypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1182)

```cpp
std::vector<MissionWaypoint> waypoints;
```

### API-794ab81eb979 · ndnsf::examples::uav::MissionPart::attempt

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1183)

```cpp
int attempt = 0;
```

### API-1624c11cedc5 · ndnsf::examples::uav::MissionPart::done

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1184)

```cpp
bool done = false;
```

### API-b844a1932bb8 · ndnsf::examples::uav::MissionPart::returnHomePlanned

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1185)

```cpp
bool returnHomePlanned = false;
```

### API-cc46b8695577 · ndnsf::examples::uav::MissionPart::firstWaypointOr

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1187)

```cpp
MissionWaypoint firstWaypointOr(MissionWaypoint fallback) const;
```

### API-be8b24febae5 · ndnsf::examples::uav::MissionPart::waypointStrings

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1188)

```cpp
std::vector<std::string> waypointStrings() const;
```

### API-61974cd1edc1 · ndnsf::examples::uav::MissionPart::waypointText

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1189)

```cpp
std::string waypointText() const;
```

### API-80e5873f5666 · ndnsf::examples::uav::MissionPart::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1190)

```cpp
std::string statusLine() const;
```

### API-576601c5838c · ndnsf::examples::uav::MissionPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1193)

```cpp
struct MissionPlan
```

### API-c2bc5272d57e · ndnsf::examples::uav::MissionPlan::taskId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1195)

```cpp
std::string taskId;
```

### API-7c5974835168 · ndnsf::examples::uav::MissionPlan::assignment

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1196)

```cpp
std::string assignment = "clustered-waypoints-return-to-start";
```

### API-affb5fa35555 · ndnsf::examples::uav::MissionPlan::completionObjective

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1197)

```cpp
std::string completionObjective = "return-to-start";
```

### API-f65643b6656b · ndnsf::examples::uav::MissionPlan::parts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1198)

```cpp
std::vector<MissionPart> parts;
```

### API-27eabf0993e8 · ndnsf::examples::uav::MissionPlan::returnHomePlanned

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1199)

```cpp
bool returnHomePlanned = false;
```

### API-47d4bca3f7f5 · ndnsf::examples::uav::MissionPlan::droneList

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1201)

```cpp
std::string droneList() const;
```

### API-7ca249c7dc75 · ndnsf::examples::uav::MissionPlan::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1202)

```cpp
std::string statusLine() const;
```

### API-1bbaadcd2755 · ndnsf::examples::uav::SelectedDroneSummaryState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1205)

```cpp
struct SelectedDroneSummaryState
```

### API-b4a830590cce · ndnsf::examples::uav::SelectedDroneSummaryState::selectedDrone

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1207)

```cpp
std::string selectedDrone = "unknown";
```

### API-da56c6b3569d · ndnsf::examples::uav::SelectedDroneSummaryState::hasTelemetry

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1208)

```cpp
bool hasTelemetry = false;
```

### API-be4578e65b78 · ndnsf::examples::uav::SelectedDroneSummaryState::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1209)

```cpp
std::string readiness = "unknown";
```

### API-05819707b11f · ndnsf::examples::uav::SelectedDroneSummaryState::missionPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1210)

```cpp
std::string missionPhase = "unknown";
```

### API-8d6e4228f0ec · ndnsf::examples::uav::SelectedDroneSummaryState::missionProgressPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1211)

```cpp
std::string missionProgressPhase = "unknown";
```

### API-d72a38943d86 · ndnsf::examples::uav::SelectedDroneSummaryState::missionSegmentState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1212)

```cpp
std::string missionSegmentState = "unknown";
```

### API-83e06b0b9611 · ndnsf::examples::uav::SelectedDroneSummaryState::missionPlanTask

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1213)

```cpp
std::string missionPlanTask = "none";
```

### API-6630c7098957 · ndnsf::examples::uav::SelectedDroneSummaryState::missionPartId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1214)

```cpp
std::string missionPartId = "none";
```

### API-ec408f5aedf0 · ndnsf::examples::uav::SelectedDroneSummaryState::missionPartWaypoints

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1215)

```cpp
uint64_t missionPartWaypoints = 0;
```

### API-6f01f33018c4 · ndnsf::examples::uav::SelectedDroneSummaryState::videoStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1216)

```cpp
std::string videoStatus = "unknown";
```

### API-b7e50e833bb3 · ndnsf::examples::uav::SelectedDroneSummaryState::videoAdaptive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1217)

```cpp
std::string videoAdaptive = "unknown";
```

### API-c364dcafd7be · ndnsf::examples::uav::SelectedDroneSummaryState::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1218)

```cpp
std::string linkState = "unknown";
```

### API-87bd86945eaf · ndnsf::examples::uav::SelectedDroneSummaryState::safetyAttention

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1219)

```cpp
bool safetyAttention = false;
```

### API-f98ae605fe7d · ndnsf::examples::uav::SelectedDroneSummaryState::canArm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1220)

```cpp
bool canArm = false;
```

### API-4fabd9e6204e · ndnsf::examples::uav::SelectedDroneSummaryState::canTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1221)

```cpp
bool canTakeoff = false;
```

### API-df5323ca6c75 · ndnsf::examples::uav::SelectedDroneSummaryState::canLand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1222)

```cpp
bool canLand = false;
```

### API-121832906c44 · ndnsf::examples::uav::SelectedDroneSummaryState::canManualControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1223)

```cpp
bool canManualControl = false;
```

### API-2a7e00f7e2c2 · ndnsf::examples::uav::SelectedDroneSummaryState::canControlPanel

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1224)

```cpp
bool canControlPanel = false;
```

### API-e499c49b50b7 · ndnsf::examples::uav::SelectedDroneSummaryState::armReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1225)

```cpp
std::string armReason = "unknown";
```

### API-0aae2d4b9f3b · ndnsf::examples::uav::SelectedDroneSummaryState::takeoffReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1226)

```cpp
std::string takeoffReason = "unknown";
```

### API-60097cbba64a · ndnsf::examples::uav::SelectedDroneSummaryState::landReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1227)

```cpp
std::string landReason = "unknown";
```

### API-d5d3b3752886 · ndnsf::examples::uav::SelectedDroneSummaryState::manualControlReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1228)

```cpp
std::string manualControlReason = "unknown";
```

### API-e7eed8ec0a9d · ndnsf::examples::uav::SelectedDroneSummaryState::controlPanelReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1229)

```cpp
std::string controlPanelReason = "unknown";
```

### API-6b4d516233ad · ndnsf::examples::uav::SelectedDroneSummaryState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1231)

```cpp
static SelectedDroneSummaryState fromStates(const std::string& selectedDrone,
                                              const std::optional<TelemetryState>& telemetry,
                                              const std::optional<ReadinessState>& readiness,
                                              const std::optional<MissionState>& mission,
                                              const std::optional<MissionPlan>& missionPlan,
                                              const std::optional<MissionPart>& missionPart,
                                              const std::optional<MissionProgressState>& missionProgress,
                                              const std::optional<VideoState>& video,
                                              const std::optional<VideoAdaptiveState>& videoAdaptive,
                                              const std::optional<SafetyState>& safety);
```

### API-3993cbaad485 · ndnsf::examples::uav::SelectedDroneSummaryState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1241)

```cpp
std::string statusLine() const;
```

### API-847e754a3718 · ndnsf::examples::uav::UavFunctionalityState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1244)

```cpp
struct UavFunctionalityState
```

### API-6450c8e98e39 · ndnsf::examples::uav::UavFunctionalityState::missionEditor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1246)

```cpp
std::string missionEditor = "missing";
```

### API-8e4346497eb5 · ndnsf::examples::uav::UavFunctionalityState::perDroneMissionReview

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1247)

```cpp
std::string perDroneMissionReview = "missing";
```

### API-0a20bb386ea9 · ndnsf::examples::uav::UavFunctionalityState::persistentMissionFiles

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1248)

```cpp
std::string persistentMissionFiles = "missing";
```

### API-bbe5a47225b3 · ndnsf::examples::uav::UavFunctionalityState::recordingLogBrowsing

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1249)

```cpp
std::string recordingLogBrowsing = "missing";
```

### API-f0ff1190cd67 · ndnsf::examples::uav::UavFunctionalityState::parameterStatusInspection

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1250)

```cpp
std::string parameterStatusInspection = "missing";
```

### API-933a1f3c46aa · ndnsf::examples::uav::UavFunctionalityState::objectDetectionDisplay

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1251)

```cpp
std::string objectDetectionDisplay = "missing";
```

### API-da99c02436b3 · ndnsf::examples::uav::UavFunctionalityState::multiDroneServiceSelection

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1252)

```cpp
std::string multiDroneServiceSelection = "missing";
```

### API-8c01f523bb24 · ndnsf::examples::uav::UavFunctionalityState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1254)

```cpp
static UavFunctionalityState fromFields(const Fields& fields);
```

### API-bdfa5316bafd · ndnsf::examples::uav::UavFunctionalityState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1255)

```cpp
static UavFunctionalityState fromStates(const std::optional<MissionPlan>& missionPlan,
                                          const std::optional<MissionPart>& selectedMissionPart,
                                          const std::optional<RecordingDataProductState>& recording,
                                          const std::optional<TelemetryState>& telemetry,
                                          bool objectDetectionServiceAvailable,
                                          size_t droneCount);
```

### API-1e4f3ed7eea5 · ndnsf::examples::uav::UavFunctionalityState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1261)

```cpp
Fields toFields() const;
```

### API-f5ef8e2ad85f · ndnsf::examples::uav::UavFunctionalityState::implementedCapabilityCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1262)

```cpp
size_t implementedCapabilityCount() const;
```

### API-b42213ae429c · ndnsf::examples::uav::UavFunctionalityState::missingOrLimitedCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1263)

```cpp
std::string missingOrLimitedCapabilities() const;
```

### API-cefda80b6b6f · ndnsf::examples::uav::UavFunctionalityState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1264)

```cpp
std::string statusLine() const;
```

### API-c3af76ba6d89 · ndnsf::examples::uav::UavPracticalityState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1267)

```cpp
struct UavPracticalityState
```

### API-552c34ea619c · ndnsf::examples::uav::UavPracticalityState::preflightSummary

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1269)

```cpp
std::string preflightSummary = "missing";
```

### API-b2291018116b · ndnsf::examples::uav::UavPracticalityState::hardwareCompatibilityNotes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1270)

```cpp
std::string hardwareCompatibilityNotes = "missing";
```

### API-0573687ad120 · ndnsf::examples::uav::UavPracticalityState::cameraDiagnostics

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1271)

```cpp
std::string cameraDiagnostics = "missing";
```

### API-79575d9a71e2 · ndnsf::examples::uav::UavPracticalityState::flightControllerDiagnostics

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1272)

```cpp
std::string flightControllerDiagnostics = "missing";
```

### API-6ce84f3834f7 · ndnsf::examples::uav::UavPracticalityState::configValidation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1273)

```cpp
std::string configValidation = "missing";
```

### API-70516fbc2e1a · ndnsf::examples::uav::UavPracticalityState::identityCertificateGuidance

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1274)

```cpp
std::string identityCertificateGuidance = "missing";
```

### API-718af5224b5f · ndnsf::examples::uav::UavPracticalityState::operatorWorkflowGuidance

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1275)

```cpp
std::string operatorWorkflowGuidance = "missing";
```

### API-f46a97cb3952 · ndnsf::examples::uav::UavPracticalityState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1277)

```cpp
static UavPracticalityState fromFields(const Fields& fields);
```

### API-add6b2d5a00c · ndnsf::examples::uav::UavPracticalityState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1278)

```cpp
static UavPracticalityState fromStates(const std::optional<TelemetryState>& telemetry,
                                         const std::optional<ReadinessState>& readiness,
                                         bool hasPreflightTool,
                                         bool hasRuntimeConfig,
                                         bool hasReleaseManual);
```

### API-e796054a023c · ndnsf::examples::uav::UavPracticalityState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1283)

```cpp
Fields toFields() const;
```

### API-db7f5bf17cc4 · ndnsf::examples::uav::UavPracticalityState::practicalCapabilityCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1284)

```cpp
size_t practicalCapabilityCount() const;
```

### API-de06b81a284d · ndnsf::examples::uav::UavPracticalityState::missingOrLimitedCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1285)

```cpp
std::string missingOrLimitedCapabilities() const;
```

### API-9390d5e2c5b2 · ndnsf::examples::uav::UavPracticalityState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1286)

```cpp
std::string statusLine() const;
```

### API-fe69a4e61c23 · ndnsf::examples::uav::UavStabilityState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1289)

```cpp
struct UavStabilityState
```

### API-cbb768a2ea42 · ndnsf::examples::uav::UavStabilityState::commandTimeoutHandling

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1291)

```cpp
std::string commandTimeoutHandling = "missing";
```

### API-3b8dab434b72 · ndnsf::examples::uav::UavStabilityState::stopVideoIdempotence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1292)

```cpp
std::string stopVideoIdempotence = "missing";
```

### API-3ad9332cb876 · ndnsf::examples::uav::UavStabilityState::streamSessionGuard

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1293)

```cpp
std::string streamSessionGuard = "missing";
```

### API-ecd63698ccee · ndnsf::examples::uav::UavStabilityState::frameSequenceGuard

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1294)

```cpp
std::string frameSequenceGuard = "missing";
```

### API-03e88bbcd6eb · ndnsf::examples::uav::UavStabilityState::adaptiveVideoPressure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1295)

```cpp
std::string adaptiveVideoPressure = "missing";
```

### API-f05a072cca41 · ndnsf::examples::uav::UavStabilityState::telemetryFreshness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1296)

```cpp
std::string telemetryFreshness = "missing";
```

### API-a0e8f325e60b · ndnsf::examples::uav::UavStabilityState::manualNeutralFallback

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1297)

```cpp
std::string manualNeutralFallback = "missing";
```

### API-040190bdce10 · ndnsf::examples::uav::UavStabilityState::longDurationProfiles

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1298)

```cpp
std::string longDurationProfiles = "missing";
```

### API-ab62ce7165c6 · ndnsf::examples::uav::UavStabilityState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1300)

```cpp
static UavStabilityState fromFields(const Fields& fields);
```

### API-6b8e0cf5c393 · ndnsf::examples::uav::UavStabilityState::fromStates

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1301)

```cpp
static UavStabilityState fromStates(const std::optional<FlightCommandState>& command,
                                      const std::optional<VideoState>& video,
                                      const std::optional<VideoAdaptiveState>& videoAdaptive,
                                      const std::optional<TelemetryState>& telemetry,
                                      const std::optional<SafetyState>& safety,
                                      bool stopVideoGuardEnabled,
                                      bool longDurationProfilesDocumented);
```

### API-d613f73b9c1f · ndnsf::examples::uav::UavStabilityState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1308)

```cpp
Fields toFields() const;
```

### API-61342a684c3b · ndnsf::examples::uav::UavStabilityState::stableCapabilityCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1309)

```cpp
size_t stableCapabilityCount() const;
```

### API-17e21765283b · ndnsf::examples::uav::UavStabilityState::missingOrLimitedCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1310)

```cpp
std::string missingOrLimitedCapabilities() const;
```

### API-4501fa9598d6 · ndnsf::examples::uav::UavStabilityState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1311)

```cpp
std::string statusLine() const;
```

### API-c291db00b3cd · ndnsf::examples::uav::MissionPlanDocument

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1314)

```cpp
struct MissionPlanDocument
```

### API-8905271e6df7 · ndnsf::examples::uav::MissionPlanDocument::schema

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1316)

```cpp
std::string schema = "ndnsf-uav-mission-plan-v2";
```

### API-a74d194bf59b · ndnsf::examples::uav::MissionPlanDocument::planId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1317)

```cpp
std::string planId = "none";
```

### API-aa6f87ad7b89 · ndnsf::examples::uav::MissionPlanDocument::displayName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1318)

```cpp
std::string displayName = "untitled";
```

### API-a2cb788cad88 · ndnsf::examples::uav::MissionPlanDocument::operatorId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1319)

```cpp
std::string operatorId = "unknown";
```

### API-25c60fe672d0 · ndnsf::examples::uav::MissionPlanDocument::createdMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1320)

```cpp
uint64_t createdMs = 0;
```

### API-d7bd73eec2dd · ndnsf::examples::uav::MissionPlanDocument::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1321)

```cpp
uint64_t updatedMs = 0;
```

### API-61aab885495b · ndnsf::examples::uav::MissionPlanDocument::plan

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1322)

```cpp
MissionPlan plan;
```

### API-ee6c609785a2 · ndnsf::examples::uav::MissionPlanDocument::geofence

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1323)

```cpp
std::vector<MissionWaypoint> geofence;
```

### API-f02fdd374b8d · ndnsf::examples::uav::MissionPlanDocument::rallyPoints

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1324)

```cpp
std::vector<MissionWaypoint> rallyPoints;
```

### API-820f2ae3667e · ndnsf::examples::uav::MissionPlanDocument::metadata

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1325)

```cpp
Fields metadata;
```

### API-3032ed546d45 · ndnsf::examples::uav::MissionPlanDocument::fromPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1327)

```cpp
static MissionPlanDocument fromPlan(const MissionPlan& plan,
                                      const std::string& planId,
                                      const std::string& displayName,
                                      const std::string& operatorId,
                                      uint64_t nowMs = 0);
```

### API-ca0da4fc4de3 · ndnsf::examples::uav::MissionPlanDocument::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1332)

```cpp
static MissionPlanDocument fromFields(const Fields& fields);
```

### API-28b0413ef838 · ndnsf::examples::uav::MissionPlanDocument::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1333)

```cpp
Fields toFields() const;
```

### API-97a26ad33d37 · ndnsf::examples::uav::MissionPlanDocument::isSaveable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1334)

```cpp
bool isSaveable() const;
```

### API-8f37cf5c0d9e · ndnsf::examples::uav::MissionPlanDocument::hasFenceOrRally

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1335)

```cpp
bool hasFenceOrRally() const;
```

### API-10926f500e7a · ndnsf::examples::uav::MissionPlanDocument::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1336)

```cpp
std::string statusLine() const;
```

### API-b93a8b18b0a6 · ndnsf::examples::uav::saveMissionPlanDocument

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1339)

```cpp
void
saveMissionPlanDocument(const MissionPlanDocument& document, const std::string& path);
```

### API-6718cd83801e · ndnsf::examples::uav::loadMissionPlanDocument

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1342)

```cpp
MissionPlanDocument
loadMissionPlanDocument(const std::string& path);
```

### API-a9ca61ef97f3 · ndnsf::examples::uav::UavDataProductCatalogState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1345)

```cpp
struct UavDataProductCatalogState
```

### API-ffedf6f32812 · ndnsf::examples::uav::UavDataProductCatalogState::repoObjects

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1347)

```cpp
uint64_t repoObjects = 0;
```

### API-5d4193847b68 · ndnsf::examples::uav::UavDataProductCatalogState::recordingProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1348)

```cpp
uint64_t recordingProducts = 0;
```

### API-28c704933a3f · ndnsf::examples::uav::UavDataProductCatalogState::telemetryLogProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1349)

```cpp
uint64_t telemetryLogProducts = 0;
```

### API-ab061578c985 · ndnsf::examples::uav::UavDataProductCatalogState::detectionProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1350)

```cpp
uint64_t detectionProducts = 0;
```

### API-2eabb059f995 · ndnsf::examples::uav::UavDataProductCatalogState::missionLogProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1351)

```cpp
uint64_t missionLogProducts = 0;
```

### API-3fb24dc119a1 · ndnsf::examples::uav::UavDataProductCatalogState::totalBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1352)

```cpp
uint64_t totalBytes = 0;
```

### API-0ad45bea80ba · ndnsf::examples::uav::UavDataProductCatalogState::sourceRepo

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1353)

```cpp
std::string sourceRepo = "unknown";
```

### API-6401393ab226 · ndnsf::examples::uav::UavDataProductCatalogState::latestProductType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1354)

```cpp
std::string latestProductType = "none";
```

### API-700eaa33cac6 · ndnsf::examples::uav::UavDataProductCatalogState::latestObjectPrefix

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1355)

```cpp
std::string latestObjectPrefix = "none";
```

### API-a86ce36eb6b6 · ndnsf::examples::uav::UavDataProductCatalogState::latestMissionId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1356)

```cpp
std::string latestMissionId = "none";
```

### API-e95365b30b88 · ndnsf::examples::uav::UavDataProductCatalogState::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1357)

```cpp
uint64_t updatedMs = 0;
```

### API-745872f84e42 · ndnsf::examples::uav::UavDataProductCatalogState::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1359)

```cpp
static UavDataProductCatalogState fromFields(const Fields& fields);
```

### API-ad3dddf49aed · ndnsf::examples::uav::UavDataProductCatalogState::fromRecording

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1360)

```cpp
static UavDataProductCatalogState fromRecording(const RecordingDataProductState& recording);
```

### API-4c939a322b50 · ndnsf::examples::uav::UavDataProductCatalogState::fromCatalogProductFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1361)

```cpp
static UavDataProductCatalogState fromCatalogProductFields(const std::vector<Fields>& entries,
                                                            const std::string& sourceRepo = "unknown",
                                                            uint64_t updatedMs = 0);
```

### API-47a62d7eaf5c · ndnsf::examples::uav::UavDataProductCatalogState::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1364)

```cpp
Fields toFields() const;
```

### API-3e35588677ff · ndnsf::examples::uav::UavDataProductCatalogState::totalProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1365)

```cpp
uint64_t totalProducts() const;
```

### API-b4b451c1d39f · ndnsf::examples::uav::UavDataProductCatalogState::hasQueryableProducts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1366)

```cpp
bool hasQueryableProducts() const;
```

### API-260362aed54f · ndnsf::examples::uav::UavDataProductCatalogState::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1367)

```cpp
std::string statusLine() const;
```

### API-329a5de29dd1 · ndnsf::examples::uav::VehicleParameterSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1370)

```cpp
struct VehicleParameterSnapshot
```

### API-817ef25d8fa7 · ndnsf::examples::uav::VehicleParameterSnapshot::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1372)

```cpp
std::string droneId = "unknown";
```

### API-7923a9d7ec7a · ndnsf::examples::uav::VehicleParameterSnapshot::source

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1373)

```cpp
std::string source = "unknown";
```

### API-f29ba79318dd · ndnsf::examples::uav::VehicleParameterSnapshot::firmware

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1374)

```cpp
std::string firmware = "unknown";
```

### API-7bc2243d3bf5 · ndnsf::examples::uav::VehicleParameterSnapshot::vehicleType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1375)

```cpp
std::string vehicleType = "unknown";
```

### API-b264327bddbb · ndnsf::examples::uav::VehicleParameterSnapshot::flightModes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1376)

```cpp
std::string flightModes = "unknown";
```

### API-80484ed35c6b · ndnsf::examples::uav::VehicleParameterSnapshot::parameterCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1377)

```cpp
uint64_t parameterCount = 0;
```

### API-d1c51d36f26d · ndnsf::examples::uav::VehicleParameterSnapshot::completePercent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1378)

```cpp
uint64_t completePercent = 0;
```

### API-c595df36c410 · ndnsf::examples::uav::VehicleParameterSnapshot::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1379)

```cpp
uint64_t updatedMs = 0;
```

### API-72d043cf8cee · ndnsf::examples::uav::VehicleParameterSnapshot::parameters

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1380)

```cpp
Fields parameters;
```

### API-d3a5a07d64eb · ndnsf::examples::uav::VehicleParameterSnapshot::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1382)

```cpp
static VehicleParameterSnapshot fromFields(const Fields& fields);
```

### API-fda49b74a8b3 · ndnsf::examples::uav::VehicleParameterSnapshot::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1383)

```cpp
Fields toFields(bool includeParameters = true) const;
```

### API-24e99dec0883 · ndnsf::examples::uav::VehicleParameterSnapshot::isUsable

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1384)

```cpp
bool isUsable() const;
```

### API-ce6fc7991895 · ndnsf::examples::uav::VehicleParameterSnapshot::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1385)

```cpp
std::string statusLine() const;
```

### API-c660df5692ce · ndnsf::examples::uav::VehicleParameterEditRequest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1388)

```cpp
struct VehicleParameterEditRequest
```

### API-93a57765eeec · ndnsf::examples::uav::VehicleParameterEditRequest::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1390)

```cpp
std::string requestId = "parameter-edit-request";
```

### API-fcd1006d6de0 · ndnsf::examples::uav::VehicleParameterEditRequest::operatorId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1391)

```cpp
std::string operatorId = "unknown";
```

### API-306682145aa8 · ndnsf::examples::uav::VehicleParameterEditRequest::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1392)

```cpp
std::string droneId = "unknown";
```

### API-0854399dbaab · ndnsf::examples::uav::VehicleParameterEditRequest::parameterName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1393)

```cpp
std::string parameterName;
```

### API-af356b6bbc38 · ndnsf::examples::uav::VehicleParameterEditRequest::expectedValue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1394)

```cpp
std::string expectedValue;
```

### API-dfac2a1b9f20 · ndnsf::examples::uav::VehicleParameterEditRequest::requestedValue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1395)

```cpp
std::string requestedValue;
```

### API-64d1bd4d8899 · ndnsf::examples::uav::VehicleParameterEditRequest::valueType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1396)

```cpp
std::string valueType = "unknown";
```

### API-aba9688d16aa · ndnsf::examples::uav::VehicleParameterEditRequest::targetSystem

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1397)

```cpp
uint64_t targetSystem = 1;
```

### API-ab6ce81ca600 · ndnsf::examples::uav::VehicleParameterEditRequest::targetComponent

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1398)

```cpp
uint64_t targetComponent = 1;
```

### API-a097fdce5d08 · ndnsf::examples::uav::VehicleParameterEditRequest::dryRun

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1399)

```cpp
bool dryRun = false;
```

### API-1e30b7855c5e · ndnsf::examples::uav::VehicleParameterEditRequest::requestedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1400)

```cpp
uint64_t requestedMs = 0;
```

### API-2d67d8fb94e6 · ndnsf::examples::uav::VehicleParameterEditRequest::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1402)

```cpp
static VehicleParameterEditRequest fromFields(const Fields& fields);
```

### API-78a05f2aac51 · ndnsf::examples::uav::VehicleParameterEditRequest::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1403)

```cpp
Fields toFields() const;
```

### API-26966760ce2c · ndnsf::examples::uav::VehicleParameterEditRequest::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1404)

```cpp
bool isValid(std::string& reason) const;
```

### API-3df2d2583b2a · ndnsf::examples::uav::VehicleParameterEditRequest::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1405)

```cpp
std::string statusLine() const;
```

### API-af9b89510065 · ndnsf::examples::uav::VehicleParameterEditResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1408)

```cpp
struct VehicleParameterEditResult
```

### API-22729ffa8650 · ndnsf::examples::uav::VehicleParameterEditResult::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1410)

```cpp
std::string requestId = "parameter-edit-request";
```

### API-8614221143d5 · ndnsf::examples::uav::VehicleParameterEditResult::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1411)

```cpp
std::string droneId = "unknown";
```

### API-3a457f710915 · ndnsf::examples::uav::VehicleParameterEditResult::parameterName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1412)

```cpp
std::string parameterName;
```

### API-c59f6483c996 · ndnsf::examples::uav::VehicleParameterEditResult::valueType

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1413)

```cpp
std::string valueType = "unknown";
```

### API-35ab418c1ebf · ndnsf::examples::uav::VehicleParameterEditResult::accepted

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1414)

```cpp
bool accepted = false;
```

### API-e61517b0e711 · ndnsf::examples::uav::VehicleParameterEditResult::applied

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1415)

```cpp
bool applied = false;
```

### API-2c06fdf9ec6c · ndnsf::examples::uav::VehicleParameterEditResult::verified

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1416)

```cpp
bool verified = false;
```

### API-d4197f72467a · ndnsf::examples::uav::VehicleParameterEditResult::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1417)

```cpp
std::string reason = "unknown";
```

### API-a6f3c4d27cca · ndnsf::examples::uav::VehicleParameterEditResult::previousValue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1418)

```cpp
std::string previousValue;
```

### API-26d5d4db9975 · ndnsf::examples::uav::VehicleParameterEditResult::requestedValue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1419)

```cpp
std::string requestedValue;
```

### API-d78b990dceb5 · ndnsf::examples::uav::VehicleParameterEditResult::verifiedValue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1420)

```cpp
std::string verifiedValue;
```

### API-d8e246e8c3a4 · ndnsf::examples::uav::VehicleParameterEditResult::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1421)

```cpp
uint64_t updatedMs = 0;
```

### API-450ee9dc4709 · ndnsf::examples::uav::VehicleParameterEditResult::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1423)

```cpp
static VehicleParameterEditResult fromFields(const Fields& fields);
```

### API-aea7ddbffd7d · ndnsf::examples::uav::VehicleParameterEditResult::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1424)

```cpp
Fields toFields() const;
```

### API-193c5c1b4bae · ndnsf::examples::uav::VehicleParameterEditResult::successful

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1425)

```cpp
bool successful() const;
```

### API-9e867a77752c · ndnsf::examples::uav::VehicleParameterEditResult::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1426)

```cpp
std::string statusLine() const;
```

### API-ac8453f79568 · ndnsf::examples::uav::PreflightCheckItem

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1429)

```cpp
struct PreflightCheckItem
```

### API-47d69f7a064b · ndnsf::examples::uav::PreflightCheckItem::checkId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1431)

```cpp
std::string checkId = "unknown";
```

### API-a5c64a23857b · ndnsf::examples::uav::PreflightCheckItem::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1432)

```cpp
std::string droneId = "unknown";
```

### API-03b0abd24c96 · ndnsf::examples::uav::PreflightCheckItem::label

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1433)

```cpp
std::string label = "unknown";
```

### API-b841baa3a79d · ndnsf::examples::uav::PreflightCheckItem::category

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1434)

```cpp
std::string category = "general";
```

### API-c501c8d531a6 · ndnsf::examples::uav::PreflightCheckItem::status

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1435)

```cpp
std::string status = "pending";
```

### API-0b9af646f6cb · ndnsf::examples::uav::PreflightCheckItem::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1436)

```cpp
std::string reason = "not-evaluated";
```

### API-8ed521d83680 · ndnsf::examples::uav::PreflightCheckItem::blocking

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1437)

```cpp
bool blocking = true;
```

### API-7bddb2325151 · ndnsf::examples::uav::PreflightCheckItem::order

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1438)

```cpp
uint64_t order = 0;
```

### API-35deb43ada54 · ndnsf::examples::uav::PreflightCheckItem::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1439)

```cpp
uint64_t updatedMs = 0;
```

### API-24a0ad5c95cc · ndnsf::examples::uav::PreflightCheckItem::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1441)

```cpp
static PreflightCheckItem fromFields(const Fields& fields);
```

### API-c4e7df433b20 · ndnsf::examples::uav::PreflightCheckItem::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1442)

```cpp
Fields toFields() const;
```

### API-b1c8b3d7d019 · ndnsf::examples::uav::PreflightCheckItem::isPass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1443)

```cpp
bool isPass() const;
```

### API-ee6c1076d0d7 · ndnsf::examples::uav::PreflightCheckItem::isBlockingFailure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1444)

```cpp
bool isBlockingFailure() const;
```

### API-e02cd53c8312 · ndnsf::examples::uav::PreflightCheckItem::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1445)

```cpp
std::string statusLine() const;
```

### API-62be55dc2953 · ndnsf::examples::uav::MavlinkMessageSummary

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1448)

```cpp
struct MavlinkMessageSummary
```

### API-5f3488b00509 · ndnsf::examples::uav::MavlinkMessageSummary::messageName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1450)

```cpp
std::string messageName = "UNKNOWN";
```

### API-22d8eeb96f96 · ndnsf::examples::uav::MavlinkMessageSummary::messageId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1451)

```cpp
uint64_t messageId = 0;
```

### API-ae786227866f · ndnsf::examples::uav::MavlinkMessageSummary::systemId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1452)

```cpp
uint64_t systemId = 0;
```

### API-9233edf92822 · ndnsf::examples::uav::MavlinkMessageSummary::componentId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1453)

```cpp
uint64_t componentId = 0;
```

### API-acad21e7c7fb · ndnsf::examples::uav::MavlinkMessageSummary::count

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1454)

```cpp
uint64_t count = 0;
```

### API-710cbc1e04e1 · ndnsf::examples::uav::MavlinkMessageSummary::rateHz

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1455)

```cpp
std::string rateHz = "0";
```

### API-875280031bad · ndnsf::examples::uav::MavlinkMessageSummary::lastSeenMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1456)

```cpp
uint64_t lastSeenMs = 0;
```

### API-ff9df197473a · ndnsf::examples::uav::MavlinkMessageSummary::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1458)

```cpp
static MavlinkMessageSummary fromFields(const Fields& fields, const std::string& prefix = "");
```

### API-926def8d014f · ndnsf::examples::uav::MavlinkMessageSummary::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1459)

```cpp
Fields toFields(const std::string& prefix = "") const;
```

### API-ef313febcdbb · ndnsf::examples::uav::MavlinkMessageSummary::isActive

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1460)

```cpp
bool isActive(uint64_t nowMs = 0, uint64_t staleAfterMs = 3000) const;
```

### API-e7d00debcacd · ndnsf::examples::uav::MavlinkMessageSummary::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1461)

```cpp
std::string statusLine() const;
```

### API-ef7674049028 · ndnsf::examples::uav::UavAnalyzeSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1464)

```cpp
struct UavAnalyzeSnapshot
```

### API-4373b99bd4f4 · ndnsf::examples::uav::UavAnalyzeSnapshot::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1466)

```cpp
std::string droneId = "unknown";
```

### API-50c3b397a54f · ndnsf::examples::uav::UavAnalyzeSnapshot::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1467)

```cpp
std::string linkState = "unknown";
```

### API-4ba08f325e77 · ndnsf::examples::uav::UavAnalyzeSnapshot::flightMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1468)

```cpp
std::string flightMode = "unknown";
```

### API-0da5468828a0 · ndnsf::examples::uav::UavAnalyzeSnapshot::missionPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1469)

```cpp
std::string missionPhase = "unknown";
```

### API-6ab870c1de12 · ndnsf::examples::uav::UavAnalyzeSnapshot::videoState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1470)

```cpp
std::string videoState = "unknown";
```

### API-c6c6d04c271f · ndnsf::examples::uav::UavAnalyzeSnapshot::parameterCacheStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1471)

```cpp
std::string parameterCacheStatus = "unknown";
```

### API-92e82e41b671 · ndnsf::examples::uav::UavAnalyzeSnapshot::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1472)

```cpp
uint64_t updatedMs = 0;
```

### API-67727ca4dd38 · ndnsf::examples::uav::UavAnalyzeSnapshot::messages

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1473)

```cpp
std::vector<MavlinkMessageSummary> messages;
```

### API-57b0ecb72ba9 · ndnsf::examples::uav::UavAnalyzeSnapshot::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1475)

```cpp
static UavAnalyzeSnapshot fromFields(const Fields& fields);
```

### API-9126edaa302c · ndnsf::examples::uav::UavAnalyzeSnapshot::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1476)

```cpp
Fields toFields() const;
```

### API-7deadc877b2f · ndnsf::examples::uav::UavAnalyzeSnapshot::activeMessageCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1477)

```cpp
uint64_t activeMessageCount(uint64_t nowMs = 0, uint64_t staleAfterMs = 3000) const;
```

### API-2b820e29b8e2 · ndnsf::examples::uav::UavAnalyzeSnapshot::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1478)

```cpp
std::string statusLine() const;
```

### API-083d960f8e5c · ndnsf::examples::uav::UavOperatorDashboardSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1481)

```cpp
struct UavOperatorDashboardSnapshot
```

### API-72008cd5a45a · ndnsf::examples::uav::UavOperatorDashboardSnapshot::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1483)

```cpp
std::string droneId = "unknown";
```

### API-204a7fbd201e · ndnsf::examples::uav::UavOperatorDashboardSnapshot::telemetryFreshness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1484)

```cpp
std::string telemetryFreshness = "unknown";
```

### API-ac2835d61de2 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::readiness

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1485)

```cpp
std::string readiness = "unknown";
```

### API-eaf14bdfb7a6 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::readinessReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1486)

```cpp
std::string readinessReason = "unknown";
```

### API-b13dcb3a7453 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::linkState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1487)

```cpp
std::string linkState = "unknown";
```

### API-972aba161d80 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::flightMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1488)

```cpp
std::string flightMode = "unknown";
```

### API-2c96ae19f109 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::missionPhase

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1489)

```cpp
std::string missionPhase = "unknown";
```

### API-51d0dd612f68 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::videoState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1490)

```cpp
std::string videoState = "unknown";
```

### API-da5dae8d4bbc · ndnsf::examples::uav::UavOperatorDashboardSnapshot::parameterCacheStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1491)

```cpp
std::string parameterCacheStatus = "unknown";
```

### API-ca8b950670eb · ndnsf::examples::uav::UavOperatorDashboardSnapshot::parameterCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1492)

```cpp
uint64_t parameterCount = 0;
```

### API-75d2388e61c0 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::preflightTotal

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1493)

```cpp
uint64_t preflightTotal = 0;
```

### API-d164eafe40da · ndnsf::examples::uav::UavOperatorDashboardSnapshot::preflightBlockingFailures

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1494)

```cpp
uint64_t preflightBlockingFailures = 0;
```

### API-3c8fec6539bd · ndnsf::examples::uav::UavOperatorDashboardSnapshot::mavlinkMessageCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1495)

```cpp
uint64_t mavlinkMessageCount = 0;
```

### API-a71ba993c37f · ndnsf::examples::uav::UavOperatorDashboardSnapshot::activeMavlinkMessageCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1496)

```cpp
uint64_t activeMavlinkMessageCount = 0;
```

### API-28be99ab99e7 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::canArm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1497)

```cpp
bool canArm = false;
```

### API-50c2d2ef3694 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::canTakeoff

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1498)

```cpp
bool canTakeoff = false;
```

### API-3c4c133fb04d · ndnsf::examples::uav::UavOperatorDashboardSnapshot::canLand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1499)

```cpp
bool canLand = false;
```

### API-4f442940546c · ndnsf::examples::uav::UavOperatorDashboardSnapshot::canManualControl

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1500)

```cpp
bool canManualControl = false;
```

### API-3e545d3f50cc · ndnsf::examples::uav::UavOperatorDashboardSnapshot::canEmergencyStop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1501)

```cpp
bool canEmergencyStop = false;
```

### API-d7b4ea2dea14 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::updatedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1502)

```cpp
uint64_t updatedMs = 0;
```

### API-54a1322a5047 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1504)

```cpp
static UavOperatorDashboardSnapshot fromFields(const Fields& fields);
```

### API-225f78afd816 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1505)

```cpp
Fields toFields() const;
```

### API-d8f741cacbce · ndnsf::examples::uav::UavOperatorDashboardSnapshot::operatorReady

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1506)

```cpp
bool operatorReady() const;
```

### API-bcfc41b837b7 · ndnsf::examples::uav::UavOperatorDashboardSnapshot::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1507)

```cpp
std::string statusLine() const;
```

### API-99d0138caf18 · ndnsf::examples::uav::OperatorAuthorityLease

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1510)

```cpp
struct OperatorAuthorityLease
```

### API-a9809f75bbf5 · ndnsf::examples::uav::OperatorAuthorityLease::leaseId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1512)

```cpp
std::string leaseId = "none";
```

### API-5a2f79439782 · ndnsf::examples::uav::OperatorAuthorityLease::operatorId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1513)

```cpp
std::string operatorId = "unknown";
```

### API-32e4aabfc21a · ndnsf::examples::uav::OperatorAuthorityLease::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1514)

```cpp
std::string droneId = "unknown";
```

### API-aea94072fe85 · ndnsf::examples::uav::OperatorAuthorityLease::scope

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1515)

```cpp
std::string scope = "monitor";
```

### API-6759b4427469 · ndnsf::examples::uav::OperatorAuthorityLease::issuedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1516)

```cpp
uint64_t issuedMs = 0;
```

### API-3e57a58547c6 · ndnsf::examples::uav::OperatorAuthorityLease::expiresMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1517)

```cpp
uint64_t expiresMs = 0;
```

### API-f501e52a09ad · ndnsf::examples::uav::OperatorAuthorityLease::revoked

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1518)

```cpp
bool revoked = false;
```

### API-c4afcab46232 · ndnsf::examples::uav::OperatorAuthorityLease::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1520)

```cpp
static OperatorAuthorityLease fromFields(const Fields& fields);
```

### API-3afc32f8d428 · ndnsf::examples::uav::OperatorAuthorityLease::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1521)

```cpp
Fields toFields() const;
```

### API-c86ed2afdc1c · ndnsf::examples::uav::OperatorAuthorityLease::isFresh

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1522)

```cpp
bool isFresh(uint64_t nowMs = 0) const;
```

### API-97f7f7c02150 · ndnsf::examples::uav::OperatorAuthorityLease::allowsCommand

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1523)

```cpp
bool allowsCommand(const std::string& targetDrone,
                     const std::string& commandName,
                     uint64_t nowMs,
                     std::string& reason) const;
```

### API-6fcad2e3f9f9 · ndnsf::examples::uav::OperatorAuthorityLease::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1527)

```cpp
std::string statusLine() const;
```

### API-e8f65e6f0bf0 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1530)

```cpp
struct OperatorAuthorityLeaseRequest
```

### API-ea3ad0d894cb · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::requestId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1532)

```cpp
std::string requestId = "lease-request";
```

### API-e46d685d7519 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::operatorId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1533)

```cpp
std::string operatorId = "unknown";
```

### API-51d45c325985 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1534)

```cpp
std::string droneId = "all";
```

### API-f160d0e71d11 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::scope

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1535)

```cpp
std::string scope = "monitor";
```

### API-0931774074a3 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::ttlMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1536)

```cpp
uint64_t ttlMs = 0;
```

### API-053e2e27c072 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::requestedMs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1537)

```cpp
uint64_t requestedMs = 0;
```

### API-ddad44287fd6 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::fromFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1539)

```cpp
static OperatorAuthorityLeaseRequest fromFields(const Fields& fields);
```

### API-64f352f0b328 · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::toFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1540)

```cpp
Fields toFields() const;
```

### API-cf0c4701aade · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::isValid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1541)

```cpp
bool isValid(std::string& reason) const;
```

### API-d544987c955b · ndnsf::examples::uav::OperatorAuthorityLeaseRequest::statusLine

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1542)

```cpp
std::string statusLine() const;
```

### API-1aec263d8767 · ndnsf::examples::uav::buildPatrolMissionPlan

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1545)

```cpp
MissionPlan
buildPatrolMissionPlan(const std::string& taskId,
                       double centerLat,
                       double centerLon,
                       double sideMeters,
                       const std::vector<std::string>& droneIds,
                       const std::vector<MissionWaypoint>& routeWaypoints = {},
                       const std::map<std::string, MissionWaypoint>& departurePoints = {});
```

### API-6f5e923f95c5 · ndnsf::examples::uav::toServiceOperationStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1554)

```cpp
ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const FlightCommandState& command,
                         const ndn::Name& serviceName = ndn::Name(),
                         const ndn::Name& providerName = ndn::Name(),
                         const ndn::Name& requestId = ndn::Name());
```

### API-d8bc00e7e87d · ndnsf::examples::uav::toServiceOperationStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1560)

```cpp
ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const RecordingDataProductState& recording,
                         const ndn::Name& serviceName = ndn::Name(),
                         const ndn::Name& providerName = ndn::Name(),
                         const ndn::Name& requestId = ndn::Name());
```

### API-b422a0899428 · ndnsf::examples::uav::toServiceOperationStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1566)

```cpp
ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const MissionState& mission,
                         const ndn::Name& serviceName = ndn::Name(),
                         const ndn::Name& providerName = ndn::Name(),
                         const ndn::Name& requestId = ndn::Name());
```

### API-3811216730f4 · ndnsf::examples::uav::toServiceOperationStatus

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1572)

```cpp
ndn_service_framework::ServiceProvider::ServiceOperationStatus
toServiceOperationStatus(const MissionProgressState& progress,
                         const ndn::Name& serviceName = ndn::Name(),
                         const ndn::Name& providerName = ndn::Name(),
                         const ndn::Name& requestId = ndn::Name());
```

### API-92216846e090 · ndnsf::examples::uav::nowMilliseconds

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1578)

```cpp
uint64_t
nowMilliseconds();
```

### API-4c485b9ec4d3 · ndnsf::examples::uav::encodeFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1581)

```cpp
std::string
encodeFields(const Fields& fields);
```

### API-b530ca6731e9 · ndnsf::examples::uav::decodeFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1584)

```cpp
Fields
decodeFields(const std::string& payload);
```

### API-3ebe5cd75ef6 · ndnsf::examples::uav::encodeVideoStreamDescriptor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1587)

```cpp
std::string
encodeVideoStreamDescriptor(const VideoStreamDescriptor& descriptor);
```

### API-6f8099a476c2 · ndnsf::examples::uav::decodeVideoStreamDescriptorStrict

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1590)

```cpp
VideoStreamDescriptor
decodeVideoStreamDescriptorStrict(const std::string& payload,
                                  const ndn::Name& expectedProvider,
                                  const ndn::Name& verifiedProvider,
                                  const ndn::Name& expectedService,
                                  const ndn::Name& responseService);
```

### API-1d5bffa6fbda · ndnsf::examples::uav::makeUavVideoDataName

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1597)

```cpp
UavVideoDataName
makeUavVideoDataName(const VideoStreamDescriptor& descriptor,
                     const VideoPacket& packet);
```

### API-35bdd7ac811c · ndnsf::examples::uav::makeUavStreamNameMapResolverConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1601)

```cpp
ndn_service_framework::StreamNameMapResolverConfig
makeUavStreamNameMapResolverConfig(const VideoStreamDescriptor& descriptor);
```

### API-d6ac3d1fd2f4 · ndnsf::examples::uav::makeUavStreamNameMapCheckpoint

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1604)

```cpp
ndn_service_framework::StreamNameMapCheckpoint
makeUavStreamNameMapCheckpoint(const VideoStreamDescriptor& descriptor);
```

### API-916e19753723 · ndnsf::examples::uav::deriveUavVideoNonce

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1607)

```cpp
ndn::Buffer
deriveUavVideoNonce(const ndn::Buffer& nonceSalt,
                    ndn_service_framework::StreamCursor cursor);
```

### API-73193224ae6b · ndnsf::examples::uav::makeUavVideoAad

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1611)

```cpp
UavVideoAad
makeUavVideoAad(const VideoStreamDescriptor& descriptor,
                const UavVideoDataName& binding);
```

### API-f349bfbb1c9f · ndnsf::examples::uav::decodeUavVideoEnvelopeStrict

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1615)

```cpp
ndn_service_framework::HybridMessageEnvelope
decodeUavVideoEnvelopeStrict(const ndn::Buffer& wire,
                             const VideoStreamDescriptor& descriptor,
                             const UavVideoDataName& binding);
```

### API-576f3b36619d · ndnsf::examples::uav::protectUavVideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1620)

```cpp
ndn::Buffer
protectUavVideoPacket(const VideoStreamDescriptor& descriptor,
                      const UavVideoDataName& binding,
                      const VideoPacket& packet,
                      UavVideoNonceUseGuard& nonceGuard);
```

### API-c4d5635a2b18 · ndnsf::examples::uav::unprotectUavVideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1626)

```cpp
VideoPacket
unprotectUavVideoPacket(const VideoStreamDescriptor& descriptor,
                        const UavVideoDataName& binding,
                        const ndn::Name& verifiedProvider,
                        const ndn::Buffer& wire);
```

### API-c9d95b93d091 · ndnsf::examples::uav::loadKeyValueConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1632)

```cpp
Fields
loadKeyValueConfig(const std::string& path);
```

### API-7a6f6e48beb6 · ndnsf::examples::uav::encodeVideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1635)

```cpp
std::vector<uint8_t>
encodeVideoPacket(const VideoPacket& packet);
```

### API-4caa6a36dbb5 · ndnsf::examples::uav::decodeVideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1638)

```cpp
VideoPacket
decodeVideoPacket(const std::vector<uint8_t>& payload);
```

### API-f04dc8f9d6ad · ndnsf::examples::uav::videoPacketToStreamChunk

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1641)

```cpp
ndn_service_framework::StreamChunk
videoPacketToStreamChunk(const VideoPacket& packet);
```

### API-b51fa74eb1ca · ndnsf::examples::uav::streamChunkToVideoPacket

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1644)

```cpp
VideoPacket
streamChunkToVideoPacket(const ndn_service_framework::StreamChunk& chunk);
```

### API-10ca8c8e80cc · ndnsf::examples::uav::buildMockMavlinkFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1647)

```cpp
std::vector<uint8_t>
buildMockMavlinkFrame(const std::string& commandName, const Fields& params);
```

### API-490299ac2def · ndnsf::examples::uav::buildMavlinkHeartbeatFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1650)

```cpp
std::vector<uint8_t>
buildMavlinkHeartbeatFrame(const Fields& params = {});
```

### API-2c9116791cb9 · ndnsf::examples::uav::buildMavlinkParamSetFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1653)

```cpp
std::vector<uint8_t>
buildMavlinkParamSetFrame(const std::string& paramName, float value,
                          uint8_t paramType, const Fields& params = {});
```

### API-0992ddb7e8c2 · ndnsf::examples::uav::buildMavlinkMissionCountFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1657)

```cpp
std::vector<uint8_t>
buildMavlinkMissionCountFrame(uint16_t count, const Fields& params = {});
```

### API-c5fa0c8d007b · ndnsf::examples::uav::buildMavlinkMissionClearAllFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1660)

```cpp
std::vector<uint8_t>
buildMavlinkMissionClearAllFrame(const Fields& params = {});
```

### API-f3e3cb206971 · ndnsf::examples::uav::buildMavlinkMissionItemIntFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1663)

```cpp
std::vector<uint8_t>
buildMavlinkMissionItemIntFrame(uint16_t seq, double latitude, double longitude,
                                float altitudeM, bool current,
                                const Fields& params = {});
```

### API-6abfe9443ba3 · ndnsf::examples::uav::buildMockJpeg

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1668)

```cpp
std::vector<uint8_t>
buildMockJpeg(const std::string& droneId, const std::string& frameId);
```

### API-049639814034 · ndnsf::examples::uav::hexEncode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1671)

```cpp
std::string
hexEncode(const std::vector<uint8_t>& value);
```

### API-6f03de520a49 · ndnsf::examples::uav::hexDecode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1674)

```cpp
std::vector<uint8_t>
hexDecode(const std::string& value);
```

### API-75796a72ed18 · ndnsf::examples::uav::makeMavlinkCommandPayload

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1677)

```cpp
std::string
makeMavlinkCommandPayload(const std::string& commandName,
                          const std::string& missionId,
                          const Fields& params);
```

### API-d60e99d7b528 · ndnsf::examples::uav::makeMissionPayload

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1682)

```cpp
std::string
makeMissionPayload(const std::string& missionId,
                   const std::string& role,
                   const std::string& area,
                   const std::vector<std::string>& waypoints,
                   bool captureRequired,
                   const std::string& objectDetectionService = "/UAV/GS/ObjectDetection");
```

### API-0bfb1cdaafff · ndnsf::examples::uav::makeVideoStartFields

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1690)

```cpp
Fields
makeVideoStartFields(uint64_t fps, uint64_t requestedBitrateKbps,
                     uint64_t requestedFrameWidth, uint64_t fecParityShards);
```

### API-73b78b9eda5e · ndnsf::examples::uav::parseVideoFecParityShards

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1694)

```cpp
uint64_t
parseVideoFecParityShards(const Fields& fields, uint64_t fallback = 1);
```

### API-6584f1f1e6f3 · ndnsf::examples::uav::fieldOr

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavProtocol.hpp#L1697)

```cpp
std::string
fieldOr(const Fields& fields, const std::string& key, const std::string& fallback);
```

## NDNSF-UAV-APP/shared/UavSensorStreams.hpp

源码 SHA-256：`ecb718a403b241e9c3c9e1b9e90283f81f9f34f8fe970b088634552b13c425ba`。

### API-c4eb024b62a8 · ndnsf::examples::uav::UAV_TELEMETRY_PERIOD_MS = 50

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L15)

```cpp
inline constexpr uint64_t UAV_TELEMETRY_PERIOD_MS = 50;
```

### API-ddaee21bd14a · ndnsf::examples::uav::UAV_ACOUSTIC_BLOCK_PERIOD_MS = 40

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L16)

```cpp
inline constexpr uint64_t UAV_ACOUSTIC_BLOCK_PERIOD_MS = 40;
```

### API-531b6fd31bcb · ndnsf::examples::uav::CompactTelemetrySample

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L18)

```cpp
struct CompactTelemetrySample
```

### API-b102239e8e4d · ndnsf::examples::uav::CompactTelemetrySample::sampleId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L20)

```cpp
uint64_t sampleId = 0;
```

### API-b57ba082bb96 · ndnsf::examples::uav::CompactTelemetrySample::sourceTimestampNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L21)

```cpp
uint64_t sourceTimestampNs = 0;
```

### API-d807f90de7c0 · ndnsf::examples::uav::CompactTelemetrySample::droneId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L22)

```cpp
std::string droneId;
```

### API-ef3ebdcbd8b1 · ndnsf::examples::uav::CompactTelemetrySample::latitudeE7

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L23)

```cpp
int32_t latitudeE7 = 0;
```

### API-a18f686d56ea · ndnsf::examples::uav::CompactTelemetrySample::longitudeE7

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L24)

```cpp
int32_t longitudeE7 = 0;
```

### API-ce068d82b5d2 · ndnsf::examples::uav::CompactTelemetrySample::altitudeMm

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L25)

```cpp
int32_t altitudeMm = 0;
```

### API-2c473390906c · ndnsf::examples::uav::CompactTelemetrySample::groundSpeedMmps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L26)

```cpp
int32_t groundSpeedMmps = 0;
```

### API-a762dd698c22 · ndnsf::examples::uav::CompactTelemetrySample::batteryPermille

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L27)

```cpp
uint16_t batteryPermille = 0;
```

### API-02dccbeca573 · ndnsf::examples::uav::CompactTelemetrySample::readinessFlags

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L28)

```cpp
uint8_t readinessFlags = 0;
```

### API-be05f18c158b · ndnsf::examples::uav::CompactTelemetrySample::linkQuality

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L29)

```cpp
uint8_t linkQuality = 0;
```

### API-f8575e3f2590 · ndnsf::examples::uav::CompactTelemetrySample::encodedSizeFor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L31)

```cpp
static size_t encodedSizeFor(uint64_t sampleId);
```

### API-4f004b3dcd38 · ndnsf::examples::uav::CompactTelemetrySample::deterministic

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L32)

```cpp
static CompactTelemetrySample deterministic(uint64_t sampleId,
                                              uint64_t sourceTimestampNs,
                                              std::string droneId);
```

### API-2bf0d508c54d · ndnsf::examples::uav::CompactTelemetrySample::encode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L35)

```cpp
std::vector<uint8_t> encode() const;
```

### API-c90671acc5c6 · ndnsf::examples::uav::CompactTelemetrySample::decode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L36)

```cpp
static std::optional<CompactTelemetrySample>
  decode(const std::vector<uint8_t>& wire);
```

### API-aef0b3bf7d5e · ndnsf::examples::uav::TelemetryAdmissionResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L40)

```cpp
struct TelemetryAdmissionResult
```

### API-8afdab9f82aa · ndnsf::examples::uav::TelemetryAdmissionResult::valid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L42)

```cpp
bool valid = false;
```

### API-24d18812b2ac · ndnsf::examples::uav::TelemetryAdmissionResult::stateAdvanced

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L43)

```cpp
bool stateAdvanced = false;
```

### API-3a6607e3b253 · ndnsf::examples::uav::TelemetryAdmissionResult::newSample

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L44)

```cpp
bool newSample = false;
```

### API-3125b51b5e27 · ndnsf::examples::uav::TelemetryAdmissionResult::duplicate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L45)

```cpp
bool duplicate = false;
```

### API-d5250bde1c5e · ndnsf::examples::uav::TelemetryAdmissionResult::outOfOrder

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L46)

```cpp
bool outOfOrder = false;
```

### API-29766f72db6c · ndnsf::examples::uav::TelemetryAdmissionResult::sampleId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L47)

```cpp
uint64_t sampleId = 0;
```

### API-836dec3c05c0 · ndnsf::examples::uav::TelemetryAdmissionResult::ageNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L48)

```cpp
uint64_t ageNs = 0;
```

### API-9a685afbf11e · ndnsf::examples::uav::TelemetryAdmissionResult::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L49)

```cpp
std::string reason;
```

### API-b640022a0318 · ndnsf::examples::uav::LatestTelemetryAdmission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L53)

```cpp
class LatestTelemetryAdmission
```

### API-04bce36c3d2b · ndnsf::examples::uav::LatestTelemetryAdmission::LatestTelemetryAdmission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L56)

```cpp
explicit LatestTelemetryAdmission(std::string expectedDroneId);
```

### API-730501b40b2c · ndnsf::examples::uav::LatestTelemetryAdmission::admit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L57)

```cpp
TelemetryAdmissionResult admit(const std::vector<uint8_t>& wire,
                                 uint64_t receivedTimestampNs);
```

### API-9d5747599514 · ndnsf::examples::uav::LatestTelemetryAdmission::latest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L59)

```cpp
std::optional<CompactTelemetrySample> latest() const;
```

### API-352e204130ad · ndnsf::examples::uav::LatestTelemetryAdmission::admittedCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L60)

```cpp
uint64_t admittedCount() const;
```

### API-ac1dc58da755 · ndnsf::examples::uav::LatestTelemetryAdmission::duplicateCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L61)

```cpp
uint64_t duplicateCount() const;
```

### API-aa094324abb3 · ndnsf::examples::uav::LatestTelemetryAdmission::outOfOrderCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L62)

```cpp
uint64_t outOfOrderCount() const;
```

### API-e646f5c7ad42 · ndnsf::examples::uav::OpaqueAcousticSource

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L73)

```cpp
struct OpaqueAcousticSource
```

### API-f324e8b6d327 · ndnsf::examples::uav::OpaqueAcousticSource::blockId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L75)

```cpp
uint64_t blockId = 0;
```

### API-9ba0a51d9c2c · ndnsf::examples::uav::OpaqueAcousticSource::captureTimestampNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L76)

```cpp
uint64_t captureTimestampNs = 0;
```

### API-f75f85466117 · ndnsf::examples::uav::OpaqueAcousticSource::sourceIndex

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L77)

```cpp
uint16_t sourceIndex = 0;
```

### API-e049e571df88 · ndnsf::examples::uav::OpaqueAcousticSource::sourceCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L78)

```cpp
uint16_t sourceCount = 0;
```

### API-990705ecccdc · ndnsf::examples::uav::OpaqueAcousticSource::opaqueBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L79)

```cpp
std::vector<uint8_t> opaqueBytes;
```

### API-c5077299eab6 · ndnsf::examples::uav::OpaqueAcousticSource::sourceCountFor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L81)

```cpp
static size_t sourceCountFor(uint64_t blockId);
```

### API-68af2608a2d6 · ndnsf::examples::uav::OpaqueAcousticSource::deterministic

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L82)

```cpp
static OpaqueAcousticSource deterministic(uint64_t blockId,
                                            uint64_t captureTimestampNs,
                                            size_t sourceIndex);
```

### API-c54c68ea3ae4 · ndnsf::examples::uav::OpaqueAcousticSource::encode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L85)

```cpp
std::vector<uint8_t> encode() const;
```

### API-2a949bcbd3e2 · ndnsf::examples::uav::OpaqueAcousticSource::decode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L86)

```cpp
static std::optional<OpaqueAcousticSource>
  decode(const std::vector<uint8_t>& wire);
```

### API-6c8f6bc279bc · ndnsf::examples::uav::acousticSourceCountClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L90)

```cpp
std::string
acousticSourceCountClass(size_t sourceCount);
```

### API-779d9486217f · ndnsf::examples::uav::CompleteAcousticBlock

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L93)

```cpp
struct CompleteAcousticBlock
```

### API-c19ac1276e11 · ndnsf::examples::uav::CompleteAcousticBlock::blockId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L95)

```cpp
uint64_t blockId = 0;
```

### API-d5f19bc0e2f0 · ndnsf::examples::uav::CompleteAcousticBlock::captureTimestampNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L96)

```cpp
uint64_t captureTimestampNs = 0;
```

### API-33b092fc1141 · ndnsf::examples::uav::CompleteAcousticBlock::completedTimestampNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L97)

```cpp
uint64_t completedTimestampNs = 0;
```

### API-41b361e67c7d · ndnsf::examples::uav::CompleteAcousticBlock::recoveredSources

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L98)

```cpp
size_t recoveredSources = 0;
```

### API-a55589547df2 · ndnsf::examples::uav::CompleteAcousticBlock::orderedSources

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L99)

```cpp
std::vector<std::vector<uint8_t>> orderedSources;
```

### API-3690ea27666b · ndnsf::examples::uav::AcousticAdmissionResult

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L102)

```cpp
struct AcousticAdmissionResult
```

### API-1bf32e9c6bdd · ndnsf::examples::uav::AcousticAdmissionResult::valid

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L104)

```cpp
bool valid = false;
```

### API-260a36c17c3a · ndnsf::examples::uav::AcousticAdmissionResult::duplicate

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L105)

```cpp
bool duplicate = false;
```

### API-94282cab2adb · ndnsf::examples::uav::AcousticAdmissionResult::late

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L106)

```cpp
bool late = false;
```

### API-224f1b999c1e · ndnsf::examples::uav::AcousticAdmissionResult::completed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L107)

```cpp
std::optional<CompleteAcousticBlock> completed;
```

### API-747627f7e46c · ndnsf::examples::uav::AcousticAdmissionResult::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L108)

```cpp
std::string reason;
```

### API-1ad7bd674706 · ndnsf::examples::uav::CompleteAcousticBlockAdmission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L112)

```cpp
class CompleteAcousticBlockAdmission
```

### API-f230d36cac11 · ndnsf::examples::uav::CompleteAcousticBlockAdmission::CompleteAcousticBlockAdmission

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L115)

```cpp
explicit CompleteAcousticBlockAdmission(std::string expectedStreamId);
```

### API-80f200cfe1af · ndnsf::examples::uav::CompleteAcousticBlockAdmission::admit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L116)

```cpp
AcousticAdmissionResult admit(
    const std::vector<uint8_t>& wire,
    ndn_service_framework::LiveStreamItemProvenance provenance,
    uint64_t receivedTimestampNs);
```

### API-49a19dba532b · ndnsf::examples::uav::CompleteAcousticBlockAdmission::completedCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L120)

```cpp
uint64_t completedCount() const;
```

### API-334567540dcb · ndnsf::examples::uav::CompleteAcousticBlockAdmission::duplicateCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L121)

```cpp
uint64_t duplicateCount() const;
```

### API-d861aa34f984 · ndnsf::examples::uav::CompleteAcousticBlockAdmission::invalidCount

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L122)

```cpp
uint64_t invalidCount() const;
```

### API-65a6cd3b3290 · ndnsf::examples::uav::makeUavTelemetryStreamDefinition

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L141)

```cpp
ndn_service_framework::LiveStreamDefinition
makeUavTelemetryStreamDefinition(const ndn::Name& provider,
                                 uint64_t sessionEpoch,
                                 uint64_t mappingVersion);
```

### API-ca7e1331ab31 · ndnsf::examples::uav::makeUavAcousticStreamDefinition

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavSensorStreams.hpp#L146)

```cpp
ndn_service_framework::LiveStreamDefinition
makeUavAcousticStreamDefinition(const ndn::Name& provider,
                                uint64_t sessionEpoch,
                                uint64_t mappingVersion);
```

## NDNSF-UAV-APP/shared/UavVideoPipeline.hpp

源码 SHA-256：`5f62b4a7fe7a6423a9f687dc1e0a76bf470a097bf10aaac97ce1d2cccf592a79`。

### API-300921b3aaec · ndnsf::examples::uav::UavVideoPipelineState

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L15)

```cpp
enum class UavVideoPipelineState
```

### API-56634ecf4426 · ndnsf::examples::uav::UavVideoPipelineState::Idle

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L17)

```cpp
Idle
```

### API-a8c87d9db501 · ndnsf::examples::uav::UavVideoPipelineState::Running

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L18)

```cpp
Running
```

### API-67ed10217781 · ndnsf::examples::uav::UavVideoPipelineState::Stopped

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L19)

```cpp
Stopped
```

### API-55cf9de6bcda · ndnsf::examples::uav::UavVideoPipelineState::Failed

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L20)

```cpp
Failed
```

### API-324f1e2c3f73 · ndnsf::examples::uav::UavVideoFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L23)

```cpp
struct UavVideoFrame
```

### API-137f420988b0 · ndnsf::examples::uav::UavVideoFrame::sessionEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L25)

```cpp
uint64_t sessionEpoch = 0;
```

### API-4d9a14db0aae · ndnsf::examples::uav::UavVideoFrame::sourceFrameId

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L26)

```cpp
uint64_t sourceFrameId = 0;
```

### API-a68a64359f8d · ndnsf::examples::uav::UavVideoFrame::captureOriginNs

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L27)

```cpp
uint64_t captureOriginNs = 0;
```

### API-38eb95bb89a7 · ndnsf::examples::uav::UavVideoFrame::codecPts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L28)

```cpp
int64_t codecPts = 0;
```

### API-dc45f11ea21d · ndnsf::examples::uav::UavVideoFrame::codecConfigEpoch

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L29)

```cpp
uint64_t codecConfigEpoch = 0;
```

### API-de9676002794 · ndnsf::examples::uav::UavVideoFrame::keyFrame

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L30)

```cpp
bool keyFrame = false;
```

### API-2ef3f6308fa1 · ndnsf::examples::uav::UavVideoFrame::bytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L31)

```cpp
std::vector<uint8_t> bytes;
```

### API-aedf0ff25d21 · ndnsf::examples::uav::UavVideoQueueSnapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L34)

```cpp
struct UavVideoQueueSnapshot
```

### API-81fd000fa615 · ndnsf::examples::uav::UavVideoQueueSnapshot::queuedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L36)

```cpp
size_t queuedFrames = 0;
```

### API-65d0ef0e97f9 · ndnsf::examples::uav::UavVideoQueueSnapshot::queuedBytes

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L37)

```cpp
size_t queuedBytes = 0;
```

### API-f7310f24d7d2 · ndnsf::examples::uav::UavVideoQueueSnapshot::droppedFrames

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L38)

```cpp
uint64_t droppedFrames = 0;
```

### API-ca0e93712982 · ndnsf::examples::uav::UavVideoQueueSnapshot::lastDropReason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L39)

```cpp
std::string lastDropReason;
```

### API-83df2d8090f4 · ndnsf::examples::uav::UavVideoPipelineCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L42)

```cpp
struct UavVideoPipelineCapabilities
```

### API-c9ad6b3d5fc4 · ndnsf::examples::uav::UavVideoPipelineCapabilities::backend

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L44)

```cpp
std::string backend;
```

### API-22718c417aa8 · ndnsf::examples::uav::UavVideoPipelineCapabilities::available

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L45)

```cpp
bool available = false;
```

### API-4281e52512ab · ndnsf::examples::uav::UavVideoPipelineCapabilities::preservesPts

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L46)

```cpp
bool preservesPts = false;
```

### API-aaf06cbe4a4d · ndnsf::examples::uav::UavVideoPipelineCapabilities::boundedLifecycle

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L47)

```cpp
bool boundedLifecycle = false;
```

### API-5563471ec240 · ndnsf::examples::uav::UavVideoPipelineCapabilities::headless

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L48)

```cpp
bool headless = false;
```

### API-d670bc007835 · ndnsf::examples::uav::UavVideoPipelineCapabilities::directPresentationObservation

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L49)

```cpp
bool directPresentationObservation = false;
```

### API-d24ad96a46a8 · ndnsf::examples::uav::UavVideoPipelineCapabilities::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L50)

```cpp
std::string reason;
```

### API-40f5f3c156ae · ndnsf::examples::uav::UavVideoPipelineFailure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L53)

```cpp
struct UavVideoPipelineFailure
```

### API-a52da5bceca3 · ndnsf::examples::uav::UavVideoPipelineFailure::direction

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L55)

```cpp
std::string direction;
```

### API-37df9d486178 · ndnsf::examples::uav::UavVideoPipelineFailure::code

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L56)

```cpp
std::string code;
```

### API-6bdd4cfcb1c2 · ndnsf::examples::uav::UavVideoPipelineFailure::reason

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L57)

```cpp
std::string reason;
```

### API-b3f917d9a858 · ndnsf::examples::uav::BoundedLatestFrameQueue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L60)

```cpp
class BoundedLatestFrameQueue
```

### API-d5f2fcb070fb · ndnsf::examples::uav::BoundedLatestFrameQueue::BoundedLatestFrameQueue

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L63)

```cpp
BoundedLatestFrameQueue(size_t maxFrames, size_t maxBytes);
```

### API-a064d74ab888 · ndnsf::examples::uav::BoundedLatestFrameQueue::push

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L65)

```cpp
bool push(UavVideoFrame frame);
```

### API-11ef609f2477 · ndnsf::examples::uav::BoundedLatestFrameQueue::popLatest

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L66)

```cpp
std::optional<UavVideoFrame> popLatest();
```

### API-a93098781dad · ndnsf::examples::uav::BoundedLatestFrameQueue::clear

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L67)

```cpp
void clear(const std::string& reason = "queue-cleared");
```

### API-3bc244d608c3 · ndnsf::examples::uav::BoundedLatestFrameQueue::snapshot

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L68)

```cpp
UavVideoQueueSnapshot snapshot() const;
```

### API-a627a8e6f598 · ndnsf::examples::uav::LegacyPipeVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L83)

```cpp
class LegacyPipeVideoPipeline
```

### API-86d4f153613d · ndnsf::examples::uav::LegacyPipeVideoPipeline::FrameCallback

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L86)

```cpp
using FrameCallback = std::function<void(const UavVideoFrame&)>;
```

### API-30deb784342c · ndnsf::examples::uav::LegacyPipeVideoPipeline::LegacyPipeVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L88)

```cpp
explicit LegacyPipeVideoPipeline(bool headless);
```

### API-cbd277c39903 · ndnsf::examples::uav::LegacyPipeVideoPipeline::probeCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L90)

```cpp
UavVideoPipelineCapabilities probeCapabilities() const;
```

### API-fb36c8e31e96 · ndnsf::examples::uav::LegacyPipeVideoPipeline::startDecode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L91)

```cpp
void startDecode(FrameCallback callback);
```

### API-d302432bb8a3 · ndnsf::examples::uav::LegacyPipeVideoPipeline::submitAccessUnit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L92)

```cpp
bool submitAccessUnit(const UavVideoFrame& frame);
```

### API-ce2b917d714a · ndnsf::examples::uav::LegacyPipeVideoPipeline::stop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L93)

```cpp
void stop();
```

### API-9dc4f7178369 · ndnsf::examples::uav::LegacyPipeVideoPipeline::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L95)

```cpp
UavVideoPipelineState state() const;
```

### API-33b9a987c73c · ndnsf::examples::uav::LegacyPipeVideoPipeline::isHeadless

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L96)

```cpp
bool isHeadless() const;
```

### API-1897653c86b6 · ndnsf::examples::uav::UavVideoCaptureConfig

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L105)

```cpp
struct UavVideoCaptureConfig
```

### API-6d739f093b46 · ndnsf::examples::uav::UavVideoCaptureConfig::source

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L107)

```cpp
std::string source = "videotestsrc";
```

### API-00a860f55d65 · ndnsf::examples::uav::UavVideoCaptureConfig::width

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L108)

```cpp
uint32_t width = 320;
```

### API-8ab4644f18a6 · ndnsf::examples::uav::UavVideoCaptureConfig::height

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L109)

```cpp
uint32_t height = 240;
```

### API-d2bc0f4f0d24 · ndnsf::examples::uav::UavVideoCaptureConfig::fps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L110)

```cpp
uint32_t fps = 30;
```

### API-762ea9579ce9 · ndnsf::examples::uav::UavVideoCaptureConfig::bitrateKbps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L111)

```cpp
uint32_t bitrateKbps = 2000;
```

### API-a98d13bfc934 · ndnsf::examples::uav::UavVideoCaptureConfig::keyFrameInterval

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L112)

```cpp
uint32_t keyFrameInterval = 30;
```

### API-cb561667b2c2 · ndnsf::examples::uav::UavVideoSampleClassMode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L115)

```cpp
enum class UavVideoSampleClassMode
```

### API-7c60e0f9d23d · ndnsf::examples::uav::UavVideoSampleClassMode::ExactKeyDelta

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L117)

```cpp
ExactKeyDelta
```

### API-2e20ae58323b · ndnsf::examples::uav::UavVideoSampleClassMode::BoundedOpaque

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L118)

```cpp
BoundedOpaque
```

### API-7808d8beb04b · ndnsf::examples::uav::UavVideoSampleClassSchedule

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L121)

```cpp
class UavVideoSampleClassSchedule
```

### API-8b57954c8a56 · ndnsf::examples::uav::UavVideoSampleClassSchedule::exactKeyDelta

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L124)

```cpp
static UavVideoSampleClassSchedule exactKeyDelta(
    uint32_t fps, size_t hardMaxSources, uint64_t sessionGeneration);
```

### API-58e60d946002 · ndnsf::examples::uav::UavVideoSampleClassSchedule::boundedOpaque

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L126)

```cpp
static UavVideoSampleClassSchedule boundedOpaque(
    uint32_t fps, size_t hardMaxSources, uint64_t sessionGeneration);
```

### API-a0e315a60920 · ndnsf::examples::uav::UavVideoSampleClassSchedule::mode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L129)

```cpp
UavVideoSampleClassMode mode() const;
```

### API-b83feb51b2ff · ndnsf::examples::uav::UavVideoSampleClassSchedule::fps

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L130)

```cpp
uint32_t fps() const;
```

### API-004d48419b0e · ndnsf::examples::uav::UavVideoSampleClassSchedule::hardMaxSources

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L131)

```cpp
size_t hardMaxSources() const;
```

### API-e78d32e2466f · ndnsf::examples::uav::UavVideoSampleClassSchedule::sessionGeneration

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L132)

```cpp
uint64_t sessionGeneration() const;
```

### API-176d8439ea4c · ndnsf::examples::uav::UavVideoSampleClassSchedule::hasExactFrameClass

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L133)

```cpp
bool hasExactFrameClass() const;
```

### API-dbcf4cd2f7ff · ndnsf::examples::uav::UavVideoSampleClassSchedule::classFor

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L134)

```cpp
std::string classFor(uint64_t sampleId) const;
```

### API-acba702f967a · ndnsf::examples::uav::UavVideoSampleClassSchedule::matchesActual

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L135)

```cpp
bool matchesActual(uint64_t sampleId, bool keyFrame) const;
```

### API-f3a5b97f53ed · ndnsf::examples::uav::GStreamerVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L149)

```cpp
class GStreamerVideoPipeline
```

### API-834401a43b47 · ndnsf::examples::uav::GStreamerVideoPipeline::FrameCallback

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L152)

```cpp
using FrameCallback = std::function<void(const UavVideoFrame&)>;
```

### API-7785b2837d52 · ndnsf::examples::uav::GStreamerVideoPipeline::GStreamerVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L154)

```cpp
GStreamerVideoPipeline();
```

### API-edf8efbf0a0b · ndnsf::examples::uav::GStreamerVideoPipeline::~GStreamerVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L155)

```cpp
~GStreamerVideoPipeline();
```

### API-e453b771f2d4 · ndnsf::examples::uav::GStreamerVideoPipeline::GStreamerVideoPipeline

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L156)

```cpp
GStreamerVideoPipeline(const GStreamerVideoPipeline&) = delete;
```

### API-03021a16b08e · ndnsf::examples::uav::GStreamerVideoPipeline::operator=

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L157)

```cpp
GStreamerVideoPipeline& operator=(const GStreamerVideoPipeline&) = delete;
```

### API-9a95772bdc09 · ndnsf::examples::uav::GStreamerVideoPipeline::probeCapabilities

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L159)

```cpp
UavVideoPipelineCapabilities probeCapabilities() const;
```

### API-0fdf1c0456af · ndnsf::examples::uav::GStreamerVideoPipeline::startCapture

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L160)

```cpp
void startCapture(const UavVideoCaptureConfig& config, FrameCallback callback);
```

### API-a40b053ea378 · ndnsf::examples::uav::GStreamerVideoPipeline::startDecode

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L161)

```cpp
void startDecode(FrameCallback callback);
```

### API-9fde1e9cfbfb · ndnsf::examples::uav::GStreamerVideoPipeline::submitAccessUnit

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L162)

```cpp
bool submitAccessUnit(const UavVideoFrame& frame);
```

### API-a4d7731a56f6 · ndnsf::examples::uav::GStreamerVideoPipeline::stop

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L163)

```cpp
void stop();
```

### API-12aebb4b4af8 · ndnsf::examples::uav::GStreamerVideoPipeline::state

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L164)

```cpp
UavVideoPipelineState state() const;
```

### API-5d2a9d2450a0 · ndnsf::examples::uav::GStreamerVideoPipeline::failure

public / application-internal；[源码](../../NDNSF-UAV-APP/shared/UavVideoPipeline.hpp#L165)

```cpp
std::optional<UavVideoPipelineFailure> failure() const;
```

## NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py

源码 SHA-256：`873e50bae8bdba8b455eeca55d335b6b3c681b3bbc9fb02fe4e536c43bbba21c`。

### API-740c5fa99eff · digest_bytes

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L31)

```python
def digest_bytes(value: bytes) -> str:
```

### API-99e1ee16dfe2 · digest_text

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L35)

```python
def digest_text(value: str) -> str:
```

### API-d60531abf938 · digest_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L39)

```python
def digest_file(path: Path) -> str:
```

### API-09ebfea64412 · git_value

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L43)

```python
def git_value(*args: str) -> str:
```

### API-ff8b00a51efa · score

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L51)

```python
def score(label: str, truth: str) -> int:
```

### API-e2f7f74de06b · macro_f1

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L55)

```python
def macro_f1(rows: Iterable[Mapping[str, Any]]) -> Optional[float]:
```

原始接口说明：

```text
Compute macro-F1 over observed labels without dropping failed rows.
```

### API-3b5501535c62 · bootstrap_ci

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L76)

```python
def bootstrap_ci(values: Iterable[float], seed: int, replicates: int = 2000) -> Dict[str, Any]:
```

### API-0971f2496b0a · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/evaluate_multiview_recognition.py#L183)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/minindn_multiview_node.py

源码 SHA-256：`eb98c364fac2d7a90db4bb1985ce0541603019ee5e58dab855cd591e150a0de3`。

### API-d24d0bb28ec8 · emit

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L35)

```python
def emit(event: str, **fields: Any) -> None:
```

### API-7547efb8dbbd · digest_bytes

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L39)

```python
def digest_bytes(value: bytes) -> str:
```

### API-5f9799d95427 · load_json

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L43)

```python
def load_json(path: str) -> Any:
```

### API-2193ca2f1ca0 · put

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L47)

```python
def put(app: NDNApp, name: str, content: bytes, freshness_ms: int = 1000) -> None:
```

### API-432f187b36ee · decode_content

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L52)

```python
def decode_content(content: Optional[bytes]) -> Dict[str, Any]:
```

### API-b39939527aa3 · run_controller

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L61)

```python
def run_controller(args: argparse.Namespace) -> int:
```

### API-52f3a694fcf4 · run_producer

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L81)

```python
def run_producer(args: argparse.Namespace) -> int:
```

### API-4eae8805bb8d · run_provider

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L198)

```python
def run_provider(args: argparse.Namespace) -> int:
```

### API-6460fdfaa7c2 · run_coordinator

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L434)

```python
def run_coordinator(args: argparse.Namespace) -> int:
```

### API-56a868ac396f · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/minindn_multiview_node.py#L557)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/multiview_contract.py

源码 SHA-256：`b15b225f8ac061b658224ddd6d976c30b99b498d58f44cd74ae2edcb7cdfbab2`。

### API-3b81051884a0 · sha256_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L33)

```python
def sha256_file(path: Path) -> str:
```

### API-24408f7dea38 · ViewReference

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L46)

```python
class ViewReference:
```

### API-1e3d8710fb15 · ViewReference.view_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L47)

```python
view_id: str
```

### API-2dde8848078b · ViewReference.producer_identity

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L48)

```python
producer_identity: str
```

### API-c71d0656a480 · ViewReference.exact_data_name

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L49)

```python
exact_data_name: str
```

### API-85b412d7e596 · ViewReference.content_digest

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L50)

```python
content_digest: str
```

### API-bcc115812452 · ViewReference.capture_time_ms

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L51)

```python
capture_time_ms: int
```

### API-4b088f3dab25 · ViewReference.target_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L52)

```python
target_id: str
```

### API-54edb6e3518e · ViewReference.media_type

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L53)

```python
media_type: str = "image/png"
```

### API-11e93803be36 · ViewReference.viewpoint

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L54)

```python
viewpoint: str = ""
```

### API-a2ea19538857 · ViewReference.nominal_distance_m

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L55)

```python
nominal_distance_m: float | None = None
```

### API-e97ab8aad068 · ViewReference.to_dict

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L57)

```python
def to_dict(self) -> dict[str, Any]:
```

### API-af30743af694 · ViewReference.from_dict

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L74)

```python
def from_dict(cls, value: Mapping[str, Any]) -> "ViewReference":
```

### API-9c91ced4a8d2 · MultiViewJob

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L93)

```python
class MultiViewJob:
```

### API-b14469cc2256 · MultiViewJob.mission_session_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L94)

```python
mission_session_id: str
```

### API-6f31815fb316 · MultiViewJob.job_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L95)

```python
job_id: str
```

### API-e568de9563f5 · MultiViewJob.attempt

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L96)

```python
attempt: int
```

### API-8003de87c754 · MultiViewJob.target_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L97)

```python
target_id: str
```

### API-117562d19cd1 · MultiViewJob.capture_window_start_ms

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L98)

```python
capture_window_start_ms: int
```

### API-aad56ae08892 · MultiViewJob.capture_window_end_ms

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L99)

```python
capture_window_end_ms: int
```

### API-10c4cc9511d8 · MultiViewJob.views

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L100)

```python
views: list[ViewReference]
```

### API-f64debbf88d1 · MultiViewJob.minimum_views

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L101)

```python
minimum_views: int = 2
```

### API-3971d159232d · MultiViewJob.minimum_distinct_producers

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L102)

```python
minimum_distinct_producers: int = 2
```

### API-7182b09e4253 · MultiViewJob.model_profile_id

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L103)

```python
model_profile_id: str = "vehicle-mvcnn-v1"
```

### API-ad706ff1b9bb · MultiViewJob.deadline_ms

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L104)

```python
deadline_ms: int = 5000
```

### API-d0446ee8e1c6 · MultiViewJob.to_dict

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L106)

```python
def to_dict(self) -> dict[str, Any]:
```

### API-1468fc8178e1 · validate_view

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L125)

```python
def validate_view(view: ViewReference, root: Path | None = None) -> list[str]:
```

### API-6ef6987d8ddd · validate_job

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L154)

```python
def validate_job(job: MultiViewJob, profile: Mapping[str, Any] | None = None) -> list[str]:
```

### API-98c94e7e667f · validate_fixture_manifest

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L211)

```python
def validate_fixture_manifest(manifest: Mapping[str, Any], fixture_root: Path) -> list[str]:
```

### API-95d485990c8d · load_json

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L246)

```python
def load_json(path: Path) -> dict[str, Any]:
```

### API-d2f8cfc1a094 · dump_json

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L251)

```python
def dump_json(path: Path, value: Mapping[str, Any]) -> None:
```

### API-8baa733eee0f · validate_result

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_contract.py#L256)

```python
def validate_result(result: Mapping[str, Any], job: MultiViewJob | None = None,
                    profile: Mapping[str, Any] | None = None) -> list[str]:
```

原始接口说明：

```text
Validate compact worker output and annotation completeness.
```

## NDNSF-UAV-APP/tools/multiview_recognition_worker.py

源码 SHA-256：`eefad99c0ca5626d491d5d98fcb10184de084e4624d286ddbad41dc243704c8c`。

### API-ac9e9b31344d · digest_bytes

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L51)

```python
def digest_bytes(value: bytes) -> str:
```

### API-30fcf3d2ac63 · digest_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L55)

```python
def digest_file(path: Path) -> str:
```

### API-8295c7a98c79 · Detector

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L87)

```python
class Detector:
```

### API-e11085531212 · Detector.__init__

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L88)

```python
def __init__(self, model_path: Path | None, confidence: float) -> None:
```

### API-77bd971e7d6a · Detector.detect

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L99)

```python
def detect(self, image_path: Path) -> tuple[tuple[int, int, int, int], float, str]:
```

### API-59a34f45f9fd · run

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L151)

```python
def run(args: argparse.Namespace) -> dict[str, Any]:
```

### API-68740ebeb684 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/multiview_recognition_worker.py#L259)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/mvcnn_model.py

源码 SHA-256：`4a8610c47ed89159293cf5b36939360ca257b9cf391b0691add652d45ce05a76`。

### API-7a960de23c69 · TinyMVCNN

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_model.py#L16)

```python
class TinyMVCNN(nn.Module):
```

原始接口说明：

```text
A compact MVCNN suitable for deterministic CPU qualification.
```

### API-ecf52f8f78e7 · TinyMVCNN.__init__

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_model.py#L19)

```python
def __init__(self, class_count: int = 3, feature_dim: int = 32) -> None:
```

### API-0f1b38e18d59 · TinyMVCNN.forward

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_model.py#L32)

```python
def forward(self, images: Tensor, view_mask: Tensor) -> tuple[Tensor, Tensor]:
```

### API-4a3f0104e50d · make_model

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_model.py#L50)

```python
def make_model(class_count: int = 3, feature_dim: int = 32) -> TinyMVCNN:
```

## NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py

源码 SHA-256：`4a8b2025d3fe27e0613854b2534187fe0d44a0a1275a78b025817b19e02017ba`。

### API-cc6d6ef5b642 · digest_bytes

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L34)

```python
def digest_bytes(value: bytes) -> str:
```

### API-d07b0c47529a · digest_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L38)

```python
def digest_file(path: Path) -> str:
```

### API-7b7302d16e5d · load_json

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L46)

```python
def load_json(path: Path) -> dict[str, Any]:
```

### API-bd8fd13a2489 · load_profile

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L53)

```python
def load_profile(registry_path: Path, profile_id: str) -> dict[str, Any]:
```

### API-024a302d95cb · resolve_artifact

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L63)

```python
def resolve_artifact(profile: Mapping[str, Any], model: str) -> Path:
```

### API-661669f86f7c · run_real_manifest

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L127)

```python
def run_real_manifest(manifest: Mapping[str, Any], output: Path, *, model: str = "",
                      model_digest: str = "", profile_id: str = "vehicle-mvcnn-v1",
                      registry: Path = DEFAULT_REGISTRY, provider: str = "/provider/cpu",
                      mission_id: str = "mission-uav-mv", job_id: str = "recognition-001",
                      attempt: int = 1, minimum_views: int = 2,
                      paired_baseline: bool = False) -> dict[str, Any]:
```

原始接口说明：

```text
Execute one verified local materialization through the real ONNX graph.
```

### API-ab935bc09ac3 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/mvcnn_onnx_worker.py#L261)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/prepare_coperception_uav.py

源码 SHA-256：`f66f338767a007a57c16c46fe5ef3b3a3a2a517d5d307a1de8bb9f04b2d1011b`。

### API-171f71022a84 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_coperception_uav.py#L20)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py

源码 SHA-256：`1f3c4dd93203c2578a7676d372fbf4ccd9c70d5ecd67fbef277f5d6794717452`。

### API-377fa76dd75a · deg2tile

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py#L25)

```python
def deg2tile(lat: float, lon: float, zoom: int) -> tuple[int, int]:
```

### API-1f3011213b79 · download_tile

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py#L33)

```python
def download_tile(zoom: int, x: int, y: int, output_root: Path, force: bool) -> bool:
```

### API-3a6eb5c6199a · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_memphis_offline_map.py#L52)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py

源码 SHA-256：`bbdaeaf63db5b60d0c44a98390645f59603f1b761a641239ab5bf0bfbac620a0`。

### API-adbfd2b0bfaf · digest_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L33)

```python
def digest_file(path: Path) -> str:
```

### API-15950cea4f7c · source_digest

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L41)

```python
def source_digest() -> str:
```

### API-4e89ef945d5a · render

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L45)

```python
def render(class_index: int, sample_index: int, view_index: int) -> torch.Tensor:
```

原始接口说明：

```text
Render one deterministic RGB view with class-specific geometry.
```

### API-e42690bdc9a1 · make_native_dataset

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L75)

```python
def make_native_dataset() -> tuple[torch.Tensor, torch.Tensor]:
```

### API-005403eba8a3 · train_subject

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L106)

```python
def train_subject() -> tuple[torch.nn.Module, dict[str, Any]]:
```

### API-fbebaba84ac8 · export_subject

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L134)

```python
def export_subject(model: torch.nn.Module, output: Path) -> None:
```

### API-6c03b5879cce · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py#L148)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/run_multiview_fixture.py

源码 SHA-256：`fe01a89f40264ba1f3e012cc5a7c1f6aad66b9a0a9efb44554992ba91956259b`。

### API-af0fbd5094d1 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_multiview_fixture.py#L15)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py

源码 SHA-256：`a84ba479f41f58caf83999680e3672291a4cee2f3fe86679a8b9e7f23a06a49a`。

### API-402e35dc9b3e · load_json

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L38)

```python
def load_json(path: Path) -> Dict[str, Any]:
```

### API-7ae002f945aa · model_profile

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L45)

```python
def model_profile() -> Dict[str, Any]:
```

### API-fb8b638f7daf · topology_nodes

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L53)

```python
def topology_nodes(path: Path) -> List[str]:
```

### API-257f6bb2806b · preflight

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L68)

```python
def preflight() -> Dict[str, Any]:
```

### API-f200fcbe4b90 · dry_trace

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L113)

```python
def dry_trace(manifest: Mapping[str, Any], provider: str, scenario: str) -> List[Dict[str, Any]]:
```

### API-2afe5124e895 · run_real

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L342)

```python
def run_real(output: Path, scenario: str, selected_provider: str,
             lifetime_ms: int, late_delay_ms: int,
             model_mode: str = "real") -> Dict[str, Any]:
```

### API-e218476ae525 · shell_quote

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L517)

```python
def shell_quote(value: str) -> str:
```

### API-a53cac41aeb9 · shlex_quote

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L522)

```python
def shlex_quote(value: str) -> str:
```

### API-757d4d339d16 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py#L526)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py

源码 SHA-256：`a64fe65bee0ffaefb952800333b3dbbea1551077d1a85d7dcd60c9bacc58e6e8`。

### API-5a379903675d · sha256_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L34)

```python
def sha256_file(path: Path) -> str:
```

### API-246b510b17d5 · command_output

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L42)

```python
def command_output(*args: str, cwd: Path | None = None) -> str:
```

### API-3e2045f3b4e8 · git_value

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L51)

```python
def git_value(repo: Path, *args: str) -> str:
```

### API-6956eb8ce130 · preflight

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L55)

```python
def preflight(repo: Path, px4: Path, output: Path) -> tuple[dict, list[str]]:
```

### API-3ae21a42b1cd · run

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L107)

```python
def run(repo: Path, px4: Path, output: Path, timeout_seconds: int) -> int:
```

### API-42581287faec · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/run_uav_px4_sitl_scenario.py#L196)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/uav_deployment_check.py

源码 SHA-256：`8b2068a16c932846a5c76d673ed079604f0b978ab65c3649074da34f9ea24d56`。

### API-fdfa2dc91fc6 · run

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L22)

```python
def run(cmd: list[str], timeout: int = 10) -> subprocess.CompletedProcess[str]:
```

### API-3435636538eb · read_fields

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L27)

```python
def read_fields(path: Path) -> dict[str, str]:
```

### API-0a9ecb5dd479 · resolve_path

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L41)

```python
def resolve_path(path_text: str, base: Path) -> Path:
```

### API-3c417239ab0f · parse_expected_cert

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L48)

```python
def parse_expected_cert(value: str) -> tuple[str, Path]:
```

### API-bf17470f2fa1 · Reporter

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L57)

```python
class Reporter:
```

### API-196ec33a0bfe · Reporter.__init__

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L58)

```python
def __init__(self) -> None:
```

### API-2fc8552aa9db · Reporter.ok

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L62)

```python
def ok(self, message: str) -> None:
```

### API-27408c5d6534 · Reporter.warn

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L65)

```python
def warn(self, message: str) -> None:
```

### API-d94e1deeedde · Reporter.fail

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L69)

```python
def fail(self, message: str) -> None:
```

### API-e5ce2ffb26a9 · check_binary

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L74)

```python
def check_binary(reporter: Reporter, name: str, required: bool = True) -> None:
```

### API-08c044558b07 · check_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L83)

```python
def check_file(reporter: Reporter, path_text: str, label: str,
               required: bool = True) -> Path:
```

### API-2ea015b18108 · check_nfd

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L95)

```python
def check_nfd(reporter: Reporter) -> None:
```

### API-0e703a93c827 · get_ndnsec_list

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L110)

```python
def get_ndnsec_list(reporter: Reporter) -> str:
```

### API-f5a25c02c3ac · cert_name_from_file

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L128)

```python
def cert_name_from_file(cert_file: Path) -> str:
```

### API-ce605f33916b · default_cert_name

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L145)

```python
def default_cert_name(identity: str) -> str:
```

### API-db7436ed290c · identity_key_names

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L160)

```python
def identity_key_names(identity: str, ndnsec_list: str) -> set[str]:
```

### API-e0ec06707226 · check_identity

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L168)

```python
def check_identity(reporter: Reporter, identity: str, ndnsec_list: str,
                   allow_multiple_certs: bool = False,
                   expected_cert_file: Path | None = None,
                   required: bool = True) -> None:
```

### API-7cda7e96cfa5 · identities_from_policy

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L208)

```python
def identities_from_policy(policy_file: Path) -> set[str]:
```

### API-d6f48e742d6d · check_trust_schema

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L221)

```python
def check_trust_schema(reporter: Reporter, trust_schema: Path) -> None:
```

### API-e98b69d86a39 · check_yolo

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L231)

```python
def check_yolo(reporter: Reporter, model: str, worker_script: str) -> None:
```

### API-7489041ae1f2 · check_udp_port

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L263)

```python
def check_udp_port(reporter: Reporter, port_text: str, label: str) -> None:
```

### API-f0a161ecf36f · check_serial_device

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L279)

```python
def check_serial_device(reporter: Reporter, device_text: str, baud_text: str) -> None:
```

### API-dcd9b3800c12 · check_recording_config

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L303)

```python
def check_recording_config(reporter: Reporter, fields: dict[str, str]) -> None:
```

### API-5c653b0a7f87 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/uav_deployment_check.py#L324)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/yolo_detect_once.py

源码 SHA-256：`d2d20e010d1cd4483e70a81b5433c30767997c8b97e4dad61e69a6016028557a`。

### API-69d2ec466083 · encode_fields

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/yolo_detect_once.py#L16)

```python
def encode_fields(fields: dict[str, str]) -> str:
```

### API-542dcd00a132 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/yolo_detect_once.py#L20)

```python
def main() -> int:
```

## NDNSF-UAV-APP/tools/yolo_detect_worker.py

源码 SHA-256：`67a159e18e4d1d7692576fa8a7b85b03fce1fee82cdfe139fde11183e0f55abd`。

### API-6045c5d940d6 · encode_fields

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/yolo_detect_worker.py#L17)

```python
def encode_fields(fields: dict[str, str]) -> str:
```

### API-c511cfda7cec · detect

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/yolo_detect_worker.py#L21)

```python
def detect(model, image: Path, wanted: set[str], conf: float) -> dict[str, str]:
```

### API-7406ec926b87 · main

public-by-name / application-internal；[源码](../../NDNSF-UAV-APP/tools/yolo_detect_worker.py#L60)

```python
def main() -> int:
```

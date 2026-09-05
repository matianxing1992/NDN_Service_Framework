#ifndef NDNSF_EXAMPLES_UAV_MULTI_VIEW_RECOGNITION_HPP
#define NDNSF_EXAMPLES_UAV_MULTI_VIEW_RECOGNITION_HPP

#include "UavProtocol.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

inline constexpr size_t UAV_MULTIVIEW_MIN_VIEWS = 2;
inline constexpr size_t UAV_MULTIVIEW_MAX_VIEWS = 6;
inline constexpr size_t UAV_MULTIVIEW_MAX_WIRE_BYTES = 16384;

enum class MultiViewTerminalStatus
{
  Completed,
  InsufficientViews,
  ValidationFailed,
  CorrelationFailed,
  UnsupportedModelProfile,
  InferenceFailed,
  AnnotationPublicationFailed,
  DeliveryTimeout,
  Cancelled,
};

const char* to_string(MultiViewTerminalStatus status) noexcept;
std::optional<MultiViewTerminalStatus>
parseMultiViewTerminalStatus(const std::string& value);

struct ViewEvidenceReference
{
  std::string viewId;
  ndn::Name producerIdentity;
  ndn::Name exactDataName;
  std::string contentDigest;
  uint64_t captureTimeMs = 0;
  std::string targetId;
  std::string mediaType = "image/png";
  std::string viewpoint;
  std::optional<double> nominalDistanceM;

  bool isValid(std::string* reason = nullptr) const;
  Fields toFields(const std::string& prefix = {}) const;
  static ViewEvidenceReference fromFields(const Fields& fields,
                                          const std::string& prefix = {});
};

struct MultiViewModelProfile
{
  std::string profileId;
  std::string algorithmId;
  std::string modelId;
  std::string modelDigest;
  std::string preprocessingProfile;
  std::string poolingOperator = "elementwise-max/v1";
  std::string deviceClass = "cpu-or-cuda";
  std::vector<std::string> supportedMedia{"image/png", "image/jpeg"};
  size_t minimumViews = UAV_MULTIVIEW_MIN_VIEWS;
  size_t maximumViews = UAV_MULTIVIEW_MAX_VIEWS;
  size_t minimumDistinctProducers = 2;
  bool requiresCalibration = false;

  bool isValid(std::string* reason = nullptr) const;
};

struct MultiViewRecognitionJob
{
  uint64_t schemaVersion = 1;
  std::string missionSessionId;
  std::string jobId;
  uint64_t attempt = 1;
  std::string targetId;
  uint64_t captureWindowStartMs = 0;
  uint64_t captureWindowEndMs = 0;
  std::vector<ViewEvidenceReference> views;
  size_t minimumViews = UAV_MULTIVIEW_MIN_VIEWS;
  size_t minimumDistinctProducers = 2;
  std::string modelProfileId;
  uint64_t deadlineMs = 5000;

  bool isValid(const MultiViewModelProfile* profile = nullptr,
               std::string* reason = nullptr) const;
  Fields toFields() const;
  static MultiViewRecognitionJob fromFields(const Fields& fields);
  ndn::Buffer wireEncode() const;
  static MultiViewRecognitionJob wireDecode(const ndn::Buffer& wire);
};

struct AnnotatedViewReference
{
  std::string viewId;
  std::string sourceDigest;
  ndn::Name exactDataName;
  std::string contentDigest;
  std::string mediaType = "image/png";
  ndn::Name signerIdentity;

  bool isValid(std::string* reason = nullptr) const;
  Fields toFields(const std::string& prefix = {}) const;
  static AnnotatedViewReference fromFields(const Fields& fields,
                                           const std::string& prefix = {});
};

struct FusedRecognitionResult
{
  uint64_t schemaVersion = 1;
  std::string missionSessionId;
  std::string jobId;
  uint64_t attempt = 1;
  MultiViewTerminalStatus status = MultiViewTerminalStatus::InferenceFailed;
  std::string fusedLabel;
  double confidence = 0.0;
  std::string profileId;
  std::string algorithmId;
  std::string modelDigest;
  std::vector<std::string> contributingViewIds;
  std::vector<std::string> rejectedViewIds;
  std::string poolingOperator;
  size_t consumedViewCount = 0;
  std::string pooledFeatureDigest;
  ndn::Name resultManifestName;
  std::string resultManifestDigest;
  std::vector<AnnotatedViewReference> annotatedViews;
  std::string failureStage;
  std::string failureReason;
  uint64_t elapsedMs = 0;
  uint64_t bytesTransferred = 0;
  ndn::Name terminalOwner;

  bool isValid(const MultiViewRecognitionJob* job = nullptr,
               const MultiViewModelProfile* profile = nullptr,
               std::string* reason = nullptr) const;
  Fields toFields() const;
  static FusedRecognitionResult fromFields(const Fields& fields);
  ndn::Buffer wireEncode() const;
  static FusedRecognitionResult wireDecode(const ndn::Buffer& wire);
};

struct VerifiedMultiViewInput
{
  ViewEvidenceReference reference;
  ndn::Buffer content;
  ndn::Name signerIdentity;
  bool nameVerified = false;
  bool signatureVerified = false;
  bool digestVerified = false;

  bool isValid(std::string* reason = nullptr) const;
};

struct MultiViewExecutionResult
{
  bool success = false;
  FusedRecognitionResult result;
  std::vector<VerifiedMultiViewInput> acceptedViews;
  std::vector<std::string> rejectedViewIds;
  std::string failureStage;
  std::string detail;
};

class IMultiViewRecognitionAlgorithm
{
public:
  virtual ~IMultiViewRecognitionAlgorithm() = default;
  virtual FusedRecognitionResult execute(
    const MultiViewRecognitionJob& job,
    const MultiViewModelProfile& profile,
    const std::vector<VerifiedMultiViewInput>& views,
    const ndn::Name& terminalOwner) const = 0;
};

/** Deterministic CPU implementation used by unit/integration gates. */
class DeterministicMultiViewAlgorithm final : public IMultiViewRecognitionAlgorithm
{
public:
  FusedRecognitionResult execute(
    const MultiViewRecognitionJob& job,
    const MultiViewModelProfile& profile,
    const std::vector<VerifiedMultiViewInput>& views,
    const ndn::Name& terminalOwner) const override;
};

bool
validateMultiViewJobAndViews(const MultiViewRecognitionJob& job,
                             const MultiViewModelProfile& profile,
                             const std::vector<VerifiedMultiViewInput>& views,
                             std::vector<std::string>* rejectedViewIds = nullptr,
                             std::string* reason = nullptr);

MultiViewExecutionResult
executeMultiViewJob(const MultiViewRecognitionJob& job,
                    const MultiViewModelProfile& profile,
                    const std::vector<VerifiedMultiViewInput>& views,
                    const ndn::Name& terminalOwner,
                    const IMultiViewRecognitionAlgorithm& algorithm =
                      DeterministicMultiViewAlgorithm{});

} // namespace ndnsf::examples::uav

#endif // NDNSF_EXAMPLES_UAV_MULTI_VIEW_RECOGNITION_HPP

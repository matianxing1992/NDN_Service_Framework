#include "UavMultiViewRecognition.hpp"
#include "UavNames.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <sstream>
#include <stdexcept>

namespace ndnsf::examples::uav {

namespace {

bool
fail(std::string* reason, const std::string& message)
{
  if (reason) {
    *reason = message;
  }
  return false;
}

std::string
field(const Fields& fields, const std::string& key,
      const std::string& fallback = {})
{
  const auto it = fields.find(key);
  return it == fields.end() ? fallback : it->second;
}

uint64_t
uintField(const Fields& fields, const std::string& key, uint64_t fallback = 0)
{
  const auto value = field(fields, key);
  if (value.empty()) return fallback;
  try {
    return std::stoull(value);
  }
  catch (const std::exception&) {
    return fallback;
  }
}

size_t
sizeField(const Fields& fields, const std::string& key, size_t fallback = 0)
{
  return static_cast<size_t>(uintField(fields, key, fallback));
}

std::string
sha256(const std::string& value)
{
  ndn::util::Sha256 digest;
  digest << value;
  return "sha256:" + digest.toString();
}

std::string
sha256(const ndn::Buffer& value)
{
  return sha256(std::string(reinterpret_cast<const char*>(value.data()), value.size()));
}

bool
isDigest(const std::string& value)
{
  return value.size() == 71 && value.compare(0, 7, "sha256:") == 0 &&
         std::all_of(value.begin() + 7, value.end(), [] (unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                  (c >= 'A' && c <= 'F');
         });
}

std::string
wireString(const ndn::Buffer& wire)
{
  return std::string(reinterpret_cast<const char*>(wire.data()), wire.size());
}

} // namespace

const char*
to_string(MultiViewTerminalStatus status) noexcept
{
  switch (status) {
  case MultiViewTerminalStatus::Completed: return "completed";
  case MultiViewTerminalStatus::InsufficientViews: return "insufficient-views";
  case MultiViewTerminalStatus::ValidationFailed: return "validation-failed";
  case MultiViewTerminalStatus::CorrelationFailed: return "correlation-failed";
  case MultiViewTerminalStatus::UnsupportedModelProfile: return "unsupported-model-profile";
  case MultiViewTerminalStatus::InferenceFailed: return "inference-failed";
  case MultiViewTerminalStatus::AnnotationPublicationFailed: return "annotation-publication-failed";
  case MultiViewTerminalStatus::DeliveryTimeout: return "delivery-timeout";
  case MultiViewTerminalStatus::Cancelled: return "cancelled";
  }
  return "inference-failed";
}

std::optional<MultiViewTerminalStatus>
parseMultiViewTerminalStatus(const std::string& value)
{
  const std::array<std::pair<const char*, MultiViewTerminalStatus>, 9> values{{
    {"completed", MultiViewTerminalStatus::Completed},
    {"insufficient-views", MultiViewTerminalStatus::InsufficientViews},
    {"validation-failed", MultiViewTerminalStatus::ValidationFailed},
    {"correlation-failed", MultiViewTerminalStatus::CorrelationFailed},
    {"unsupported-model-profile", MultiViewTerminalStatus::UnsupportedModelProfile},
    {"inference-failed", MultiViewTerminalStatus::InferenceFailed},
    {"annotation-publication-failed", MultiViewTerminalStatus::AnnotationPublicationFailed},
    {"delivery-timeout", MultiViewTerminalStatus::DeliveryTimeout},
    {"cancelled", MultiViewTerminalStatus::Cancelled},
  }};
  for (const auto& item : values) {
    if (value == item.first) return item.second;
  }
  return std::nullopt;
}

bool
ViewEvidenceReference::isValid(std::string* reason) const
{
  if (viewId.empty() || producerIdentity.empty() || exactDataName.empty() ||
      targetId.empty() || mediaType.empty()) {
    return fail(reason, "view reference has missing identity fields");
  }
  if (!producerIdentity.isPrefixOf(exactDataName) ||
      exactDataName.toUri().find("://") != std::string::npos ||
      exactDataName.toUri().find(':') != std::string::npos) {
    return fail(reason, "view reference is not a producer-owned NDN name");
  }
  if (!isDigest(contentDigest)) {
    return fail(reason, "view reference has an invalid content digest");
  }
  if (mediaType != "image/png" && mediaType != "image/jpeg") {
    return fail(reason, "view reference media type is unsupported");
  }
  return true;
}

Fields
ViewEvidenceReference::toFields(const std::string& prefix) const
{
  const auto p = prefix.empty() ? std::string() : prefix + ".";
  Fields result{
    {p + "view_id", viewId},
    {p + "producer", producerIdentity.toUri()},
    {p + "exact_name", exactDataName.toUri()},
    {p + "content_digest", contentDigest},
    {p + "capture_time_ms", std::to_string(captureTimeMs)},
    {p + "target_id", targetId},
    {p + "media_type", mediaType},
    {p + "viewpoint", viewpoint},
  };
  if (nominalDistanceM) {
    result[p + "nominal_distance_m"] = std::to_string(*nominalDistanceM);
  }
  return result;
}

ViewEvidenceReference
ViewEvidenceReference::fromFields(const Fields& fields, const std::string& prefix)
{
  const auto p = prefix.empty() ? std::string() : prefix + ".";
  ViewEvidenceReference result;
  result.viewId = field(fields, p + "view_id");
  result.producerIdentity = ndn::Name(field(fields, p + "producer"));
  result.exactDataName = ndn::Name(field(fields, p + "exact_name"));
  result.contentDigest = field(fields, p + "content_digest");
  result.captureTimeMs = uintField(fields, p + "capture_time_ms");
  result.targetId = field(fields, p + "target_id");
  result.mediaType = field(fields, p + "media_type", "image/png");
  result.viewpoint = field(fields, p + "viewpoint");
  const auto distance = field(fields, p + "nominal_distance_m");
  if (!distance.empty()) {
    try { result.nominalDistanceM = std::stod(distance); }
    catch (const std::exception&) { result.nominalDistanceM.reset(); }
  }
  return result;
}

bool
MultiViewModelProfile::isValid(std::string* reason) const
{
  if (profileId.empty() || algorithmId.empty() || modelId.empty() ||
      !isDigest(modelDigest) || preprocessingProfile.empty() ||
      poolingOperator.empty() || supportedMedia.empty()) {
    return fail(reason, "model profile is incomplete");
  }
  if (minimumViews < UAV_MULTIVIEW_MIN_VIEWS || maximumViews < minimumViews ||
      maximumViews > UAV_MULTIVIEW_MAX_VIEWS || minimumDistinctProducers < 1 ||
      minimumDistinctProducers > minimumViews) {
    return fail(reason, "model profile view bounds are invalid");
  }
  return true;
}

bool
MultiViewRecognitionJob::isValid(const MultiViewModelProfile* profile,
                                  std::string* reason) const
{
  if (schemaVersion != 1 || missionSessionId.empty() || jobId.empty() ||
      attempt == 0 || targetId.empty() || modelProfileId.empty() || deadlineMs == 0) {
    return fail(reason, "multi-view job identity or deadline is incomplete");
  }
  if (captureWindowStartMs > captureWindowEndMs) {
    return fail(reason, "multi-view capture window is inverted");
  }
  if (minimumViews < UAV_MULTIVIEW_MIN_VIEWS || minimumViews > UAV_MULTIVIEW_MAX_VIEWS ||
      minimumDistinctProducers == 0 || minimumDistinctProducers > minimumViews ||
      views.size() > UAV_MULTIVIEW_MAX_VIEWS) {
    return fail(reason, "multi-view job policy is outside the bounded range");
  }
  std::set<std::string> viewIds;
  std::set<std::string> names;
  std::set<std::string> producers;
  for (const auto& view : views) {
    std::string viewReason;
    if (!view.isValid(&viewReason)) return fail(reason, viewReason);
    if (!viewIds.insert(view.viewId).second ||
        !names.insert(view.exactDataName.toUri()).second) {
      return fail(reason, "duplicate multi-view reference");
    }
    producers.insert(view.producerIdentity.toUri());
    if (view.targetId != targetId) return fail(reason, "cross-target view reference");
    if (view.captureTimeMs < captureWindowStartMs || view.captureTimeMs > captureWindowEndMs) {
      return fail(reason, "view capture time is outside the job window");
    }
  }
  if (profile) {
    std::string profileReason;
    if (!profile->isValid(&profileReason)) return fail(reason, profileReason);
    if (modelProfileId != profile->profileId || minimumViews < profile->minimumViews ||
        views.size() > profile->maximumViews ||
        minimumDistinctProducers < profile->minimumDistinctProducers) {
      return fail(reason, "job does not match the registered model profile");
    }
    for (const auto& view : views) {
      if (std::find(profile->supportedMedia.begin(), profile->supportedMedia.end(),
                    view.mediaType) == profile->supportedMedia.end()) {
        return fail(reason, "job contains media unsupported by the model profile");
      }
    }
  }
  if (views.size() < minimumViews) {
    return fail(reason, "multi-view job does not contain the minimum number of views");
  }
  if (producers.size() < minimumDistinctProducers) {
    return fail(reason, "multi-view job does not contain enough distinct producers");
  }
  return true;
}

Fields
MultiViewRecognitionJob::toFields() const
{
  Fields result{
    {"schema_version", std::to_string(schemaVersion)},
    {"mission_session_id", missionSessionId},
    {"job_id", jobId},
    {"attempt", std::to_string(attempt)},
    {"target_id", targetId},
    {"window_start_ms", std::to_string(captureWindowStartMs)},
    {"window_end_ms", std::to_string(captureWindowEndMs)},
    {"minimum_views", std::to_string(minimumViews)},
    {"minimum_distinct_producers", std::to_string(minimumDistinctProducers)},
    {"model_profile_id", modelProfileId},
    {"deadline_ms", std::to_string(deadlineMs)},
    {"view_count", std::to_string(views.size())},
  };
  for (size_t i = 0; i < views.size(); ++i) {
    const auto fields = views[i].toFields("view." + std::to_string(i));
    result.insert(fields.begin(), fields.end());
  }
  return result;
}

MultiViewRecognitionJob
MultiViewRecognitionJob::fromFields(const Fields& fields)
{
  MultiViewRecognitionJob result;
  result.schemaVersion = uintField(fields, "schema_version", 1);
  result.missionSessionId = field(fields, "mission_session_id");
  result.jobId = field(fields, "job_id");
  result.attempt = uintField(fields, "attempt", 1);
  result.targetId = field(fields, "target_id");
  result.captureWindowStartMs = uintField(fields, "window_start_ms");
  result.captureWindowEndMs = uintField(fields, "window_end_ms");
  result.minimumViews = sizeField(fields, "minimum_views", UAV_MULTIVIEW_MIN_VIEWS);
  result.minimumDistinctProducers = sizeField(fields, "minimum_distinct_producers", 2);
  result.modelProfileId = field(fields, "model_profile_id");
  result.deadlineMs = uintField(fields, "deadline_ms", 5000);
  const auto count = sizeField(fields, "view_count");
  if (count > UAV_MULTIVIEW_MAX_VIEWS) throw std::invalid_argument("multi-view wire view bound exceeded");
  for (size_t i = 0; i < count; ++i) {
    result.views.push_back(ViewEvidenceReference::fromFields(
      fields, "view." + std::to_string(i)));
  }
  return result;
}

ndn::Buffer
MultiViewRecognitionJob::wireEncode() const
{
  const auto encoded = encodeFields(toFields());
  if (encoded.size() > UAV_MULTIVIEW_MAX_WIRE_BYTES) {
    throw std::invalid_argument("multi-view job wire exceeds bounded size");
  }
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
}

MultiViewRecognitionJob
MultiViewRecognitionJob::wireDecode(const ndn::Buffer& wire)
{
  if (wire.empty() || wire.size() > UAV_MULTIVIEW_MAX_WIRE_BYTES) {
    throw std::invalid_argument("invalid multi-view job wire size");
  }
  return fromFields(decodeFields(wireString(wire)));
}

bool
AnnotatedViewReference::isValid(std::string* reason) const
{
  if (viewId.empty() || sourceDigest.empty() || exactDataName.empty() ||
      !isDigest(contentDigest) || signerIdentity.empty() || mediaType.empty()) {
    return fail(reason, "annotated view reference is incomplete");
  }
  return true;
}

Fields
AnnotatedViewReference::toFields(const std::string& prefix) const
{
  const auto p = prefix.empty() ? std::string() : prefix + ".";
  return {
    {p + "view_id", viewId},
    {p + "source_digest", sourceDigest},
    {p + "exact_name", exactDataName.toUri()},
    {p + "content_digest", contentDigest},
    {p + "media_type", mediaType},
    {p + "signer", signerIdentity.toUri()},
  };
}

AnnotatedViewReference
AnnotatedViewReference::fromFields(const Fields& fields, const std::string& prefix)
{
  const auto p = prefix.empty() ? std::string() : prefix + ".";
  AnnotatedViewReference result;
  result.viewId = field(fields, p + "view_id");
  result.sourceDigest = field(fields, p + "source_digest");
  result.exactDataName = ndn::Name(field(fields, p + "exact_name"));
  result.contentDigest = field(fields, p + "content_digest");
  result.mediaType = field(fields, p + "media_type", "image/png");
  result.signerIdentity = ndn::Name(field(fields, p + "signer"));
  return result;
}

bool
FusedRecognitionResult::isValid(const MultiViewRecognitionJob* job,
                                 const MultiViewModelProfile* profile,
                                 std::string* reason) const
{
  if (schemaVersion != 1 || missionSessionId.empty() || jobId.empty() || attempt == 0 ||
      terminalOwner.empty() || !isDigest(modelDigest)) {
    return fail(reason, "fused result provenance is incomplete");
  }
  if (job && (missionSessionId != job->missionSessionId || jobId != job->jobId ||
              attempt != job->attempt)) {
    return fail(reason, "fused result does not match the request job");
  }
  if (profile && (profileId != profile->profileId || algorithmId != profile->algorithmId ||
                  modelDigest != profile->modelDigest || poolingOperator != profile->poolingOperator)) {
    return fail(reason, "fused result model provenance does not match profile");
  }
  if (status != MultiViewTerminalStatus::Completed) {
    if (failureStage.empty() || failureReason.empty()) return fail(reason, "failed result lacks failure detail");
    return true;
  }
  if (fusedLabel.empty() || confidence < 0.0 || confidence > 1.0 ||
      consumedViewCount < UAV_MULTIVIEW_MIN_VIEWS ||
      consumedViewCount != contributingViewIds.size() ||
      consumedViewCount != annotatedViews.size() || !isDigest(pooledFeatureDigest) ||
      resultManifestName.empty() || !isDigest(resultManifestDigest)) {
    return fail(reason, "completed fused result is incomplete");
  }
  std::set<std::string> ids(contributingViewIds.begin(), contributingViewIds.end());
  if (ids.size() != contributingViewIds.size()) return fail(reason, "duplicate contributing view");
  for (const auto& annotation : annotatedViews) {
    std::string annotationReason;
    if (!annotation.isValid(&annotationReason)) return fail(reason, annotationReason);
    if (ids.count(annotation.viewId) == 0) return fail(reason, "annotation is not bound to a contributing view");
  }
  if (job && consumedViewCount < job->minimumViews) return fail(reason, "result violates minimum view policy");
  if (profile && consumedViewCount > profile->maximumViews) return fail(reason, "result exceeds profile view bound");
  return true;
}

Fields
FusedRecognitionResult::toFields() const
{
  Fields result{
    {"schema_version", std::to_string(schemaVersion)},
    {"mission_session_id", missionSessionId},
    {"job_id", jobId},
    {"attempt", std::to_string(attempt)},
    {"status", to_string(status)},
    {"fused_label", fusedLabel},
    {"confidence", std::to_string(confidence)},
    {"profile_id", profileId},
    {"algorithm_id", algorithmId},
    {"model_digest", modelDigest},
    {"pooling_operator", poolingOperator},
    {"consumed_view_count", std::to_string(consumedViewCount)},
    {"pooled_feature_digest", pooledFeatureDigest},
    {"result_manifest_name", resultManifestName.toUri()},
    {"result_manifest_digest", resultManifestDigest},
    {"failure_stage", failureStage},
    {"failure_reason", failureReason},
    {"elapsed_ms", std::to_string(elapsedMs)},
    {"bytes_transferred", std::to_string(bytesTransferred)},
    {"terminal_owner", terminalOwner.toUri()},
    {"contributing_count", std::to_string(contributingViewIds.size())},
    {"rejected_count", std::to_string(rejectedViewIds.size())},
    {"annotation_count", std::to_string(annotatedViews.size())},
  };
  for (size_t i = 0; i < contributingViewIds.size(); ++i) {
    result["contributing." + std::to_string(i)] = contributingViewIds[i];
  }
  for (size_t i = 0; i < rejectedViewIds.size(); ++i) {
    result["rejected." + std::to_string(i)] = rejectedViewIds[i];
  }
  for (size_t i = 0; i < annotatedViews.size(); ++i) {
    const auto fields = annotatedViews[i].toFields("annotation." + std::to_string(i));
    result.insert(fields.begin(), fields.end());
  }
  return result;
}

FusedRecognitionResult
FusedRecognitionResult::fromFields(const Fields& fields)
{
  FusedRecognitionResult result;
  result.schemaVersion = uintField(fields, "schema_version", 1);
  result.missionSessionId = field(fields, "mission_session_id");
  result.jobId = field(fields, "job_id");
  result.attempt = uintField(fields, "attempt", 1);
  const auto status = parseMultiViewTerminalStatus(field(fields, "status"));
  if (!status) throw std::invalid_argument("unknown multi-view terminal status");
  result.status = *status;
  result.fusedLabel = field(fields, "fused_label");
  try { result.confidence = std::stod(field(fields, "confidence", "0")); }
  catch (const std::exception&) { result.confidence = 0.0; }
  result.profileId = field(fields, "profile_id");
  result.algorithmId = field(fields, "algorithm_id");
  result.modelDigest = field(fields, "model_digest");
  result.poolingOperator = field(fields, "pooling_operator");
  result.consumedViewCount = sizeField(fields, "consumed_view_count");
  result.pooledFeatureDigest = field(fields, "pooled_feature_digest");
  result.resultManifestName = ndn::Name(field(fields, "result_manifest_name"));
  result.resultManifestDigest = field(fields, "result_manifest_digest");
  result.failureStage = field(fields, "failure_stage");
  result.failureReason = field(fields, "failure_reason");
  result.elapsedMs = uintField(fields, "elapsed_ms");
  result.bytesTransferred = uintField(fields, "bytes_transferred");
  result.terminalOwner = ndn::Name(field(fields, "terminal_owner"));
  const auto contributing = sizeField(fields, "contributing_count");
  const auto rejected = sizeField(fields, "rejected_count");
  const auto annotations = sizeField(fields, "annotation_count");
  if (contributing > UAV_MULTIVIEW_MAX_VIEWS || rejected > UAV_MULTIVIEW_MAX_VIEWS ||
      annotations > UAV_MULTIVIEW_MAX_VIEWS) {
    throw std::invalid_argument("multi-view result list bound exceeded");
  }
  for (size_t i = 0; i < contributing; ++i) result.contributingViewIds.push_back(field(fields, "contributing." + std::to_string(i)));
  for (size_t i = 0; i < rejected; ++i) result.rejectedViewIds.push_back(field(fields, "rejected." + std::to_string(i)));
  for (size_t i = 0; i < annotations; ++i) result.annotatedViews.push_back(
    AnnotatedViewReference::fromFields(fields, "annotation." + std::to_string(i)));
  return result;
}

ndn::Buffer
FusedRecognitionResult::wireEncode() const
{
  const auto encoded = encodeFields(toFields());
  if (encoded.size() > UAV_MULTIVIEW_MAX_WIRE_BYTES) {
    throw std::invalid_argument("multi-view result wire exceeds bounded size");
  }
  return ndn::Buffer(reinterpret_cast<const uint8_t*>(encoded.data()), encoded.size());
}

FusedRecognitionResult
FusedRecognitionResult::wireDecode(const ndn::Buffer& wire)
{
  if (wire.empty() || wire.size() > UAV_MULTIVIEW_MAX_WIRE_BYTES) {
    throw std::invalid_argument("invalid multi-view result wire size");
  }
  return fromFields(decodeFields(wireString(wire)));
}

bool
VerifiedMultiViewInput::isValid(std::string* reason) const
{
  std::string referenceReason;
  if (!reference.isValid(&referenceReason)) return fail(reason, referenceReason);
  if (!nameVerified || !signatureVerified || !digestVerified || signerIdentity != reference.producerIdentity) {
    return fail(reason, "view did not cross the signed-Data acceptance boundary");
  }
  if (sha256(content) != reference.contentDigest) {
    return fail(reason, "verified view content digest mismatch");
  }
  return true;
}

bool
validateMultiViewJobAndViews(const MultiViewRecognitionJob& job,
                             const MultiViewModelProfile& profile,
                             const std::vector<VerifiedMultiViewInput>& views,
                             std::vector<std::string>* rejectedViewIds,
                             std::string* reason)
{
  std::string jobReason;
  if (!job.isValid(&profile, &jobReason)) {
    if (rejectedViewIds) {
      for (const auto& view : job.views) rejectedViewIds->push_back(view.viewId);
    }
    return fail(reason, jobReason);
  }
  std::set<std::string> seen;
  for (const auto& input : views) {
    std::string inputReason;
    if (!input.isValid(&inputReason)) {
      if (rejectedViewIds) rejectedViewIds->push_back(input.reference.viewId);
      return fail(reason, "invalid verified multi-view evidence: " + inputReason);
    }
    const auto it = std::find_if(job.views.begin(), job.views.end(), [&](const auto& reference) {
      return reference.toFields() == input.reference.toFields();
    });
    if (it == job.views.end() || !seen.insert(input.reference.viewId).second) {
      if (rejectedViewIds) rejectedViewIds->push_back(input.reference.viewId);
      return fail(reason, it == job.views.end() ?
        "verified view is not an exact job reference" : "duplicate verified view");
    }
  }
  if (seen.size() < job.minimumViews) return fail(reason, "insufficient verified multi-view evidence");
  std::set<std::string> producers;
  for (const auto& input : views) {
    if (seen.count(input.reference.viewId)) producers.insert(input.reference.producerIdentity.toUri());
  }
  if (producers.size() < job.minimumDistinctProducers) return fail(reason, "insufficient distinct verified producers");
  return true;
}

FusedRecognitionResult
DeterministicMultiViewAlgorithm::execute(
  const MultiViewRecognitionJob& job, const MultiViewModelProfile& profile,
  const std::vector<VerifiedMultiViewInput>& views, const ndn::Name& terminalOwner) const
{
  FusedRecognitionResult result;
  result.missionSessionId = job.missionSessionId;
  result.jobId = job.jobId;
  result.attempt = job.attempt;
  result.profileId = profile.profileId;
  result.algorithmId = profile.algorithmId;
  result.modelDigest = profile.modelDigest;
  result.poolingOperator = profile.poolingOperator;
  result.terminalOwner = terminalOwner;
  result.status = MultiViewTerminalStatus::Completed;
  result.fusedLabel = "car";
  result.confidence = std::min(0.99, 0.60 + 0.05 * static_cast<double>(views.size()));
  std::ostringstream material;
  for (const auto& input : views) {
    material << input.reference.viewId << '|' << input.reference.exactDataName.toUri() << '|'
             << input.reference.contentDigest << '|';
    material.write(reinterpret_cast<const char*>(input.content.data()), input.content.size());
    result.contributingViewIds.push_back(input.reference.viewId);
    result.bytesTransferred += input.content.size();
  }
  result.consumedViewCount = views.size();
  result.pooledFeatureDigest = sha256(material.str());
  result.resultManifestName = makeUavMultiViewResultName(
    terminalOwner, job.missionSessionId, job.jobId, job.attempt, 1);
  result.resultManifestDigest = sha256(result.pooledFeatureDigest + result.resultManifestName.toUri());
  for (const auto& input : views) {
    AnnotatedViewReference annotation;
    annotation.viewId = input.reference.viewId;
    annotation.sourceDigest = input.reference.contentDigest;
    annotation.exactDataName = makeUavMultiViewAnnotationName(
      terminalOwner, job.missionSessionId, job.jobId, job.attempt,
      input.reference.viewId, 1);
    annotation.contentDigest = sha256(input.reference.contentDigest + profile.modelDigest + input.reference.viewId);
    annotation.mediaType = "image/png";
    annotation.signerIdentity = terminalOwner;
    result.annotatedViews.push_back(std::move(annotation));
  }
  return result;
}

MultiViewExecutionResult
executeMultiViewJob(const MultiViewRecognitionJob& job,
                    const MultiViewModelProfile& profile,
                    const std::vector<VerifiedMultiViewInput>& views,
                    const ndn::Name& terminalOwner,
                    const IMultiViewRecognitionAlgorithm& algorithm)
{
  MultiViewExecutionResult execution;
  execution.acceptedViews.clear();
  execution.rejectedViewIds.clear();
  std::string reason;
  if (terminalOwner.empty() || !profile.isValid(&reason)) {
    execution.failureStage = "validation";
    execution.detail = reason.empty() ? "terminal owner is missing" : reason;
    execution.result.status = MultiViewTerminalStatus::UnsupportedModelProfile;
  }
  else if (!validateMultiViewJobAndViews(job, profile, views,
                                         &execution.rejectedViewIds, &reason)) {
    execution.failureStage = reason.find("insufficient") != std::string::npos ? "correlation" : "validation";
    execution.detail = reason;
    execution.result.status = reason.find("insufficient") != std::string::npos ?
      MultiViewTerminalStatus::InsufficientViews : MultiViewTerminalStatus::ValidationFailed;
  }
  else {
    execution.acceptedViews = views;
    execution.result = algorithm.execute(job, profile, views, terminalOwner);
    std::string resultReason;
    if (!execution.result.isValid(&job, &profile, &resultReason)) {
      execution.failureStage = "inference";
      execution.detail = resultReason;
      execution.result.status = MultiViewTerminalStatus::InferenceFailed;
      execution.result.failureStage = "inference";
      execution.result.failureReason = resultReason;
    }
    else {
      execution.success = true;
      execution.failureStage.clear();
      execution.detail = "multi-view algorithm consumed the verified view set";
    }
  }
  if (!execution.success) {
    execution.result.missionSessionId = job.missionSessionId;
    execution.result.jobId = job.jobId;
    execution.result.attempt = job.attempt;
    execution.result.profileId = profile.profileId;
    execution.result.algorithmId = profile.algorithmId;
    execution.result.modelDigest = profile.modelDigest;
    execution.result.terminalOwner = terminalOwner;
    execution.result.failureStage = execution.failureStage;
    execution.result.failureReason = execution.detail;
  }
  return execution;
}

} // namespace ndnsf::examples::uav

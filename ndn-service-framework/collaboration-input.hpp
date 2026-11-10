#ifndef NDN_SERVICE_FRAMEWORK_COLLABORATION_INPUT_HPP
#define NDN_SERVICE_FRAMEWORK_COLLABORATION_INPUT_HPP

#include "ServiceProvider.hpp"

namespace ndn_service_framework {

enum class InputDigestKind { PlaintextSha256, OrderedWireSha256 };
enum class InputObjectEncoding {
  CollaborationLarge,
  ReusableCollaborationObject,
  ImmutableProtected,
  SignedExactPackets
};
enum class InputProvenanceBinding { SelectedRole, AuthorizedObject };

struct CollaborationInputSpec
{
  std::string slotId;
  CollaborationRole producerRole;
  InputProvenanceBinding provenanceBinding = InputProvenanceBinding::SelectedRole;
  ServiceProvider::DataProductReference object;
  InputDigestKind digestKind = InputDigestKind::PlaintextSha256;
  KeyScope keyScope;
  InputObjectEncoding encoding = InputObjectEncoding::CollaborationLarge;
  uint64_t maxBytes = 0;
  uint64_t maxPackets = 0;
};

/** A transport-produced credential, not an application assertion. Constructors
 * are private; owners retain the original authenticated packet bytes. */
class VerifiedInputObject
{
public:
  const ndn::Name& name() const noexcept { return m_name; }
  const ndn::Name& producer() const noexcept { return m_producer; }
  const ndn::Buffer& content() const;
  const std::vector<std::shared_ptr<const ndn::Data>>& packets() const;

private:
  friend class VerifiedExactDataFetchOperation;
  friend class ServiceProvider::CollaborationContext;
  VerifiedInputObject(ndn::Name name, ndn::Name producer,
      std::shared_ptr<const std::vector<std::shared_ptr<const ndn::Data>>> packets,
      std::string dispatchBinding, std::function<bool()> current);
  ndn::Name m_name;
  ndn::Name m_producer;
  std::shared_ptr<const ndn::Buffer> m_content;
  std::shared_ptr<const std::vector<std::shared_ptr<const ndn::Data>>> m_packets;
  std::string m_dispatchBinding;
  std::function<bool()> m_current;
  std::shared_ptr<void> m_retention;
};

class AdmittedInputSet
{
public:
  const std::vector<VerifiedInputObject>& objects() const noexcept { return m_objects; }
  const VerifiedInputObject& at(const std::string& slotId) const;
  bool isCurrent() const { return m_current && m_current(); }
private:
  friend class ServiceProvider::CollaborationContext;
  AdmittedInputSet(std::vector<VerifiedInputObject> objects,
                   std::vector<std::string> slotIds,
                   std::function<bool()> current)
    : m_objects(std::move(objects)), m_slotIds(std::move(slotIds)),
      m_current(std::move(current)) {}
  std::vector<VerifiedInputObject> m_objects;
  std::vector<std::string> m_slotIds;
  std::function<bool()> m_current;
  std::shared_ptr<void> m_retention;
};

enum class InputAdmissionStatus { Ready, Rejected, Cancelled, DeadlineExpired, Overloaded };

struct InputAdmissionResult
{
  InputAdmissionStatus status = InputAdmissionStatus::Rejected;
  std::string reasonCode;
  std::optional<AdmittedInputSet> inputs;
  CollaborationTransferMetrics metrics;
};

/** Bounded native acquisition. get() must not block Face. Cancellation affects
 * this operation; keep Provider/Face alive until workers and callbacks drain. */
class InputAdmissionOperation
{
public:
  InputAdmissionOperation(InputAdmissionOperation&&) noexcept = default;
  InputAdmissionOperation& operator=(InputAdmissionOperation&&) noexcept;
  ~InputAdmissionOperation();
  std::future_status wait_for(std::chrono::milliseconds duration) const;
  InputAdmissionResult get();
  void cancel() noexcept;
private:
  friend class ServiceProvider::CollaborationContext;
  struct Impl;
  explicit InputAdmissionOperation(std::shared_ptr<Impl> impl) : m_impl(std::move(impl)) {}
  std::shared_ptr<Impl> m_impl;
};

struct VerifiedExactDataBatchResult
{
  VerifiedInputObject object;
  CollaborationTransferMetrics metrics;
};

/** Shares the existing exact-read lifecycle. The dispatch must remain current
 * at completion. Destruction requests cancellation; Face draining is separate. */
class VerifiedExactDataFetchOperation
{
public:
  VerifiedExactDataFetchOperation(VerifiedExactDataFetchOperation&&) noexcept = default;
  VerifiedExactDataFetchOperation& operator=(VerifiedExactDataFetchOperation&&) noexcept = default;
  std::future_status wait_for(std::chrono::milliseconds duration) const;
  std::optional<VerifiedExactDataBatchResult> get();
  CollaborationTransferMetrics metricsSnapshot() const;
  void cancel();

private:
  friend class ServiceProvider::CollaborationContext;
  VerifiedExactDataFetchOperation(ServiceProvider::ExactDataFetchOperation operation,
      ndn::Name name, ndn::Name producer, std::string dispatchBinding,
      std::function<bool()> current, std::shared_ptr<std::atomic<bool>> signatureRejected);
  ServiceProvider::ExactDataFetchOperation m_operation;
  ndn::Name m_name;
  ndn::Name m_producer;
  std::string m_dispatchBinding;
  std::function<bool()> m_current;
  std::shared_ptr<std::atomic<bool>> m_signatureRejected;
};

} // namespace ndn_service_framework
#endif

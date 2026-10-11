#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"
#include <map>
#include <utility>

namespace ndnsf::di {

class NativePreparedPlanningTemplate;
/** Exact legacy snapshot bytes, with verified immutable subtree reuse. */
std::string nativePlacementObservationCanonicalJson(const NativeJson& snapshot,
  const NativePreparedPlanningTemplate* prepared = nullptr);

/** Immutable graph/planning facts produced by User::prepare.  This contains
 * no ACK, Provider, grant, placement, request, or projection state. */
struct NativePreparedPlanningCandidate
{
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
};

/** Factory-owned static facts; callers cannot manufacture a validation proof. */
class NativePreparedPlanningTemplate
{
public:
  const std::vector<NativePreparedPlanningCandidate>& candidates() const noexcept
  { return m_candidates; }
  const NativeExecutionPlan& execution(std::size_t index) const
  { return m_execution.at(index); }
  const NativeJson& roleJson(std::size_t index) const
  { return m_roleJson.at(index); }
  bool matches(const NativeInspectedModel& model,
               const CooperativeModelSplitStrategy* splitter) const noexcept
  { return m_model.get() == &model && m_splitter.get() == splitter; }
  std::size_t retainedBytes() const noexcept { return m_retainedBytes; }

private:
  friend struct NativeRequestCatalog;
  friend std::string nativePlacementObservationCanonicalJson(
    const NativeJson&, const NativePreparedPlanningTemplate*);
  NativePreparedPlanningTemplate(std::shared_ptr<const NativeInspectedModel> model,
    std::shared_ptr<const CooperativeModelSplitStrategy> splitter,
    std::vector<NativePreparedPlanningCandidate> candidates);
  std::shared_ptr<const NativeInspectedModel> m_model;
  std::shared_ptr<const CooperativeModelSplitStrategy> m_splitter;
  std::vector<NativePreparedPlanningCandidate> m_candidates;
  std::vector<NativeExecutionPlan> m_execution;
  std::vector<NativeJson> m_roleJson;
  std::vector<std::map<std::string, std::string>> m_snapshotFixedCanonical;
  std::size_t m_retainedBytes = 0;
};

struct NativePreparedPlanningCache
{
  NativeStrategyIdentity splitter;
  std::vector<NativePreparedPlanningCandidate> candidates;
  /** Operator-pinned source profiles; usable only for their exact initial input. */
  NativeJson requestTransferProfiles = NativeJson::array();

  NativePreparedPlanningCache() = default;
  NativePreparedPlanningCache(NativeStrategyIdentity identity,
    std::vector<NativePreparedPlanningCandidate> prepared,
    NativeJson profiles = NativeJson::array())
    : splitter(std::move(identity)), candidates(std::move(prepared)),
      requestTransferProfiles(std::move(profiles)) {}
  // A public copy can be modified through its compatibility fields. Never
  // transfer the private factory proof to that independently mutable object.
  NativePreparedPlanningCache(const NativePreparedPlanningCache& other)
    : NativePreparedPlanningCache(other.splitter, other.candidates,
                                  other.requestTransferProfiles) {}
  NativePreparedPlanningCache(NativePreparedPlanningCache&& other)
    : NativePreparedPlanningCache(std::move(other.splitter), std::move(other.candidates),
                                  std::move(other.requestTransferProfiles)) {}
  NativePreparedPlanningCache& operator=(const NativePreparedPlanningCache& other)
  {
    if (this != &other) {
      m_template.reset();
      splitter = other.splitter; candidates = other.candidates;
      requestTransferProfiles = other.requestTransferProfiles;
    }
    return *this;
  }
  NativePreparedPlanningCache& operator=(NativePreparedPlanningCache&& other)
  {
    if (this != &other) {
      m_template.reset();
      splitter = std::move(other.splitter); candidates = std::move(other.candidates);
      requestTransferProfiles = std::move(other.requestTransferProfiles);
    }
    return *this;
  }
  std::shared_ptr<const NativePreparedPlanningTemplate> validatedTemplate(
    const NativeInspectedModel& model,
    const CooperativeModelSplitStrategy* strategy) const noexcept
  {
    return m_template && m_template->matches(model, strategy) ? m_template : nullptr;
  }
  std::size_t templateRetainedBytes() const noexcept
  { return m_template ? m_template->retainedBytes() : 0; }
  /** Return prepared wire values only for this exact, still unchanged role set. */
  const NativeJson* preparedRoleJson(std::size_t index) const;

private:
  friend struct NativeRequestCatalog;
  NativePreparedPlanningCache(NativeStrategyIdentity identity,
    std::vector<NativePreparedPlanningCandidate> prepared, NativeJson profiles,
    std::shared_ptr<const NativePreparedPlanningTemplate> owned)
    : NativePreparedPlanningCache(std::move(identity), std::move(prepared), std::move(profiles))
  { m_template = std::move(owned); }
  std::shared_ptr<const NativePreparedPlanningTemplate> m_template;
};

/** Loaded from operator-pinned model metadata and owned source bytes. This
 * does not authenticate remotely supplied metadata or perform network I/O. */
struct NativeRequestCatalog
{
  NativeInspectedModel model;
  std::shared_ptr<const NativeCanonicalPreparationCatalog> preparation;
  std::shared_ptr<const NativeModelSplitStrategy> splitter;
  std::shared_ptr<const CooperativeModelSplitStrategy> cooperativeSplitter;
  NativeStateTensorMapping stateMapping;
  /** Prepare-time candidate/role analysis reused by every request. */
  std::shared_ptr<const NativePreparedPlanningCache> planningCache;

  static NativeRequestCatalog load(const std::string& configurationJson,
    NativeCanonicalSource source, const NativeAssemblyControl& control);
};
} // namespace ndnsf::di

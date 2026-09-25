#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"

namespace ndnsf::di {

/** Immutable graph/planning facts produced by User::prepare.  This contains
 * no ACK, Provider, grant, placement, request, or projection state. */
struct NativePreparedPlanningCandidate
{
  NativeSplitCandidate candidate;
  std::vector<NativeSelectionRoleV3> roles;
};

struct NativePreparedPlanningCache
{
  NativeStrategyIdentity splitter;
  std::vector<NativePreparedPlanningCandidate> candidates;
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

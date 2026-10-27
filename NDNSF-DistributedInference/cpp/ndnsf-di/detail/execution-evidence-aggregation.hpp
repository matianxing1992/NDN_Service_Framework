#ifndef NDNSF_DI_DETAIL_EXECUTION_EVIDENCE_AGGREGATION_HPP
#define NDNSF_DI_DETAIL_EXECUTION_EVIDENCE_AGGREGATION_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/ExecutionEvidence.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace ndnsf::di::detail {

/** Aggregate ordered runner observations into an owning readiness snapshot.
 *  Inputs are borrowed only during this call. Empty, inconsistent or invalid
 *  evidence throws; no input is modified. Multiple observations cannot claim
 *  one request's completed execution. The map overload visits values in key
 *  order without copying their potentially large node inventories first.
 */
ExecutionEvidence
aggregateExecutionEvidence(const std::vector<ExecutionEvidence>& items);

ExecutionEvidence
aggregateExecutionEvidence(const std::map<std::string, ExecutionEvidence>& items);

ExecutionEvidence
aggregateExecutionEvidence(
  const std::vector<std::reference_wrapper<const ExecutionEvidence>>& items);

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_EXECUTION_EVIDENCE_AGGREGATION_HPP

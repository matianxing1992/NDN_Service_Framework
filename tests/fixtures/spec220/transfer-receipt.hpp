#ifndef NDNSF_TESTS_SPEC220_TRANSFER_RECEIPT_HPP
#define NDNSF_TESTS_SPEC220_TRANSFER_RECEIPT_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/GenerationEpochLineage.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

namespace ndnsf::di::test::fixture {

/** Fake IO's scalar transport receipt, including lineage-free STOP controls.
 * Admission remains in the real coordinator/worker; creating a receipt must
 * not apply worker-generation prerequisites to an in-band terminal control.
 */
inline std::shared_ptr<StageTransferObservation>
makeTransportReceipt(const DependencyEdge& edge, const TensorBundle& bundle,
                     const std::string& direction)
{
  auto observation = std::make_shared<StageTransferObservation>();
  observation->edgeScope = edge.scope;
  observation->plannedDataName = edge.plannedDataName;
  observation->direction = direction;
  observation->phase = canonicalStagePhase(edge.operationKind);
  observation->identity = edge.plannedDataName + "|" + direction;
  observation->encodedPayloadBytes = bundle.payload.size();
  if (isEncodedTensorBundle(bundle.payload)) {
    for (const auto& tensor : decodeTensorBundleViews(bundle.payload)) {
      if (tensor.name != generationEpochLineageTensorName()) {
        observation->tensorNames.push_back(tensor.name);
        observation->tensorBytes += tensor.payload.size();
      }
    }
  }
  else {
    observation->tensorBytes = bundle.payload.size();
  }
  if (const auto lineage = extractGenerationEpochLineage(bundle)) {
    observation->lineagePresent = true;
    observation->positionDigest = lineage->positionDigest;
    observation->lineageIdentity = edge.plannedDataName + "|" + lineage->positionDigest;
  }
  return observation;
}

} // namespace ndnsf::di::test::fixture

#endif // NDNSF_TESTS_SPEC220_TRANSFER_RECEIPT_HPP

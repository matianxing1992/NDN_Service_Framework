#include "common.hpp"

namespace ndn_service_framework {

NDN_LOG_MEMBER_INIT(SerializedWorkerQueue, ndn_service_framework.SerializedWorkerQueue);
NDN_LOG_MEMBER_INIT(BoundedWorkerPool, ndn_service_framework.BoundedWorkerPool);
NDN_LOG_MEMBER_INIT(MessageValidator, ndn_service_framework.MessageValidator);

const char*
selectionExecutionStateToString(SelectionExecutionState state)
{
  switch (state) {
  case SelectionExecutionState::Unknown: return "Unknown";
  case SelectionExecutionState::Received: return "Received";
  case SelectionExecutionState::Queued: return "Queued";
  case SelectionExecutionState::Running: return "Running";
  case SelectionExecutionState::Completed: return "Completed";
  case SelectionExecutionState::Failed: return "Failed";
  case SelectionExecutionState::Rejected: return "Rejected";
  case SelectionExecutionState::Expired: return "Expired";
  case SelectionExecutionState::Cancelled: return "Cancelled";
  }
  return "Unknown";
}

SelectionExecutionState
selectionExecutionStateFromString(const std::string& state)
{
  if (state == "Received") return SelectionExecutionState::Received;
  if (state == "Queued") return SelectionExecutionState::Queued;
  if (state == "Running") return SelectionExecutionState::Running;
  if (state == "Completed") return SelectionExecutionState::Completed;
  if (state == "Failed") return SelectionExecutionState::Failed;
  if (state == "Rejected") return SelectionExecutionState::Rejected;
  if (state == "Expired") return SelectionExecutionState::Expired;
  if (state == "Cancelled") return SelectionExecutionState::Cancelled;
  return SelectionExecutionState::Unknown;
}

} // namespace ndn_service_framework

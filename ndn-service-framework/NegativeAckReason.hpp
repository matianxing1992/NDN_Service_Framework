#ifndef NDN_SERVICE_FRAMEWORK_NEGATIVE_ACK_REASON_HPP
#define NDN_SERVICE_FRAMEWORK_NEGATIVE_ACK_REASON_HPP

#include <string>

namespace ndn_service_framework {
namespace negative_ack_reason {

inline constexpr const char* QueueFull = "QUEUE_FULL";
inline constexpr const char* ProviderBusy = "PROVIDER_BUSY";
inline constexpr const char* GpuBusy = "GPU_BUSY";
inline constexpr const char* ModelUnavailable = "MODEL_UNAVAILABLE";
inline constexpr const char* PermissionDenied = "PERMISSION_DENIED";
inline constexpr const char* UnsupportedRequest = "UNSUPPORTED_REQUEST";
inline constexpr const char* InternalError = "INTERNAL_ERROR";
inline constexpr const char* LeaseRejected = "LEASE_REJECTED";
inline constexpr const char* LeaseExpired = "LEASE_EXPIRED";
inline constexpr const char* OperationExpired = "OPERATION_EXPIRED";

inline bool
isRecommended(const std::string& reason)
{
    return reason == QueueFull ||
           reason == ProviderBusy ||
           reason == GpuBusy ||
           reason == ModelUnavailable ||
           reason == PermissionDenied ||
           reason == UnsupportedRequest ||
           reason == InternalError ||
           reason == LeaseRejected ||
           reason == LeaseExpired ||
           reason == OperationExpired;
}

} // namespace negative_ack_reason
} // namespace ndn_service_framework

#endif // NDN_SERVICE_FRAMEWORK_NEGATIVE_ACK_REASON_HPP

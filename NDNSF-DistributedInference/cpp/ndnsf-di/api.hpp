#ifndef NDNSF_DI_API_HPP
#define NDNSF_DI_API_HPP

// Stable application entry point. Runtime/User and the immutable PreparedModel
// view are defined by the owning implementation batches; later batches add
// request values.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/PreparedModel.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Conversation.hpp"

// Keep this umbrella dependency-light: the public PreparedModel view imports
// only value types and does not expose the native planner or Python binding.
#include <cstdint>
#include <memory>

namespace ndnsf::di {

class Runtime;
class User;
class PreparedModel;
class Input;
class RequestHandle;
class Result;
class Conversation;
class DiError;
struct RuntimeConfig;
struct UserConfig;
struct PrepareOptions;
struct RequestOptions;

inline constexpr std::uint32_t kPreparedModelRuntimeApiVersion = 1;

} // namespace ndnsf::di

#endif // NDNSF_DI_API_HPP

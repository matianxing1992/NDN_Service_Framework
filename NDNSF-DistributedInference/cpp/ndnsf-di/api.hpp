#ifndef NDNSF_DI_API_HPP
#define NDNSF_DI_API_HPP

// Stable application entry point.  Runtime/User are defined by the owning
// implementation batch; later batches add PreparedModel and request values.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/Runtime.hpp"

// Keep this umbrella dependency-light: Runtime.hpp itself imports only the
// standard library and does not expose the native planner or Python binding.
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

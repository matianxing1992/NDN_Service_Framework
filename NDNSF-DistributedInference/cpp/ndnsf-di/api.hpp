#ifndef NDNSF_DI_API_HPP
#define NDNSF_DI_API_HPP

// Stable application entry point.  The concrete Runtime/User/PreparedModel
// definitions are added by their owning implementation batches; this header
// is intentionally dependency-light so an installed consumer can include it
// without importing DI internals or the Python binding.
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

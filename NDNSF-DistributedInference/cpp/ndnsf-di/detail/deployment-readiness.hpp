#ifndef NDNSF_DI_DETAIL_DEPLOYMENT_READINESS_HPP
#define NDNSF_DI_DETAIL_DEPLOYMENT_READINESS_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include <unistd.h>

namespace ndnsf::di::detail {

/** Report the actual native readiness boundary to the owning deployment process. */
inline void
emitDeploymentReadiness(const std::string& identity, const std::string& nativeBootId)
{
  const char* actor = ::getenv("NDNSF_DEPLOYMENT_ACTOR_ID");
  const char* boot = ::getenv("NDNSF_DEPLOYMENT_BOOT_ID");
  const char* digest = ::getenv("NDNSF_DEPLOYMENT_CONFIG_SHA256");
  if (actor == nullptr && boot == nullptr && digest == nullptr)
    return;
  const auto token = [](const char* value) {
    if (value == nullptr)
      return false;
    const std::string text(value);
    return !text.empty() && text.size() <= 128 &&
      text.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-.") ==
        std::string::npos;
  };
  if (!token(actor) || !token(boot) || digest == nullptr ||
      std::string(digest).size() != 64 ||
      std::string(digest).find_first_not_of("0123456789abcdef") != std::string::npos)
    throw std::invalid_argument("invalid managed deployment readiness binding");
  auto line = NativeJson({{"event", "NDNSF_DEPLOYMENT_READY"}, {"actor_id", actor},
                          {"boot_id", boot}, {"config_sha256", digest},
                          {"identity", identity}, {"native_boot_id", nativeBootId},
                          {"pid", ::getpid()}}).dump() + "\n";
  if (identity.empty() || nativeBootId.empty() || line.size() > 4096)
    throw std::invalid_argument("invalid native deployment readiness identity or size");
  ssize_t count;
  do {
    count = ::write(STDOUT_FILENO, line.data(), line.size());
  } while (count < 0 && errno == EINTR);
  if (count < 0 || static_cast<std::size_t>(count) != line.size())
    throw std::runtime_error("cannot publish managed deployment readiness");
}

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_DEPLOYMENT_READINESS_HPP

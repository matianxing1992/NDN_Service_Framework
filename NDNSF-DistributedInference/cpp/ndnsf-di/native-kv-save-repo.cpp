#include "NDNSF-DistributedInference/cpp/ndnsf-di/native-kv-save.hpp"

#include "ndn-service-framework/ServiceUser.hpp"

#include <linux/magic.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <filesystem>
#include <stdexcept>

namespace ndnsf::di {

void
nativeValidateKvSaveConfig(const NativeProviderHandoffConfig& config, const ndn::Name& producer)
{
  const auto require = [](bool condition, const char* reason) {
    if (!condition) throw std::invalid_argument(reason);
  };
  struct stat info{};
  struct statfs filesystem{};
  require(config.storageWriter && config.storageWriter->getName() == producer &&
    config.signingCertificate.getIdentity() == producer &&
    config.signingCertificate.getValidityPeriod().isValid(), "KV save Producer mismatch");
  require(config.workspace.is_absolute() &&
    std::filesystem::weakly_canonical(config.workspace) == config.workspace &&
    ::lstat(config.workspace.c_str(), &info) == 0 && S_ISDIR(info.st_mode) &&
    info.st_uid == ::geteuid() && (info.st_mode & 077) == 0,
    "KV save requires a private existing disk workspace");
  require(::statfs(config.workspace.c_str(), &filesystem) == 0 &&
    filesystem.f_type != TMPFS_MAGIC && filesystem.f_type != RAMFS_MAGIC,
    "KV save workspace must use disk storage");
  require(!ndn::Name(config.repoNode).empty() && !ndn::Name(config.repoProvider).empty() &&
    ndn::Name(config.repoProvider) != producer &&
    !ndn::Name(config.repoServicePrefix).empty() && config.maxStateBytes > 0 &&
    config.maxStateBytes <= 128ULL * 1024ULL * 1024ULL, "KV save storage budget invalid");
}

} // namespace ndnsf::di

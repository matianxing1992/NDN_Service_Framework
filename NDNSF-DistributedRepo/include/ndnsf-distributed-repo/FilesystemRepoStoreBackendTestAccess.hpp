#ifndef NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_TEST_ACCESS_HPP
#define NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_TEST_ACCESS_HPP

namespace ndnsf_distributed_repo::detail {

/**
 * Narrow native fault-injection seam for the filesystem metadata boundary.
 * Production callers leave callbacks null and use the POSIX operations.
 * Tests install callbacks for one scoped selector and restore the prior table.
 */
struct FilesystemRepoStoreIoHooks
{
  int (*fsync)(int) = nullptr;
  int (*close)(int) = nullptr;
  // Notification only; callback must not reenter the backend or install hooks.
  void (*catalogParse)() = nullptr;
};

FilesystemRepoStoreIoHooks
installFilesystemRepoStoreIoHooks(FilesystemRepoStoreIoHooks hooks) noexcept;

} // namespace ndnsf_distributed_repo::detail

#endif // NDNSF_DISTRIBUTED_REPO_FILESYSTEM_REPO_STORE_BACKEND_TEST_ACCESS_HPP

#ifndef NDNSF_DI_DETAIL_NATIVE_PRIVATE_CHECKPOINT_FILE_HPP
#define NDNSF_DI_DETAIL_NATIVE_PRIVATE_CHECKPOINT_FILE_HPP

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"

namespace ndnsf::di::detail {

// Synchronous private writer; callback borrows fd and must not close or retain it.
void
writePrivateCheckpointFile(const std::filesystem::path& destination,
                                const NativeCheckpointExportOptions& options,
                                const std::function<void(int)>& writer,
                                const std::function<bool()>& cancelled = {});

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_NATIVE_PRIVATE_CHECKPOINT_FILE_HPP

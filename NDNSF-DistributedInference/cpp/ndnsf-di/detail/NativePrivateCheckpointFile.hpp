#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCheckpointExport.hpp"

namespace ndnsf::di::detail {

// Synchronous private writer; callback borrows fd and must not close or retain it.
void writePrivateCheckpointFile(const std::filesystem::path& destination,
                                const NativeCheckpointExportOptions& options,
                                const std::function<void(int)>& writer);

} // namespace ndnsf::di::detail

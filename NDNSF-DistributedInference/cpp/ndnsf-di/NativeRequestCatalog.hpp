#pragma once
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalPreparationCatalog.hpp"

namespace ndnsf::di {
/** Loaded from operator-pinned model metadata and owned source bytes. This
 * does not authenticate remotely supplied metadata or perform network I/O. */
struct NativeRequestCatalog
{
  NativeInspectedModel model;
  std::shared_ptr<const NativeCanonicalPreparationCatalog> preparation;
  std::shared_ptr<const NativeModelSplitStrategy> splitter;
  NativeStateTensorMapping stateMapping;

  static NativeRequestCatalog load(const std::string& configurationJson,
    NativeCanonicalSource source, const NativeAssemblyControl& control);
};
} // namespace ndnsf::di

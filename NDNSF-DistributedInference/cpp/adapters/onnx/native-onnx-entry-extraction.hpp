#ifndef NDNSF_DI_NATIVE_ONNX_ENTRY_EXTRACTION_HPP
#define NDNSF_DI_NATIVE_ONNX_ENTRY_EXTRACTION_HPP

#include "NativeOnnxRecipeAssembler.hpp"

namespace ndnsf::di {

/** Extract the selected CALL graphs from the already authenticated compact role.
 * The caller keeps its role file and shared model.onnx.data alive beside the entries.
 * The sink runs once per entry after graph closure and the aggregate byte budget
 * have been checked. Its output directory must have request-owned rollback.
 */
void
extractNativeOnnxEntryGraphs(
  const NativeSelectionProjectionV3& projection, const std::vector<std::uint8_t>& roleBytes,
  std::uint64_t initializerBytes, const NativeAssemblyControl& control,
  const std::function<void(const NativeExecutionStepSpec&, std::vector<std::uint8_t>)>& sink);

} // namespace ndnsf::di

#endif // NDNSF_DI_NATIVE_ONNX_ENTRY_EXTRACTION_HPP

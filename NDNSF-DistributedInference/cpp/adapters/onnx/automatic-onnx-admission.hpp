#ifndef NDNSF_DI_AUTOMATIC_ONNX_ADMISSION_HPP
#define NDNSF_DI_AUTOMATIC_ONNX_ADMISSION_HPP

#include "automatic-onnx-model.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::di {

struct NativeModelRunnerSpec;
struct NativeSelectionProjectionV3;
struct NativeRequestControl;
struct RoleExecutionContext;

/** Immutable model facts. No request authority, seed, payload or readiness. */
class AutomaticOnnxSourceBinding
{
public:
  static AutomaticOnnxSourceBinding
  fromSourceMetadata(const NativeJson& automatic, const std::string& modelDigest,
                     std::uint64_t initializerBytes,
                     const std::string& initializerObjectDigest,
                     const NativeAssemblyControl& control);

  static AutomaticOnnxSourceBinding
  fromCanonicalJson(const std::string& encoded, const NativeAssemblyControl& control);

  std::string canonicalJson(const NativeAssemblyControl& control) const;
  const AutomaticOnnxContract& sourceContract() const { return *m_contract; }
  const std::string& modelDigest() const { return m_modelDigest; }

private:
  friend class AutomaticOnnxRequestBinding;

  AutomaticOnnxSourceBinding(std::string modelDigest, std::uint64_t initializerBytes,
                             std::string initializerObjectDigest,
                             std::vector<AutomaticOnnxSourceFile> files,
                             std::shared_ptr<const AutomaticOnnxContract> contract);

  std::string m_modelDigest;
  std::uint64_t m_initializerBytes;
  std::string m_initializerObjectDigest;
  std::vector<AutomaticOnnxSourceFile> m_files;
  std::shared_ptr<const AutomaticOnnxContract> m_contract;
};

/** Actual tensor metadata, including device-resident allocations.
 * No host pointer is invented for a device payload.
 */
struct AutomaticOnnxTensorObservation
{
  std::string name;
  TensorElementType elementType{};
  std::vector<std::int64_t> shape;
  std::uint64_t payloadBytes = 0;
};

/** One admitted invocation. No permission, callback or execution-state owner. */
class AutomaticOnnxRequestBinding
{
public:
  static std::shared_ptr<const AutomaticOnnxRequestBinding>
  admit(const AutomaticOnnxSourceBinding& source,
        const NativeSelectionProjectionV3& projection, const NativeRequestControl& control);

  const AutomaticOnnxContract& source() const { return *m_source; }
  const ExecutionShapeEnvelope& envelope() const { return m_envelope; }
  const std::vector<std::string>& controlInputs() const { return m_controlInputs; }

  void
  requireInvocation(const RoleExecutionContext& ctx, const NativeModelRunnerSpec& spec,
                    const AutomaticOnnxContract& loadedSource) const;

  void
  validateInputs(const std::vector<AutomaticOnnxTensorObservation>& observed,
                 const std::vector<NamedTensorView>& actualCpuControls,
                 const RoleExecutionContext& ctx) const;

  void
  validateOutputs(const std::vector<AutomaticOnnxTensorObservation>& observed,
                  const RoleExecutionContext& ctx) const;

private:
  AutomaticOnnxRequestBinding(std::shared_ptr<const AutomaticOnnxContract> source,
                              ExecutionShapeEnvelope envelope,
                              const NativeSelectionProjectionV3& projection,
                              const NativeRequestControl& control);

  NativeAssemblyControl
  validationControl(const RoleExecutionContext& ctx) const;

  std::shared_ptr<const AutomaticOnnxContract> m_source;
  ExecutionShapeEnvelope m_envelope;
  std::vector<std::string> m_inputs;
  std::vector<std::string> m_outputs;
  std::vector<std::string> m_controlInputs;
  std::string m_requestId;
  std::uint64_t m_attempt = 0;
  std::string m_role;
  std::string m_assemblyRole;
  std::string m_backend;
  std::string m_recipe;
  std::chrono::steady_clock::time_point m_deadline;
  std::uint64_t m_maxSourceBytes = 0;
  std::uint64_t m_maxMetadataBytes = 0;
};

/** Reconstruct current bounds; immutable source facts confer no permission. */
ExecutionShapeEnvelope
admitAutomaticOnnxProjection(const AutomaticOnnxSourceBinding& binding,
                             const NativeSelectionProjectionV3& projection,
                             const NativeAssemblyControl& control);

/** Template check before protected descriptor rebind or cache invalidation.
 * Does not require an already materialized plaintext path. Actual loaded
 * runners additionally use the complete prepared-spec validator.
 */
void
validateAutomaticOnnxRunnerSource(const NativeModelRunnerSpec& spec,
                                  const NativeSelectionProjectionV3& projection,
                                  const NativeAssemblyControl& control);

} // namespace ndnsf::di

#endif // NDNSF_DI_AUTOMATIC_ONNX_ADMISSION_HPP

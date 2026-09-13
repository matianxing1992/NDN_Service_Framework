#pragma once

#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativePlanning.hpp"

#include <functional>

namespace ndnsf::di {

/** Immutable descriptor catalog and encoded task-byte boundary.
 * Bootstrap supplies trusted descriptors and an explicit format. Source/graph
 * inspection and network authentication remain with NativeRequestPreparation's
 * ports; a descriptor lookup never claims to have authenticated model bytes.
 */
class NativeCatalogModelAdapter final : public NativeModelAdapter
{
public:
  enum class Format { OpaqueBytes, JsonBytes };
  using ConversationTokenEncoder = std::function<std::vector<std::int64_t>(
    const std::vector<std::uint8_t>&)>;

  NativeCatalogModelAdapter(std::vector<NativeModelDescriptor> models,
                            Format format, std::size_t maxPayloadBytes,
                            ConversationTokenEncoder conversationTokenEncoder = {});
  NativeCatalogModelAdapter& operator=(const NativeCatalogModelAdapter&) = delete;
  NativeCatalogModelAdapter& operator=(NativeCatalogModelAdapter&&) = delete;
  std::string adapterId() const override;
  std::string adapterVersion() const override;
  NativeModelDescriptor inspect(const std::string& modelName,
                                const std::string& modelDigest) const override;
  std::vector<std::uint8_t> encodeInput(const std::vector<std::uint8_t>& bytes) const override;
  std::vector<std::uint8_t> decodeResult(const std::vector<std::uint8_t>& bytes) const override;
  std::vector<std::int64_t> conversationInputTokens(
    const std::vector<std::uint8_t>& applicationInput) const override;

  /** Maximum decoded application input accepted by this catalog adapter. */
  std::size_t maxPayloadBytes() const noexcept { return m_maxPayloadBytes; }

private:
  void validateBytes(const std::vector<std::uint8_t>& bytes) const;
  std::map<std::pair<std::string, std::string>, NativeModelDescriptor> m_models;
  std::string m_adapterId;
  std::string m_adapterVersion;
  Format m_format;
  std::size_t m_maxPayloadBytes;
  ConversationTokenEncoder m_conversationTokenEncoder;
};

} // namespace ndnsf::di

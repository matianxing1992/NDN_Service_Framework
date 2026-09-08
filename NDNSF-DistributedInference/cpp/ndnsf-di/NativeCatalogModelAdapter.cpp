#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCatalogModelAdapter.hpp"
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

namespace ndnsf::di {

NativeCatalogModelAdapter::NativeCatalogModelAdapter(std::vector<NativeModelDescriptor> models,
  Format format, std::size_t maxPayloadBytes)
  : m_format(format), m_maxPayloadBytes(maxPayloadBytes)
{
  if (models.empty() || !maxPayloadBytes ||
      (format != Format::OpaqueBytes && format != Format::JsonBytes))
    throw std::invalid_argument("native model catalog requires models, format and a byte bound");
  m_adapterId = models.front().adapterId;
  m_adapterVersion = models.front().adapterVersion;
  for (auto& model : models) {
    model.validate();
    if (model.adapterId != m_adapterId || model.adapterVersion != m_adapterVersion)
      throw std::invalid_argument("native model catalog mixes adapter identities");
    const auto key = std::make_pair(model.modelName, model.contentDigest);
    if (!m_models.emplace(key, std::move(model)).second)
      throw std::invalid_argument("native model catalog contains a duplicate model revision");
  }
}

std::string NativeCatalogModelAdapter::adapterId() const { return m_adapterId; }
std::string NativeCatalogModelAdapter::adapterVersion() const { return m_adapterVersion; }

NativeModelDescriptor NativeCatalogModelAdapter::inspect(const std::string& name,
  const std::string& digest) const
{
  const auto found = m_models.find({name, digest});
  if (found == m_models.end())
    throw std::invalid_argument("native model catalog has no matching model revision");
  return found->second;
}

void NativeCatalogModelAdapter::validateBytes(const std::vector<std::uint8_t>& bytes) const
{
  if (bytes.size() > m_maxPayloadBytes)
    throw std::invalid_argument("native task payload exceeds the adapter byte bound");
  if (m_format == Format::JsonBytes)
    nativeParseJson(std::string(bytes.begin(), bytes.end()));
}

std::vector<std::uint8_t> NativeCatalogModelAdapter::encodeInput(
  const std::vector<std::uint8_t>& bytes) const
{
  validateBytes(bytes);
  return bytes;
}

std::vector<std::uint8_t> NativeCatalogModelAdapter::decodeResult(
  const std::vector<std::uint8_t>& bytes) const
{
  validateBytes(bytes);
  return bytes;
}

} // namespace ndnsf::di

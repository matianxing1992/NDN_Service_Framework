#include "AbePublicParametersArchive.hpp"

#include <ndn-cxx/encoding/buffer-stream.hpp>

#include <nac-abe/common.hpp>

namespace ndn_service_framework {

ndn::Name
archivedPublicParametersName(const ndn::Name& identity, uint64_t generation, uint64_t epoch)
{
  if (generation == 0)
    throw std::invalid_argument("archived public parameters need a generation");
  ndn::Name name = identity;
  name.append(ndn::nacabe::PUBLIC_PARAMS);
  name.append(ndn::nacabe::ABE_TYPE_KP_ABE);
  name.appendVersion(generation * 1000ULL + epoch);
  return name;
}

bool
buildArchivedPublicParametersData(const ControllerGenerationStore::AbeHistoryEntry& entry,
                                  const ndn::Name& identity,
                                  const std::function<void(ndn::Data&)>& sign,
                                  ndn::Data& result)
{
  if (entry.generationTimestamp == 0 || entry.publicParameters.empty())
    return false;
  const auto expected = archivedPublicParametersName(identity, entry.generationTimestamp,
                                                     entry.epoch);
  if (entry.publicParametersName != expected.toUri(ndn::name::UriFormat::CANONICAL))
    return false;
  result.setName(expected);
  result.setContent(ndn::Buffer(reinterpret_cast<const uint8_t*>(entry.publicParameters.data()),
                                entry.publicParameters.size()));
  result.setFreshnessPeriod(ndn::time::milliseconds(0));
  if (sign)
    sign(result);
  return true;
}

} // namespace ndn_service_framework

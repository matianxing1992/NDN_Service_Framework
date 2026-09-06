#include "ControllerVersion.hpp"

#include <limits>
#include <sstream>

namespace ndn_service_framework {

bool
ControllerVersion::isValid() const
{
  return controllerGenerationTimestamp != 0 && controllerEpoch != 0;
}

int
ControllerVersion::compare(const ControllerVersion& other) const
{
  if (controllerGenerationTimestamp < other.controllerGenerationTimestamp)
    return -1;
  if (controllerGenerationTimestamp > other.controllerGenerationTimestamp)
    return 1;
  if (controllerEpoch < other.controllerEpoch)
    return -1;
  if (controllerEpoch > other.controllerEpoch)
    return 1;
  return 0;
}

bool
ControllerVersion::operator==(const ControllerVersion& other) const
{
  return compare(other) == 0;
}

bool
ControllerVersion::operator!=(const ControllerVersion& other) const
{
  return !(*this == other);
}

std::string
ControllerVersion::toString() const
{
  std::ostringstream os;
  os << controllerGenerationTimestamp << ':' << controllerEpoch;
  return os.str();
}

ndn::Block
ControllerVersion::wireEncode() const
{
  if (!isValid())
    throw std::invalid_argument("invalid ControllerVersion");

  ndn::Block block(TYPE);
  block.push_back(ndn::makeNonNegativeIntegerBlock(
      GenerationTimestampType, controllerGenerationTimestamp));
  block.push_back(ndn::makeNonNegativeIntegerBlock(EpochType, controllerEpoch));
  block.encode();
  return block;
}

bool
ControllerVersion::wireDecode(const ndn::Block& block)
{
  try {
    if (block.type() != TYPE)
      return false;
    block.parse();
    const auto& elements = block.elements();
    if (elements.size() != 2 ||
        elements[0].type() != GenerationTimestampType ||
        elements[1].type() != EpochType)
      return false;

    ControllerVersion decoded;
    decoded.controllerGenerationTimestamp =
        ndn::readNonNegativeInteger(elements[0]);
    decoded.controllerEpoch = ndn::readNonNegativeInteger(elements[1]);
    if (!decoded.isValid())
      return false;
    *this = decoded;
    return true;
  }
  catch (const std::exception&) {
    return false;
  }
}

} // namespace ndn_service_framework

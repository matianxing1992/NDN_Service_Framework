#include "FileKeyRing.hpp"

#include <cstdlib>

namespace ndn_service_framework {

std::optional<std::filesystem::path>
fileKeyRingPath()
{
  const char* configured = std::getenv("NDNSF_FILE_KEYRING");
  if (configured == nullptr || *configured == '\0')
    return std::nullopt;
  return std::filesystem::path(configured);
}

std::vector<FileKeyRingEntry>
loadFileKeyRingEntries()
{
  const auto path = fileKeyRingPath();
  if (!path)
    return {};
  return readKeyRingFile(*path);
}

std::optional<FileKeyRingEntry>
loadFileKeyRingEntry(const std::string& keyId)
{
  if (keyId.empty())
    return std::nullopt;
  for (auto& entry : loadFileKeyRingEntries())
    if (entry.keyId == keyId)
      return entry;
  return std::nullopt;
}

} // namespace ndn_service_framework

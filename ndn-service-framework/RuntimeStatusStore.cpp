#include "RuntimeStatusStore.hpp"

#include <cstdlib>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework {
namespace {

constexpr char STORE_MAGIC[] = "NDNSFRS1";
constexpr size_t STORE_MAGIC_SIZE = sizeof(STORE_MAGIC) - 1;

// Per-record and per-store sanity bounds (mirror ControllerGenerationStore).
constexpr uint64_t MAX_RECORD_LENGTH = (1ULL << 20);
constexpr uint64_t MAX_RECORD_COUNT = 100000;

void
writeU64(std::ostream& stream, uint64_t value)
{
  for (int shift = 56; shift >= 0; shift -= 8)
    stream.put(static_cast<char>((value >> shift) & 0xff));
}

uint64_t
readU64(std::istream& stream)
{
  uint64_t value = 0;
  for (int shift = 56; shift >= 0; shift -= 8) {
    const int byte = stream.get();
    if (byte == std::char_traits<char>::eof())
      throw std::runtime_error("truncated runtime status store");
    value = (value << 8) | static_cast<uint8_t>(byte);
  }
  return value;
}

void
writeString(std::ostream& stream, const std::string& value)
{
  writeU64(stream, value.size());
  stream.write(value.data(), static_cast<std::streamsize>(value.size()));
}

std::string
readString(std::istream& stream)
{
  const auto length = readU64(stream);
  if (length > MAX_RECORD_LENGTH)
    throw std::runtime_error("invalid runtime status store string length");
  std::string value(static_cast<size_t>(length), '\0');
  stream.read(value.data(), static_cast<std::streamsize>(value.size()));
  if (stream.gcount() != static_cast<std::streamsize>(value.size()))
    throw std::runtime_error("truncated runtime status store string");
  return value;
}

void
writeBuffer(std::ostream& stream, const ndn::Buffer& value)
{
  writeU64(stream, value.size());
  if (!value.empty())
    stream.write(reinterpret_cast<const char*>(value.data()),
                 static_cast<std::streamsize>(value.size()));
}

bool
readBuffer(std::istream& stream, ndn::Buffer& value)
{
  const auto length = readU64(stream);
  if (length > MAX_RECORD_LENGTH)
    throw std::runtime_error("invalid runtime status store buffer length");
  value = ndn::Buffer(length);
  if (length > 0) {
    stream.read(reinterpret_cast<char*>(value.data()),
                static_cast<std::streamsize>(value.size()));
    if (stream.gcount() != static_cast<std::streamsize>(value.size()))
      throw std::runtime_error("truncated runtime status store buffer");
  }
  return true;
}

} // namespace

bool
RuntimeStatusStore::enabled()
{
  const char* value = std::getenv("NDNSF_PERSIST_RUNTIME_STATE");
  if (value == nullptr)
    return false;
  const std::string text(value);
  return text == "1" || text == "true" || text == "yes" || text == "on";
}

std::filesystem::path
RuntimeStatusStore::defaultStorePath(const std::string& role,
                                     const ndn::Name& identity)
{
  std::filesystem::path directory;
  if (const char* envDir = std::getenv("NDNSF_RUNTIME_STATE_DIR");
      envDir != nullptr && *envDir != '\0') {
    directory = envDir;
  }
  else if (const char* stateHome = std::getenv("XDG_STATE_HOME");
           stateHome != nullptr && *stateHome != '\0') {
    directory = std::filesystem::path(stateHome) / "ndnsf" / "runtime";
  }
  else {
    const char* home = std::getenv("HOME");
    directory = std::filesystem::path(home != nullptr ? home : "/tmp") /
                ".local" / "state" / "ndnsf" / "runtime";
  }
  std::string file = role;
  file.push_back('-');
  std::string escaped = identity.toUri();
  for (auto& ch : escaped) {
    if (ch == '/' || ch == ':' || ch == '%')
      ch = '_';
  }
  file += escaped;
  file += ".rts";
  return directory / file;
}

RuntimeStatusStore::RuntimeStatusStore(std::filesystem::path storePath)
  : m_storePath(std::move(storePath))
{
}

bool
RuntimeStatusStore::load(std::vector<Record>& out) const
{
  out.clear();
  if (!std::filesystem::exists(m_storePath))
    return false;

  std::ifstream store(m_storePath, std::ios::binary);
  if (!store)
    throw std::runtime_error("cannot open runtime status store: " +
                             m_storePath.string());

  char magic[STORE_MAGIC_SIZE] = {};
  store.read(magic, STORE_MAGIC_SIZE);
  if (store.gcount() != static_cast<std::streamsize>(STORE_MAGIC_SIZE))
    return false; // truncated: fail closed, nothing restored
  if (!std::equal(std::begin(magic), std::end(magic), STORE_MAGIC))
    return false; // not our format: fail closed

  uint64_t count = 0;
  std::vector<Record> decoded;
  try {
    count = readU64(store);
    if (count > MAX_RECORD_COUNT)
      return false;
    decoded.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
      Record record;
      const auto serviceUri = readString(store);
      record.serviceName = ndn::Name(serviceUri);
      record.controllerVersion.controllerGenerationTimestamp = readU64(store);
      record.controllerVersion.controllerEpoch = readU64(store);
      record.installTimeMs = readU64(store);
      const auto paramsNameUri = readString(store);
      if (!paramsNameUri.empty())
        record.abePublicParametersName = ndn::Name(paramsNameUri);
      record.abePublicParametersDigest = readString(store);
      ndn::Buffer policyWire;
      readBuffer(store, policyWire);
      ndn::Buffer dataWire;
      readBuffer(store, dataWire);
      // Structural sanity before accepting the record.  A record that does
      // not pass is corruption, not a partial update: fail the whole load.
      if (serviceUri.empty() || dataWire.empty() ||
          !record.controllerVersion.isValid()) {
        throw std::runtime_error("invalid runtime status store record");
      }
      record.policyStatusWire = std::move(policyWire);
      record.statusDataWire = std::move(dataWire);
      decoded.push_back(std::move(record));
    }
  }
  catch (const std::exception&) {
    return false; // truncated or structurally corrupt: fail closed
  }
  if (store.peek() != std::char_traits<char>::eof())
    return false; // trailing garbage: fail closed
  out = std::move(decoded);
  return true;
}

bool
RuntimeStatusStore::persist(const std::vector<Record>& records) const
{
  std::error_code ec;
  std::filesystem::create_directories(m_storePath.parent_path(), ec);
  if (ec)
    return false;
  ::chmod(m_storePath.parent_path().c_str(), 0700);

  const auto temporary = m_storePath.string() + ".tmp." +
                         std::to_string(::getpid());
  {
    std::ofstream store(temporary, std::ios::binary | std::ios::trunc);
    if (!store)
      return false;
    ::chmod(temporary.c_str(), 0600);
    store.write(STORE_MAGIC, STORE_MAGIC_SIZE);
    writeU64(store, records.size());
    for (const auto& record : records) {
      if (record.serviceName.empty() || record.statusDataWire.empty()) {
        store.close();
        std::filesystem::remove(temporary, ec);
        return false;
      }
      writeString(store, record.serviceName.toUri());
      writeU64(store, record.controllerVersion.controllerGenerationTimestamp);
      writeU64(store, record.controllerVersion.controllerEpoch);
      writeU64(store, record.installTimeMs);
      writeString(store, record.abePublicParametersName.toUri());
      writeString(store, record.abePublicParametersDigest);
      writeBuffer(store, record.policyStatusWire);
      writeBuffer(store, record.statusDataWire);
    }
    store.flush();
    if (!store)
      return false;
  }
  std::filesystem::rename(temporary, m_storePath, ec);
  if (ec) {
    std::filesystem::remove(temporary, ec);
    return false;
  }
  return true;
}

} // namespace ndn_service_framework

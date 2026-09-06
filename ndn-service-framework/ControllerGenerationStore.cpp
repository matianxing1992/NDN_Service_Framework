#include "ControllerGenerationStore.hpp"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndn_service_framework {
namespace {

constexpr char STATE_MAGIC[] = "NDNSFCS2";
constexpr size_t STATE_MAGIC_SIZE = sizeof(STATE_MAGIC) - 1;

std::atomic<uint64_t> nextFence{1};

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
      throw std::runtime_error("truncated Controller generation state");
    value = (value << 8) | static_cast<uint8_t>(byte);
  }
  return value;
}

} // namespace

ControllerGenerationStore::ControllerGenerationStore(std::filesystem::path statePath)
  : m_statePath(std::move(statePath))
  , m_lockPath(m_statePath.string() + ".lock")
{
}

ControllerGenerationStore::~ControllerGenerationStore()
{
  releaseWriter();
}

bool
ControllerGenerationStore::acquireWriter(const std::string& owner)
{
  if (owner.empty() || ownsWriter())
    return false;
  const int fd = ::open(m_lockPath.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0)
    return false;
  m_owner = owner;
  m_fence = nextFence.fetch_add(1);
  const auto contents = m_owner + "\n" + std::to_string(m_fence) + "\n";
  const auto written = ::write(fd, contents.data(), contents.size());
  ::fsync(fd);
  ::close(fd);
  if (written != static_cast<ssize_t>(contents.size())) {
    std::filesystem::remove(m_lockPath);
    m_owner.clear();
    m_fence = 0;
    return false;
  }
  return true;
}

bool
ControllerGenerationStore::ownsWriter() const
{
  if (m_owner.empty() || m_fence == 0)
    return false;
  std::ifstream lock(m_lockPath, std::ios::binary);
  std::string owner;
  uint64_t fence = 0;
  if (!(lock >> owner >> fence))
    return false;
  return owner == m_owner && fence == m_fence;
}

uint64_t
ControllerGenerationStore::fencingValue() const
{
  return m_fence;
}

void
ControllerGenerationStore::releaseWriter()
{
  if (!ownsWriter()) {
    m_owner.clear();
    m_fence = 0;
    return;
  }
  std::error_code ec;
  std::filesystem::remove(m_lockPath, ec);
  m_owner.clear();
  m_fence = 0;
}

void
ControllerGenerationStore::requireWriter() const
{
  if (!ownsWriter())
    throw std::runtime_error("Controller generation writer lease is not held");
}

std::optional<ControllerVersion>
ControllerGenerationStore::load() const
{
  if (!std::filesystem::exists(m_statePath))
    return std::nullopt;
  return loadState().version;
}

ControllerGenerationStore::PersistedState
ControllerGenerationStore::loadState() const
{
  if (!std::filesystem::exists(m_statePath))
    return {};
  std::ifstream state(m_statePath, std::ios::binary);
  if (!state)
    throw std::runtime_error("cannot open Controller generation state");

  char magic[STATE_MAGIC_SIZE] = {};
  state.read(magic, STATE_MAGIC_SIZE);
  if (state.gcount() != static_cast<std::streamsize>(STATE_MAGIC_SIZE))
    throw std::runtime_error("truncated Controller generation state");

  PersistedState result;
  if (std::equal(std::begin(magic), std::end(magic), STATE_MAGIC)) {
    result.version.controllerGenerationTimestamp = readU64(state);
    result.version.controllerEpoch = readU64(state);
    const auto count = readU64(state);
    if (!result.version.isValid() || count > 100000)
      throw std::runtime_error("invalid Controller generation state");
    result.revocations.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
      const auto length = readU64(state);
      if (length == 0 || length > (1ULL << 20))
        throw std::runtime_error("invalid persisted revocation length");
      std::vector<uint8_t> bytes(static_cast<size_t>(length));
      state.read(reinterpret_cast<char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
      if (state.gcount() != static_cast<std::streamsize>(bytes.size()))
        throw std::runtime_error("truncated persisted revocation");
      ndn::Block block(ndn::Buffer(bytes.data(), bytes.size()));
      RevocationTarget target;
      if (!target.wireDecode(block))
        throw std::runtime_error("invalid persisted revocation");
      result.revocations.push_back(std::move(target));
    }
  }
  else {
    // Read-only compatibility with the original version-only record.  The
    // next atomic write upgrades it to the revocation-preserving format.
    state.clear();
    state.seekg(0);
    result.version.controllerGenerationTimestamp = readU64(state);
    result.version.controllerEpoch = readU64(state);
    if (!result.version.isValid() || state.peek() != std::char_traits<char>::eof())
      throw std::runtime_error("invalid Controller generation state");
  }
  return result;
}

std::vector<RevocationTarget>
ControllerGenerationStore::loadRevocations() const
{
  return loadState().revocations;
}

void
ControllerGenerationStore::persist(
    const ControllerVersion& version,
    const std::vector<RevocationTarget>& revocations) const
{
  requireWriter();
  if (!version.isValid())
    throw std::invalid_argument("invalid Controller version");
  const auto temporary = m_statePath.string() + ".tmp." + std::to_string(m_fence);
  {
    std::ofstream state(temporary, std::ios::binary | std::ios::trunc);
    if (!state)
      throw std::runtime_error("cannot write Controller generation state");
    state.write(STATE_MAGIC, STATE_MAGIC_SIZE);
    writeU64(state, version.controllerGenerationTimestamp);
    writeU64(state, version.controllerEpoch);
    writeU64(state, revocations.size());
    for (const auto& target : revocations) {
      const auto wire = target.wireEncode();
      writeU64(state, wire.size());
      state.write(reinterpret_cast<const char*>(wire.data()),
                  static_cast<std::streamsize>(wire.size()));
    }
    state.flush();
    if (!state)
      throw std::runtime_error("cannot flush Controller generation state");
  }
  std::error_code ec;
  std::filesystem::rename(temporary, m_statePath, ec);
  if (ec) {
    std::filesystem::remove(temporary);
    throw std::runtime_error("cannot atomically replace Controller generation state");
  }
}

ControllerVersion
ControllerGenerationStore::startGeneration(
    uint64_t nowMs, const std::vector<RevocationTarget>& revocations)
{
  requireWriter();
  if (nowMs == 0)
    throw std::invalid_argument("Controller generation timestamp must be nonzero");
  const auto previous = loadState();
  uint64_t generation = nowMs;
  if (previous.version.isValid()) {
    if (previous.version.controllerGenerationTimestamp == std::numeric_limits<uint64_t>::max())
      throw std::overflow_error("Controller generation timestamp overflow");
    generation = std::max(generation, previous.version.controllerGenerationTimestamp + 1);
  }
  const ControllerVersion version{generation, 1};
  persist(version, revocations);
  return version;
}

ControllerVersion
ControllerGenerationStore::advanceEpoch(
    const std::vector<RevocationTarget>& revocations)
{
  requireWriter();
  const auto previous = loadState();
  if (!previous.version.isValid())
    throw std::runtime_error("Controller generation has not started");
  if (previous.version.controllerEpoch == std::numeric_limits<uint64_t>::max())
    throw std::overflow_error("Controller epoch overflow");
  const ControllerVersion version{previous.version.controllerGenerationTimestamp,
                                  previous.version.controllerEpoch + 1};
  persist(version, revocations);
  return version;
}

} // namespace ndn_service_framework

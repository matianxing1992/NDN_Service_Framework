#include "UavTrackingWorkerTransaction.hpp"

#include "UavDigest.hpp"

#include <ndn-cxx/util/sha256.hpp>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace ndnsf::examples::uav {
namespace {

using boost::property_tree::ptree;

constexpr std::size_t MAX_TRANSACTION_FILE_BYTES = 64U * 1024U * 1024U;
constexpr std::string_view TRANSACTION_SCHEMA = "uav-tracking-worker-transaction-v1";

struct Manifest
{
  std::string windowId;
  std::string inputDigest;
  std::string resultDigest;
  std::string stateDigest;
};

std::string
digest(const ndn::Buffer& bytes)
{
  ndn::util::Sha256 hash;
  if (!bytes.empty()) {
    hash << std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  }
  return canonicalSha256Digest(hash.toString());
}

void
requireDigest(const std::string& value, const char* label)
{
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0 ||
      canonicalSha256Digest(value.substr(7)) != value) {
    throw std::invalid_argument(std::string(label) + " must be a canonical SHA-256 digest");
  }
}

void
requireWindowId(const std::string& value)
{
  constexpr std::string_view prefix = "window-";
  if (value.compare(0, prefix.size(), prefix) != 0 || value.size() == prefix.size()) {
    throw std::invalid_argument("tracking transaction window id is invalid");
  }
  const auto digits = std::string_view(value).substr(prefix.size());
  if (!std::all_of(digits.begin(), digits.end(), [] (unsigned char ch) {
        return ch >= '0' && ch <= '9';
      }) || (digits.size() > 1 && digits.front() == '0')) {
    throw std::invalid_argument("tracking transaction window id is not canonical");
  }
}

ndn::Buffer
readBuffer(const std::filesystem::path& path)
{
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read tracking transaction file: " + path.string());
  input.seekg(0, std::ios::end);
  const auto size = input.tellg();
  if (size < 0 || static_cast<std::uint64_t>(size) > MAX_TRANSACTION_FILE_BYTES) {
    throw std::runtime_error("tracking transaction file exceeds its size limit");
  }
  input.seekg(0, std::ios::beg);
  ndn::Buffer result(static_cast<std::size_t>(size));
  if (!result.empty()) {
    input.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(result.size()));
    if (input.gcount() != static_cast<std::streamsize>(result.size())) {
      throw std::runtime_error("short read from tracking transaction file: " + path.string());
    }
  }
  return result;
}

void
writeBuffer(const std::filesystem::path& path, const ndn::Buffer& bytes)
{
  if (bytes.empty() || bytes.size() > MAX_TRANSACTION_FILE_BYTES) {
    throw std::runtime_error("tracking transaction payload is empty or too large");
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) throw std::runtime_error("cannot create tracking transaction file: " + path.string());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.flush();
  if (!output) throw std::runtime_error("cannot write tracking transaction file: " + path.string());
}

std::optional<Manifest>
readManifest(const std::filesystem::path& root, const std::string& windowId)
{
  const auto path = root / windowId / "manifest.json";
  if (!std::filesystem::is_regular_file(path)) return std::nullopt;
  const auto bytes = readBuffer(path);
  const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  std::istringstream input(text);
  ptree tree;
  boost::property_tree::read_json(input, tree);
  if (tree.get<std::string>("schema") != TRANSACTION_SCHEMA ||
      tree.get<std::string>("windowId") != windowId) {
    throw std::runtime_error("tracking transaction manifest identity is invalid");
  }
  Manifest value{windowId,
                 tree.get<std::string>("inputDigest"),
                 tree.get<std::string>("resultDigest"),
                 tree.get<std::string>("stateDigest")};
  requireDigest(value.inputDigest, "transaction input digest");
  requireDigest(value.resultDigest, "transaction result digest");
  requireDigest(value.stateDigest, "transaction state digest");
  const auto directory = root / windowId;
  const auto result = readBuffer(directory / "result.json");
  const auto state = readBuffer(directory / "state.json");
  if (digest(result) != value.resultDigest || digest(state) != value.stateDigest) {
    throw std::runtime_error("tracking transaction payload digest mismatch");
  }
  return value;
}

} // namespace

UavTrackingWorkerTransaction::UavTrackingWorkerTransaction(std::filesystem::path root)
  : m_root(std::move(root))
{
}

std::optional<ndn::Buffer>
UavTrackingWorkerTransaction::loadCommittedResult(const std::string& windowId,
                                                 const std::string& inputDigest) const
{
  requireWindowId(windowId);
  requireDigest(inputDigest, "transaction input digest");
  const auto manifest = readManifest(m_root, windowId);
  if (!manifest) return std::nullopt;
  if (manifest->inputDigest != inputDigest) {
    throw std::runtime_error("tracking window was already committed with different inputs");
  }
  return readBuffer(m_root / windowId / "result.json");
}

std::optional<std::filesystem::path>
UavTrackingWorkerTransaction::committedStatePath(const std::string& windowId) const
{
  requireWindowId(windowId);
  if (!readManifest(m_root, windowId)) return std::nullopt;
  return m_root / windowId / "state.json";
}

bool
UavTrackingWorkerTransaction::commit(const std::string& windowId,
                                     const std::string& inputDigest,
                                     const ndn::Buffer& result,
                                     const ndn::Buffer& state,
                                     const std::function<bool()>& stillAuthorized) const
{
  requireWindowId(windowId);
  requireDigest(inputDigest, "transaction input digest");
  if (result.empty() || state.empty() || result.size() > MAX_TRANSACTION_FILE_BYTES ||
      state.size() > MAX_TRANSACTION_FILE_BYTES) {
    throw std::invalid_argument("tracking transaction result/state size is invalid");
  }
  const auto committed = m_root / windowId;
  if (const auto manifest = readManifest(m_root, windowId)) {
    if (manifest->inputDigest != inputDigest) {
      throw std::runtime_error("tracking window was already committed with different inputs");
    }
    throw std::runtime_error("tracking window transaction is already committed");
  }

  std::filesystem::create_directories(m_root);
  if (std::filesystem::exists(committed)) {
    // A directory without a manifest is an interrupted candidate, never a
    // committed state/result pair.
    std::filesystem::remove_all(committed);
  }
  std::filesystem::create_directories(committed);
  try {
    writeBuffer(committed / "result.json", result);
    writeBuffer(committed / "state.json", state);
    const auto resultDigest = digest(result);
    const auto stateDigest = digest(state);
    const auto temporaryManifest = committed / "manifest.json.tmp";
    {
      std::ofstream output(temporaryManifest, std::ios::binary | std::ios::trunc);
      if (!output) throw std::runtime_error("cannot create tracking transaction manifest");
      output << "{\"schema\":\"" << TRANSACTION_SCHEMA
             << "\",\"windowId\":\"" << windowId
             << "\",\"inputDigest\":\"" << inputDigest
             << "\",\"resultDigest\":\"" << resultDigest
             << "\",\"stateDigest\":\"" << stateDigest << "\"}\n";
      output.flush();
      if (!output) throw std::runtime_error("cannot write tracking transaction manifest");
    }
    if (!stillAuthorized || !stillAuthorized()) {
      std::filesystem::remove_all(committed);
      return false;
    }
    // This same-filesystem rename is the commit point for the immutable pair.
    std::filesystem::rename(temporaryManifest, committed / "manifest.json");
    return true;
  }
  catch (...) {
    if (!std::filesystem::is_regular_file(committed / "manifest.json")) {
      std::error_code ignored;
      std::filesystem::remove_all(committed, ignored);
    }
    throw;
  }
}

} // namespace ndnsf::examples::uav

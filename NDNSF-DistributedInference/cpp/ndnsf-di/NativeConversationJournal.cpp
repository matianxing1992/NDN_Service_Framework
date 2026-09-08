#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeConversationJournal.hpp"

#include <openssl/crypto.h>
#include <openssl/sha.h>
#include <cerrno>
#include <fcntl.h>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ndnsf::di {
namespace {
constexpr const char* journalSchema = "ndnsf-di-app-runtime-journal-v1";
void require(bool ok, const char* message)
{
  if (!ok) throw std::runtime_error(message);
}

std::string sha(const std::string& value)
{
  unsigned char bytes[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const unsigned char*>(value.data()), value.size(), bytes);
  std::string out;
  for (const auto byte : bytes) {
    out += "0123456789abcdef"[byte >> 4]; out += "0123456789abcdef"[byte & 15];
  }
  return out;
}

struct Descriptor
{
  int fd = -1;
  ~Descriptor() { if (fd >= 0) ::close(fd); }
};

void syncDirectory(const std::filesystem::path& path)
{
  Descriptor descriptor{::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW)};
  require(descriptor.fd >= 0 && ::fsync(descriptor.fd) == 0, "conversation directory sync failed");
}

void ensureDirectory(const std::filesystem::path& path)
{
  const bool created = std::filesystem::create_directories(path);
  struct stat value{};
  require(::lstat(path.c_str(), &value) == 0 && S_ISDIR(value.st_mode) &&
          value.st_uid == ::geteuid(), "conversation journal directory ownership invalid");
  require(::chmod(path.c_str(), 0700) == 0, "conversation journal permissions failed");
  if (created) syncDirectory(path.parent_path());
}

int openOwnedFile(const std::filesystem::path& path)
{
  const auto fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  require(fd >= 0, "conversation journal file open failed");
  struct stat value{};
  if (::fstat(fd, &value) != 0 || !S_ISREG(value.st_mode) || value.st_uid != ::geteuid() ||
      value.st_nlink != 1 || ::fchmod(fd, 0600) != 0) {
    ::close(fd);
    throw std::runtime_error("conversation journal file ownership invalid");
  }
  return fd;
}

NativeJson checkedRecord(NativeJson record)
{
  require(record.is_object() && record.contains("checksum") && record.at("checksum").is_string(),
          "conversation journal record malformed");
  const auto checksum = record.at("checksum").get<std::string>();
  record.erase("checksum");
  require(checksum == sha(record.dump(-1, ' ', true)), "conversation journal checksum mismatch");
  require(record.at("schema") == journalSchema && record.at("kind").is_string() &&
          record.at("payload").is_object(), "conversation journal schema mismatch");
  record["checksum"] = checksum;
  return record;
}

NativeJson makeRecord(const std::string& kind, NativeJson payload, std::uint64_t now)
{
  NativeJson record{{"schema", journalSchema}, {"kind", kind},
                    {"timestampMs", now}, {"payload", std::move(payload)}};
  record["checksum"] = sha(record.dump(-1, ' ', true));
  return record;
}
} // namespace

struct NativeConversationJournal::Impl
{
  explicit Impl(NativeConversationJournalConfig value) : config(std::move(value)) {}
  ~Impl()
  {
    for (auto& key : config.keys) OPENSSL_cleanse(key.bytes.data(), key.bytes.size());
  }
  NativeConversationJournalConfig config;
  Descriptor lock;
  Descriptor file;
  std::size_t size = 0;
  std::size_t usage = 0;
  std::vector<NativeJson> records;
  mutable std::mutex mutex;
  bool poisoned = false;
};

NativeConversationJournal::NativeConversationJournal(NativeConversationJournalConfig config)
  : m_impl(std::make_unique<Impl>(std::move(config)))
{
  auto& state = *m_impl;
  const auto& settings = state.config;
  require(!settings.stateRoot.empty() && !settings.identity.empty() &&
          settings.identity != "." && settings.identity != ".." &&
          settings.identity.find_first_of("/\\") == std::string::npos &&
          settings.quotaBytes > 0 && settings.quotaBytes <= 64 * 1024 * 1024,
          "conversation journal configuration invalid");
  auto derived = nativeConversationAuthenticationKeys(settings.identity, settings.keys);
  for (auto& key : derived) OPENSSL_cleanse(key.data(), key.size());
  const auto root = std::filesystem::absolute(settings.stateRoot).lexically_normal();
  const auto rootString = root.string();
  const bool ephemeral = rootString == "/tmp" || rootString.compare(0, 5, "/tmp/") == 0 ||
    rootString == "/dev/shm" || rootString.compare(0, 9, "/dev/shm/") == 0 ||
    rootString == "/run" || rootString.compare(0, 5, "/run/") == 0;
  require(!ephemeral || settings.testOnlyAllowEphemeralRoot,
          "conversation ephemeral root requires test override");
  ensureDirectory(root);
  const auto directory = root / settings.identity;
  ensureDirectory(directory);
  state.lock.fd = openOwnedFile(directory / "journal.lock");
  require(::flock(state.lock.fd, LOCK_EX | LOCK_NB) == 0, "conversation journal writer lease unavailable");
  state.file.fd = openOwnedFile(directory / "journal.jsonl");
  syncDirectory(directory);
  struct stat fileStat{};
  require(::fstat(state.file.fd, &fileStat) == 0 && fileStat.st_size >= 0 &&
          static_cast<std::uint64_t>(fileStat.st_size) <= settings.quotaBytes,
          "conversation journal quota exceeded");
  std::string wire(static_cast<std::size_t>(fileStat.st_size), '\0');
  std::size_t read = 0;
  while (read < wire.size()) {
    const auto count = ::pread(state.file.fd, wire.data() + read, wire.size() - read, read);
    if (count < 0 && errno == EINTR) continue;
    require(count > 0, "conversation journal read failed");
    read += static_cast<std::size_t>(count);
  }
  std::size_t position = 0;
  while (position < wire.size()) {
    const auto end = wire.find('\n', position);
    const bool tail = end == std::string::npos;
    const auto line = wire.substr(position, tail ? wire.size() - position : end - position);
    try { (void)NativeJson::parse(line); }
    catch (const NativeJson::parse_error&) {
      // Only an unterminated malformed final append is a torn transaction.
      if (tail) break;
      throw;
    }
    // Duplicate keys are a complete but invalid record, never a torn tail.
    const auto record = checkedRecord(nativeParseJson(line));
    if (record.at("kind") == "journal-transaction") {
      const auto& nested = record.at("payload").at("records");
      require(nested.is_array(), "conversation journal transaction malformed");
      for (const auto& entry : nested) {
        auto logical = checkedRecord(entry);
        require(logical.at("kind") != "journal-transaction", "nested journal transaction invalid");
        state.records.push_back(std::move(logical));
      }
    }
    else state.records.push_back(record);
    position = tail ? wire.size() : end + 1;
  }
  if (position < wire.size()) {
    require(::ftruncate(state.file.fd, position) == 0 && ::fsync(state.file.fd) == 0,
            "conversation torn tail repair failed");
  }
  // A valid legacy last record may omit its final newline; preserve its bytes
  // and separate the next append rather than merging two JSON documents.
  if (position && wire[position - 1] != '\n') {
    require(position < settings.quotaBytes && ::pwrite(state.file.fd, "\n", 1, position) == 1 &&
            ::fsync(state.file.fd) == 0, "conversation journal newline repair failed");
    ++position;
  }
  state.size = position;
  // The old journal quota includes compatibility spool mirrors as well as
  // the append log. Keep that accounting even though new writes need no mirror.
  for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
    require(!entry.is_symlink(), "conversation journal contains symlink");
    if (!entry.is_regular_file()) continue;
    const auto bytes = entry.file_size();
    require(bytes <= settings.quotaBytes - state.usage, "conversation journal quota exceeded");
    state.usage += static_cast<std::size_t>(bytes);
  }
}

NativeConversationJournal::~NativeConversationJournal() = default;

std::filesystem::path NativeConversationJournal::stateRoot() const
{
  return m_impl->config.stateRoot;
}

std::vector<std::vector<std::uint8_t>> NativeConversationJournal::authenticationKeys() const
{
  return nativeConversationAuthenticationKeys(m_impl->config.identity, m_impl->config.keys);
}

void NativeConversationJournal::appendConversation(
  const std::string& checkpointWire, const NativeJson& transcript, std::uint64_t nowMs,
  std::optional<std::size_t> nativeInitialPromptTokenCount)
{
  auto& state = *m_impl;
  std::lock_guard<std::mutex> guard(state.mutex);
  require(!state.poisoned && nowMs > 0, "conversation journal unavailable");
  const auto checkpoint = nativeParseJson(checkpointWire);
  nativeValidateConversationTranscript(transcript, checkpoint, nativeInitialPromptTokenCount);
  require(checkpoint.at("expiresAtMs").get<std::uint64_t>() > nowMs, "conversation journal checkpoint expired");
  const auto conversationId = checkpoint.at("conversationId").get<std::string>();
  const auto epoch = checkpoint.at("contextEpoch").get<std::uint64_t>();
  std::uint64_t durableParent = 0;
  for (auto it = state.records.rbegin(); it != state.records.rend(); ++it) {
    if (it->at("kind") == "conversation-checkpoint" &&
        it->at("payload").at("conversationId") == conversationId) {
      durableParent = it->at("payload").at("contextEpoch").get<std::uint64_t>();
      break;
    }
  }
  require(checkpoint.at("parentContextEpoch") == durableParent && epoch > durableParent &&
          epoch - durableParent == 1, "conversation durable parent changed");
  const auto envelopeId = "conversation-" + sha(conversationId) + "-" + std::to_string(epoch);
  const auto plaintext = nativeConversationCanonicalJson({{"checkpoint", nativeConversationBase64Encode(checkpointWire)},
                                              {"transcript", transcript}});
  const auto expiry = checkpoint.at("expiresAtMs").get<std::uint64_t>();
  const auto encoded = nativeSealConversationEnvelope(plaintext, envelopeId, expiry, state.config.keys.front());
  const auto wireDigest = "sha256:" + sha(encoded);
  auto envelope = makeRecord("protected-envelope", {
    {"request_id", envelopeId}, {"expires_at_ms", expiry}, {"wire_digest", wireDigest},
    {"encoded", nativeConversationBase64Encode(encoded)}}, nowMs);
  NativeJson indexPayload{
    {"conversationId", conversationId}, {"contextEpoch", epoch},
    {"checkpointDigest", checkpoint.at("checkpointDigest")}, {"envelopeId", envelopeId},
    {"wireDigest", wireDigest}, {"payloadDigest", "sha256:" + sha(plaintext)}, {"expiresAtMs", expiry}};
  if (nativeInitialPromptTokenCount) indexPayload["nativeInitialPromptTokenCount"] = *nativeInitialPromptTokenCount;
  auto index = makeRecord("conversation-checkpoint", std::move(indexPayload), nowMs);
  auto nextRecords = state.records;
  nextRecords.push_back(envelope); nextRecords.push_back(index);
  const auto transaction = makeRecord("journal-transaction", {{"records", {envelope, index}}}, nowMs);
  const auto wire = transaction.dump(-1, ' ', true) + "\n";
  require(wire.size() <= state.config.quotaBytes - state.usage, "conversation journal quota exceeded");
  std::size_t written = 0;
  try {
    while (written < wire.size()) {
      const auto count = ::pwrite(state.file.fd, wire.data() + written, wire.size() - written, state.size + written);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "conversation journal append failed");
      written += static_cast<std::size_t>(count);
    }
    require(::fsync(state.file.fd) == 0, "conversation journal append sync failed");
  }
  catch (...) {
    if (::ftruncate(state.file.fd, state.size) != 0 || ::fsync(state.file.fd) != 0) state.poisoned = true;
    throw;
  }
  state.records.swap(nextRecords);
  state.size += written;
  state.usage += written;
}

std::vector<NativeJson> NativeConversationJournal::readConversations(std::uint64_t nowMs) const
{
  const auto& state = *m_impl;
  std::lock_guard<std::mutex> guard(state.mutex);
  require(!state.poisoned && nowMs > 0, "conversation journal unavailable");
  std::map<std::string, NativeJson> envelopes;
  std::vector<NativeJson> result;
  for (const auto& record : state.records) {
    const auto& payload = record.at("payload");
    if (record.at("kind") == "protected-envelope") {
      envelopes[payload.at("request_id").get<std::string>()] = payload;
    }
    else if (record.at("kind") == "conversation-checkpoint") {
      const auto id = payload.at("envelopeId").get<std::string>();
      const auto it = envelopes.find(id);
      require(it != envelopes.end(), "conversation committed envelope missing");
      const auto encoded = nativeConversationBase64Decode(it->second.at("encoded").get<std::string>());
      const auto hash = "sha256:" + sha(encoded);
      require(hash == it->second.at("wire_digest") && hash == payload.at("wireDigest"),
              "conversation encrypted wire digest mismatch");
      // Authenticate even expired ciphertext before omitting retained state.
      const auto plaintext = nativeReadConversationEnvelope(encoded, id, state.config.keys, 1);
      if (payload.contains("payloadDigest"))
        require(payload.at("payloadDigest") == "sha256:" + sha(plaintext), "conversation payload digest mismatch");
      auto body = nativeParseJson(plaintext);
      require(body.is_object() && body.size() == 2 && body.contains("checkpoint") && body.contains("transcript"),
              "conversation journal body malformed");
      const auto checkpointWire = nativeConversationBase64Decode(body.at("checkpoint").get<std::string>());
      const auto checkpoint = nativeParseJson(checkpointWire);
      for (const auto field : {"conversationId", "contextEpoch", "checkpointDigest", "expiresAtMs"})
        require(checkpoint.at(field) == payload.at(field), "conversation journal index binding mismatch");
      require(checkpoint.at("expiresAtMs") == nativeParseJson(encoded).at("expiresAtMs"),
              "conversation envelope lifetime mismatch");
      std::optional<std::size_t> nativeInitialPromptTokenCount;
      if (payload.contains("nativeInitialPromptTokenCount")) {
        const auto& count = payload.at("nativeInitialPromptTokenCount");
        require(count.is_number_unsigned() || (count.is_number_integer() && count.get<std::int64_t>() >= 0),
                "conversation initial prompt count type invalid");
        require(count.get<std::uint64_t>() <= 1024 * 1024, "conversation initial prompt count exceeds bound");
        nativeInitialPromptTokenCount = count.get<std::size_t>();
        body["nativeInitialPromptTokenCount"] = *nativeInitialPromptTokenCount;
      }
      nativeValidateConversationTranscript(body.at("transcript"), checkpoint, nativeInitialPromptTokenCount);
      if (checkpoint.at("expiresAtMs").get<std::uint64_t>() <= nowMs) continue;
      body["checkpointWire"] = checkpointWire;
      result.push_back(std::move(body));
    }
  }
  return result;
}

} // namespace ndnsf::di

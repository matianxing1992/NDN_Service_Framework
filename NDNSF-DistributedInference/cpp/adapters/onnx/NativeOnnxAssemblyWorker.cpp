// Bounded native assembly worker: fixed anonymous-pipe protocol (T006-C).
//
// Structure mirrors native-onnx-assembly-design.md:
//   OA02 runNativeOnnxAssemblyWorker[At]  - parent transport: fixed installed
//       binary, registered library identity, spawn with only stdio mapped,
//       nonblocking pipes polled in both directions, steady deadline and
//       requireActive on every round, cancel TERM -> 1s -> KILL -> waitpid.
//       A late success has to pass requireActive again before publication.
//   OA03 runNativeOnnxAssemblyWorkerMain - worker child: closes every
//       non-stdio descriptor, parses one request frame under the fixed
//       "--stdio-v1 --metadata-bytes <decimal>" mode, drains trailing input,
//       revalidates the certified recipe and its digest (S1), assembles
//       through OA04 and writes exactly one response frame; exit 0/1/2.
//
// Only owned value types cross this TU; the worker never parses a network
// request, never sees credentials, and stdout carries protocol bytes only.

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <limits>
#include <mutex>
#include <optional>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace ndnsf::di {
namespace {

constexpr std::uint64_t MaxWorkerFramePayloadBytes = 2ULL * 1024 * 1024 * 1024;
constexpr std::size_t WorkerPayloadReserveChunk = 16 * 1024 * 1024;

void
workerFail(const char* code)
{
  throw std::runtime_error(std::string("DI_NATIVE_ONNX_") + code);
}

std::string
hexDigest(const std::vector<std::uint8_t>& bytes)
{
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256(bytes.data(), bytes.size(), hash);
  std::string out = "sha256:";
  for (unsigned char value : hash) {
    out.push_back("0123456789abcdef"[value >> 4]);
    out.push_back("0123456789abcdef"[value & 0x0f]);
  }
  return out;
}

std::string
hexDigestOfFile(int fd)
{
  SHA256_CTX context;
  SHA256_Init(&context);
  unsigned char buffer[65536];
  for (;;) {
    const ssize_t got = read(fd, buffer, sizeof(buffer));
    if (got == 0) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      return {};
    }
    SHA256_Update(&context, buffer, static_cast<std::size_t>(got));
  }
  unsigned char hash[SHA256_DIGEST_LENGTH];
  SHA256_Final(hash, &context);
  std::string out = "sha256:";
  for (unsigned char value : hash) {
    out.push_back("0123456789abcdef"[value >> 4]);
    out.push_back("0123456789abcdef"[value & 0x0f]);
  }
  return out;
}

// JSON string escaping matching python json.dumps(ensure_ascii=False): bytes
// >= 0x20 pass through raw except '"' and '\\'; \b \f \n \r \t get short
// escapes; other control bytes become lowercase \u00xx.
std::string
jsonString(const std::string& raw)
{
  static const char* kHex = "0123456789abcdef";
  std::string out;
  out.reserve(raw.size() + 8);
  for (unsigned char ch : raw) {
    switch (ch) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (ch < 0x20) {
        out += "\\u00";
        out.push_back(kHex[(ch >> 4) & 0x0f]);
        out.push_back(kHex[ch & 0x0f]);
      }
      else {
        out.push_back(static_cast<char>(ch));
      }
    }
  }
  return out;
}

std::string
jsonArrayOfStrings(const std::vector<std::string>& values)
{
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += '"' + jsonString(values[i]) + '"';
  }
  out += ']';
  return out;
}

std::string
jsonUint(std::uint64_t value)
{
  return std::to_string(value);
}

bool
parseStrictDecimal(const std::string& text, std::uint64_t& out)
{
  if (text.empty()) return false;
  if (text.size() > 1 && text.front() == '0') return false;
  if (!std::all_of(text.begin(), text.end(), [] (unsigned char ch) {
        return std::isdigit(ch) != 0;
      })) {
    return false;
  }
  try {
    out = std::stoull(text);
    return true;
  }
  catch (...) {
    return false;
  }
}

bool
isSha256Digest(const std::string& value)
{
  // "sha256:" (7) + 64 lowercase-or-uppercase hex digits = 71 bytes.
  if (value.size() != 71 || value.compare(0, 7, "sha256:") != 0) return false;
  return std::all_of(value.begin() + 7, value.end(), [] (unsigned char ch) {
    return std::isxdigit(ch) != 0;
  });
}

std::string
sanitizeMessage(const std::string& raw, std::size_t bound = 512)
{
  std::string out;
  out.reserve(std::min(raw.size(), bound));
  for (unsigned char ch : raw) {
    if (out.size() >= bound) break;
    if (ch == '\r' || ch == '\n' || ch == '\t') {
      out.push_back(' ');
    }
    else if (ch < 0x20) {
      // Other control bytes never carry meaning in a diagnostic.
    }
    else {
      out.push_back(static_cast<char>(ch));
    }
  }
  return out;
}

std::uint64_t
readU64le(const std::uint8_t* data)
{
  std::uint64_t value = 0;
  for (int i = 7; i >= 0; --i) {
    value = (value << 8) | data[i];
  }
  return value;
}

void
pushU64le(std::vector<std::uint8_t>& out, std::uint64_t value)
{
  for (int i = 0; i < 8; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
  }
}

void
pushU32le(std::vector<std::uint8_t>& out, std::uint32_t value)
{
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xff));
  }
}

bool
appendWorkerPayload(std::vector<std::uint8_t>& target,
                    const std::uint8_t* data, std::size_t size)
{
  if (target.size() > MaxWorkerFramePayloadBytes ||
      size > MaxWorkerFramePayloadBytes - target.size()) {
    return false;
  }
  if (target.capacity() - target.size() < size) {
    const auto required = target.size() + size;
    const auto growth = std::min<std::uint64_t>(
      WorkerPayloadReserveChunk, MaxWorkerFramePayloadBytes - required);
    try {
      target.reserve(required + static_cast<std::size_t>(growth));
    }
    catch (const std::exception&) {
      return false;
    }
  }
  target.insert(target.end(), data, data + size);
  return true;
}

} // namespace

// ---------------------------------------------------------------------------
// Minimal strict JSON DOM (grammar errors only; semantic rules live in the
// metadata validation).  Numbers must be canonical decimal integers; string
// escapes follow python json semantics.
// ---------------------------------------------------------------------------
namespace {

struct JsonNode
{
  enum class Type { Null, Bool, Number, String, Array, Object };

  Type type = Type::Null;
  bool boolean = false;
  std::string text;  // Number keeps its validated canonical digits here
  std::vector<JsonNode> array;
  std::vector<std::pair<std::string, JsonNode>> object;

  const JsonNode* find(const std::string& key) const
  {
    for (const auto& member : object) {
      if (member.first == key) return &member.second;
    }
    return nullptr;
  }
};

class JsonParser
{
public:
  explicit JsonParser(const std::string& text) : m_text(text) {}

  bool parse(JsonNode& out, std::string& error)
  {
    m_pos = 0;
    if (!skipWhitespace() || m_pos >= m_text.size() || !parseValue(out) ||
        !skipWhitespace() || m_pos != m_text.size()) {
      error = m_error.empty() ? "json grammar error"
                              : sanitizeMessage(m_error, 200);
      return false;
    }
    return true;
  }

private:
  const std::string& m_text;
  std::size_t m_pos = 0;
  std::string m_error;

  bool skipWhitespace()
  {
    while (m_pos < m_text.size()) {
      const char ch = m_text[m_pos];
      if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
      ++m_pos;
    }
    return true;
  }

  bool fail(const std::string& reason)
  {
    if (m_error.empty()) m_error = reason;
    return false;
  }

  bool parseValue(JsonNode& out)
  {
    if (!skipWhitespace()) return false;
    if (m_pos >= m_text.size()) return fail("unexpected end of input");
    const char ch = m_text[m_pos];
    if (ch == '{') return parseObject(out);
    if (ch == '[') return parseArray(out);
    if (ch == '"') return parseString(out);
    if (ch == 't' || ch == 'f') return parseBool(out);
    if (ch == 'n') {
      if (m_text.compare(m_pos, 4, "null") != 0) return fail("bad literal");
      m_pos += 4;
      out.type = JsonNode::Type::Null;
      return true;
    }
    if (ch == '-' || std::isdigit(static_cast<unsigned char>(ch)) != 0) {
      return parseNumber(out);
    }
    return fail("unexpected character");
  }

  bool parseBool(JsonNode& out)
  {
    if (m_text.compare(m_pos, 4, "true") == 0) {
      m_pos += 4;
      out.type = JsonNode::Type::Bool;
      out.boolean = true;
      return true;
    }
    if (m_text.compare(m_pos, 5, "false") == 0) {
      m_pos += 5;
      out.type = JsonNode::Type::Bool;
      out.boolean = false;
      return true;
    }
    return fail("bad literal");
  }

  bool parseNumber(JsonNode& out)
  {
    const std::size_t begin = m_pos;
    if (m_text[m_pos] == '-') ++m_pos;
    const std::size_t digits = m_pos;
    while (m_pos < m_text.size() &&
           std::isdigit(static_cast<unsigned char>(m_text[m_pos])) != 0) {
      ++m_pos;
    }
    if (m_pos == digits) return fail("expected digits");
    if (m_pos - digits > 1 && m_text[digits] == '0') {
      return fail("leading zeros are not canonical");
    }
    if (m_pos < m_text.size()) {
      const char ch = m_text[m_pos];
      if (ch == '.' || ch == 'e' || ch == 'E') {
        return fail("fractional or exponent numbers are not canonical");
      }
    }
    out.type = JsonNode::Type::Number;
    out.text.assign(m_text, begin, m_pos - begin);
    return true;
  }

  bool parseString(JsonNode& out)
  {
    ++m_pos;  // opening quote
    std::string value;
    for (;;) {
      if (m_pos >= m_text.size()) return fail("unterminated string");
      const unsigned char ch = static_cast<unsigned char>(m_text[m_pos]);
      if (ch == '"') {
        ++m_pos;
        out.type = JsonNode::Type::String;
        out.text = std::move(value);
        return true;
      }
      if (ch == '\\') {
        ++m_pos;
        if (m_pos >= m_text.size()) return fail("unterminated escape");
        const char esc = m_text[m_pos];
        switch (esc) {
        case '"': value.push_back('"'); ++m_pos; break;
        case '\\': value.push_back('\\'); ++m_pos; break;
        case '/': value.push_back('/'); ++m_pos; break;
        case 'b': value.push_back('\b'); ++m_pos; break;
        case 'f': value.push_back('\f'); ++m_pos; break;
        case 'n': value.push_back('\n'); ++m_pos; break;
        case 'r': value.push_back('\r'); ++m_pos; break;
        case 't': value.push_back('\t'); ++m_pos; break;
        case 'u': {
          if (!appendUnicodeEscape(value)) return false;
          break;
        }
        default:
          return fail("unknown escape");
        }
        continue;
      }
      if (ch < 0x20) return fail("raw control byte in string");
      value.push_back(static_cast<char>(ch));
      ++m_pos;
    }
  }

  int hexDigit(char ch)
  {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
  }

  bool appendUnicodeEscape(std::string& value)
  {
    if (m_pos + 5 > m_text.size()) return fail("short \\u escape");
    unsigned int code = 0;
    for (int i = 0; i < 4; ++i) {
      const int digit = hexDigit(m_text[m_pos + 1 + static_cast<std::size_t>(i)]);
      if (digit < 0) return fail("bad \\u escape");
      code = (code << 4) | static_cast<unsigned int>(digit);
    }
    m_pos += 5;
    if (code >= 0xd800 && code <= 0xdbff) {
      // Combine a surrogate pair; lone surrogates are rejected.
      if (m_pos + 6 > m_text.size() || m_text[m_pos] != '\\' ||
          m_text[m_pos + 1] != 'u') {
        return fail("lone high surrogate");
      }
      unsigned int low = 0;
      for (int i = 0; i < 4; ++i) {
        const int digit =
          hexDigit(m_text[m_pos + 2 + static_cast<std::size_t>(i)]);
        if (digit < 0) return fail("bad low surrogate");
        low = (low << 4) | static_cast<unsigned int>(digit);
      }
      if (low < 0xdc00 || low > 0xdfff) return fail("lone high surrogate");
      code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
      m_pos += 6;
    }
    else if (code >= 0xdc00 && code <= 0xdfff) {
      return fail("lone low surrogate");
    }
    if (code < 0x80) {
      value.push_back(static_cast<char>(code));
    }
    else if (code < 0x800) {
      value.push_back(static_cast<char>(0xc0 | (code >> 6)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    else if (code < 0x10000) {
      value.push_back(static_cast<char>(0xe0 | (code >> 12)));
      value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    else {
      value.push_back(static_cast<char>(0xf0 | (code >> 18)));
      value.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3f)));
      value.push_back(static_cast<char>(0x80 | (code & 0x3f)));
    }
    return true;
  }

  bool parseArray(JsonNode& out)
  {
    ++m_pos;  // '['
    out.type = JsonNode::Type::Array;
    for (;;) {
      if (!skipWhitespace()) return false;
      if (m_pos >= m_text.size()) return fail("unterminated array");
      if (m_text[m_pos] == ']') {
        ++m_pos;
        return true;
      }
      JsonNode item;
      if (!parseValue(item)) return false;
      out.array.push_back(std::move(item));
      if (!skipWhitespace()) return false;
      if (m_pos >= m_text.size()) return fail("unterminated array");
      const char ch = m_text[m_pos];
      if (ch == ',') {
        ++m_pos;
        continue;
      }
      if (ch == ']') {
        ++m_pos;
        return true;
      }
      return fail("expected , or ]");
    }
  }

  bool parseObject(JsonNode& out)
  {
    ++m_pos;  // '{'
    out.type = JsonNode::Type::Object;
    for (;;) {
      if (!skipWhitespace()) return false;
      if (m_pos >= m_text.size()) return fail("unterminated object");
      if (m_text[m_pos] == '}') {
        ++m_pos;
        return true;
      }
      if (m_text[m_pos] != '"') return fail("expected object key");
      JsonNode key;
      if (!parseString(key)) return false;
      if (!skipWhitespace()) return false;
      if (m_pos >= m_text.size() || m_text[m_pos] != ':') {
        return fail("expected :");
      }
      ++m_pos;
      JsonNode value;
      if (!parseValue(value)) return false;
      for (const auto& member : out.object) {
        if (member.first == key.text) return fail("duplicate object key");
      }
      out.object.emplace_back(key.text, std::move(value));
      if (!skipWhitespace()) return false;
      if (m_pos >= m_text.size()) return fail("unterminated object");
      const char ch = m_text[m_pos];
      if (ch == ',') {
        ++m_pos;
        continue;
      }
      if (ch == '}') {
        ++m_pos;
        return true;
      }
      return fail("expected , or }");
    }
  }
};

bool
parseJson(const std::string& text, JsonNode& out, std::string& error)
{
  return JsonParser(text).parse(out, error);
}

// Sorted compact object writer: python canonicalization sorts keys at every
// nesting level, so callers hand in key/value pairs and the writer sorts.
std::string
jsonSortedObject(std::vector<std::pair<std::string, std::string>> members)
{
  std::sort(members.begin(), members.end(),
            [] (const auto& left, const auto& right) {
              return left.first < right.first;
            });
  std::string out = "{";
  for (std::size_t i = 0; i < members.size(); ++i) {
    if (i != 0) out += ",";
    out += '"' + jsonString(members[i].first) + "\":" + members[i].second;
  }
  out += '}';
  return out;
}

std::string
jsonNumberArray(const std::vector<std::uint64_t>& values)
{
  std::string out = "[";
  for (std::size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out += ",";
    out += jsonUint(values[i]);
  }
  out += ']';
  return out;
}

std::string
jsonContract(const NativeAssemblyTensorContractV3& contract)
{
  std::string shape = "[";
  for (const auto& dimension : contract.shape) {
    if (shape.size() > 1) shape += ',';
    if (const auto* number = std::get_if<std::int64_t>(&dimension)) shape += std::to_string(*number);
    else shape += '"' + jsonString(std::get<std::string>(dimension)) + '"';
  }
  shape += ']';
  // Keys sorted: dtype < name < shape.
  return jsonSortedObject({
    {"dtype", '"' + jsonString(contract.dtype) + '"'},
    {"name", '"' + jsonString(contract.name) + '"'},
    {"shape", shape},
  });
}

std::string
jsonContracts(const std::vector<NativeAssemblyTensorContractV3>& contracts)
{
  std::string out = "[";
  for (std::size_t i = 0; i < contracts.size(); ++i) {
    if (i != 0) out += ",";
    out += jsonContract(contracts[i]);
  }
  out += ']';
  return out;
}

// Convert a canonical decimal Number node (unsigned only) into uint64.
bool
unsignedFromJson(const JsonNode& node, std::uint64_t& out)
{
  if (node.type != JsonNode::Type::Number) return false;
  if (node.text.empty() || node.text.front() == '-') return false;
  return parseStrictDecimal(node.text, out);
}

bool
requireObject(const JsonNode& node)
{
  return node.type == JsonNode::Type::Object;
}

const JsonNode*
requireObjectMember(const JsonNode& object, const std::string& key)
{
  if (object.type != JsonNode::Type::Object) return nullptr;
  return object.find(key);
}

} // namespace

// ---------------------------------------------------------------------------
// Certified recipe canonical JSON + worker metadata (S1).
// ---------------------------------------------------------------------------
std::string
canonicalNativeOnnxRecipeJson(const NativeCertifiedRecipe& recipe)
{
  // inputNames/outputNames are not native members: the certified payload
  // binds them to contract order.  Ordering differences between a payload's
  // own names and its contracts cannot be represented natively and are
  // rejected by the digest comparison (a compliant parent re-derives the
  // same bytes before spawning, so no such payload ever reaches a worker).
  std::vector<std::string> inputNames;
  inputNames.reserve(recipe.expectedInputs.size());
  for (const auto& contract : recipe.expectedInputs) {
    inputNames.push_back(contract.name);
  }
  std::vector<std::string> outputNames;
  outputNames.reserve(recipe.expectedOutputs.size());
  for (const auto& contract : recipe.expectedOutputs) {
    outputNames.push_back(contract.name);
  }
  return jsonSortedObject({
    {"adapterDescriptorDigest",
     '"' + jsonString(recipe.adapterDescriptorDigest) + '"'},
    {"artifactProfileDigest",
     '"' + jsonString(recipe.artifactProfileDigest) + '"'},
    {"assemblerDescriptorDigest",
     '"' + jsonString(recipe.assemblerDescriptorDigest) + '"'},
    {"backendAbi", '"' + jsonString(recipe.backendAbi) + '"'},
    {"canonicalInitializerDigest",
     '"' + jsonString(recipe.canonicalInitializerDigest) + '"'},
    {"expectedInputs", jsonContracts(recipe.expectedInputs)},
    {"expectedOutputs", jsonContracts(recipe.expectedOutputs)},
    {"graphDigest", '"' + jsonString(recipe.graphDigest) + '"'},
    {"inputNames", jsonArrayOfStrings(inputNames)},
    {"layerBegin", jsonUint(recipe.layerBegin)},
    {"layerEnd", jsonUint(recipe.layerEnd)},
    {"layout", '"' + jsonString(recipe.layout) + '"'},
    {"maxAssembledBytes", jsonUint(recipe.maxAssembledBytes)},
    {"maxNodes", jsonUint(recipe.maxNodes)},
    {"maxSourceBytes", jsonUint(recipe.maxSourceBytes)},
    {"modelManifestDigest",
     '"' + jsonString(recipe.modelManifestDigest) + '"'},
    {"nodeIndices", jsonNumberArray(recipe.nodeIndices)},
    {"outputNames", jsonArrayOfStrings(outputNames)},
    {"padding", '"' + jsonString(recipe.padding) + '"'},
    {"precision", '"' + jsonString(recipe.precision) + '"'},
    {"quantization", '"' + jsonString(recipe.quantization) + '"'},
    {"roleKind", '"' + jsonString(recipe.roleKind) + '"'},
    {"schema", '"' + std::string(kNativeOnnxCertifiedRecipeSchema) + '"'},
  });
}

namespace {

// Certified payload rules mirrored from CertifiedOnnxAssemblyRecipe.__post_init__.
bool
readCertifiedSlice(const JsonNode& recipeNode, NativeCertifiedRecipe& out,
                   std::string& failure)
{
  const auto* schema = requireObjectMember(recipeNode, "schema");
  const auto* roleKind = requireObjectMember(recipeNode, "roleKind");
  const auto* backendAbi = requireObjectMember(recipeNode, "backendAbi");
  if (schema == nullptr || roleKind == nullptr || backendAbi == nullptr) {
    failure = "certified recipe is missing schema/roleKind/backendAbi";
    return false;
  }
  if (schema->type != JsonNode::Type::String ||
      schema->text != kNativeOnnxCertifiedRecipeSchema) {
    failure = "certified recipe schema mismatch";
    return false;
  }
  if (roleKind->type != JsonNode::Type::String || roleKind->text.empty()) {
    failure = "certified roleKind must be a non-empty string";
    return false;
  }
  if (backendAbi->type != JsonNode::Type::String || backendAbi->text.empty()) {
    failure = "certified backendAbi must be a non-empty string";
    return false;
  }
  out.roleKind = roleKind->text;
  out.backendAbi = backendAbi->text;

  const char* kDigestKeys[] = {
    "modelManifestDigest", "artifactProfileDigest", "graphDigest",
    "canonicalInitializerDigest", "adapterDescriptorDigest",
    "assemblerDescriptorDigest",
  };
  std::string* kDigestFields[] = {
    &out.modelManifestDigest, &out.artifactProfileDigest, &out.graphDigest,
    &out.canonicalInitializerDigest, &out.adapterDescriptorDigest,
    &out.assemblerDescriptorDigest,
  };
  for (std::size_t i = 0; i < 6; ++i) {
    const auto* node = requireObjectMember(recipeNode, kDigestKeys[i]);
    if (node == nullptr || node->type != JsonNode::Type::String ||
        !isSha256Digest(node->text)) {
      failure = "certified digest field is invalid";
      return false;
    }
    *kDigestFields[i] = node->text;
  }

  const auto* layerBegin = requireObjectMember(recipeNode, "layerBegin");
  const auto* layerEnd = requireObjectMember(recipeNode, "layerEnd");
  if (layerBegin == nullptr || layerEnd == nullptr ||
      !unsignedFromJson(*layerBegin, out.layerBegin) ||
      !unsignedFromJson(*layerEnd, out.layerEnd)) {
    failure = "certified layer interval is invalid";
    return false;
  }
  if (out.roleKind == "COMPONENT_SET") {
    if (out.layerBegin != 0 || out.layerEnd != 0) {
      failure = "COMPONENT_SET recipe must not use a layer interval";
      return false;
    }
  }
  else if (out.layerEnd <= out.layerBegin) {
    failure = "range/rank recipe requires a non-empty layer interval";
    return false;
  }

  const auto* nodeIndices = requireObjectMember(recipeNode, "nodeIndices");
  if (nodeIndices == nullptr || nodeIndices->type != JsonNode::Type::Array ||
      nodeIndices->array.empty()) {
    failure = "certified node cover is missing or empty";
    return false;
  }
  for (const auto& index : nodeIndices->array) {
    std::uint64_t value = 0;
    if (!unsignedFromJson(index, value)) {
      failure = "certified node cover is invalid";
      return false;
    }
    if (!out.nodeIndices.empty() && value <= out.nodeIndices.back()) {
      failure = "certified node cover is missing or overlapping";
      return false;
    }
    out.nodeIndices.push_back(value);
  }

  const auto* maxSourceBytes = requireObjectMember(recipeNode, "maxSourceBytes");
  const auto* maxAssembledBytes =
    requireObjectMember(recipeNode, "maxAssembledBytes");
  const auto* maxNodes = requireObjectMember(recipeNode, "maxNodes");
  if (maxSourceBytes == nullptr || maxAssembledBytes == nullptr ||
      maxNodes == nullptr || !unsignedFromJson(*maxSourceBytes, out.maxSourceBytes) ||
      !unsignedFromJson(*maxAssembledBytes, out.maxAssembledBytes) ||
      !unsignedFromJson(*maxNodes, out.maxNodes) || out.maxSourceBytes == 0 ||
      out.maxAssembledBytes == 0 || out.maxNodes == 0) {
    failure = "certified resource bounds must be positive";
    return false;
  }

  const auto* precision = requireObjectMember(recipeNode, "precision");
  const auto* quantization = requireObjectMember(recipeNode, "quantization");
  const auto* layout = requireObjectMember(recipeNode, "layout");
  const auto* padding = requireObjectMember(recipeNode, "padding");
  if (precision == nullptr || precision->type != JsonNode::Type::String ||
      quantization == nullptr || quantization->type != JsonNode::Type::String ||
      layout == nullptr || layout->type != JsonNode::Type::String ||
      padding == nullptr || padding->type != JsonNode::Type::String) {
    failure = "certified precision family is invalid";
    return false;
  }
  out.precision = precision->text;
  out.quantization = quantization->text;
  out.layout = layout->text;
  out.padding = padding->text;

  // Names and contracts.  The payload carries inputNames/outputNames that the
  // native type has no members for; they are cross-checked against contract
  // names (python exact-cover rule) before the digest comparison pins the
  // contract order as canonical.
  const auto readGroup = [&] (const char* namesKey, const char* contractsKey,
                              std::vector<NativeAssemblyTensorContractV3>&
                                contracts,
                              std::string& groupFailure) {
    const auto* namesNode = requireObjectMember(recipeNode, namesKey);
    const auto* contractsNode = requireObjectMember(recipeNode, contractsKey);
    if (namesNode == nullptr || namesNode->type != JsonNode::Type::Array ||
        namesNode->array.empty() || contractsNode == nullptr ||
        contractsNode->type != JsonNode::Type::Array ||
        contractsNode->array.empty()) {
      groupFailure = "certified io cover is missing or empty";
      return false;
    }
    std::vector<std::string> names;
    for (const auto& name : namesNode->array) {
      if (name.type != JsonNode::Type::String || name.text.empty()) {
        groupFailure = "certified io names must be non-empty strings";
        return false;
      }
      if (std::find(names.begin(), names.end(), name.text) != names.end()) {
        groupFailure = "certified io names must be unique";
        return false;
      }
      names.push_back(name.text);
    }
    std::vector<NativeAssemblyTensorContractV3> parsed;
    for (const auto& item : contractsNode->array) {
      const auto* name = requireObjectMember(item, "name");
      const auto* dtype = requireObjectMember(item, "dtype");
      const auto* shape = requireObjectMember(item, "shape");
      if (name == nullptr || name->type != JsonNode::Type::String ||
          name->text.empty() || dtype == nullptr ||
          dtype->type != JsonNode::Type::String || dtype->text.empty() ||
          shape == nullptr || shape->type != JsonNode::Type::Array) {
        groupFailure = "certified io contract is invalid";
        return false;
      }
      NativeAssemblyTensorContractV3 contract;
      contract.name = name->text;
      contract.dtype = dtype->text;
      for (const auto& dim : shape->array) {
        if (dim.type == JsonNode::Type::String) {
          contract.shape.emplace_back(dim.text);
          continue;
        }
        if (dim.type != JsonNode::Type::Number) {
          groupFailure = "certified io contract shape is invalid";
          return false;
        }
        try {
          std::size_t consumed = 0;
          const auto number = std::stoll(dim.text, &consumed);
          if (consumed != dim.text.size()) throw std::invalid_argument("noninteger shape");
          contract.shape.emplace_back(static_cast<std::int64_t>(number));
        }
        catch (const std::exception&) {
          groupFailure = "certified io contract shape is invalid";
          return false;
        }
      }
      parsed.push_back(std::move(contract));
    }
    if (parsed.size() != names.size()) {
      groupFailure = "certified io cover does not match contract count";
      return false;
    }
    std::vector<std::string> contractNames;
    for (const auto& contract : parsed) {
      contractNames.push_back(contract.name);
    }
    std::vector<std::string> sortedPayload = names;
    std::vector<std::string> sortedContract = contractNames;
    std::sort(sortedPayload.begin(), sortedPayload.end());
    std::sort(sortedContract.begin(), sortedContract.end());
    if (sortedPayload != sortedContract) {
      groupFailure = "certified io cover does not match contract names";
      return false;
    }
    contracts = std::move(parsed);
    return true;
  };
  if (!readGroup("inputNames", "expectedInputs", out.expectedInputs, failure)) {
    return false;
  }
  if (!readGroup("outputNames", "expectedOutputs", out.expectedOutputs,
                 failure)) {
    return false;
  }
  return true;
}

std::string
digestOfRecipeJson(const std::string& canonicalJson)
{
  std::vector<std::uint8_t> bytes(canonicalJson.begin(), canonicalJson.end());
  return hexDigest(bytes);
}

// Sanitized bounded protocol log line for the child stderr (never carries
// source or secret content).
void
childDiag(const std::string& message)
{
  std::string line =
    "DI_NATIVE_ONNX_WORKER: " + sanitizeMessage(message, 400) + "\n";
  std::size_t offset = 0;
  while (offset < line.size()) {
    const ssize_t written =
      write(STDERR_FILENO, line.data() + offset, line.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return;
    }
    offset += static_cast<std::size_t>(written);
  }
}

} // namespace

std::string
buildNativeOnnxWorkerRequestMetadata(const NativeCertifiedRecipe& recipe)
{
  const std::string canonicalRecipe = canonicalNativeOnnxRecipeJson(recipe);
  return jsonSortedObject({
    {"backend", '"' + jsonString(recipe.backend) + '"'},
    {"adapterId", '"' + jsonString(recipe.adapterId) + '"'},
    {"recipe", canonicalRecipe},
    {"recipeDigest",
     '"' + jsonString(digestOfRecipeJson(canonicalRecipe)) + '"'},
    {"schema", '"' + std::string(kNativeOnnxAssemblyRequestSchema) + '"'},
  });
}

NativeOnnxMetadataCheck
validateNativeOnnxWorkerMetadata(const std::string& json)
{
  NativeOnnxMetadataCheck check;
  JsonNode root;
  std::string error;
  if (!parseJson(json, root, error)) {
    check.failureCode = "DI_NATIVE_ONNX_WORKER_METADATA";
    check.failureMessage = "request metadata is not valid JSON";
    return check;
  }
  const auto* schema = requireObjectMember(root, "schema");
  const auto* backend = requireObjectMember(root, "backend");
  const auto* adapterId = requireObjectMember(root, "adapterId");
  const auto* recipeDigest = requireObjectMember(root, "recipeDigest");
  const auto* recipe = requireObjectMember(root, "recipe");
  if (schema == nullptr || schema->type != JsonNode::Type::String ||
      schema->text != kNativeOnnxAssemblyRequestSchema ||
      backend == nullptr || backend->type != JsonNode::Type::String ||
      backend->text.empty() || adapterId == nullptr ||
      adapterId->type != JsonNode::Type::String || adapterId->text.empty() ||
      recipe == nullptr || !requireObject(*recipe)) {
    check.failureCode = "DI_NATIVE_ONNX_WORKER_METADATA";
    check.failureMessage = "request metadata envelope is invalid";
    return check;
  }
  if (recipeDigest == nullptr || recipeDigest->type != JsonNode::Type::String ||
      !isSha256Digest(recipeDigest->text)) {
    check.failureCode = "DI_NATIVE_ONNX_WORKER_METADATA";
    check.failureMessage = "request metadata recipeDigest is invalid";
    return check;
  }

  NativeCertifiedRecipe slice;
  std::string ruleFailure;
  if (!readCertifiedSlice(*recipe, slice, ruleFailure)) {
    check.failureCode = "DI_NATIVE_ONNX_RECIPE";
    check.failureMessage = ruleFailure;
    return check;
  }

  // The payload itself is not authorization evidence: its digest must match
  // the recipeDigest the role certificate binds.  Re-serialization is the
  // canonical python form (sorted compact keys), never an IPC alias.
  if (digestOfRecipeJson(canonicalNativeOnnxRecipeJson(slice)) !=
      recipeDigest->text) {
    check.failureCode = "DI_NATIVE_ONNX_RECIPE";
    check.failureMessage = "recipe digest does not match the certified payload";
    return check;
  }

  slice.adapterId = adapterId->text;
  slice.backend = backend->text;
  check.ok = true;
  check.value.recipe = std::move(slice);
  check.value.recipeDigest = recipeDigest->text;
  check.value.schema = kNativeOnnxAssemblyRequestSchema;
  return check;
}

// ---------------------------------------------------------------------------
// Frame decoders.
// ---------------------------------------------------------------------------
NativeOnnxRequestDecoder::Result
NativeOnnxRequestDecoder::feed(const std::uint8_t* data, std::size_t size)
{
  // Terminal states: any byte observed after a completed frame is a
  // duplicate/second frame by protocol.
  if (m_result == Result::Complete || m_result == Result::ProtocolError) {
    if (size != 0) {
      m_result = Result::ProtocolError;
      m_phase = Phase::Done;
    }
    return m_result;
  }
  for (;;) {
    if (m_phase == Phase::Header) {
      // 8B magic + 3 x u64le lengths + 1B hasInitializer.  Accumulated in a
      // member because header bytes may trickle in across feed calls.
      static constexpr std::size_t kHeaderSize = 33;
      const std::size_t take = std::min(size, kHeaderSize - m_headerBytes);
      std::memcpy(m_headerData.data() + m_headerBytes, data, take);
      data += take;
      size -= take;
      m_headerBytes += take;
      if (m_headerBytes < kHeaderSize) return Result::NeedMore;
      const std::uint8_t* header = m_headerData.data();
      if (std::memcmp(header, kNdnSf182RequestMagic, 8) != 0) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      m_header.metadataLength = readU64le(header + 8);
      m_header.modelLength = readU64le(header + 16);
      m_header.initializerLength = readU64le(header + 24);
      const std::uint8_t flag = header[32];
      if (flag > 1 || (flag == 0 && m_header.initializerLength != 0)) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      m_header.hasInitializer = flag != 0;
      m_metadataRemaining = m_header.metadataLength;
      m_modelRemaining = m_header.modelLength;
      m_initializerRemaining = m_header.initializerLength;
      // The request frame is produced by a bounded parent, but the child
      // still treats its pipe as untrusted input.  Apply a fixed total
      // payload bound before accepting bytes.  Model and initializer storage
      // grows in bounded chunks below; never reserve a header-declared GiB
      // amount before the metadata recipe has been checked.
      if (m_header.metadataLength > kNativeOnnxWorkerMaxMetadataBytes ||
          m_header.modelLength > MaxWorkerFramePayloadBytes ||
          m_header.initializerLength > MaxWorkerFramePayloadBytes ||
          m_header.initializerLength > MaxWorkerFramePayloadBytes -
            m_header.modelLength) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      try {
        m_metadata.reserve(static_cast<std::size_t>(m_header.metadataLength));
      }
      catch (const std::exception&) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      m_phase = Phase::Payload;
      continue;
    }
    // Payload: metadata, model, initializer segments in fixed order.
    // Zero-length segments complete without needing any input byte.
    if (!m_metadataDone) {
      if (m_metadataRemaining == 0) {
        m_metadataDone = true;
        continue;
      }
      const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_metadataRemaining,
                                static_cast<std::uint64_t>(size)));
      if (take == 0) return Result::NeedMore;
      m_metadata.insert(m_metadata.end(), data, data + take);
      data += take;
      size -= take;
      m_metadataRemaining -= take;
      continue;
    }
    if (!m_modelDone) {
      if (m_modelRemaining == 0) {
        m_modelDone = true;
        continue;
      }
      const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_modelRemaining,
                                static_cast<std::uint64_t>(size)));
      if (take == 0) return Result::NeedMore;
      if (!appendWorkerPayload(m_model, data, take)) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      data += take;
      size -= take;
      m_modelRemaining -= take;
      continue;
    }
    if (!m_initializerDone) {
      if (m_initializerRemaining == 0) {
        m_initializerDone = true;
        continue;
      }
      const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_initializerRemaining,
                                static_cast<std::uint64_t>(size)));
      if (take == 0) return Result::NeedMore;
      if (!appendWorkerPayload(m_initializer, data, take)) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      data += take;
      size -= take;
      m_initializerRemaining -= take;
      continue;
    }
    // Frame complete.  Bytes still left in this same call would be a second
    // frame; report it now rather than on a later feed.
    m_phase = Phase::Done;
    m_result = (size != 0) ? Result::ProtocolError : Result::Complete;
    return m_result;
  }
}

NativeOnnxResponseDecoder::Result
NativeOnnxResponseDecoder::feed(const std::uint8_t* data, std::size_t size)
{
  if (m_result == Result::Complete || m_result == Result::ProtocolError) {
    if (size != 0) {
      m_result = Result::ProtocolError;
      m_phase = Phase::Done;
    }
    return m_result;
  }
  for (;;) {
    if (m_phase == Phase::Header) {
      // 8B magic + u32le metadata + u64le model + 1B status.
      static constexpr std::size_t kHeaderSize = 21;
      const std::size_t take = std::min(size, kHeaderSize - m_headerBytes);
      std::memcpy(m_headerData.data() + m_headerBytes, data, take);
      data += take;
      size -= take;
      m_headerBytes += take;
      if (m_headerBytes < kHeaderSize) return Result::NeedMore;
      const std::uint8_t* header = m_headerData.data();
      if (std::memcmp(header, kNdnSf182ResponseMagic, 8) != 0) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      std::uint32_t metadataLength = 0;
      for (int i = 0; i < 4; ++i) {
        metadataLength |=
          static_cast<std::uint32_t>(header[8 + i]) << (8 * i);
      }
      m_header.metadataLength = metadataLength;
      m_header.modelLength = readU64le(header + 12);
      m_header.status = header[20];
      if (m_header.status > 2 ||
          m_header.metadataLength > kNativeOnnxWorkerMaxMetadataBytes ||
          (m_header.status == 0 && m_header.modelLength == 0) ||
          (m_header.status != 0 && m_header.modelLength != 0)) {
        m_result = Result::ProtocolError;
        m_phase = Phase::Done;
        return m_result;
      }
      m_metadataRemaining = m_header.metadataLength;
      m_modelRemaining = m_header.modelLength;
      m_phase = Phase::Payload;
      continue;
    }
    // Payload: metadata then model; zero-length segments complete without
    // needing any input byte.
    if (!m_metadataDone) {
      if (m_metadataRemaining == 0) {
        m_metadataDone = true;
        continue;
      }
      const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_metadataRemaining,
                                static_cast<std::uint64_t>(size)));
      if (take == 0) return Result::NeedMore;
      m_metadata.insert(m_metadata.end(), data, data + take);
      data += take;
      size -= take;
      m_metadataRemaining -= take;
      continue;
    }
    if (!m_modelDone) {
      if (m_modelRemaining == 0) {
        m_modelDone = true;
        continue;
      }
      const std::size_t take = static_cast<std::size_t>(
        std::min<std::uint64_t>(m_modelRemaining,
                                static_cast<std::uint64_t>(size)));
      if (take == 0) return Result::NeedMore;
      m_model.insert(m_model.end(), data, data + take);
      data += take;
      size -= take;
      m_modelRemaining -= take;
      continue;
    }
    m_phase = Phase::Done;
    m_result = (size != 0) ? Result::ProtocolError : Result::Complete;
    return m_result;
  }
}

std::vector<std::uint8_t>
composeNativeOnnxWorkerRequest(const std::string& metadata,
                               const std::vector<std::uint8_t>& model,
                               const std::vector<std::uint8_t>& initializer,
                               bool hasInitializer)
{
  if (hasInitializer != !initializer.empty()) {
    workerFail("WORKER_PROTOCOL");
  }
  std::vector<std::uint8_t> frame;
  frame.reserve(33 + metadata.size() + model.size() + initializer.size());
  frame.insert(frame.end(), kNdnSf182RequestMagic,
               kNdnSf182RequestMagic + 8);
  pushU64le(frame, static_cast<std::uint64_t>(metadata.size()));
  pushU64le(frame, static_cast<std::uint64_t>(model.size()));
  pushU64le(frame, static_cast<std::uint64_t>(initializer.size()));
  frame.push_back(hasInitializer ? 1 : 0);
  frame.insert(frame.end(), metadata.begin(), metadata.end());
  frame.insert(frame.end(), model.begin(), model.end());
  if (hasInitializer) {
    frame.insert(frame.end(), initializer.begin(), initializer.end());
  }
  return frame;
}

std::vector<std::uint8_t>
composeNativeOnnxWorkerResponse(std::uint8_t status,
                                const std::string& metadata,
                                const std::vector<std::uint8_t>& model)
{
  if (status > 2 || (status != 0) != model.empty() ||
      metadata.size() > kNativeOnnxWorkerMaxMetadataBytes) {
    workerFail("WORKER_PROTOCOL");
  }
  std::vector<std::uint8_t> frame;
  frame.reserve(21 + metadata.size() + model.size());
  frame.insert(frame.end(), kNdnSf182ResponseMagic,
               kNdnSf182ResponseMagic + 8);
  pushU32le(frame, static_cast<std::uint32_t>(metadata.size()));
  pushU64le(frame, static_cast<std::uint64_t>(model.size()));
  frame.push_back(status);
  frame.insert(frame.end(), metadata.begin(), metadata.end());
  frame.insert(frame.end(), model.begin(), model.end());
  return frame;
}

// ---------------------------------------------------------------------------
// Parent-side result revalidation.
// ---------------------------------------------------------------------------
NativeOnnxWorkerOutcome
finalizeNativeOnnxWorkerResponse(bool childExitZero, std::uint8_t status,
                                 const std::string& metadataJson,
                                 const std::vector<std::uint8_t>& modelBytes,
                                 const NativeCertifiedRecipe& recipe,
                                 std::uint64_t maxAssembledBytes,
                                 bool activeAfterResponse)
{
  NativeOnnxWorkerOutcome outcome;
  const std::string errorPrefix = "DI_NATIVE_ONNX_";

  // An ok response is never accepted from a worker that did not exit cleanly.
  if (status == 0 && !childExitZero) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_EXITED";
    outcome.failureMessage = "worker exited nonzero after an ok response";
    return outcome;
  }

  JsonNode metadata;
  std::string parseError;
  if (!parseJson(metadataJson, metadata, parseError)) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
    outcome.failureMessage = "worker response metadata is not valid JSON";
    return outcome;
  }
  const auto* schema = requireObjectMember(metadata, "schema");
  if (schema == nullptr || schema->type != JsonNode::Type::String) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
    outcome.failureMessage = "worker response metadata has no schema";
    return outcome;
  }

  if (status != 0) {
    // Algorithm reject (1) or protocol input error (2): the failure metadata
    // carries the fixed code and a bounded sanitized message.  The response
    // frame already forced modelLength == 0 for these statuses.
    if (schema->text != kNativeOnnxAssemblyErrorSchema) {
      outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
      outcome.failureMessage = "worker error response has the wrong schema";
      return outcome;
    }
    const auto* code = requireObjectMember(metadata, "code");
    const auto* message = requireObjectMember(metadata, "message");
    if (code == nullptr || code->type != JsonNode::Type::String ||
        code->text.empty() || code->text.size() > 96 ||
        code->text.compare(0, errorPrefix.size(), errorPrefix) != 0) {
      outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
      outcome.failureMessage = "worker error response is malformed";
      return outcome;
    }
    if (message == nullptr || message->type != JsonNode::Type::String ||
        message->text.size() > 1024) {
      outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
      outcome.failureMessage = "worker error message is malformed";
      return outcome;
    }
    outcome.failureCode = code->text;
    outcome.failureMessage = message->text;
    return outcome;
  }

  if (schema->text != kNativeOnnxAssemblyResultSchema) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_PROTOCOL";
    outcome.failureMessage = "worker ok response has the wrong schema";
    return outcome;
  }
  if (!activeAfterResponse) {
    // The cancellation arrived after the response was fully validated; the
    // late success must never be published.
    outcome.failureCode = "DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT";
    outcome.failureMessage = "cancellation after worker success";
    return outcome;
  }
  if (modelBytes.empty() || modelBytes.size() > recipe.maxAssembledBytes ||
      modelBytes.size() > maxAssembledBytes) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT";
    outcome.failureMessage = "worker model bytes violate the assembly budget";
    return outcome;
  }
  const auto* modelDigest = requireObjectMember(metadata, "modelDigest");
  const auto* nodeCount = requireObjectMember(metadata, "nodeCount");
  const auto* inputNames = requireObjectMember(metadata, "inputNames");
  const auto* outputNames = requireObjectMember(metadata, "outputNames");
  if (modelDigest == nullptr || modelDigest->type != JsonNode::Type::String ||
      !isSha256Digest(modelDigest->text) || nodeCount == nullptr ||
      !unsignedFromJson(*nodeCount, outcome.value.nodeCount) ||
      inputNames == nullptr || inputNames->type != JsonNode::Type::Array ||
      outputNames == nullptr || outputNames->type != JsonNode::Type::Array) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT";
    outcome.failureMessage = "worker result metadata is malformed";
    return outcome;
  }
  if (hexDigest(modelBytes) != modelDigest->text) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT_DIGEST";
    outcome.failureMessage = "worker model bytes do not match modelDigest";
    return outcome;
  }
  if (outcome.value.nodeCount != recipe.nodeIndices.size() ||
      inputNames->array.size() != recipe.expectedInputs.size() ||
      outputNames->array.size() != recipe.expectedOutputs.size()) {
    outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT";
    outcome.failureMessage = "worker result io counts mismatch the recipe";
    return outcome;
  }
  std::size_t position = 0;
  for (const auto& expected : recipe.expectedInputs) {
    const auto& name = inputNames->array[position];
    if (name.type != JsonNode::Type::String || name.text != expected.name) {
      outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT";
      outcome.failureMessage = "worker result io names mismatch the recipe";
      return outcome;
    }
    ++position;
  }
  position = 0;
  for (const auto& expected : recipe.expectedOutputs) {
    const auto& name = outputNames->array[position];
    if (name.type != JsonNode::Type::String || name.text != expected.name) {
      outcome.failureCode = "DI_NATIVE_ONNX_WORKER_RESULT";
      outcome.failureMessage = "worker result io names mismatch the recipe";
      return outcome;
    }
    ++position;
  }
  outcome.ok = true;
  outcome.value.modelBytes = modelBytes;
  outcome.value.nodeCount = recipe.nodeIndices.size();
  outcome.value.inputNames.reserve(inputNames->array.size());
  for (const auto& name : inputNames->array) {
    outcome.value.inputNames.push_back(name.text);
  }
  outcome.value.outputNames.reserve(outputNames->array.size());
  for (const auto& name : outputNames->array) {
    outcome.value.outputNames.push_back(name.text);
  }
  outcome.value.modelDigest = modelDigest->text;
  return outcome;
}

// ---------------------------------------------------------------------------
// OA02 parent transport.
// ---------------------------------------------------------------------------
namespace {

// Registered fixed worker location; the library identity is process-global
// and there is no PATH search or arbitrary-worker fallback.
std::mutex gWorkerLocationMutex;
std::optional<NativeOnnxWorkerLocation> gWorkerLocation;

std::uint64_t
millisUntil(const std::chrono::steady_clock::time_point& deadline)
{
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return 0;
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
    deadline - now);
  if (remaining.count() <= 0) return 0;
  return static_cast<std::uint64_t>(remaining.count());
}

void
entryActiveGate(const NativeAssemblyControl& control)
{
  if (!control.requireActive ||
      std::chrono::steady_clock::now() >= control.deadline) {
    workerFail("ASSEMBLY_TIMEOUT");
  }
  control.requireActive();
}

// Mirror of the in-process chain entry gates so the parent never spawns a
// worker for input the chain would deterministically reject.
void
entryBudgetGate(const NativeCanonicalSource& source,
                const NativeCertifiedRecipe& recipe,
                const NativeAssemblyControl& control)
{
  if (source.modelBytes.size() > control.maxSourceBytes) {
    workerFail("SOURCE_LIMIT");
  }
  if (source.initializerBytes &&
      source.initializerBytes->size() > control.maxSourceBytes) {
    workerFail("INITIALIZER_LIMIT");
  }
  if (recipe.adapterId.empty() || recipe.backend.empty() ||
      recipe.roleKind.empty() || recipe.nodeIndices.empty() ||
      recipe.nodeIndices.size() > recipe.maxNodes) {
    workerFail("RECIPE");
  }
  if (recipe.expectedInputs.empty() || recipe.expectedOutputs.empty()) {
    workerFail("IO_CONTRACT");
  }
}

// Fixed worker endpoints; every other parent descriptor is CLOEXEC so the
// child exec never inherits it (the child additionally sweeps fd >= 3).
class WorkerProcess
{
public:
  ~WorkerProcess()
  {
    if (toChild >= 0) close(toChild);
    if (fromChild >= 0) close(fromChild);
    if (childErr >= 0) close(childErr);
  }

  void start(const NativeOnnxWorkerLocation& location,
             std::uint64_t metadataBytes)
  {
    int stdinPipe[2] = {-1, -1};
    int stdoutPipe[2] = {-1, -1};
    int stderrPipe[2] = {-1, -1};
    if (pipe2(stdinPipe, O_CLOEXEC | O_NONBLOCK) != 0 ||
        pipe2(stdoutPipe, O_CLOEXEC | O_NONBLOCK) != 0 ||
        pipe2(stderrPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
      close(stdinPipe[0]); close(stdinPipe[1]);
      close(stdoutPipe[0]); close(stdoutPipe[1]);
      close(stderrPipe[0]); close(stderrPipe[1]);
      workerFail("WORKER_PREFLIGHT");
    }
    toChild = stdinPipe[1];
    fromChild = stdoutPipe[0];
    childErr = stderrPipe[0];

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (posix_spawn_file_actions_init(&actions) != 0 ||
        posix_spawnattr_init(&attributes) != 0) {
      // Member destructor closes the parent-side ends; close the three that
      // would have become the child's stdio before giving up.
      close(stdinPipe[0]);
      close(stdoutPipe[1]);
      close(stderrPipe[1]);
      workerFail("WORKER_PREFLIGHT");
    }
    posix_spawn_file_actions_adddup2(&actions, stdinPipe[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stdoutPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
    // Own process group so cancellation can TERM/KILL the whole child side.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);

    // Minimal environment: only loader-relevant variables survive.  The
    // worker never receives credentials or request state through the env.
    const char* pathValue = std::getenv("PATH");
    const char* libraryPath = std::getenv("LD_LIBRARY_PATH");
    const std::string pathEnv = std::string("PATH=") +
      (pathValue != nullptr ? pathValue : "");
    const std::string ldEnv = std::string("LD_LIBRARY_PATH=") +
      (libraryPath != nullptr ? libraryPath : "");
    char* envp[3] = {nullptr, nullptr, nullptr};
    std::size_t envCount = 0;
    if (pathValue != nullptr) {
      envp[envCount++] = const_cast<char*>(pathEnv.c_str());
    }
    if (libraryPath != nullptr) {
      envp[envCount++] = const_cast<char*>(ldEnv.c_str());
    }
    envp[envCount] = nullptr;

    // Fixed argv: the declared metadata length is the actually serialized
    // metadata length the parent just produced.
    const std::string metadataArg = std::to_string(metadataBytes);
    const char* argv[] = {
      location.path.c_str(), "--stdio-v1", "--metadata-bytes",
      metadataArg.c_str(), nullptr,
    };

    // Ignore SIGPIPE around the transport so writes to an exited child
    // surface as EPIPE instead of killing the caller.
    struct sigaction previous{};
    struct sigaction ignore{};
    std::memset(&ignore, 0, sizeof(ignore));
    ignore.sa_handler = SIG_IGN;
    sigemptyset(&ignore.sa_mask);
    sigaction(SIGPIPE, &ignore, &previous);

    const int spawnResult =
      posix_spawn(&pid, location.path.c_str(), &actions, &attributes,
                  const_cast<char* const*>(argv), envp);
    sigaction(SIGPIPE, &previous, nullptr);

    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    close(stderrPipe[1]);
    if (spawnResult != 0) {
      started = false;
      workerFail("WORKER_SPAWN");
    }
    started = true;
  }

  // Cancel path: TERM the own process group, wait up to 1s, KILL, reap.
  bool killAndReap()
  {
    if (!started || reaped) return reaped;
    ::kill(-pid, SIGTERM);
    for (int i = 0; i < 100; ++i) {
      if (tryReap()) return true;
      poll(nullptr, 0, 10);
    }
    ::kill(-pid, SIGKILL);
    while (!tryReap()) {
      poll(nullptr, 0, 20);
    }
    return true;
  }

  bool tryReap()
  {
    if (reaped || !started) return reaped;
    int status = 0;
    const pid_t result = waitpid(pid, &status, WNOHANG);
    if (result == pid) {
      reaped = true;
      m_status = status;
    }
    else if (result < 0 && errno != EINTR) {
      // ESRCH/ECHILD: gone already; keep m_status as unknown (-1).
      reaped = true;
    }
    return reaped;
  }

  bool exitedZero() const
  {
    return reaped && m_status >= 0 && WIFEXITED(m_status) &&
           WEXITSTATUS(m_status) == 0;
  }

  bool exitedClean() const { return reaped; }
  bool wasSignaled() const
  {
    return reaped && m_status >= 0 && WIFSIGNALED(m_status);
  }

  pid_t pid = -1;
  bool started = false;
  bool reaped = false;
  int m_status = -1;
  int toChild = -1;
  int fromChild = -1;
  int childErr = -1;
};

} // namespace

void
registerNativeOnnxWorkerLocation(const NativeOnnxWorkerLocation& location)
{
  if (location.path.empty()) workerFail("WORKER_PREFLIGHT");
  const int fd = open(location.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) workerFail("WORKER_PREFLIGHT");
  const std::string computed = hexDigestOfFile(fd);
  close(fd);
  if (computed.empty()) workerFail("WORKER_PREFLIGHT");
  NativeOnnxWorkerLocation stored = location;
  if (!stored.sha256.empty() && stored.sha256 != computed) {
    workerFail("WORKER_PREFLIGHT");
  }
  stored.sha256 = computed;
  std::lock_guard<std::mutex> lock(gWorkerLocationMutex);
  gWorkerLocation = std::move(stored);
}

NativeCertifiedAssembly
runNativeOnnxAssemblyWorker(const NativeCanonicalSource& source,
                            const NativeCertifiedRecipe& recipe,
                            const NativeAssemblyControl& control)
{
  std::optional<NativeOnnxWorkerLocation> location;
  {
    std::lock_guard<std::mutex> lock(gWorkerLocationMutex);
    location = gWorkerLocation;
  }
  if (!location) workerFail("WORKER_UNREGISTERED");
  return runNativeOnnxAssemblyWorkerAt(*location, source, recipe, control);
}

NativeCertifiedAssembly
runNativeOnnxAssemblyWorkerAt(const NativeOnnxWorkerLocation& location,
                              const NativeCanonicalSource& source,
                              const NativeCertifiedRecipe& recipe,
                              const NativeAssemblyControl& control)
{
  entryActiveGate(control);
  entryBudgetGate(source, recipe, control);

  // The digest the worker revalidates must be the role's certified digest
  // when one is provided; the derived digest is authoritative for the
  // payload.  A mismatch means the certificate is not for this payload.
  const std::string canonicalJson = canonicalNativeOnnxRecipeJson(recipe);
  const std::string derivedDigest = digestOfRecipeJson(canonicalJson);
  if (!recipe.recipeDigest.empty() && recipe.recipeDigest != derivedDigest) {
    workerFail("RECIPE");
  }
  const std::string metadata = buildNativeOnnxWorkerRequestMetadata(recipe);
  // Keep the parent bounded while sending a large canonical initializer.  The
  // old composer materialized metadata + model + initializer as one more
  // contiguous vector before the nonblocking pipe could consume it.  Each
  // part below points at storage owned by this synchronous call and is sent
  // in order without an additional multi-gigabyte allocation.
  std::array<std::uint8_t, 33> requestHeader{};
  std::memcpy(requestHeader.data(), kNdnSf182RequestMagic,
              sizeof(kNdnSf182RequestMagic));
  const auto writeU64le = [&requestHeader](std::size_t offset,
                                           std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
      requestHeader[offset + static_cast<std::size_t>(i)] =
        static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
    }
  };
  writeU64le(8, static_cast<std::uint64_t>(metadata.size()));
  writeU64le(16, static_cast<std::uint64_t>(source.modelBytes.size()));
  writeU64le(24, static_cast<std::uint64_t>(
    source.initializerBytes ? source.initializerBytes->size() : 0));
  requestHeader[32] = source.initializerBytes.has_value() ? 1 : 0;
  struct RequestPart
  {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;
  };
  const auto* initializer = source.initializerBytes ?
    &*source.initializerBytes : nullptr;
  const std::array<RequestPart, 4> requestParts{{
    {requestHeader.data(), requestHeader.size()},
    {reinterpret_cast<const std::uint8_t*>(metadata.data()), metadata.size()},
    {source.modelBytes.data(), source.modelBytes.size()},
    {initializer ? initializer->data() : nullptr,
     initializer ? initializer->size() : 0},
  }};

  // Preflight: the fixed binary must still be the registered one.
  const int probe = open(location.path.c_str(), O_RDONLY | O_CLOEXEC);
  if (probe < 0) workerFail("WORKER_PREFLIGHT");
  const std::string probeDigest = hexDigestOfFile(probe);
  close(probe);
  if (probeDigest.empty() ||
      (!location.sha256.empty() && probeDigest != location.sha256)) {
    workerFail("WORKER_PREFLIGHT");
  }

  NativeOnnxResponseDecoder response;
  std::string stderrLog;
  bool stderrTruncated = false;
  WorkerProcess child;

  try {
    child.start(location, metadata.size());

    std::size_t writePart = 0;
    std::size_t writeOffset = 0;
    bool writeDone = false;
    bool stdoutEof = false;

    for (;;) {
      entryActiveGate(control);  // every round; throws on cancel/deadline

      child.tryReap();
      // Exit-vs-frame completeness is judged only after the byte stream has
      // ended: a reaped child may still have written a complete frame whose
      // bytes sit unread in the pipe.
      if (stdoutEof && child.reaped && !response.complete()) {
        if (child.wasSignaled()) {
          workerFail("WORKER_SIGNALED");
        }
        if (child.exitedZero()) {
          workerFail("WORKER_INCOMPLETE");
        }
        workerFail("WORKER_EXITED");
      }
      if (writeDone && stdoutEof && response.complete() && child.reaped) {
        break;
      }

      struct pollfd fds[3];
      std::size_t fdCount = 0;
      if (!writeDone) {
        while (writePart < requestParts.size() &&
               writeOffset == requestParts[writePart].size) {
          ++writePart;
          writeOffset = 0;
        }
        if (writePart == requestParts.size()) {
          writeDone = true;
          close(child.toChild);
          child.toChild = -1;
        }
      }
      if (!writeDone) {
        fds[fdCount].fd = child.toChild;
        fds[fdCount].events = POLLOUT;
        fds[fdCount].revents = 0;
        ++fdCount;
      }
      // After EOF no further bytes can arrive; every byte read so far was
      // already fed to the decoder, so a second frame would have tripped it.
      if (!stdoutEof) {
        fds[fdCount].fd = child.fromChild;
        fds[fdCount].events = POLLIN;
        fds[fdCount].revents = 0;
        ++fdCount;
      }
      fds[fdCount].fd = child.childErr;
      fds[fdCount].events = POLLIN;
      fds[fdCount].revents = 0;
      ++fdCount;

      const int ready = ::poll(fds, static_cast<nfds_t>(fdCount),
                               static_cast<int>(std::min<std::uint64_t>(
                                 millisUntil(control.deadline),
                                 std::numeric_limits<int>::max())));
      if (ready < 0) {
        if (errno == EINTR) continue;
        workerFail("WORKER_POLL");
      }
      if (ready == 0) {
        entryActiveGate(control);  // deadline reached -> timeout family
        continue;
      }

      for (std::size_t i = 0; i < fdCount; ++i) {
        const short events = fds[i].revents;
        if (events == 0) continue;
        if (fds[i].fd == child.toChild) {
          if ((events & POLLOUT) != 0) {
            while (writePart < requestParts.size() &&
                   writeOffset == requestParts[writePart].size) {
              ++writePart;
              writeOffset = 0;
            }
            if (writePart == requestParts.size()) {
              writeDone = true;
              close(child.toChild);
              child.toChild = -1;
              continue;
            }
            const auto& part = requestParts[writePart];
            const ssize_t written = write(child.toChild,
                                          part.data + writeOffset,
                                          part.size - writeOffset);
            if (written > 0) {
              writeOffset += static_cast<std::size_t>(written);
              if (writeOffset == part.size) {
                ++writePart;
                writeOffset = 0;
              }
              if (writePart == requestParts.size()) {
                writeDone = true;
                close(child.toChild);
                child.toChild = -1;
              }
            }
            else if (written < 0 && errno != EINTR && errno != EAGAIN) {
              // EPIPE: the child closed its stdin; exit decides the outcome.
              writeDone = true;
              close(child.toChild);
              child.toChild = -1;
            }
          }
          else if ((events & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
            writeDone = true;
            close(child.toChild);
            child.toChild = -1;
          }
        }
        else if (fds[i].fd == child.fromChild &&
                 (events & (POLLIN | POLLHUP)) != 0) {
          for (;;) {
            std::array<std::uint8_t, 16384> buffer{};
            const ssize_t got =
              read(child.fromChild, buffer.data(), buffer.size());
            if (got > 0) {
              if (response.feed(buffer.data(),
                                static_cast<std::size_t>(got)) ==
                  NativeOnnxResponseDecoder::Result::ProtocolError) {
                workerFail("WORKER_PROTOCOL");
              }
              continue;
            }
            if (got == 0) {
              stdoutEof = true;
              break;
            }
            if (errno == EAGAIN || errno == EINTR) break;
            workerFail("WORKER_READ");
          }
        }
        else if (fds[i].fd == child.childErr &&
                 (events & (POLLIN | POLLHUP)) != 0) {
          for (;;) {
            std::array<std::uint8_t, 4096> buffer{};
            const ssize_t got =
              read(child.childErr, buffer.data(), buffer.size());
            if (got > 0) {
              if (!stderrTruncated) {
                const std::size_t take =
                  std::min<std::size_t>(got,
                    65536 - std::min<std::size_t>(stderrLog.size(), 65536));
                if (take != 0) {
                  stderrLog.append(
                    reinterpret_cast<const char*>(buffer.data()), take);
                }
                if (stderrLog.size() >= 65536) stderrTruncated = true;
              }
              continue;
            }
            break;  // 0 / EAGAIN / EINTR
          }
        }
      }
    }

    // Frame complete and child reaped; every byte up to EOF was fed to the
    // decoder, so trailing/second-frame bytes were already rejected there.
    // Now revalidate content and authorization.
    const std::string metadataJson(response.metadata().begin(),
                                   response.metadata().end());
    entryActiveGate(control);  // the mandatory final requireActive
    const NativeOnnxWorkerOutcome outcome = finalizeNativeOnnxWorkerResponse(
      child.exitedZero(), response.header().status, metadataJson,
      response.model(), recipe, control.maxAssembledBytes, true);
    if (!outcome.ok) {
      throw std::runtime_error(outcome.failureCode);
    }
    return outcome.value;
  }
  catch (...) {
    // Cancellation or failure: terminate the worker side first (TERM, 1s,
    // KILL), reap it, then rethrow the original error.
    child.killAndReap();
    throw;
  }
}

// ---------------------------------------------------------------------------
// OA03 worker child entry.
// ---------------------------------------------------------------------------
namespace {

// Close every descriptor >= 3 before touching the protocol stream.  The
// sweep itself needs an open directory handle, so the numeric descriptors
// are collected first and closed after the handle is gone.
bool
closeNonStdioDescriptors()
{
  DIR* dir = opendir("/proc/self/fd");
  if (dir == nullptr) return false;
  const int dirFd = dirfd(dir);
  std::vector<int> victims;
  while (dirent* entry = readdir(dir)) {
    char* end = nullptr;
    const long value = strtol(entry->d_name, &end, 10);
    if (end == entry->d_name || *end != '\0') continue;  // ".", "..", others
    if (value >= 0 && value != dirFd) {
      victims.push_back(static_cast<int>(value));
    }
  }
  closedir(dir);
  for (const int fd : victims) {
    if (fd >= STDERR_FILENO + 1) ::close(fd);
  }
  return true;
}

// The parent's nonblocking pipe ends are dup2'd onto 0/1/2, and dup2 keeps
// the file-description flags, so the child clears O_NONBLOCK before doing
// blocking protocol reads and writes.
bool
makeStdioBlocking()
{
  for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0) return false;
    if ((flags & O_NONBLOCK) != 0 &&
        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
      return false;
    }
  }
  return true;
}

// Fully write a response frame on stdout; false on a broken pipe.
bool
writeResponseFrame(std::uint8_t status, const std::string& metadata,
                   const std::vector<std::uint8_t>& model)
{
  const std::vector<std::uint8_t> frame =
    composeNativeOnnxWorkerResponse(status, metadata, model);
  std::size_t offset = 0;
  while (offset < frame.size()) {
    const ssize_t written =
      write(STDOUT_FILENO, frame.data() + offset, frame.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

bool
writeErrorFrame(std::uint8_t status, const std::string& code,
                const std::string& message)
{
  // Sorted compact: code < message < schema.
  const std::string metadata = jsonSortedObject({
    {"code", '"' + jsonString(sanitizeMessage(code, 96)) + '"'},
    {"message", '"' + jsonString(sanitizeMessage(message, 1024)) + '"'},
    {"schema", '"' + std::string(kNativeOnnxAssemblyErrorSchema) + '"'},
  });
  return writeResponseFrame(status, metadata, {});
}

std::string
buildResultMetadata(const NativeCertifiedAssembly& assembly)
{
  return jsonSortedObject({
    {"inputNames", jsonArrayOfStrings(assembly.inputNames)},
    {"modelDigest", '"' + jsonString(assembly.modelDigest) + '"'},
    {"nodeCount", jsonUint(assembly.nodeCount)},
    {"outputNames", jsonArrayOfStrings(assembly.outputNames)},
    {"schema", '"' + std::string(kNativeOnnxAssemblyResultSchema) + '"'},
  });
}

} // namespace

int
runNativeOnnxAssemblyWorkerMain(int argc, char** argv)
{
  if (!closeNonStdioDescriptors() || !makeStdioBlocking()) {
    childDiag("cannot prepare the protocol descriptors");
    return 2;
  }
  if (argc != 4 || std::strcmp(argv[1], "--stdio-v1") != 0 ||
      std::strcmp(argv[2], "--metadata-bytes") != 0) {
    childDiag("invalid invocation mode");
    return 2;
  }
  std::uint64_t declaredMetadata = 0;
  if (!parseStrictDecimal(argv[3], declaredMetadata)) {
    childDiag("metadata-bytes must be a complete decimal length");
    return 2;
  }

  // One request frame, then the input must end (any trailing byte is a
  // second frame).
  NativeOnnxRequestDecoder request;
  for (;;) {
    std::array<std::uint8_t, 16384> buffer{};
    const ssize_t got = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (got > 0) {
      if (request.feed(buffer.data(), static_cast<std::size_t>(got)) ==
          NativeOnnxRequestDecoder::Result::ProtocolError) {
        if (writeErrorFrame(2, "DI_NATIVE_ONNX_WORKER_PROTOCOL",
                            "malformed request frame")) {
          childDiag("malformed request frame");
        }
        return 2;
      }
      continue;
    }
    if (got == 0) break;  // parent closed its write end after one frame
    if (errno == EINTR || errno == EAGAIN) continue;
    childDiag("request read failure");
    return 2;
  }
  if (!request.complete()) {
    if (writeErrorFrame(2, "DI_NATIVE_ONNX_WORKER_PROTOCOL",
                        "request frame is truncated")) {
      childDiag("truncated request frame");
    }
    return 2;
  }
  if (request.header().metadataLength != declaredMetadata) {
    if (writeErrorFrame(2, "DI_NATIVE_ONNX_WORKER_METADATA",
                        "frame metadata length does not match the argv "
                        "declared metadata length")) {
      childDiag("metadata length mismatch");
    }
    return 2;
  }

  NativeOnnxMetadataCheck metadata = validateNativeOnnxWorkerMetadata(
    std::string(request.metadata().begin(), request.metadata().end()));
  if (!metadata.ok) {
    const std::uint8_t status =
      metadata.failureCode.rfind("DI_NATIVE_ONNX_WORKER_", 0) == 0 ? 2 : 1;
    if (writeErrorFrame(status, metadata.failureCode,
                        metadata.failureMessage)) {
      childDiag(metadata.failureMessage);
    }
    return status == 1 ? 1 : 2;
  }

  NativeCanonicalSource source;
  source.modelBytes = std::move(request.model());
  if (request.header().hasInitializer) {
    source.initializerBytes = std::move(request.initializer());
  }
  try {
    NativeCertifiedAssembly assembly =
      assembleInProcess(source, metadata.value.recipe);
    const std::string resultMetadata = buildResultMetadata(assembly);
    if (!writeResponseFrame(0, resultMetadata, assembly.modelBytes)) {
      return 2;
    }
    return 0;
  }
  catch (const std::exception& error) {
    const std::string what = error.what();
    // The family literal is 15 bytes; the three-argument compare() treats
    // the whole literal as the right side, so the bound must be 15 or every
    // chain rejection is mislabeled DI_NATIVE_ONNX_WORKER_INTERNAL.
    const bool known =
      what.compare(0, 15, "DI_NATIVE_ONNX_") == 0 && what.size() <= 96;
    const std::string code = known ? what : "DI_NATIVE_ONNX_WORKER_INTERNAL";
    const std::string message = known ? what
      : "internal worker failure while assembling the certified model";
    if (writeErrorFrame(1, code, message)) {
      childDiag(message);
    }
    return 1;
  }
}

} // namespace ndnsf::di

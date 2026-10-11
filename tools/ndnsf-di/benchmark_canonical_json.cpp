// Offline scalar-rendering cost comparison; never prints input contents.
#include "NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalJson.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>

using ndnsf::di::NativeJson;

namespace {
// Original append path before CD-002a. Floating/escaping code is unchanged.
void originalAppend(std::string& output, const NativeJson& value)
{
  if (value.is_number_float()) {
    output += ndnsf::di::canonical_json_detail::floating(value.get<double>());
  }
  else if (value.is_array()) {
    output += '[';
    bool first = true;
    for (const auto& item : value) {
      if (!first) output += ',';
      first = false;
      originalAppend(output, item);
    }
    output += ']';
  }
  else if (value.is_object()) {
    output += '{';
    bool first = true;
    for (auto it = value.begin(); it != value.end(); ++it) {
      if (!first) output += ',';
      first = false;
      output += NativeJson(it.key()).dump(-1, ' ', true);
      output += ':';
      originalAppend(output, it.value());
    }
    output += '}';
  }
  else {
    if (value.is_binary() || value.is_discarded())
      throw std::invalid_argument("canonical JSON requires a JSON value");
    output += value.dump(-1, ' ', true);
  }
}

template<typename Append>
double measure(const NativeJson& value, Append append)
{
  constexpr unsigned repeats = 100;
  std::size_t observedBytes = 0;
  const auto start = std::chrono::steady_clock::now();
  for (unsigned i = 0; i < repeats; ++i) {
    std::string output;
    append(output, value);
    observedBytes += output.size();
  }
  const auto elapsed = std::chrono::duration<double, std::milli>(
    std::chrono::steady_clock::now() - start).count();
  std::cout << " observedBytes=" << observedBytes;
  return elapsed / repeats;
}

bool compare(const NativeJson& value)
{
  std::string original;
  originalAppend(original, value);
  const auto current = ndnsf::di::nativeCanonicalJson(value);
  std::cout << "bytes=" << current.size() << " equal=" << (current == original);
  const auto before = measure(value, originalAppend);
  const auto after = measure(value, ndnsf::di::canonical_json_detail::append);
  std::cout << " beforeMs=" << before << " afterMs=" << after << '\n';
  return current == original;
}
} // namespace

int main(int argc, char** argv)
{
  try {
    bool equal = compare(NativeJson::array({nullptr, true, false, 0, -1,
      std::numeric_limits<NativeJson::number_integer_t>::min(),
      std::numeric_limits<NativeJson::number_integer_t>::max(),
      std::numeric_limits<NativeJson::number_unsigned_t>::max(),
      -0.0, 1.0, 1e-7, "unicode-\u4e2d\u6587\n"}));
    for (int i = 1; i < argc; ++i) {
      std::ifstream input(argv[i]);
      if (!input) throw std::runtime_error("cannot open input document");
      const std::string wire{std::istreambuf_iterator<char>(input), {}};
      equal = compare(ndnsf::di::nativeParseJson(wire)) && equal;
    }
    return equal ? 0 : 1;
  }
  catch (const std::exception& error) {
    std::cerr << "canonical benchmark: " << error.what() << '\n';
    return 2;
  }
}

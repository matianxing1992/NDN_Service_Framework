// A real C++ caller of the candidate Rust C ABI; no Python runtime calls.
#include "tokenizer-abi.h"
#include <boost/property_tree/json_parser.hpp>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

class Result {
public:
  explicit Result(NdiTokenResult value) : m_value(value) {}
  Result(const Result&) = delete;
  Result& operator=(const Result&) = delete;
  ~Result() { ndi_token_free(m_value.data, m_value.size); }
  std::string bytes() const {
    return m_value.size ? std::string(reinterpret_cast<const char*>(m_value.data), m_value.size) : std::string();
  }
  int code() const { return m_value.code; }
  void requireSuccess() const {
    if (code()) throw std::runtime_error(bytes());
  }
private:
  NdiTokenResult m_value;
};

int main(int argc, char** argv)
{
  try {
    if (argc != 2) throw std::runtime_error("expected frozen vector path");
    boost::property_tree::ptree fixture;
    boost::property_tree::read_json(argv[1], fixture);
    std::set<std::string> required{"legacy-ascii", "legacy-unicode", "byte-fallback-special"};
    std::size_t cases = 0, negatives = 0;
    auto reject = [&](const Result& result) {
      if (result.code() != 1) throw std::runtime_error("missing input rejection");
      ++negatives;
    };
    for (const auto& entry : fixture.get_child("fixtures")) {
      const auto& test = entry.second;
      const auto name = test.get<std::string>("name");
      if (!required.erase(name)) throw std::runtime_error("duplicate or unexpected tokenizer");
      const auto json = test.get<std::string>("tokenizerJson");
      void* handle = nullptr;
      Result created(ndi_token_create(reinterpret_cast<const uint8_t*>(json.data()), json.size(), &handle));
      std::unique_ptr<void, decltype(&ndi_token_destroy)> owner(handle, ndi_token_destroy);
      created.requireSuccess();
      if (!owner) throw std::runtime_error("successful create returned null");
      std::size_t localCases = 0;
      for (const auto& row : test.get_child("cases")) {
        const auto& item = row.second;
        const auto input = item.get<std::string>("input");
        Result encoded(ndi_token_encode(owner.get(), reinterpret_cast<const uint8_t*>(input.data()),
                                        input.size(), item.get<bool>("addSpecial")));
        encoded.requireSuccess();
        const auto bytes = encoded.bytes();
        if (bytes.size() % 4) throw std::runtime_error("invalid ID buffer size");
        std::vector<uint32_t> ids;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 4) {
          uint32_t id = 0;
          for (unsigned i = 0; i < 4; ++i) id |= uint32_t(uint8_t(bytes[offset + i])) << (8 * i);
          ids.push_back(id);
        }
        std::vector<uint32_t> expected;
        for (const auto& value : item.get_child("ids")) expected.push_back(value.second.get_value<uint32_t>());
        if (ids != expected) throw std::runtime_error(name + ": encoded IDs differ");
        Result decoded(ndi_token_decode(owner.get(), ids.data(), ids.size(), item.get<bool>("skipSpecial")));
        decoded.requireSuccess();
        if (decoded.bytes() != item.get<std::string>("decoded")) throw std::runtime_error(name + ": decoded text differs");
        ++localCases;
        ++cases;
      }
      if (localCases != 28) throw std::runtime_error("missing option/Unicode vectors");
      const uint32_t invalidId = UINT32_MAX;
      reject(Result(ndi_token_decode(owner.get(), &invalidId, 1, 1)));
      reject(Result(ndi_token_encode(owner.get(), nullptr, 0, 2)));
      reject(Result(ndi_token_decode(owner.get(), nullptr, 0, 2)));
      const uint8_t invalidUtf8[] = {0xff};
      reject(Result(ndi_token_encode(owner.get(), invalidUtf8, 1, 1)));
      std::cout << name << " PASS cases=" << localCases << '\n';
    }
    if (!required.empty()) throw std::runtime_error("missing tokenizer fixture");
    void* invalid = nullptr;
    reject(Result(ndi_token_create(nullptr, 0, &invalid)));
    if (invalid) throw std::runtime_error("failed create leaked owner");
    reject(Result(ndi_token_encode(nullptr, nullptr, 0, 0)));
    std::cout << "TOKENIZER_ABI_PROBE PASS cases=" << cases << " negatives=" << negatives << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "TOKENIZER_ABI_PROBE ERROR " << error.what() << '\n';
    return 1;
  }
}

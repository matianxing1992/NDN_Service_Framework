// Exercise the installed production tokenizer ABI with frozen prefix vectors.
#include "tokenizer-abi.h"

#include <boost/property_tree/json_parser.hpp>

#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::string
text(NdiTokenResult result, bool reject = false)
{
  struct Release
  {
    NdiTokenResult value;
    ~Release() { ndi_token_free(value.data, value.size); }
  } release{result};
  if (result.code != (reject ? 1 : 0)) {
    throw std::runtime_error("unexpected stable decoder status");
  }
  return result.size ? std::string(reinterpret_cast<const char*>(result.data), result.size)
                     : std::string();
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc != 2) {
      throw std::runtime_error("expected frozen stable vector path");
    }
    boost::property_tree::ptree fixture;
    boost::property_tree::read_json(argv[1], fixture);
    if (fixture.get<std::string>("schema") != "spec182-tokenizer-stable-vectors-v1") {
      throw std::runtime_error("unexpected vector schema");
    }
    std::set<std::string> required{
      "byte-fallback-special", "bytelevel", "reject-fuse", "legacy-ascii", "legacy-unicode"};
    std::size_t cases = 0;
    std::size_t prefixes = 0;
    std::size_t negatives = 0;
    for (const auto& entry : fixture.get_child("fixtures")) {
      const auto& test = entry.second;
      const auto name = test.get<std::string>("name");
      if (required.erase(name) != 1) {
        throw std::runtime_error("unexpected or duplicate tokenizer fixture");
      }
      const auto json = test.get<std::string>("tokenizerJson");
      void* handle = nullptr;
      auto result = ndi_token_create(reinterpret_cast<const uint8_t*>(json.data()),
                                     json.size(), &handle);
      std::unique_ptr<void, decltype(&ndi_token_destroy)> owner(handle, ndi_token_destroy);
      text(result);
      if (!owner) {
        throw std::runtime_error("tokenizer owner is missing");
      }
      for (const auto& row : test.get_child("cases")) {
        const auto& item = row.second;
        std::vector<uint32_t> ids;
        std::vector<std::string> expected;
        for (const auto& value : item.get_child("ids")) {
          ids.push_back(value.second.get_value<uint32_t>());
        }
        for (const auto& value : item.get_child("prefixes")) {
          expected.push_back(value.second.get_value<std::string>());
        }
        if (expected.size() != ids.size()) {
          throw std::runtime_error("incomplete prefix vectors");
        }
        const auto skip = item.get<bool>("skip");
        for (std::size_t size = 1; size <= ids.size(); ++size) {
          if (text(ndi_token_decode_stable(owner.get(), ids.data(), size, skip, 0)) !=
              expected[size - 1]) {
            throw std::runtime_error(name + ": stable prefix differs");
          }
          ++prefixes;
        }
        if (text(ndi_token_decode_stable(owner.get(), ids.data(), ids.size(), skip, 1)) !=
            item.get<std::string>("finalText")) {
          throw std::runtime_error(name + ": final text differs");
        }
        ++cases;
      }
      const uint32_t invalidId = UINT32_MAX;
      text(ndi_token_decode_stable(owner.get(), &invalidId, 1, 1, 0), true);
      text(ndi_token_decode_stable(owner.get(), nullptr, 0, 2, 0), true);
      text(ndi_token_decode_stable(owner.get(), nullptr, 0, 1, 2), true);
      negatives += 3;
      if (name == "reject-fuse") {
        text(ndi_token_decode_stable(owner.get(), nullptr, 0, 1, 0), true);
        ++negatives;
      }
    }
    text(ndi_token_decode_stable(nullptr, nullptr, 0, 1, 0), true);
    ++negatives;
    if (!required.empty() || cases != 28 || prefixes == 0) {
      throw std::runtime_error("stable fixture coverage is incomplete");
    }
    std::cout << "TOKENIZER_STABLE_ABI_PASS cases=" << cases << " prefixes=" << prefixes
              << " negatives=" << negatives << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "TOKENIZER_STABLE_ABI_FAILED " << error.what() << '\n';
    return 1;
  }
}

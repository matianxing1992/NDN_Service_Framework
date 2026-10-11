#include "ndnsf-distributed-repo/LegacyRepoImporter.hpp"

#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void
printUsage(std::ostream& output)
{
  output << "Usage: ndnsf-repo-import --source-db PATH --source-payload-root PATH "
            "--destination PATH --import-id ID [--max-range-bytes N] "
            "[--max-data-wire-bytes N]\n";
}

uint64_t
parsePositiveSize(const std::string& name, const std::string& text)
{
  if (text.empty() || text.front() == '-') {
    throw std::invalid_argument("invalid value for " + name);
  }
  size_t consumed = 0;
  uint64_t value = 0;
  try {
    value = std::stoull(text, &consumed, 10);
  }
  catch (...) {
    throw std::invalid_argument("invalid value for " + name);
  }
  if (consumed != text.size() || value == 0) {
    throw std::invalid_argument("invalid value for " + name);
  }
  return value;
}

ndnsf_distributed_repo::LegacyRepoImportConfig
parseArguments(int argc, char* argv[])
{
  std::map<std::string, std::string> values;
  for (int i = 1; i < argc; ++i) {
    const std::string option(argv[i]);
    if (option.empty() || option[0] != '-' || i + 1 >= argc) {
      throw std::invalid_argument("expected option followed by value");
    }
    const std::string value(argv[++i]);
    if (!values.emplace(option, value).second) {
      throw std::invalid_argument("duplicate option: " + option);
    }
  }

  for (const auto& entry : values) {
    if (entry.first != "--source-db" &&
        entry.first != "--source-payload-root" &&
        entry.first != "--destination" &&
        entry.first != "--import-id" &&
        entry.first != "--max-range-bytes" &&
        entry.first != "--max-data-wire-bytes") {
      throw std::invalid_argument("unknown option: " + entry.first);
    }
  }
  for (const auto* required : {"--source-db", "--source-payload-root",
                               "--destination", "--import-id"}) {
    if (values.count(required) == 0 || values.at(required).empty()) {
      throw std::invalid_argument(std::string("missing option: ") + required);
    }
  }

  ndnsf_distributed_repo::LegacyRepoImportConfig config;
  config.sourceDatabase = values.at("--source-db");
  config.sourcePayloadRoot = values.at("--source-payload-root");
  config.destinationRoot = values.at("--destination");
  config.importId = values.at("--import-id");
  if (values.count("--max-range-bytes") != 0) {
    config.maxRangeBytes = parsePositiveSize("--max-range-bytes",
                                             values.at("--max-range-bytes"));
  }
  if (values.count("--max-data-wire-bytes") != 0) {
    config.maxDataWireBytes = parsePositiveSize("--max-data-wire-bytes",
                                                values.at("--max-data-wire-bytes"));
  }
  return config;
}

} // namespace

int
main(int argc, char* argv[])
{
  if (argc == 2 && std::string(argv[1]) == "--help") {
    printUsage(std::cout);
    return 0;
  }
  try {
    const auto config = parseArguments(argc, argv);
    const auto result = ndnsf_distributed_repo::LegacyRepoImporter::import(config);
    std::cout << result.toJson() << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << "ndnsf-repo-import: " << error.what() << '\n';
    printUsage(std::cerr);
    return 2;
  }
}

#include "ndnsf-di/api.hpp"
#include "ndnsf-di/provider.hpp"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <csignal>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

namespace {

volatile std::sig_atomic_t stopped = 0;
void onSignal(int) { stopped = 1; }

boost::property_tree::ptree
readConfig(const std::filesystem::path& path)
{
  boost::property_tree::ptree tree;
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("provider configuration is unavailable");
  boost::property_tree::read_json(input, tree);
  return tree;
}

std::string
argument(const boost::property_tree::ptree& tree, const std::string& key)
{
  const auto arguments = tree.get_child("arguments");
  bool found = false;
  std::string value;
  for (const auto& item : arguments) {
    if (found) {
      value = item.second.get_value<std::string>();
      break;
    }
    found = item.second.get_value<std::string>() == key;
  }
  if (value.empty())
    throw std::runtime_error("provider configuration is missing " + key);
  return value;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Usage: DI_PreparedProvider --config FILE [--serve]\n";
      return 0;
    }
    if (argc < 3 || std::string(argv[1]) != "--config") {
      std::cerr << "Usage: DI_PreparedProvider --config FILE [--serve]\n";
      return 2;
    }
    const auto configPath = std::filesystem::absolute(argv[2]);
    const auto tree = readConfig(configPath);
    const auto config = ndnsf::di::ProviderConfig::fromFile(configPath);
    auto runtime = ndnsf::di::Runtime::open(config);
    auto provider = runtime->provider();
    std::cout << "PREPARED_PROVIDER_ROUTE=Runtime.open(ProviderConfig)->Provider\n";
    bool serve = argc > 3 && std::string(argv[3]) == "--serve";
    if (serve) {
      ndnsf::di::ServiceDefinition definition;
      definition.serviceName = argument(tree, "--service");
      definition.allowedRoles.push_back(argument(tree, "--role"));
      auto registration = provider.serve(definition);
      std::signal(SIGINT, onSignal);
      std::signal(SIGTERM, onSignal);
      std::cout << "PREPARED_PROVIDER_SERVING service=" << definition.serviceName << '\n';
      while (!stopped)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      registration.close();
    }
    runtime->close();
    (void)runtime->drain(std::chrono::seconds(5));
    return 0;
  }
  catch (const ndnsf::di::DiError& error) {
    std::cerr << error.code() << " boundary=" << error.boundary()
              << " message=" << error.what() << '\n';
    return 1;
  }
  catch (const std::exception& error) {
    std::cerr << "PREPARED_PROVIDER_FAILED: " << error.what() << '\n';
    return 1;
  }
}

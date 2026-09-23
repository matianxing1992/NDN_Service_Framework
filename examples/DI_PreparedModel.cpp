#include "ndnsf-di/api.hpp"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace ndnsf::di;

std::vector<std::uint8_t>
readBytes(const std::string& path)
{
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input)
    throw std::runtime_error("input is unavailable: " + path);
  const auto size = input.tellg();
  if (size < 0)
    throw std::runtime_error("input size is invalid");
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    throw std::runtime_error("input read failed");
  return bytes;
}

void
writeBytes(const std::string& path, const std::vector<std::uint8_t>& bytes)
{
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output || (!bytes.empty() && !output.write(
      reinterpret_cast<const char*>(bytes.data()), bytes.size())))
    throw std::runtime_error("output write failed: " + path);
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Usage: DI_PreparedModel --config FILE --input FILE --output FILE "
                   "[--stream] [--conversation] [--checkpoint-in FILE] "
                   "[--checkpoint-out FILE]\n";
      return 0;
    }
    if (argc < 7 || std::string(argv[1]) != "--config" ||
        std::string(argv[3]) != "--input" || std::string(argv[5]) != "--output") {
      std::cerr << "Usage: DI_PreparedModel --config FILE --input FILE --output FILE\n";
      return 2;
    }

    std::string checkpointIn;
    std::string checkpointOut;
    bool streaming = false;
    bool conversationMode = false;
    for (int i = 7; i < argc; ++i) {
      const std::string option = argv[i];
      if (option == "--stream")
        streaming = true;
      else if (option == "--conversation")
        conversationMode = true;
      else if (option == "--checkpoint-in" && i + 1 < argc)
        checkpointIn = argv[++i];
      else if (option == "--checkpoint-out" && i + 1 < argc)
        checkpointOut = argv[++i];
      else
        throw std::invalid_argument("unknown or incomplete option: " + option);
    }

    RuntimeConfig config;
    config.nativeConfigPath = std::filesystem::absolute(argv[2]).string();
    auto runtime = Runtime::open(std::move(config));
    auto user = runtime->user();
    auto preparation = user.prepareAsync();
    const auto model = preparation.result();
    std::cout << "PREPARED_MODEL_ROUTE=Runtime.open->User.prepareAsync->User.request\n";

    RequestOptions options;
    if (streaming) {
      options.outputMode = "TOKEN_STREAMING";
      options.stream = StreamOptions{true};
    }
    std::optional<Conversation> conversation;
    if (conversationMode || !checkpointIn.empty()) {
      ConversationOptions conversationOptions;
      if (!checkpointIn.empty())
        conversationOptions.checkpoint = ConversationCheckpoint::fromBytes(readBytes(checkpointIn));
      conversation.emplace(user.openConversation(model, conversationOptions));
    }
    const auto input = Input::inlineBytes(readBytes(argv[4]));
    RequestHandle request = conversation
      ? conversation->request(input, options)
      : user.request(model, input, options);
    const auto result = request.result(options.timeout);
    if (streaming) {
      auto reader = request.events();
      std::size_t events = 0;
      while (const auto event = reader.next(std::chrono::milliseconds(0))) {
        ++events;
        if (event->terminal)
          break;
      }
      std::cout << "PREPARED_MODEL_STREAM_EVENTS=" << events << '\n';
    }
    writeBytes(argv[6], result.payload);
    if (conversation && !checkpointOut.empty())
      writeBytes(checkpointOut, conversation->checkpoint().bytes());
    std::cout << "PREPARED_MODEL_RESULT request=" << request.id()
              << " model=" << result.modelDigest << "\n";
    runtime->close();
    (void)runtime->drain(std::chrono::seconds(5));
    return 0;
  }
  catch (const DiError& error) {
    std::cerr << error.code() << " boundary=" << error.boundary()
              << " message=" << error.what() << '\n';
    return 1;
  }
  catch (const std::exception& error) {
    std::cerr << "PREPARED_MODEL_FAILED: " << error.what() << '\n';
    return 1;
  }
}

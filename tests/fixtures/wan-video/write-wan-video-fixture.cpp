#include "wan-video-fixture.hpp"

#include <iostream>
#include <string_view>

int
main(int argc, char** argv)
{
  if (argc == 3 && std::string_view(argv[1]) == "--checkpoint-oracle") {
    try {
      ndnsf::di::wan::fixture::writeWanCheckpointOracleFixture(argv[2]);
      std::cout << (std::filesystem::absolute(argv[2]) / "oracle.json").string() << '\n';
      return 0;
    }
    catch (const std::exception& error) {
      std::cerr << error.what() << '\n';
      return 1;
    }
  }
  const bool tileDit = argc == 6 && std::string_view(argv[5]) == "--tile-dit";
  if (argc != 5 && !tileDit) {
    std::cerr << "Usage: write-wan-video-fixture OUTPUT REQUESTER_NAME SERVICE_NAME EPOCH "
                 "[--tile-dit]\n"
                 "       write-wan-video-fixture --checkpoint-oracle NEW_OUTPUT\n";
    return 2;
  }
  try {
    namespace fixture = ndnsf::di::wan::fixture;
    fixture::writeWanVideoFixture(argv[1], ndn::Name(argv[2]), ndn::Name(argv[3]), argv[4],
                                  tileDit);
    std::cout << (std::filesystem::absolute(argv[1]) / "fixture.json").string() << '\n';
    return 0;
  }
  catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

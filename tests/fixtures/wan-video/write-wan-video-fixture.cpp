#include "wan-video-fixture.hpp"

#include <iostream>
#include <string_view>

int
main(int argc, char** argv)
{
  const bool tileDit = argc == 6 && std::string_view(argv[5]) == "--tile-dit";
  if (argc != 5 && !tileDit) {
    std::cerr << "Usage: write-wan-video-fixture OUTPUT REQUESTER_NAME SERVICE_NAME EPOCH "
                 "[--tile-dit]\n";
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

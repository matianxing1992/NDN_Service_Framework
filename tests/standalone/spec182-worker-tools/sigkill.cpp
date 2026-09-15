#include <csignal>

int
main()
{
  std::raise(SIGKILL);
  return 127;
}

#include <csignal>
#include <unistd.h>

int
main()
{
  // The transport deliberately waits for its TERM grace period and then
  // escalates to SIGKILL.  This fixture must not emit a partial protocol
  // frame: the test is about cancellation and process-group cleanup.
  std::signal(SIGTERM, SIG_IGN);
  for (;;) {
    pause();
  }
}

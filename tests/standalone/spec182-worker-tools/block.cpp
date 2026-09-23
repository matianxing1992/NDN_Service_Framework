// Spec182 worker-protocol test tool: never respond (T006-C).
//
// Deliberately-misbehaving worker child for the Spec182OnnxWorkerProtocol
// subprocess cases.  The suite spawns this binary through the same OA02
// transport as the real worker; it drains stdin and then blocks forever,
// ignoring SIGTERM, so cancellation must escalate TERM -> (1s) -> KILL
// inside the transport and the parent must reap it.  Not installed and
// never referenced by production code.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

// dup2 keeps the parent's nonblocking pipe ends on 0/1/2; clear the flag so
// the blocking drains below behave like the real worker child.
void
makeStdioBlocking()
{
  for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0 && (flags & O_NONBLOCK) != 0) {
      fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
  }
}

void
drainStdin()
{
  char buffer[8192];
  while (read(STDIN_FILENO, buffer, sizeof(buffer)) > 0) {
  }
}

} // namespace

int
main()
{
  makeStdioBlocking();
  signal(SIGTERM, SIG_IGN);
  drainStdin();
  for (;;) {
    pause();
  }
}

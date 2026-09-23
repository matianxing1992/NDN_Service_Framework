// Spec182 worker-protocol test tool: die by SIGKILL after input EOF
// (T006-C).
//
// The OA02 transport must classify this as a worker killed by a signal
// (DI_NATIVE_ONNX_WORKER_SIGNALED) once the stdout stream has ended and the
// child has been reaped, never as an incomplete or exited worker.  The tool
// first drains stdin to EOF so the parent has finished writing its frame and
// closed the pipe before the kill; the SIGKILL then surfaces only through
// waitpid status.  Not installed and never referenced by production code.

#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

namespace {

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
  drainStdin();
  ::kill(::getpid(), SIGKILL);
  for (;;) {
    pause();
  }
}

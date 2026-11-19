// Spec182 worker-protocol test tool: exit nonzero without any frame
// (T006-C).
//
// A worker that ends its stdout stream and exits 7 with no response frame:
// the OA02 transport must fail with DI_NATIVE_ONNX_WORKER_EXITED (a nonzero
// exit that is not a signal and carried no frame).  Not installed and never
// referenced by production code.

#include <fcntl.h>
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
  _exit(7);
}

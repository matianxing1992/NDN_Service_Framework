// Spec182 worker-protocol test tool: exit silently with 0 after input EOF
// (T006-C).
//
// A worker that ends its stdout stream without any response frame: the OA02
// transport must fail with DI_NATIVE_ONNX_WORKER_INCOMPLETE (exit zero, no
// frame), never publish an empty result.  Not installed and never referenced
// by production code.

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
  _exit(0);
}

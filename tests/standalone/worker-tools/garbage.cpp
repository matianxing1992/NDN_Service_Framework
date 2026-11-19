// Spec182 worker-protocol test tool: fill stdout with non-protocol bytes
// (T006-C).
//
// A worker whose stdout is not the framed protocol at all (no response
// magic): the OA02 response decoder must reject the stream with a protocol
// error, and the transport must report DI_NATIVE_ONNX_WORKER_PROTOCOL.  The
// tool first drains stdin to EOF so the parent has finished writing, then
// writes a stream of plain bytes larger than the response header.  Not
// installed and never referenced by production code.

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
  const char junk[4096] = {'X'};
  for (int i = 0; i < 32; ++i) {  // 128 KiB of non-protocol stdout
    if (write(STDOUT_FILENO, junk, sizeof(junk)) < 0) break;
  }
  _exit(0);
}

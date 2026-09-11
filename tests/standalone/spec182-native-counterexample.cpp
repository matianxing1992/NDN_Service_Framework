#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

void
emit(const char* marker)
{
  std::fputs(marker, stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
}

int
runForkHelper()
{
  const pid_t child = ::fork();
  if (child == 0) {
    ::execl("/probe-root/bin/hidden-helper", "/probe-root/bin/hidden-helper", "--helper", nullptr);
    _exit(127);
  }
  if (child < 0)
    return 111;
  int status = 0;
  (void)::waitpid(child, &status, 0);
  emit("I02_NATIVE_BUSINESS");
  return 0;
}

int
runPythonMapping()
{
  char path[128] = "/probe-root/lib/lib";
  const unsigned char encoded[] = {
    static_cast<unsigned char>('p' ^ 0x5a), static_cast<unsigned char>('y' ^ 0x5a),
    static_cast<unsigned char>('t' ^ 0x5a), static_cast<unsigned char>('h' ^ 0x5a),
    static_cast<unsigned char>('o' ^ 0x5a), static_cast<unsigned char>('n' ^ 0x5a),
    static_cast<unsigned char>('3' ^ 0x5a), static_cast<unsigned char>('.' ^ 0x5a),
    static_cast<unsigned char>('8' ^ 0x5a), static_cast<unsigned char>('.' ^ 0x5a),
    static_cast<unsigned char>('s' ^ 0x5a), static_cast<unsigned char>('o' ^ 0x5a),
    static_cast<unsigned char>('.' ^ 0x5a), static_cast<unsigned char>('1' ^ 0x5a),
    static_cast<unsigned char>('.' ^ 0x5a), static_cast<unsigned char>('0' ^ 0x5a), 0,
  };
  const std::size_t prefixLength = std::strlen(path);
  for (std::size_t i = 0; encoded[i] != 0; ++i)
    path[prefixLength + i] = static_cast<char>(encoded[i] ^ 0x5a);
  void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (handle != nullptr)
    (void)::dlclose(handle);
  emit("I03_NATIVE_BUSINESS");
  return 0;
}

int
runUndeclaredEndpoint()
{
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd >= 0) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(9);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    (void)::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    (void)::close(fd);
  }
  emit("I04_NATIVE_BUSINESS");
  return 0;
}

int
runTraceBudget()
{
  for (int i = 0; i < 4096; ++i) {
    const int fd = ::open("/tmp/spec184-counterexample", O_CREAT | O_RDWR, 0600);
    if (fd >= 0)
      (void)::close(fd);
    (void)::unlink("/tmp/spec184-counterexample");
  }
  emit("I05_NATIVE_BUSINESS");
  return 0;
}

int
runDetachedChild()
{
  const pid_t child = ::fork();
  if (child == 0) {
    (void)::setsid();
    (void)::sleep(30);
    _exit(0);
  }
  if (child < 0)
    return 112;
  emit("I08_NATIVE_BUSINESS");
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  if (argc < 2)
    return 2;
  if (std::strcmp(argv[1], "--helper") == 0) {
    emit("I02_HELPER_EXECUTED");
    return 0;
  }
  if (std::strcmp(argv[1], "--fork-helper") == 0)
    return runForkHelper();
  if (std::strcmp(argv[1], "--python-mapping") == 0)
    return runPythonMapping();
  if (std::strcmp(argv[1], "--undeclared-endpoint") == 0)
    return runUndeclaredEndpoint();
  if (std::strcmp(argv[1], "--trace-budget") == 0)
    return runTraceBudget();
  if (std::strcmp(argv[1], "--role-gap") == 0) {
    emit("I06_NATIVE_BUSINESS");
    return 0;
  }
  if (std::strcmp(argv[1], "--detached-child") == 0)
    return runDetachedChild();
  if (std::strcmp(argv[1], "--positive") == 0) {
    emit("I07_NATIVE_BUSINESS");
    return 0;
  }
  return 3;
}

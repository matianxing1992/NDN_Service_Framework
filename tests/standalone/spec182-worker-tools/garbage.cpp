#include <unistd.h>

int
main()
{
  static constexpr char kGarbage[] = "not-a-worker-frame\n";
  (void)::write(STDOUT_FILENO, kGarbage, sizeof(kGarbage) - 1);
  return 0;
}

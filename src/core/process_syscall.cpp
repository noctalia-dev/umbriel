#include <cerrno>
#include <sys/syscall.h>
#include <unistd.h>

extern "C" int umbrielCloseRange(unsigned int first, unsigned int last, int flags) {
#ifdef SYS_close_range
  return static_cast<int>(syscall(SYS_close_range, first, last, flags));
#else
  errno = ENOSYS;
  return -1;
#endif
}

// The system calls this tool needs, brought across the module boundary.
//
// Only declarations and the wrappers that a macro cannot cross: a macro is
// not exported by a module, so WIFEXITED and friends are re-stated here as
// functions. Nothing else belongs in this file.

module;

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

export module mandk.posix;

export using ::pid_t;
export using ::ssize_t;

export using ::_exit;
export using ::chdir;
export using ::close;
export using ::dup2;
export using ::execvp;
export using ::fork;
export using ::isatty;
export using ::pipe;
export using ::read;
export using ::setenv;
export using ::strerror;
export using ::waitpid;

export inline constexpr int kStdoutFileNo = STDOUT_FILENO;
export inline constexpr int kStderrFileNo = STDERR_FILENO;

export inline constexpr int kInterrupted = EINTR;

export inline int currentErrno() { return errno; }
export inline bool exitedNormally(int status) { return WIFEXITED(status); }
export inline int exitStatusOf(int status) { return WEXITSTATUS(status); }
export inline bool wasSignalled(int status) { return WIFSIGNALED(status); }
export inline int terminatingSignal(int status) { return WTERMSIG(status); }

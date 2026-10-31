// Speed gate: parent/child 16-byte RUN_UNTIL / STOPPED round trips over a Unix socket.
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>

struct Msg { uint32_t type; uint32_t pad; uint64_t t_ns; };
static_assert(sizeof(Msg) == 16, "16-byte message");
enum { RUN_UNTIL = 1, STOPPED = 2 };
static const int ROUNDS = 10000;

static void xfer(int fd, Msg *m, bool wr) {
  char *p = reinterpret_cast<char *>(m); size_t n = sizeof *m;
  while (n) {
    ssize_t r = wr ? write(fd, p, n) : read(fd, p, n);
    if (r <= 0) _exit(2);
    p += r; n -= (size_t)r;
  }
}

static double now_s() {
  timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main() {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv)) { perror("socketpair"); return 1; }
  pid_t pid = fork();
  if (pid < 0) { perror("fork"); return 1; }
  if (pid == 0) {                       // child: the "firmware" process
    close(sv[0]);
    Msg m;
    for (int i = 0; i < ROUNDS; i++) {
      xfer(sv[1], &m, false);
      m.type = STOPPED; m.t_ns += 1;
      xfer(sv[1], &m, true);
    }
    _exit(0);
  }
  close(sv[1]);
  double t0 = now_s();
  uint64_t t = 0, sum = 0;
  for (int i = 0; i < ROUNDS; i++) {
    Msg m{RUN_UNTIL, 0, t + 100000};    // 100 us quantum
    xfer(sv[0], &m, true);
    xfer(sv[0], &m, false);
    t = m.t_ns; sum += m.type;
  }
  double dt = now_s() - t0;
  int st; waitpid(pid, &st, 0);
  fprintf(stderr, "t=%llu sum=%llu child_ok=%d\n", (unsigned long long)t,
          (unsigned long long)sum, WIFEXITED(st) && WEXITSTATUS(st) == 0);
  printf("sync_s=%.6f\n", dt);
  return 0;
}

// Speed gate: parent/child 16-byte RUN_UNTIL / STOPPED round trips over a Unix socket,
// one per quantum of one virtual second. Usage: bench_sync [quantum_us] (default 100: 10,000
// round trips; 88, the CAN quantum at 500 kbit/s: 11,364).
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

int main(int argc, char **argv) {
  long q_us = argc > 1 ? strtol(argv[1], nullptr, 10) : 100;
  if (q_us <= 0 || q_us > 1000000) { fprintf(stderr, "bench_sync: bad quantum_us\n"); return 1; }
  const uint64_t q_ns = (uint64_t)q_us * 1000;
  const int ROUNDS = (int)((1000000000ull + q_ns - 1) / q_ns);  // quanta in one virtual second
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
    Msg m{RUN_UNTIL, 0, t + q_ns};
    xfer(sv[0], &m, true);
    xfer(sv[0], &m, false);
    t = m.t_ns; sum += m.type;
  }
  double dt = now_s() - t0;
  int st; waitpid(pid, &st, 0);
  fprintf(stderr, "t=%llu sum=%llu child_ok=%d\n", (unsigned long long)t,
          (unsigned long long)sum, WIFEXITED(st) && WEXITSTATUS(st) == 0);
  printf("quantum_us=%ld rounds=%d\nsync_s=%.6f\n", q_us, ROUNDS, dt);
  return 0;
}

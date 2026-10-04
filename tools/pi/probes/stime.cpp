// Prints "Summit launch <system_time s>" to stderr, then execs the command,
// so a log of Summit's own system_time() trace lines has the launch moment.
#include <OS.h>
#include <cstdio>
#include <unistd.h>
int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: stime command [args]\n"); return 2; }
    std::fprintf(stderr, "Summit launch %.3f\n", system_time() / 1e6);
    std::fflush(stderr);
    execvp(argv[1], argv + 1);
    std::perror("execvp");
    return 127;
}

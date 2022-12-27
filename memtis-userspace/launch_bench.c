#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <err.h>
#include <sys/wait.h>

int syscall_htmm_start = 449;
int syscall_htmm_end = 450;

long htmm_start(pid_t pid, int node)
{
    return syscall(syscall_htmm_start, pid, node);
}

long htmm_end(pid_t pid)
{
    return syscall(syscall_htmm_end, pid);
}

int main(int argc, char** argv)
{
    pid_t pid;
    int state;

    if (argc < 2) {
	printf("Usage ./launch_bench [BENCHMARK]");	
	htmm_end(-1);
	return 0;
    }

    pid = fork();
    if (pid == 0) {
	execvp(argv[1], &argv[1]);
	perror("Fails to run bench");
	exit(-1);
    }
#ifdef __NOPID
    htmm_start(-1, 0);
#else
    htmm_start(pid, 0);
#endif
    printf("pid: %d\n", pid);
    waitpid(pid, &state, 0);

    htmm_end(-1);
    
    return 0;
}

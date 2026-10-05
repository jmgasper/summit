#include <fcntl.h>
#include <stdio.h>
#include <sys/socket.h>
int main(void) {
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	printf("SOCK_NONBLOCK=%#x socket=%d F_GETFL=%#x nonblocking=%d\n", SOCK_NONBLOCK, s, fcntl(s, F_GETFL, 0), !!(fcntl(s, F_GETFL, 0) & O_NONBLOCK));
	int t = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	printf("SOCK_CLOEXEC F_GETFD=%#x\n", fcntl(t, F_GETFD, 0));
	return 0;
}

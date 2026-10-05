#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
int main(void) {
	struct addrinfo h = {0}, *r; h.ai_socktype = SOCK_STREAM; h.ai_family = AF_INET;
	if (getaddrinfo("example.com", "80", &h, &r)) { puts("dns fail"); return 1; }
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (connect(s, r->ai_addr, r->ai_addrlen)) { perror("connect"); return 1; }
	int fl = fcntl(s, F_GETFL, 0);
	printf("flags before %#x set=%d\n", fl, fcntl(s, F_SETFL, fl | O_NONBLOCK));
	printf("flags after %#x (O_NONBLOCK=%#x)\n", fcntl(s, F_GETFL, 0), O_NONBLOCK);
	char b[16]; time_t t = time(NULL);
	ssize_t n = recv(s, b, sizeof b, 0);
	printf("recv -> %zd errno %d (%s) after %lds\n", n, errno, strerror(errno), (long)(time(NULL) - t));
	fflush(stdout);
	return 0;
}

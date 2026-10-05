#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
int main(void) {
	struct addrinfo h = {0}, *r; h.ai_socktype = SOCK_STREAM; h.ai_family = AF_INET;
	getaddrinfo("example.com", "80", &h, &r);
	int s = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	int c = connect(s, r->ai_addr, r->ai_addrlen);
	printf("connect %d errno %s, flags %#x\n", c, strerror(errno), fcntl(s, F_GETFL, 0));
	struct pollfd p = { s, POLLOUT, 0 }; poll(&p, 1, 5000);
	printf("poll revents %#x flags %#x\n", p.revents, fcntl(s, F_GETFL, 0));
	const char* req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
	send(s, req, strlen(req), 0);
	struct pollfd q = { s, POLLIN, 0 }; poll(&q, 1, 5000);
	char b[65536]; ssize_t n = recv(s, b, sizeof b, 0);
	printf("first recv %zd flags %#x\n", n, fcntl(s, F_GETFL, 0)); fflush(stdout);
	time_t t = time(NULL);
	n = recv(s, b, sizeof b, 0);
	printf("second recv %zd errno %s after %lds\n", n, strerror(errno), (long)(time(NULL) - t)); fflush(stdout);
	for (int i = 0; i < 3; i++) { t = time(NULL); n = recv(s, b, sizeof b, 0); printf("recv %d -> %zd errno %s after %lds\n", i, n, strerror(errno), (long)(time(NULL) - t)); fflush(stdout); }
	t = time(NULL); n = read(s, b, sizeof b); printf("read -> %zd errno %s after %lds\n", n, strerror(errno), (long)(time(NULL) - t)); fflush(stdout);
	return 0;
}

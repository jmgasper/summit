#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
/* mode 0: SOCK_NONBLOCK at socket(); 1: fcntl before connect; 2: fcntl after connect */
int main(int argc, char** argv) {
	int mode = atoi(argv[1]);
	struct addrinfo h = {0}, *r; h.ai_socktype = SOCK_STREAM; h.ai_family = AF_INET;
	getaddrinfo("example.com", "80", &h, &r);
	int s = socket(AF_INET, SOCK_STREAM | (mode == 0 ? SOCK_NONBLOCK : 0), 0);
	if (mode == 1) fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	int c = connect(s, r->ai_addr, r->ai_addrlen);
	int ce = errno;
	struct pollfd p = { s, POLLOUT, 0 }; poll(&p, 1, 5000);
	if (mode == 2) fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
	char b[256]; time_t t = time(NULL);
	ssize_t n = recv(s, b, sizeof b, 0);
	printf("mode %d connect %d (%s) flags %#x: idle recv %zd (%s) after %lds\n", mode, c, strerror(ce), fcntl(s, F_GETFL, 0), n, strerror(errno), (long)(time(NULL) - t));
	return 0;
}

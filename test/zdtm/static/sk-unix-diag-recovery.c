#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "zdtmtst.h"

const char *test_doc = "Recover unnamed seqpacket sockets omitted from bulk diagnostics";
const char *test_author = "CRIU contributors";

static const char first_packet[] = "first packet";
static const char second_packet[] = "second, longer packet";

static int send_packet(int fd, const char *packet, size_t length)
{
	if (send(fd, packet, length, MSG_DONTWAIT | MSG_NOSIGNAL) != (ssize_t)length) {
		fail("send packet on fd %d", fd);
		return -1;
	}
	return 0;
}

static int receive_packet(int fd, const char *packet, size_t length)
{
	char buffer[64];
	ssize_t received;

	received = recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_TRUNC);
	if (received != (ssize_t)length || memcmp(buffer, packet, length)) {
		fail("packet length, contents or order changed on fd %d: %zd", fd, received);
		return -1;
	}
	return 0;
}

int main(int argc, char **argv)
{
	int pair[2], closed_pair[2], temporary;
	struct stat left, right;
	char buffer;

	test_init(argc, argv);

	if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair) ||
	    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, closed_pair)) {
		fail("socketpair");
		return 1;
	}
	if (fstat(pair[0], &left) || fstat(pair[1], &right)) {
		fail("fstat");
		return 1;
	}
	if (left.st_ino > right.st_ino) {
		temporary = dup(pair[0]);
		if (temporary < 0 || dup2(pair[1], pair[0]) < 0 || dup2(temporary, pair[1]) < 0) {
			fail("reorder socketpair descriptors");
			return 1;
		}
		close(temporary);
	}
	test_msg("Connected socket inodes: %lu %lu\n",
		 (unsigned long)left.st_ino, (unsigned long)right.st_ino);

	if (send_packet(pair[0], first_packet, sizeof(first_packet)) ||
	    send_packet(pair[0], second_packet, sizeof(second_packet)) ||
	    send_packet(pair[1], second_packet, sizeof(second_packet)) ||
	    send_packet(pair[1], first_packet, sizeof(first_packet)) ||
	    send_packet(closed_pair[1], first_packet, sizeof(first_packet)))
		return 1;
	close(closed_pair[1]);

	test_daemon();
	test_waitsig();

	if (receive_packet(pair[1], first_packet, sizeof(first_packet)) ||
	    receive_packet(pair[1], second_packet, sizeof(second_packet)) ||
	    receive_packet(pair[0], second_packet, sizeof(second_packet)) ||
	    receive_packet(pair[0], first_packet, sizeof(first_packet)) ||
	    receive_packet(closed_pair[0], first_packet, sizeof(first_packet)))
		return 1;
	if (recv(closed_pair[0], &buffer, sizeof(buffer), MSG_DONTWAIT) != 0) {
		fail("closed peer did not produce EOF");
		return 1;
	}
	if (send(closed_pair[0], first_packet, sizeof(first_packet), MSG_DONTWAIT | MSG_NOSIGNAL) != -1 ||
	    errno != EPIPE) {
		fail("closed peer did not produce EPIPE");
		return 1;
	}
	if (send_packet(pair[0], first_packet, sizeof(first_packet)) ||
	    receive_packet(pair[1], first_packet, sizeof(first_packet)) ||
	    send_packet(pair[1], second_packet, sizeof(second_packet)) ||
	    receive_packet(pair[0], second_packet, sizeof(second_packet)))
		return 1;

	close(pair[0]);
	close(pair[1]);
	close(closed_pair[0]);
	pass();
	return 0;
}

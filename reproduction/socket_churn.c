/* SPDX-License-Identifier: GPL-2.0-only */

#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int host = 0;

static void stop(int) {
    _exit(0);
}

static void fail(const char *operation) {
    perror(operation);
    exit(EXIT_FAILURE);
}

static void parse_arguments(int argc, char **argv) {
    if (argc > 2)
        exit(EXIT_FAILURE);
    if (argc == 2) {
        if (strcmp(argv[1], "--host") != 0)
            exit(EXIT_FAILURE);
        host = 1;
    }
}

static void setup_signals(void) {
    struct sigaction action = {0};
    action.sa_handler = stop;
    if (sigemptyset(&action.sa_mask) != 0)
        fail("sigemptyset");
    if (sigaction(SIGINT, &action, NULL) != 0)
        fail("sigaction");
    if (sigaction(SIGTERM, &action, NULL) != 0)
        fail("sigaction");
}

static void *churn(void *) {
    for (;;) {
        int sockets[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sockets) != 0)
            fail("socketpair");
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
            ssize_t result;
            do {
                result = send(sockets[endpoint], "x", 1, MSG_NOSIGNAL);
            } while (result < 0 && errno == EINTR);
            if (result != 1)
                fail("send");
        }
        if (!host)
            usleep(10000);
        for (unsigned endpoint = 0; endpoint < 2; endpoint++) {
            char packet;
            ssize_t result;
            do {
                result = recv(sockets[endpoint], &packet, 1, 0);
            } while (result < 0 && errno == EINTR);
            if (result != 1)
                fail("recv");
            if (packet != 'x') {
                errno = EIO;
                fail("recv");
            }
        }
        close(sockets[0]);
        if (!host)
            usleep(10000);
        close(sockets[1]);
    }
    return NULL;
}

int main(int argc, char **argv) {
    parse_arguments(argc, argv);
    setup_signals();
    pthread_t worker;
    for (unsigned created = 0; created < 100; created++) {
        int error = pthread_create(&worker, NULL, churn, NULL);
        if (error != 0) {
            errno = error;
            fail("pthread_create");
        }
    }
    for (;;)
        pause();
}

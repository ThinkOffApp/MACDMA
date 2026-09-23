/* Offline test of mcdma-rpcd's control socket: a second daemon must never take over a live daemon's socket. */
#include "../rpc/rpcd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "test_rpcd_socket: %s\n", what);
        exit(1);
    }
}

int main(void) {
    char path[64];
    snprintf(path, sizeof(path), "/tmp/rpcd-test-%d.sock", (int)getpid());
    int first = unix_listen(path);
    check(first >= 0, "the first daemon owns the socket");
    check(socket_in_use(path), "a live socket is reported in use");
    check(unix_listen(path) < 0, "a second daemon must not replace a live socket");
    close(first);
    check(!socket_in_use(path), "a socket left by a stopped daemon is not in use");
    int again = unix_listen(path);
    check(again >= 0, "a stale socket is replaced");
    close(again);
    unlink(path);
    puts("test_rpcd_socket: ok");
    return 0;
}

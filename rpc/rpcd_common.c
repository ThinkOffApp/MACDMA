/* mcdma-rpcd: time, logging, names, locks and line I/O on control and service sockets. */
#include "rpcd.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

volatile sig_atomic_t g_stop;

uint64_t now_ns(void) {
#ifdef __APPLE__
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
#endif
}

void logf_(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mcdma-rpcd: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void log_rate(const char *name, const char *what, uint64_t bytes, uint64_t ns) {
    logf_("%s: %s %.1f MiB in %.2f ms (%.1f Gbit/s)", name, what, (double)bytes / 1048576.0, (double)ns / 1e6,
          ns ? (double)bytes * 8.0 / (double)ns : 0.0);
}

/* Hold an exclusive lock on MCDMA_RPC_LOCK_DIR/mcdma-rpc.NAME.lock until exit, so one daemon serves each link. */
int hold_link_lock(const char *name) {
    char path[192];
    snprintf(path, sizeof(path), "%s/mcdma-rpc.%s.lock", MCDMA_RPC_LOCK_DIR, name);
    /* open before create: in a sticky directory, O_CREAT over another user's file is refused */
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) {
        fd = open(path, O_RDONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0) fchmod(fd, 0644);
        else if (errno == EEXIST) fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    }
    if (fd < 0) {
        logf_("lock %s errno=%d", path, errno);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB)) {
        logf_("another mcdma-rpcd serves link %s; stop it with SHUTDOWN first", name);
        close(fd);
        return -1;
    }
    return fd;
}

/* Connect to host:port within `seconds`, or return -1. */
int tcp_connect(const char *host, int port, int seconds) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    char service[16];
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) || !res) return -1;
    int fd = socket(res->ai_family, res->ai_socktype, 0), ok = 0;
    if (fd >= 0) {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        if (!connect(fd, res->ai_addr, res->ai_addrlen)) {
            ok = 1;
        } else if (errno == EINPROGRESS) {
            struct pollfd wait = {.fd = fd, .events = POLLOUT};
            int err = 0;
            socklen_t len = sizeof(err);
            ok = poll(&wait, 1, seconds * 1000) == 1 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) && !err;
        }
        fcntl(fd, F_SETFL, flags);
    }
    freeaddrinfo(res);
    if (!ok && fd >= 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

/* Bound every blocking send and receive on `fd`, so no silent peer can hold a thread forever. */
void set_io_timeout(int fd, int seconds) {
    struct timeval limit = {seconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &limit, sizeof(limit));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &limit, sizeof(limit));
}

/* TCP keepalive that notices a peer which crashed or rebooted without closing, within about half a minute. */
void set_keepalive(int fd) {
    int one = 1, idle = 15, interval = 5, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef __APPLE__
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#else
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}

int valid_name(const char *name) {
    size_t n = strlen(name);
    if (n < 1 || n > 20) return 0;
    for (size_t i = 0; i < n; ++i) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

int parse_mib(unsigned mib, uint64_t *out) {
    if (!mib || mib % 4 || mib > 4 * MAX_SEGS) return -1;
    *out = (uint64_t)mib << 20;
    return 0;
}

int send_line(int fd, const char *text) {
    size_t n = strlen(text);
    char *buf = malloc(n + 1);
    if (!buf) return -1;
    memcpy(buf, text, n);
    buf[n] = '\n';
    int rc = 0;
    for (size_t off = 0; off < n + 1;) {
        ssize_t w = send(fd, buf + off, n + 1 - off, 0);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            rc = -1;
            break;
        }
        off += (size_t)w;
    }
    free(buf);
    return rc;
}

/* One line; 1 if a line is ready, 0 if none yet (non-blocking use), -1 on EOF or error. */
int take_line(struct reader *r, char *out, size_t n, int block) {
    for (;;) {
        char *nl = memchr(r->buf, '\n', r->len);
        if (nl) {
            size_t line = (size_t)(nl - r->buf);
            if (line >= n) return -1;
            memcpy(out, r->buf, line);
            out[line] = 0;
            memmove(r->buf, nl + 1, r->len - line - 1);
            r->len -= line + 1;
            return 1;
        }
        if (r->len == sizeof(r->buf)) return -1;
        ssize_t got = recv(r->fd, r->buf + r->len, sizeof(r->buf) - r->len, block ? 0 : MSG_DONTWAIT);
        if (got == 0) return -1;
        if (got < 0) {
            if (errno == EINTR) {
                if (g_stop) return -1;
                continue;
            }
            if (!block && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
            return -1;
        }
        r->len += (size_t)got;
    }
}

/* 1 when a daemon is accepting on `path`; a socket file left by a stopped daemon refuses and reads as free. */
int socket_in_use(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) return 0;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    int live = fd >= 0 && !connect(fd, (struct sockaddr *)&a, sizeof(a));
    if (fd >= 0) close(fd);
    return live;
}

int unix_listen(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(a.sun_path)) {
        logf_("unix socket path too long: %s", path);
        return -1;
    }
    /* never take a live daemon's socket: it could then be stopped only by a signal, with its QPs still open */
    if (socket_in_use(path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", path);
        return -1;
    }
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    unlink(path);
    mode_t old = umask(077);
    int bound = fd >= 0 && !bind(fd, (struct sockaddr *)&a, sizeof(a));
    umask(old);
    if (!bound || listen(fd, 4) || chmod(path, 0600)) {
        logf_("unix socket %s errno=%d", path, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

const char *socket_path(const char *fallback) {
    const char *env = getenv("MCDMA_RPCD_SOCKET");
    return env && *env ? env : fallback;
}

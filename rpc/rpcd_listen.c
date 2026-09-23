/* mcdma-rpcd listen: the Linux end of one link, serving one registered service through its mailbox. */
#include "rpcd.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HANDSHAKE_NS ((uint64_t)MCDMA_RPC_HANDSHAKE_S * 1000000000ull)
#define PENDING_NS ((uint64_t)PENDING_S * 1000000000ull)

struct listen_state {
    const char *name;
    struct ep e;
    struct box b;
    int ctl, svc, direct, nrseg;
    int hello;                     /* our QP reached RTS for the current control connection */
    int armed;                     /* the connect end reported READY: replies may be written */
    uint64_t ctl_since;
    uint32_t rseg_rkey[MAX_SEGS];
    uint64_t rseg_addr[MAX_SEGS];
    uint64_t served, failures, bytes;
    long long since;
    struct piece pieces[4 * MAX_SEGS + 4];
};

static void listen_status(struct listen_state *s, int fd) {
    char out[512];
    snprintf(out, sizeof(out), "VERSION mcdma-rpcd %d %s", PROTOCOL, RELEASE);
    send_line(fd, out);
    snprintf(out, sizeof(out),
             "PEER %s %s calls %" PRIu64 " failures %" PRIu64 " MiB %" PRIu64
             " device=%s req_mib=%" PRIu64 " rep_mib=%" PRIu64 " since=%lld service=%s",
             s->name, s->armed ? "up" : "down", s->served, s->failures, s->bytes >> 20, s->e.device, s->b.req >> 20,
             s->b.rep >> 20, s->armed ? s->since : 0, s->svc >= 0 ? "attached" : "none");
    send_line(fd, out);
    send_line(fd, "END");
}

/* HELLO PROTOCOL qpn psn gid mode R P n rkey addr ... : the connect end's QP is still in INIT. */
static void listen_hello(struct listen_state *s, char *args) {
    volatile uint64_t *req_word = (volatile uint64_t *)s->b.base;
    volatile uint64_t *staged_word = (volatile uint64_t *)(s->b.base + s->b.req + 128);
    /* a new HELLO replaces the old QP, so nothing may be written until this one completes and READY arrives */
    s->armed = 0;
    s->hello = 0;
    char *save = NULL, *tok = strtok_r(args, " ", &save);
    char *field[8] = {0};
    for (int i = 0; i < 8 && tok; ++i) {
        field[i] = tok;
        tok = strtok_r(NULL, " ", &save);
    }
    if (!field[7]) {
        send_line(s->ctl, "ERR bad hello");
        return;
    }
    if (strtol(field[0], NULL, 10) != PROTOCOL) {
        send_line(s->ctl, "ERR protocol");
        return;
    }
    unsigned qpn = (unsigned)strtoul(field[1], NULL, 10), psn = (unsigned)strtoul(field[2], NULL, 10);
    const char *gid = field[3], *mode = field[4];
    uint64_t req = strtoull(field[5], NULL, 10), rep = strtoull(field[6], NULL, 10);
    long n = strtol(field[7], NULL, 10);
    if (req != s->b.req || rep != s->b.rep || n != (long)(s->b.rep / SEG)) {
        send_line(s->ctl, "ERR mailbox sizes differ");
        return;
    }
    for (long i = 0; i < n; ++i) {
        char *rkey = tok;
        char *addr = rkey ? strtok_r(NULL, " ", &save) : NULL;
        if (!rkey || !addr) {
            send_line(s->ctl, "ERR bad hello");
            return;
        }
        s->rseg_rkey[i] = (uint32_t)strtoul(rkey, NULL, 10);
        s->rseg_addr[i] = strtoull(addr, NULL, 10);
        tok = strtok_r(NULL, " ", &save);
    }
    /* our QP goes all the way to RTS before we answer, so a failure here never leaves the other side holding a
       live QP */
    if (ep_create_qp(&s->e) || ep_connect(&s->e, qpn, psn, gid)) {
        send_line(s->ctl, "ERR qp");
        ep_destroy_qp(&s->e);
        return;
    }
    s->nrseg = (int)n;
    s->direct = !strcmp(mode, "DIRECT");
    store_word(req_word, 0);
    store_word(staged_word, 0);
    char mine[80], reply[256];
    gid_string(&s->e.gid, mine, sizeof(mine));
    snprintf(reply, sizeof(reply), "HELLO %u %u %s %u %llu", s->e.qp->qp_num, PSN, mine, s->b.all->rkey,
             (unsigned long long)(uintptr_t)s->b.base);
    send_line(s->ctl, reply);
    s->hello = 1;
}

/* Send the staged reply: direct mode writes it and the client's done word, pull mode only the ready word. */
static int listen_reply(struct listen_state *s, uint32_t seq, uint32_t len) {
    volatile uint64_t *ready_word = (volatile uint64_t *)(s->b.base + s->b.req);
    store_word(ready_word, WORD(seq, len));
    int n;
    if (s->direct) {
        n = cut(&s->b, s->b.req + CTRL, len, 0, 0, s->rseg_rkey, s->rseg_addr, CTRL, SEG, s->pieces, 4 * MAX_SEGS);
        if (n < 0) return -1;
        s->pieces[n] = (struct piece){(void *)ready_word, s->b.all->lkey, s->rseg_addr[0] + 64, s->rseg_rkey[0], 8};
        n++;
    } else {
        s->pieces[0] = (struct piece){(void *)ready_word, s->b.all->lkey, s->rseg_addr[0], s->rseg_rkey[0], 8};
        n = 1;
    }
    uint64_t began = now_ns();
    if (post_pieces(&s->e, IBV_WR_RDMA_WRITE, s->pieces, n, WINDOW_PEER, 10000000000ull)) return -1;
    if (len >= BULK) log_rate(s->name, "reply", len, now_ns() - began);
    s->served++;
    s->bytes += len;
    return 0;
}

static void listen_drop(struct listen_state *s, const char *why) {
    if (s->armed) logf_("%s: link down (%s)", s->name, why);
    /* the service's requests died with the link: ending its registration lets it fail fast instead of waiting */
    if (s->svc >= 0 && (s->armed || s->hello)) {
        send_line(s->svc, "BYE");
        close(s->svc);
        s->svc = -1;
    }
    if (s->ctl >= 0) {
        close(s->ctl);
        s->ctl = -1;
    }
    s->armed = 0;
    s->hello = 0;
    ep_destroy_qp(&s->e);
}

/* Open the link's mailbox without following links, creating it only when absent, and size it to `total`. */
static int open_box(const char *path, uint64_t total, const struct owner *owner) {
    /* open before create: root may not O_CREAT over the service user's file in sticky /dev/shm */
    int fd = open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno == ENOENT) fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0 || fchmod(fd, 0600) || (owner->set && fchown(fd, owner->uid, owner->gid)) || ftruncate(fd, 0) ||
        ftruncate(fd, (off_t)total)) {
        logf_("mailbox %s errno=%d", path, errno);
        if (fd >= 0) close(fd);
        return -1;
    }
    return fd;
}

/* Open the control port and the service socket before any device or mailbox, so a clash costs nothing. */
static int open_sockets(struct in_addr bind_addr, int port, const char *sock_path, const struct owner *owner,
                        int *ls_out, int *us_out) {
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr = bind_addr;
    if (ls < 0 || setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
        bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 2)) {
        logf_("control port %d errno=%d", port, errno);
        if (ls >= 0) close(ls);
        return -1;
    }
    int us = unix_listen(sock_path);
    if (us >= 0 && owner->set && chown(sock_path, owner->uid, owner->gid)) {
        logf_("chown %s errno=%d", sock_path, errno);
        close(us);
        unlink(sock_path);
        us = -1;
    }
    if (us < 0) {
        close(ls);
        return -1;
    }
    *ls_out = ls;
    *us_out = us;
    return 0;
}

int run_listen(const char *name, const char *device, int gid_index, int mtu, struct in_addr bind_addr, int port,
               uint64_t req_bytes, uint64_t rep_bytes, const struct owner *owner) {
    static struct listen_state s;
    static struct reader crd, srd, prd[MAX_PENDING];
    uint64_t pending_since[MAX_PENDING] = {0};
    s.name = name;
    s.ctl = s.svc = -1;
    char default_sock[128], box_path[160];
    snprintf(default_sock, sizeof(default_sock), "/tmp/mcdma-rpcd.%s.sock", name);
    snprintf(box_path, sizeof(box_path), "%s/mcdma-rpc.%s", MCDMA_RPC_BOX_DIR, name);
    const char *sock_path = socket_path(default_sock);
    /* the lock comes first: a second daemon for this link must leave the live mailbox alone */
    if (hold_link_lock(name) < 0) return 2;
    if (socket_in_use(sock_path)) {
        logf_("another mcdma-rpcd is serving %s; stop it with SHUTDOWN first", sock_path);
        return 2;
    }
    int ls = -1, us = -1;
    if (open_sockets(bind_addr, port, sock_path, owner, &ls, &us)) return 2;
    s.b.req = req_bytes;
    s.b.rep = rep_bytes;
    uint64_t total = s.b.req + s.b.rep;
    int bf = -1;
    if (ep_open(&s.e, device, gid_index, mtu) || (bf = open_box(box_path, total, owner)) < 0) goto fail;
    s.b.base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, bf, 0);
    close(bf);
    if (s.b.base == MAP_FAILED) {
        s.b.base = NULL;
        logf_("mmap mailbox");
        goto fail;
    }
    memset(s.b.base, 0, total);
    write_sizes(&s.b);
    if (!(s.b.all = ep_reg(&s.e, s.b.base, total, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                                     IBV_ACCESS_REMOTE_READ))) {
        logf_("register mailbox");
        goto fail;
    }
    volatile uint64_t *staged_word = (volatile uint64_t *)(s.b.base + s.b.req + 128);
    char where[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &bind_addr, where, sizeof(where));
    logf_("listen %s: %s gid %d mtu %d, control %s:%d, socket %s, mailbox %s (%" PRIu64 " + %" PRIu64 " MiB)", name,
          device, gid_index, mtu, where, port, sock_path, box_path, s.b.req >> 20, s.b.rep >> 20);

    uint32_t last_staged = 0;
    uint64_t active = now_ns();
    int one = 1;
    char *line = malloc(LINE);
    for (int i = 0; i < MAX_PENDING; ++i) prd[i].fd = -1;
    while (!g_stop && line) {
        struct pollfd fds[4 + MAX_PENDING];
        int nf = 0;
        fds[nf++] = (struct pollfd){.fd = ls, .events = POLLIN};
        fds[nf++] = (struct pollfd){.fd = us, .events = POLLIN};
        fds[nf++] = (struct pollfd){.fd = s.ctl, .events = POLLIN};
        fds[nf++] = (struct pollfd){.fd = s.svc, .events = POLLIN};
        for (int i = 0; i < MAX_PENDING; ++i) fds[nf++] = (struct pollfd){.fd = prd[i].fd, .events = POLLIN};
        int waited = poll(fds, (nfds_t)nf, (s.armed && s.svc >= 0) ? 0 : 200);
        if (waited < 0 && errno != EINTR) break;
        uint64_t now = now_ns();
        if (fds[0].revents & POLLIN) {
            int fd = accept(ls, NULL, NULL);
            if (fd >= 0) {
                if (s.ctl >= 0) {
                    send_line(fd, "ERR busy");
                    close(fd);
                } else {
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                    set_io_timeout(fd, IO_TIMEOUT_S);
                    set_keepalive(fd);
                    s.ctl = fd;
                    s.ctl_since = now;
                    memset(&crd, 0, sizeof(crd));
                    crd.fd = fd;
                    logf_("listen %s: control connection", name);
                }
            }
        }
        if (fds[1].revents & POLLIN) {
            int fd = accept(us, NULL, NULL);
            int slot = -1;
            for (int i = 0; i < MAX_PENDING && fd >= 0; ++i)
                if (prd[i].fd < 0) {
                    slot = i;
                    break;
                }
            if (fd >= 0 && slot < 0) close(fd);
            if (slot >= 0) {
                set_io_timeout(fd, IO_TIMEOUT_S);
                memset(&prd[slot], 0, sizeof(prd[slot]));
                prd[slot].fd = fd;
                pending_since[slot] = now;
            }
        }
        for (int i = 0; i < MAX_PENDING; ++i) {
            if (prd[i].fd < 0) continue;
            int got = take_line(&prd[i], line, LINE, 0);
            /* a client that never sends its command gives its slot up, so SHUTDOWN and STATUS always get in */
            if (got == 0 && now - pending_since[i] <= PENDING_NS) continue;
            int fd = prd[i].fd;
            prd[i].fd = -1;
            if (got <= 0) {
                close(fd);
            } else if (!strcmp(line, "MODE poll")) {
                if (s.svc >= 0) {
                    send_line(fd, "ERR busy");
                    close(fd);
                } else if (send_line(fd, "OK")) {
                    close(fd);
                } else {
                    s.svc = fd;
                    memset(&srd, 0, sizeof(srd));
                    srd.fd = fd;
                    last_staged = WORD_SEQ(load_word(staged_word));
                    logf_("listen %s: service attached", name);
                }
            } else if (!strcmp(line, "STATUS")) {
                listen_status(&s, fd);
                close(fd);
            } else if (!strcmp(line, "SHUTDOWN")) {
                send_line(fd, "BYE");
                close(fd);
                g_stop = 1;
            } else {
                send_line(fd, "ERR unknown command");
                close(fd);
            }
        }
        if (s.ctl >= 0) {
            int got;
            while ((got = take_line(&crd, line, LINE, 0)) == 1) {
                if (!strncmp(line, "HELLO ", 6)) {
                    listen_hello(&s, line + 6);
                    last_staged = 0;
                } else if (!strcmp(line, "READY") && s.hello && !s.armed) {
                    s.armed = 1;
                    s.since = (long long)time(NULL);
                    logf_("listen %s: peer ready (%s replies, %d reply segments)", name,
                          s.direct ? "direct" : "pull", s.nrseg);
                } else if (!strcmp(line, "PING")) {
                    send_line(s.ctl, "PONG");
                }
            }
            if (got < 0) {
                logf_("listen %s: control connection closed after %" PRIu64 " calls", name, s.served);
                listen_drop(&s, "control closed");
            } else if (!s.armed && now - s.ctl_since > HANDSHAKE_NS) {
                /* an idle or half-open connection must not lock the real connect end out */
                logf_("listen %s: no HELLO and READY within %d s; dropping the control connection", name,
                      MCDMA_RPC_HANDSHAKE_S);
                listen_drop(&s, "handshake timeout");
            }
        }
        if (s.svc >= 0 && take_line(&srd, line, LINE, 0) < 0) {
            logf_("listen %s: service detached", name);
            close(s.svc);
            s.svc = -1;
        }
        if (s.svc >= 0 && s.armed) {
            uint64_t sw = load_word(staged_word);
            uint32_t seq = WORD_SEQ(sw), len = WORD_LEN(sw);
            if (seq && seq != last_staged) {
                last_staged = seq;
                active = now_ns();
                if (len > s.b.rep - CTRL || listen_reply(&s, seq, len)) {
                    s.failures++;
                    logf_("listen %s: reply write failed; dropping the connection", name);
                    listen_drop(&s, "reply write");
                }
            }
        }
        if (s.armed && s.svc >= 0 && !waited) {
            if (now_ns() - active > 50000000ull)
                usleep(20);
            else
                cpu_relax();
        }
    }
    listen_drop(&s, "shutdown");
    if (s.svc >= 0) {
        send_line(s.svc, "BYE");
        close(s.svc);
    }
    for (int i = 0; i < MAX_PENDING; ++i)
        if (prd[i].fd >= 0) close(prd[i].fd);
    free(line);
    close(ls);
    close(us);
    unlink(sock_path);
    teardown_all();
    munmap(s.b.base, total);
    unlink(box_path);
    logf_("listen %s: every verbs object destroyed, exiting", name);
    return 0;

fail:
    close(ls);
    close(us);
    unlink(sock_path);
    teardown_all();
    if (s.b.base) munmap(s.b.base, total);
    /* the link lock is ours, so whatever mailbox file exists belongs to this failed start */
    unlink(box_path);
    return 2;
}

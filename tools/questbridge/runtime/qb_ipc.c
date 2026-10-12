/* qb_ipc.c - the questbridge socket: connect, then one locked sendmsg/recvmsg round trip per request,
 * fds both ways in SCM_RIGHTS (questbridge_protocol.h). */
#define _GNU_SOURCE
#include "qb_ipc.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

XrResult qb_ipc_connect(struct qb_ipc *ipc, const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    size_t n = path ? strlen(path) : 0;
    if (!n || n >= sizeof(addr.sun_path) || (path[0] == '@' && n == 1)) return XR_ERROR_RUNTIME_UNAVAILABLE;
    memcpy(addr.sun_path, path, n);
    socklen_t len = sizeof(addr);
    if (path[0] == '@') { /* "@name": the abstract namespace, how the app names its sockets */
        addr.sun_path[0] = '\0';
        len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + n);
    }
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return XR_ERROR_RUNTIME_FAILURE;
    int rc;
    while ((rc = connect(fd, (struct sockaddr *)&addr, len)) < 0 && errno == EINTR) {
    }
    if (rc < 0 && errno != EISCONN) {
        close(fd);
        return XR_ERROR_RUNTIME_UNAVAILABLE;
    }
    ipc->fd = fd;
    ipc->seq = 0;
    ipc->lost = 0;
    pthread_mutex_init(&ipc->lock, NULL);
    return XR_SUCCESS;
}

/* The stream is out of step with the service, or the service is gone: nothing later can be trusted. */
static XrResult broken(struct qb_ipc *ipc, XrResult r) {
    ipc->lost = 1;
    return r;
}

static XrResult io_error(struct qb_ipc *ipc, int err) {
    return broken(ipc, err == EPIPE || err == ECONNRESET || err == ENOTCONN ? XR_ERROR_INSTANCE_LOST
                                                                              : XR_ERROR_RUNTIME_FAILURE);
}

static void close_fds(const int *fds, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) close(fds[i]);
}

static XrResult round_trip(struct qb_ipc *ipc, uint32_t op, const void *body, uint32_t body_len, const int *fds_in,
                           uint32_t nfds_in, void *reply, uint32_t reply_cap, int *fds_out, uint32_t nfds_out_cap,
                           uint32_t *nfds_out) {
    union {
        struct cmsghdr align;
        char buf[CMSG_SPACE(sizeof(int) * QB_MAX_SWAPCHAIN_IMAGES)];
    } ctl;
    struct qb_hdr hdr = {.op = op, .seq = ++ipc->seq, .body_len = body_len, .fd_count = nfds_in}, rh;
    struct iovec iov[2] = {{&hdr, sizeof(hdr)}, {(void *)body, body_len}};
    struct msghdr msg = {.msg_iov = iov, .msg_iovlen = 2};
    int fds[QB_MAX_SWAPCHAIN_IMAGES];
    uint32_t got = 0;
    ssize_t n;

    if (nfds_in) {
        memset(&ctl, 0, sizeof(ctl));
        msg.msg_control = ctl.buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * nfds_in);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds_in);
        memcpy(CMSG_DATA(c), fds_in, sizeof(int) * nfds_in);
    }
    /* MSG_NOSIGNAL: a dead service must not SIGPIPE the game. */
    while ((n = sendmsg(ipc->fd, &msg, MSG_NOSIGNAL)) < 0 && errno == EINTR) {
    }
    if (n < 0) return io_error(ipc, errno);
    if ((size_t)n != sizeof(hdr) + body_len) return broken(ipc, XR_ERROR_RUNTIME_FAILURE);

    iov[0] = (struct iovec){&rh, sizeof(rh)};
    iov[1] = (struct iovec){reply, reply_cap};
    msg = (struct msghdr){.msg_iov = iov, .msg_iovlen = 2, .msg_control = ctl.buf, .msg_controllen = sizeof(ctl.buf)};
    while ((n = recvmsg(ipc->fd, &msg, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR) {
    }
    if (n == 0) return broken(ipc, XR_ERROR_INSTANCE_LOST);
    if (n < 0) return io_error(ipc, errno);
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < k && got < QB_MAX_SWAPCHAIN_IMAGES; i++)
            memcpy(&fds[got++], CMSG_DATA(c) + i * sizeof(int), sizeof(int));
    }

    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || (size_t)n < sizeof(rh) + sizeof(struct qb_status) ||
        rh.op != op || rh.seq != hdr.seq || rh.body_len != (size_t)n - sizeof(rh) || rh.fd_count != got) {
        close_fds(fds, got);
        return broken(ipc, XR_ERROR_RUNTIME_FAILURE);
    }
    XrResult r = ((const struct qb_status *)reply)->xr_result;
    if (XR_FAILED(r)) {
        close_fds(fds, got); /* a failed reply is the status alone */
        return r;
    }
    if (rh.body_len != reply_cap || got > nfds_out_cap) {
        close_fds(fds, got);
        return broken(ipc, XR_ERROR_RUNTIME_FAILURE);
    }
    if (got) memcpy(fds_out, fds, got * sizeof(int));
    if (nfds_out) *nfds_out = got;
    return r;
}

XrResult qb_ipc_call(struct qb_ipc *ipc, uint32_t op, const void *body, uint32_t body_len, const int *fds_in,
                     uint32_t nfds_in, void *reply, uint32_t reply_cap, int *fds_out, uint32_t nfds_out_cap,
                     uint32_t *nfds_out) {
    if (nfds_out) *nfds_out = 0;
    if (nfds_in > QB_MAX_SWAPCHAIN_IMAGES || reply_cap < sizeof(struct qb_status)) return XR_ERROR_RUNTIME_FAILURE;
    pthread_mutex_lock(&ipc->lock);
    XrResult r = ipc->lost ? XR_ERROR_INSTANCE_LOST
                           : round_trip(ipc, op, body, body_len, fds_in, nfds_in, reply, reply_cap, fds_out,
                                        nfds_out_cap, nfds_out);
    pthread_mutex_unlock(&ipc->lock);
    return r;
}

void qb_ipc_close(struct qb_ipc *ipc) {
    close(ipc->fd);
    ipc->fd = -1;
    pthread_mutex_destroy(&ipc->lock);
}

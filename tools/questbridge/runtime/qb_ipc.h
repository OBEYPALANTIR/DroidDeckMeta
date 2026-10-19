/*
 * qb_ipc.h - the guest end of the questbridge socket (questbridge_protocol.h): one SOCK_SEQPACKET
 * connection per XrInstance with at most one request in flight on it.
 */
#ifndef QB_IPC_H
#define QB_IPC_H

#include <pthread.h>
#include <stdint.h>

#include <openxr/openxr.h>

#include "questbridge_protocol.h"

#pragma GCC visibility push(hidden)

struct qb_ipc {
    int fd;
    uint32_t seq;
    int lost;               /* the service is gone (or out of step): every later call fails */
    pthread_mutex_t lock;   /* held from sendmsg to the matching recvmsg */
};

/* `path` is a filesystem path, or "@name" for the abstract namespace. XR_ERROR_RUNTIME_UNAVAILABLE
 * when nothing listens there (or it is empty or too long). Only a connected ipc may be passed to the
 * other calls and to qb_ipc_close. */
XrResult qb_ipc_connect(struct qb_ipc *ipc, const char *path);
/* One request (body + fds_in) and its reply. `reply` is the whole reply body, `reply_cap` its exact
 * size; every reply starts with struct qb_status. A negative status comes back as the XrResult, with
 * only the status written and no fds. Received fds (close-on-exec) belong to the caller. A closed
 * socket is XR_ERROR_INSTANCE_LOST, a malformed reply XR_ERROR_RUNTIME_FAILURE; both end the
 * connection. */
XrResult qb_ipc_call(struct qb_ipc *ipc, uint32_t op, const void *body, uint32_t body_len, const int *fds_in,
                     uint32_t nfds_in, void *reply, uint32_t reply_cap, int *fds_out, uint32_t nfds_out_cap,
                     uint32_t *nfds_out);
void qb_ipc_close(struct qb_ipc *ipc);

#pragma GCC visibility pop

#endif

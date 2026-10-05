// ikcp.h - A fast and reliable ARQ protocol (KCP)
//
// Vendored, minimal copy of the reference KCP implementation.
// Upstream: https://github.com/skywind3000/kcp
// License: MIT (see LICENSE in this directory).
//
// This file is kept source-compatible with the upstream public API so that a
// future native client can link the very same transport implementation.

#ifndef __IKCP_H__
#define __IKCP_H__

#include <stddef.h>
#include <stdlib.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif

//=====================================================================
// Basic types
//=====================================================================
typedef unsigned char IUINT8;
typedef unsigned short IUINT16;
typedef unsigned int IUINT32;
typedef char IINT8;
typedef short IINT16;
typedef int IINT32;
typedef long long IINT64;
typedef unsigned long long IUINT64;

#ifndef IKCP_EXPORT
#define IKCP_EXPORT
#endif

//=====================================================================
// KCP BASIC
//=====================================================================
#define IKCP_RTO_NDL 30
#define IKCP_RTO_MIN 100
#define IKCP_RTO_DEF 200
#define IKCP_RTO_MAX 60000

#define IKCP_CMD_PUSH 81
#define IKCP_CMD_ACK 82
#define IKCP_CMD_WASK 83
#define IKCP_CMD_WINS 84

#define IKCP_ASK_SEND 1
#define IKCP_ASK_TELL 2

#define IKCP_WND_SND 32
#define IKCP_WND_RCV 128

#define IKCP_MTU_DEF 1400
#define IKCP_ACK_FAST 3
#define IKCP_INTERVAL 100
#define IKCP_OVERHEAD 24
#define IKCP_DEADLINK 20
#define IKCP_THRESH_INIT 2
#define IKCP_THRESH_MIN 2
#define IKCP_PROBE_INIT 7000
#define IKCP_PROBE_LIMIT 120000
#define IKCP_FASTACK_LIMIT 5

//=====================================================================
// QUEUE (intrusive doubly linked list with a sentinel head)
//=====================================================================
typedef struct IQUEUEHEAD {
    struct IQUEUEHEAD *next, *prev;
} IQUEUEHEAD;

#define IQUEUE_INIT(ptr) ((ptr)->next = (ptr), (ptr)->prev = (ptr))
#define IQUEUE_HEAD_INIT(name) { &(name), &(name) }
#define IQUEUE_ENTRY(ptr, type, member) ((type*)((char*)(ptr) - (size_t)(&((type*)0)->member)))

#define IQUEUE_FOREACH(node, que) \
    if ((node) = (que)->next, (node) != (que)) \
        for (; (node) != (que); (node) = (node)->next)

#define IQUEUE_FOREACH_SAFE(node, tmp, que) \
    if ((node) = (que)->next, (tmp) = (node)->next, (node) != (que)) \
        for (; (node) != (que); (node) = (tmp), (tmp) = (node)->next)

// Insert `ptr` immediately before `head`.
#define IQUEUE_ADD(ptr, head) \
    do { \
        (ptr)->next = (head); \
        (head)->prev->next = (ptr); \
        (ptr)->prev = (head)->prev; \
        (head)->prev = (ptr); \
    } while (0)

#define IQUEUE_DEL(entry) \
    do { \
        (entry)->prev->next = (entry)->next; \
        (entry)->next->prev = (entry)->prev; \
        (entry)->next = (entry); \
        (entry)->prev = (entry); \
    } while (0)

#define IQUEUE_IS_EMPTY(entry) ((entry) == (entry)->next)

//=====================================================================
// SEGMENT
//=====================================================================
typedef struct IKCPSEG {
    IQUEUEHEAD node;
    IUINT32 conv;
    IUINT32 cmd;
    IUINT32 frg;
    IUINT32 wnd;
    IUINT32 ts;
    IUINT32 sn;
    IUINT32 una;
    IUINT32 len;
    IUINT32 resendts;
    IUINT32 rto;
    IUINT32 fastack;
    IUINT32 xmit;
    char data[1];
} IKCPSEG;

//=====================================================================
// KCP CONTROL
//=====================================================================
typedef struct IKCPCB {
    IUINT32 conv, mtu, mss, state;
    IUINT32 snd_una, snd_nxt, rcv_nxt;
    IUINT32 ts_recent, ts_lastack, ssthresh;
    IINT32 rx_rttval, rx_srtt, rx_rto, rx_minrto;
    IUINT32 snd_wnd, rcv_wnd, rmt_wnd, cwnd, probe;
    IUINT32 current, interval, ts_flush, xmit;
    IUINT32 nrcv_buf, nsnd_buf;
    IUINT32 nrcv_que, nsnd_que;
    IUINT32 nodelay, updated;
    IUINT32 ts_probe, probe_wait;
    IUINT32 dead_link, incr;
    IQUEUEHEAD snd_queue;
    IQUEUEHEAD rcv_queue;
    IQUEUEHEAD snd_buf;
    IQUEUEHEAD rcv_buf;
    IUINT32 *acklist;
    IUINT32 ackcount;
    IUINT32 ackblock;
    void *user;
    char *buffer;
    int fastresend;
    int fastlimit;
    int nocwnd, stream;
    int logmask;
    int (*output)(const char *buf, int len, struct IKCPCB *kcp, void *user);
    void (*writelog)(const char *log, struct IKCPCB *kcp, void *user);
} ikcpcb;

#define IKCP_LOG_OUTPUT 1
#define IKCP_LOG_INPUT 2
#define IKCP_LOG_SEND 4
#define IKCP_LOG_RECV 8
#define IKCP_LOG_IN_DATA 16
#define IKCP_LOG_IN_ACK 32
#define IKCP_LOG_IN_PROBE 64
#define IKCP_LOG_IN_WINS 128
#define IKCP_LOG_OUT_DATA 256
#define IKCP_LOG_OUT_ACK 512
#define IKCP_LOG_OUT_PROBE 1024
#define IKCP_LOG_OUT_WINS 2048

//---------------------------------------------------------------------
// interface
//---------------------------------------------------------------------
IKCP_EXPORT ikcpcb *ikcp_create(IUINT32 conv, void *user);
IKCP_EXPORT void ikcp_release(ikcpcb *kcp);

IKCP_EXPORT void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len, ikcpcb *kcp, void *user));
IKCP_EXPORT void ikcp_setlogmask(ikcpcb *kcp, int mask);
IKCP_EXPORT void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...);

IKCP_EXPORT IINT32 ikcp_recv(ikcpcb *kcp, char *buffer, int len);
IKCP_EXPORT IINT32 ikcp_send(ikcpcb *kcp, const char *buffer, int len);
IKCP_EXPORT IINT32 ikcp_peeksize(const ikcpcb *kcp);

IKCP_EXPORT void ikcp_update(ikcpcb *kcp, IUINT32 current);
IKCP_EXPORT IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current);
IKCP_EXPORT IINT32 ikcp_input(ikcpcb *kcp, const char *data, long size);
IKCP_EXPORT void ikcp_flush(ikcpcb *kcp);

IKCP_EXPORT IINT32 ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd);
IKCP_EXPORT IINT32 ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc);
IKCP_EXPORT void ikcp_setmtu(ikcpcb *kcp, int mtu);

IKCP_EXPORT IUINT32 ikcp_getconv(const void *ptr);

#ifdef __cplusplus
}
#endif

#endif  // __IKCP_H__

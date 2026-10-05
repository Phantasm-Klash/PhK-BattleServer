// ikcp.c - A fast and reliable ARQ protocol (KCP)
//
// Vendored, minimal copy of the reference KCP implementation.
// Upstream: https://github.com/skywind3000/kcp
// License: MIT (see LICENSE in this directory).

#include "ikcp.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

//---------------------------------------------------------------------
// arithmetic helpers
//---------------------------------------------------------------------
static inline IUINT32 _imin_(IUINT32 a, IUINT32 b) { return a <= b ? a : b; }
static inline IUINT32 _imax_(IUINT32 a, IUINT32 b) { return a >= b ? a : b; }

static inline IUINT32 _ibound_(IUINT32 lower, IUINT32 middle, IUINT32 upper) {
    return _imin_(_imax_(lower, middle), upper);
}

static inline IINT32 _itimediff(IUINT32 later, IUINT32 earlier) {
    return ((IINT32)(later - earlier));
}

//---------------------------------------------------------------------
// little-endian wire codec (portable, no host-endian assumptions)
//---------------------------------------------------------------------
static inline char *ikcp_encode8u(char *p, IUINT8 c) {
    *(IUINT8 *)p = c;
    return p + 1;
}
static inline char *ikcp_encode16u(char *p, IUINT16 w) {
    *(IUINT8 *)(p + 0) = (IUINT8)(w & 0xff);
    *(IUINT8 *)(p + 1) = (IUINT8)((w >> 8) & 0xff);
    return p + 2;
}
static inline char *ikcp_encode32u(char *p, IUINT32 l) {
    *(IUINT8 *)(p + 0) = (IUINT8)(l & 0xff);
    *(IUINT8 *)(p + 1) = (IUINT8)((l >> 8) & 0xff);
    *(IUINT8 *)(p + 2) = (IUINT8)((l >> 16) & 0xff);
    *(IUINT8 *)(p + 3) = (IUINT8)((l >> 24) & 0xff);
    return p + 4;
}
static inline const char *ikcp_decode8u(const char *p, IUINT8 *c) {
    *c = *(const IUINT8 *)p;
    return p + 1;
}
static inline const char *ikcp_decode16u(const char *p, IUINT16 *w) {
    *w = (IUINT16)((IUINT16)(*(const IUINT8 *)(p + 0)) |
                   ((IUINT16)(*(const IUINT8 *)(p + 1)) << 8));
    return p + 2;
}
static inline const char *ikcp_decode32u(const char *p, IUINT32 *l) {
    *l = (IUINT32)((IUINT32)(*(const IUINT8 *)(p + 0)) |
                   ((IUINT32)(*(const IUINT8 *)(p + 1)) << 8) |
                   ((IUINT32)(*(const IUINT8 *)(p + 2)) << 16) |
                   ((IUINT32)(*(const IUINT8 *)(p + 3)) << 24));
    return p + 4;
}

//---------------------------------------------------------------------
// memory
//---------------------------------------------------------------------
static void *ikcp_malloc(size_t size) { return malloc(size); }
static void ikcp_free(void *ptr) { free(ptr); }

static IKCPSEG *ikcp_segment_new(ikcpcb *kcp, int size) {
    (void)kcp;
    return (IKCPSEG *)ikcp_malloc(sizeof(IKCPSEG) + (size_t)size);
}

static void ikcp_segment_delete(ikcpcb *kcp, IKCPSEG *seg) {
    (void)kcp;
    ikcp_free(seg);
}

//---------------------------------------------------------------------
// logging
//---------------------------------------------------------------------
void ikcp_log(ikcpcb *kcp, int mask, const char *fmt, ...) {
    char buffer[512];
    va_list argptr;
    if (kcp == NULL) return;
    if ((mask & kcp->logmask) == 0 || kcp->writelog == 0) return;
    va_start(argptr, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, argptr);
    va_end(argptr);
    kcp->writelog(buffer, kcp, kcp->user);
}

//---------------------------------------------------------------------
// create / release
//---------------------------------------------------------------------
ikcpcb *ikcp_create(IUINT32 conv, void *user) {
    ikcpcb *kcp = (ikcpcb *)ikcp_malloc(sizeof(ikcpcb));
    if (kcp == NULL) return NULL;

    kcp->conv = conv;
    kcp->user = user;
    kcp->state = 0;
    kcp->mtu = IKCP_MTU_DEF;
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
    kcp->stream = 0;
    kcp->buffer = (char *)ikcp_malloc((kcp->mtu + IKCP_OVERHEAD) * 3);
    if (kcp->buffer == NULL) {
        ikcp_free(kcp);
        return NULL;
    }

    IQUEUE_INIT(&kcp->snd_queue);
    IQUEUE_INIT(&kcp->rcv_queue);
    IQUEUE_INIT(&kcp->snd_buf);
    IQUEUE_INIT(&kcp->rcv_buf);

    kcp->nrcv_buf = 0;
    kcp->nsnd_buf = 0;
    kcp->nrcv_que = 0;
    kcp->nsnd_que = 0;

    kcp->acklist = NULL;
    kcp->ackblock = 0;
    kcp->ackcount = 0;

    kcp->rx_srtt = 0;
    kcp->rx_rttval = 0;
    kcp->rx_rto = IKCP_RTO_DEF;
    kcp->rx_minrto = IKCP_RTO_MIN;

    kcp->current = 0;
    kcp->interval = IKCP_INTERVAL;
    kcp->ts_flush = IKCP_INTERVAL;
    kcp->nodelay = 0;
    kcp->updated = 0;

    kcp->snd_wnd = IKCP_WND_SND;
    kcp->rcv_wnd = IKCP_WND_RCV;
    kcp->rmt_wnd = IKCP_WND_RCV;
    // Start with a usable congestion window so the first segment can leave
    // immediately instead of waiting for the first flush to bootstrap it.
    kcp->cwnd = 1;
    kcp->incr = kcp->mss;
    kcp->probe = 0;

    kcp->ts_probe = 0;
    kcp->probe_wait = 0;
    kcp->ssthresh = IKCP_THRESH_INIT;
    kcp->dead_link = IKCP_DEADLINK;
    kcp->fastresend = 0;
    kcp->fastlimit = IKCP_FASTACK_LIMIT;
    kcp->nocwnd = 0;
    kcp->xmit = 0;

    kcp->output = NULL;
    kcp->writelog = NULL;
    kcp->logmask = 0;

    return kcp;
}

void ikcp_release(ikcpcb *kcp) {
    if (kcp == NULL) return;
    {
        IQUEUEHEAD *p, *n;
        IQUEUE_FOREACH_SAFE(p, n, &kcp->snd_buf) {
            ikcp_segment_delete(kcp, IQUEUE_ENTRY(p, IKCPSEG, node));
        }
        IQUEUE_FOREACH_SAFE(p, n, &kcp->rcv_buf) {
            ikcp_segment_delete(kcp, IQUEUE_ENTRY(p, IKCPSEG, node));
        }
        IQUEUE_FOREACH_SAFE(p, n, &kcp->snd_queue) {
            ikcp_segment_delete(kcp, IQUEUE_ENTRY(p, IKCPSEG, node));
        }
        IQUEUE_FOREACH_SAFE(p, n, &kcp->rcv_queue) {
            ikcp_segment_delete(kcp, IQUEUE_ENTRY(p, IKCPSEG, node));
        }
    }
    ikcp_free(kcp->buffer);
    ikcp_free(kcp->acklist);
    ikcp_free(kcp);
}

void ikcp_setoutput(ikcpcb *kcp, int (*output)(const char *buf, int len, ikcpcb *kcp, void *user)) {
    kcp->output = output;
}

void ikcp_setlogmask(ikcpcb *kcp, int mask) {
    kcp->logmask = mask;
}

void ikcp_setmtu(ikcpcb *kcp, int mtu) {
    char *buffer;
    if (mtu < 50 || mtu < IKCP_OVERHEAD) return;
    buffer = (char *)ikcp_malloc((IUINT32)(mtu + IKCP_OVERHEAD) * 3);
    if (buffer == NULL) return;
    ikcp_free(kcp->buffer);
    kcp->buffer = buffer;
    kcp->mtu = (IUINT32)mtu;
    kcp->mss = kcp->mtu - IKCP_OVERHEAD;
}

IINT32 ikcp_wndsize(ikcpcb *kcp, int sndwnd, int rcvwnd) {
    if (kcp != NULL) {
        if (sndwnd > 0) kcp->snd_wnd = (IUINT32)sndwnd;
        if (rcvwnd > 0) kcp->rcv_wnd = (IUINT32)rcvwnd;
    }
    return 0;
}

IINT32 ikcp_nodelay(ikcpcb *kcp, int nodelay, int interval, int resend, int nc) {
    if (kcp == NULL) return -1;
    if (nodelay >= 0) {
        kcp->nodelay = (IUINT32)nodelay;
        if (nodelay) kcp->rx_minrto = IKCP_RTO_NDL;
        else kcp->rx_minrto = IKCP_RTO_MIN;
    }
    if (interval >= 0) {
        if (interval > 5000) interval = 5000;
        else if (interval < 10) interval = 10;
        kcp->interval = (IUINT32)interval;
    }
    if (resend >= 0) kcp->fastresend = resend;
    if (nc >= 0) kcp->nocwnd = nc;
    return 0;
}

IUINT32 ikcp_getconv(const void *ptr) {
    IUINT32 conv;
    ikcp_decode32u((const char *)ptr, &conv);
    return conv;
}

//---------------------------------------------------------------------
// segment encoding
//---------------------------------------------------------------------
static char *ikcp_encode_seg(char *ptr, const IKCPSEG *seg) {
    ptr = ikcp_encode32u(ptr, seg->conv);
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->cmd);
    ptr = ikcp_encode8u(ptr, (IUINT8)seg->frg);
    ptr = ikcp_encode16u(ptr, (IUINT16)seg->wnd);
    ptr = ikcp_encode32u(ptr, seg->ts);
    ptr = ikcp_encode32u(ptr, seg->sn);
    ptr = ikcp_encode32u(ptr, seg->una);
    ptr = ikcp_encode32u(ptr, seg->len);
    return ptr;
}

static void ikcp_output(const ikcpcb *kcp, const char *data, int size) {
    if (kcp->output == 0) return;
    kcp->output(data, size, (ikcpcb *)kcp, kcp->user);
}

static IUINT32 ikcp_wnd_unused(const ikcpcb *kcp) {
    if (kcp->nrcv_que < kcp->rcv_wnd) return kcp->rcv_wnd - kcp->nrcv_que;
    return 0;
}

//---------------------------------------------------------------------
// receive queue
//---------------------------------------------------------------------
IINT32 ikcp_peeksize(const ikcpcb *kcp) {
    IQUEUEHEAD *p;
    IKCPSEG *seg;
    int length = 0;

    if (IQUEUE_IS_EMPTY(&kcp->rcv_queue)) return -1;

    seg = IQUEUE_ENTRY(kcp->rcv_queue.next, IKCPSEG, node);
    if (kcp->nrcv_que < seg->frg + 1) return -2;

    IQUEUE_FOREACH(p, &kcp->rcv_queue) {
        seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        length += (int)seg->len;
        if (seg->frg == 0) break;
    }
    return length;
}

static void ikcp_move_rcvbuf_to_rcvqueue(ikcpcb *kcp) {
    while (!IQUEUE_IS_EMPTY(&kcp->rcv_buf)) {
        IKCPSEG *seg = IQUEUE_ENTRY(kcp->rcv_buf.next, IKCPSEG, node);
        if (seg->sn == kcp->rcv_nxt && kcp->nrcv_que < kcp->rcv_wnd) {
            IQUEUE_DEL(&seg->node);
            kcp->nrcv_buf--;
            IQUEUE_ADD(&seg->node, &kcp->rcv_queue);
            kcp->nrcv_que++;
            kcp->rcv_nxt++;
        } else {
            break;
        }
    }
}

IINT32 ikcp_recv(ikcpcb *kcp, char *buffer, int len) {
    IQUEUEHEAD *p;
    IKCPSEG *seg;
    int peek = 0;
    int size = 0;

    if (IQUEUE_IS_EMPTY(&kcp->rcv_queue)) return -1;

    if (len < 0) {
        len = -len;
        peek = 1;
    }

    seg = IQUEUE_ENTRY(kcp->rcv_queue.next, IKCPSEG, node);
    if (kcp->nrcv_que < seg->frg + 1) return -2;

    {
        int total = 0;
        IQUEUE_FOREACH(p, &kcp->rcv_queue) {
            seg = IQUEUE_ENTRY(p, IKCPSEG, node);
            total += (int)seg->len;
            if (seg->frg == 0) break;
        }
        if (total > len) return -3;
        size = total;
    }

    if (peek) {
        char *out = buffer;
        IQUEUE_FOREACH(p, &kcp->rcv_queue) {
            seg = IQUEUE_ENTRY(p, IKCPSEG, node);
            if (buffer != NULL) {
                memcpy(out, seg->data, seg->len);
                out += seg->len;
            }
            if (seg->frg == 0) break;
        }
        return size;
    }

    // merge fragment (consume)
    {
        IQUEUEHEAD *n;
        char *out = buffer;
        for (p = kcp->rcv_queue.next; p != &kcp->rcv_queue; p = n) {
            int fragment;
            seg = IQUEUE_ENTRY(p, IKCPSEG, node);
            n = seg->node.next;
            if (buffer != NULL) {
                memcpy(out, seg->data, seg->len);
                out += seg->len;
            }
            fragment = (int)seg->frg;
            IQUEUE_DEL(&seg->node);
            ikcp_segment_delete(kcp, seg);
            kcp->nrcv_que--;
            if (fragment == 0) break;
        }
    }

    ikcp_move_rcvbuf_to_rcvqueue(kcp);
    return size;
}

//---------------------------------------------------------------------
// send
//---------------------------------------------------------------------
IINT32 ikcp_send(ikcpcb *kcp, const char *buffer, int len) {
    IKCPSEG *seg;
    int count, i;
    int mss;

    if (kcp == NULL) return -1;
    mss = (int)kcp->mss;
    if (mss <= 0) return -1;
    if (len <= 0) return -1;

    if (kcp->stream != 0) {
        if (!IQUEUE_IS_EMPTY(&kcp->snd_queue)) {
            IKCPSEG *old = IQUEUE_ENTRY(kcp->snd_queue.prev, IKCPSEG, node);
            if (old->len < (IUINT32)mss) {
                int capacity = mss - (int)old->len;
                int extend = (len < capacity) ? len : capacity;
                seg = ikcp_segment_new(kcp, (int)old->len + extend);
                if (seg == NULL) return -2;
                IQUEUE_ADD(&seg->node, &kcp->snd_queue);
                memcpy(seg->data, old->data, old->len);
                if (buffer != NULL) {
                    memcpy(seg->data + old->len, buffer, extend);
                    buffer += extend;
                }
                seg->len = old->len + (IUINT32)extend;
                seg->frg = 0;
                len -= extend;
                IQUEUE_DEL(&old->node);
                ikcp_segment_delete(kcp, old);
            }
        }
        if (len <= 0) return 0;
    }

    if (len <= mss) {
        count = 1;
    } else {
        count = (len + mss - 1) / mss;
    }

    for (i = 0; i < count; i++) {
        int size = (len > mss) ? mss : len;
        seg = ikcp_segment_new(kcp, size);
        if (seg == NULL) return -2;
        if (buffer != NULL && len > 0) {
            memcpy(seg->data, buffer, size);
        }
        seg->len = (IUINT32)size;
        seg->frg = (kcp->stream == 0) ? (IUINT32)(count - i - 1) : 0;
        IQUEUE_INIT(&seg->node);
        IQUEUE_ADD(&seg->node, &kcp->snd_queue);
        kcp->nsnd_que++;
        if (buffer != NULL) buffer += size;
        len -= size;
    }

    return 0;
}

//---------------------------------------------------------------------
// input processing
//---------------------------------------------------------------------
static void ikcp_parse_una(ikcpcb *kcp, IUINT32 una) {
    IQUEUEHEAD *p, *n;
    IQUEUE_FOREACH_SAFE(p, n, &kcp->snd_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        if (_itimediff(una, seg->sn) > 0) {
            IQUEUE_DEL(&seg->node);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
        } else {
            break;
        }
    }
}

static void ikcp_shrink_buf(ikcpcb *kcp) {
    IQUEUEHEAD *p = kcp->snd_buf.next;
    if (p != &kcp->snd_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        kcp->snd_una = seg->sn;
    } else {
        kcp->snd_una = kcp->snd_nxt;
    }
}

static void ikcp_parse_ack(ikcpcb *kcp, IUINT32 sn) {
    IQUEUEHEAD *p, *n;
    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0) return;
    IQUEUE_FOREACH_SAFE(p, n, &kcp->snd_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        if (sn == seg->sn) {
            IQUEUE_DEL(&seg->node);
            ikcp_segment_delete(kcp, seg);
            kcp->nsnd_buf--;
            break;
        }
        if (_itimediff(sn, seg->sn) < 0) break;
    }
}

static void ikcp_parse_fastack(ikcpcb *kcp, IUINT32 sn, IUINT32 ts) {
    IQUEUEHEAD *p, *n;
    (void)ts;
    if (_itimediff(sn, kcp->snd_una) < 0 || _itimediff(sn, kcp->snd_nxt) >= 0) return;
    IQUEUE_FOREACH_SAFE(p, n, &kcp->snd_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        if (_itimediff(sn, seg->sn) < 0) break;
        else if (sn != seg->sn) seg->fastack++;
    }
}

static void ikcp_update_ack(ikcpcb *kcp, IINT32 rtt) {
    IINT32 rto = 0;
    if (kcp->rx_srtt == 0) {
        kcp->rx_srtt = rtt;
        kcp->rx_rttval = rtt / 2;
    } else {
        long delta = (long)rtt - (long)kcp->rx_srtt;
        if (delta < 0) delta = -delta;
        kcp->rx_rttval = (IINT32)((3 * kcp->rx_rttval + delta) / 4);
        kcp->rx_srtt = (IINT32)((7 * (long)kcp->rx_srtt + rtt) / 8);
        if (kcp->rx_srtt < 1) kcp->rx_srtt = 1;
    }
    rto = kcp->rx_srtt + (IINT32)_imax_(kcp->interval, (IUINT32)(4 * kcp->rx_rttval));
    kcp->rx_rto = (IINT32)_ibound_((IUINT32)kcp->rx_minrto, (IUINT32)rto, IKCP_RTO_MAX);
}

static void ikcp_ack_push(ikcpcb *kcp, IUINT32 sn, IUINT32 ts) {
    IUINT32 newsize = kcp->ackcount + 1;
    if (newsize > kcp->ackblock) {
        IUINT32 *acklist;
        IUINT32 newblock;
        IUINT32 x;
        for (newblock = 8; newblock < newsize; newblock <<= 1) {
        }
        acklist = (IUINT32 *)ikcp_malloc((size_t)newblock * sizeof(IUINT32) * 2);
        if (acklist == NULL) return;
        for (x = 0; x < kcp->ackcount; x++) {
            acklist[x * 2 + 0] = kcp->acklist[x * 2 + 0];
            acklist[x * 2 + 1] = kcp->acklist[x * 2 + 1];
        }
        ikcp_free(kcp->acklist);
        kcp->acklist = acklist;
        kcp->ackblock = newblock;
    }
    kcp->acklist[kcp->ackcount * 2 + 0] = sn;
    kcp->acklist[kcp->ackcount * 2 + 1] = ts;
    kcp->ackcount++;
}

static void ikcp_ack_get(const ikcpcb *kcp, int p, IUINT32 *sn, IUINT32 *ts) {
    if (sn) *sn = kcp->acklist[p * 2 + 0];
    if (ts) *ts = kcp->acklist[p * 2 + 1];
}

static void ikcp_parse_data(ikcpcb *kcp, IKCPSEG *newseg) {
    IQUEUEHEAD *p, *n;
    IUINT32 sn = newseg->sn;
    int repeat = 0;

    if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) >= 0 ||
        _itimediff(sn, kcp->rcv_nxt) < 0) {
        ikcp_segment_delete(kcp, newseg);
        return;
    }

    IQUEUE_FOREACH_SAFE(p, n, &kcp->rcv_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        if (seg->sn == sn) {
            repeat = 1;
            break;
        }
        if (_itimediff(sn, seg->sn) < 0) {
            break;
        }
    }

    if (repeat == 0) {
        IQUEUE_ADD(&newseg->node, p);
        kcp->nrcv_buf++;
    } else {
        ikcp_segment_delete(kcp, newseg);
    }

    ikcp_move_rcvbuf_to_rcvqueue(kcp);
}

IINT32 ikcp_input(ikcpcb *kcp, const char *data, long size) {
    IUINT32 una;
    IUINT32 maxack = 0;
    int flag = 0;
    IUINT32 latest_ts = 0;

    if (kcp == NULL) return -1;
    if (data == NULL || size < IKCP_OVERHEAD) return -1;

    una = kcp->snd_una;

    while (1) {
        IUINT32 ts, sn, len, conv;
        IUINT16 wnd;
        IUINT8 cmd, frg;

        if (size < IKCP_OVERHEAD) break;

        data = ikcp_decode32u(data, &conv);
        if (conv != kcp->conv) return -1;

        data = ikcp_decode8u(data, &cmd);
        data = ikcp_decode8u(data, &frg);
        data = ikcp_decode16u(data, &wnd);
        data = ikcp_decode32u(data, &ts);
        data = ikcp_decode32u(data, &sn);
        data = ikcp_decode32u(data, &una);
        data = ikcp_decode32u(data, &len);

        size -= IKCP_OVERHEAD;

        if ((long)size < (long)len) return -2;

        if (cmd != IKCP_CMD_PUSH && cmd != IKCP_CMD_ACK &&
            cmd != IKCP_CMD_WASK && cmd != IKCP_CMD_WINS) {
            return -3;
        }

        kcp->rmt_wnd = wnd;
        ikcp_parse_una(kcp, una);
        ikcp_shrink_buf(kcp);

        if (cmd == IKCP_CMD_ACK) {
            if (_itimediff(kcp->current, ts) >= 0) {
                ikcp_update_ack(kcp, _itimediff(kcp->current, ts));
            }
            ikcp_parse_ack(kcp, sn);
            ikcp_shrink_buf(kcp);
            if (flag == 0) {
                flag = 1;
                maxack = sn;
                latest_ts = ts;
            } else if (_itimediff(sn, maxack) > 0) {
                maxack = sn;
                latest_ts = ts;
            }
            ikcp_log(kcp, IKCP_LOG_IN_ACK, "input ack: sn=%lu rtt=%ld rto=%ld",
                     (unsigned long)sn, (long)_itimediff(kcp->current, ts), (long)kcp->rx_rto);
        } else if (cmd == IKCP_CMD_PUSH) {
            ikcp_log(kcp, IKCP_LOG_IN_DATA, "input psh: sn=%lu ts=%lu",
                     (unsigned long)sn, (unsigned long)ts);
            if (_itimediff(sn, kcp->rcv_nxt + kcp->rcv_wnd) < 0) {
                ikcp_ack_push(kcp, sn, ts);
                if (_itimediff(sn, kcp->rcv_nxt) >= 0) {
                    IKCPSEG *seg = ikcp_segment_new(kcp, (int)len);
                    if (seg == NULL) return -4;
                    seg->conv = conv;
                    seg->cmd = cmd;
                    seg->frg = frg;
                    seg->wnd = wnd;
                    seg->ts = ts;
                    seg->sn = sn;
                    seg->una = una;
                    seg->len = len;
                    if (len > 0) memcpy(seg->data, data, len);
                    ikcp_parse_data(kcp, seg);
                }
            }
        } else if (cmd == IKCP_CMD_WASK) {
            kcp->probe |= IKCP_ASK_TELL;
            ikcp_log(kcp, IKCP_LOG_IN_PROBE, "input probe");
        } else if (cmd == IKCP_CMD_WINS) {
            ikcp_log(kcp, IKCP_LOG_IN_WINS, "input wins: %lu", (unsigned long)wnd);
        }

        data += len;
        size -= (long)len;
    }

    if (flag != 0) {
        ikcp_parse_fastack(kcp, maxack, latest_ts);
    }

    if (_itimediff(kcp->snd_una, una) > 0) {
        if (kcp->cwnd < kcp->rmt_wnd) {
            IUINT32 mss = kcp->mss;
            if (kcp->cwnd < kcp->ssthresh) {
                kcp->cwnd++;
                kcp->incr += mss;
            } else {
                if (kcp->incr < mss) kcp->incr = mss;
                kcp->incr += (mss * mss) / kcp->incr + (mss / 16);
                if ((kcp->cwnd + 1) * mss <= kcp->incr) {
                    kcp->cwnd = (kcp->incr + mss - 1) / ((mss > 0) ? mss : 1);
                }
            }
            if (kcp->cwnd > kcp->rmt_wnd) {
                kcp->cwnd = kcp->rmt_wnd;
                kcp->incr = kcp->rmt_wnd * mss;
            }
        }
    }

    return 0;
}

//---------------------------------------------------------------------
// flush
//---------------------------------------------------------------------
void ikcp_flush(ikcpcb *kcp) {
    IUINT32 current = kcp->current;
    char *buffer = kcp->buffer;
    char *ptr = buffer;
    int count, size, i;
    IUINT32 resent, cwnd;
    IUINT32 rtomin;
    IQUEUEHEAD *p;
    int change = 0;
    int lost = 0;
    IKCPSEG seg;

    if (kcp->updated == 0) return;

    if (kcp->cwnd < 1) {
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
    }

    seg.conv = kcp->conv;
    seg.cmd = IKCP_CMD_ACK;
    seg.frg = 0;
    seg.wnd = ikcp_wnd_unused(kcp);
    seg.una = kcp->rcv_nxt;
    seg.len = 0;
    seg.sn = 0;
    seg.ts = 0;

    // flush acknowledges
    count = (int)kcp->ackcount;
    for (i = 0; i < count; i++) {
        size = (int)(ptr - buffer);
        if (size + IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ikcp_ack_get(kcp, i, &seg.sn, &seg.ts);
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    kcp->ackcount = 0;

    // probe window size
    if (kcp->rmt_wnd == 0) {
        if (kcp->probe_wait == 0) {
            kcp->probe_wait = IKCP_PROBE_INIT;
            kcp->ts_probe = kcp->current + kcp->probe_wait;
        } else {
            if (_itimediff(kcp->current, kcp->ts_probe) >= 0) {
                if (kcp->probe_wait < IKCP_PROBE_INIT) kcp->probe_wait = IKCP_PROBE_INIT;
                kcp->probe_wait += kcp->probe_wait / 2;
                if (kcp->probe_wait > IKCP_PROBE_LIMIT) kcp->probe_wait = IKCP_PROBE_LIMIT;
                kcp->ts_probe = kcp->current + kcp->probe_wait;
                kcp->probe |= IKCP_ASK_SEND;
            }
        }
    } else {
        kcp->ts_probe = 0;
        kcp->probe_wait = 0;
    }

    if (kcp->probe & IKCP_ASK_SEND) {
        seg.cmd = IKCP_CMD_WASK;
        size = (int)(ptr - buffer);
        if (size + IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    if (kcp->probe & IKCP_ASK_TELL) {
        seg.cmd = IKCP_CMD_WINS;
        size = (int)(ptr - buffer);
        if (size + IKCP_OVERHEAD > (int)kcp->mtu) {
            ikcp_output(kcp, buffer, size);
            ptr = buffer;
        }
        ptr = ikcp_encode_seg(ptr, &seg);
    }
    kcp->probe = 0;

    // calculate window size
    cwnd = _imin_(kcp->snd_wnd, kcp->rmt_wnd);
    if (kcp->nocwnd == 0) cwnd = _imin_(kcp->cwnd, cwnd);

    // move data from snd_queue to snd_buf
    while (_itimediff(kcp->snd_nxt, kcp->snd_una + cwnd) < 0) {
        IKCPSEG *newseg;
        if (IQUEUE_IS_EMPTY(&kcp->snd_queue)) break;
        newseg = IQUEUE_ENTRY(kcp->snd_queue.next, IKCPSEG, node);
        IQUEUE_DEL(&newseg->node);
        IQUEUE_ADD(&newseg->node, &kcp->snd_buf);
        kcp->nsnd_que--;
        kcp->nsnd_buf++;
        newseg->conv = kcp->conv;
        newseg->cmd = IKCP_CMD_PUSH;
        newseg->wnd = seg.wnd;
        newseg->ts = current;
        newseg->sn = kcp->snd_nxt++;
        newseg->una = kcp->rcv_nxt;
        newseg->resendts = current;
        newseg->rto = (IUINT32)kcp->rx_rto;
        newseg->fastack = 0;
        newseg->xmit = 0;
    }

    // calculate resent
    resent = (kcp->fastresend > 0) ? (IUINT32)kcp->fastresend : 0xffffffffu;
    rtomin = (kcp->nodelay == 0) ? ((IUINT32)kcp->rx_rto >> 3) : 0;

    // flush data segments
    for (p = kcp->snd_buf.next; p != &kcp->snd_buf; p = p->next) {
        IKCPSEG *segment = IQUEUE_ENTRY(p, IKCPSEG, node);
        int needsend = 0;
        if (segment->xmit == 0) {
            needsend = 1;
            segment->xmit++;
            segment->rto = (IUINT32)kcp->rx_rto;
            segment->resendts = current + segment->rto + rtomin;
        } else if (_itimediff(current, segment->resendts) >= 0) {
            needsend = 1;
            segment->xmit++;
            kcp->xmit++;
            if (kcp->nodelay == 0) {
                segment->rto += _imax_(segment->rto, (IUINT32)kcp->rx_rto);
            } else {
                IINT32 step = (kcp->nodelay < 2) ? ((IINT32)(segment->rto)) : kcp->rx_rto;
                segment->rto += (IUINT32)(step / 2);
            }
            segment->resendts = current + segment->rto;
            lost = 1;
        } else if (segment->fastack >= resent) {
            if ((int)segment->xmit <= kcp->fastlimit || kcp->fastlimit <= 0) {
                needsend = 1;
                segment->xmit++;
                segment->fastack = 0;
                segment->resendts = current + segment->rto;
                change++;
            }
        }

        if (needsend) {
            int need;
            segment->ts = current;
            segment->wnd = seg.wnd;
            segment->una = kcp->rcv_nxt;

            size = (int)(ptr - buffer);
            need = IKCP_OVERHEAD + (int)segment->len;

            if (size + need > (int)kcp->mtu) {
                ikcp_output(kcp, buffer, size);
                ptr = buffer;
            }

            ptr = ikcp_encode_seg(ptr, segment);

            if (segment->len > 0) {
                memcpy(ptr, segment->data, segment->len);
                ptr += segment->len;
            }

            if (segment->xmit >= kcp->dead_link) {
                kcp->state = (IUINT32)-1;
            }
        }
    }

    // flush remaining segments
    size = (int)(ptr - buffer);
    if (size > 0) {
        ikcp_output(kcp, buffer, size);
    }

    // update ssthresh
    if (change) {
        IUINT32 inflight = kcp->snd_nxt - kcp->snd_una;
        kcp->ssthresh = inflight / 2;
        if (kcp->ssthresh < IKCP_THRESH_MIN) kcp->ssthresh = IKCP_THRESH_MIN;
        kcp->cwnd = kcp->ssthresh + resent;
        kcp->incr = kcp->cwnd * kcp->mss;
    }

    if (lost) {
        kcp->ssthresh = cwnd / 2;
        if (kcp->ssthresh < IKCP_THRESH_MIN) kcp->ssthresh = IKCP_THRESH_MIN;
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
    }

    if (kcp->cwnd < 1) {
        kcp->cwnd = 1;
        kcp->incr = kcp->mss;
    }
}

//---------------------------------------------------------------------
// update
//---------------------------------------------------------------------
void ikcp_update(ikcpcb *kcp, IUINT32 current) {
    IINT32 slap;

    kcp->current = current;

    if (kcp->updated == 0) {
        kcp->updated = 1;
        kcp->ts_flush = kcp->current;
    }

    slap = _itimediff(kcp->current, kcp->ts_flush);

    if (slap >= 10000 || slap < -10000) {
        kcp->ts_flush = kcp->current;
        slap = 0;
    }

    if (slap >= 0) {
        kcp->ts_flush += kcp->interval;
        if (_itimediff(kcp->current, kcp->ts_flush) >= 0) {
            kcp->ts_flush = kcp->current + kcp->interval;
        }
        ikcp_flush(kcp);
    }
}

IUINT32 ikcp_check(const ikcpcb *kcp, IUINT32 current) {
    IUINT32 ts_flush = kcp->ts_flush;
    IINT32 tm_flush = 0x7fffffff;
    IINT32 tm_packet = 0x7fffffff;
    IQUEUEHEAD *p;

    if (kcp->updated == 0) return current;

    if (_itimediff(current, ts_flush) >= 10000 ||
        _itimediff(current, ts_flush) < -10000) {
        ts_flush = current;
    }

    if (_itimediff(current, ts_flush) >= 0) return current;

    tm_flush = _itimediff(ts_flush, current);

    IQUEUE_FOREACH(p, &kcp->snd_buf) {
        IKCPSEG *seg = IQUEUE_ENTRY(p, IKCPSEG, node);
        IINT32 diff = _itimediff(seg->resendts, current);
        if (diff <= 0) return current;
        if (diff < tm_packet) tm_packet = diff;
    }

    {
        IUINT32 minimal = (IUINT32)(tm_packet < tm_flush ? tm_packet : tm_flush);
        if (minimal >= kcp->interval) minimal = kcp->interval;
        return current + minimal;
    }
}

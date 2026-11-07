/* rd_inflate.c — see rd_inflate.h */
#include "rd_inflate.h"

#define MAXBITS   15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288
#define WMASK     (RD_WINDOW_SIZE - 1u)
#define FLUSH_AT  4096u

struct huff {
    int16_t count[MAXBITS + 1];
    int16_t symbol[FIXLCODES];
};

struct st {
    const uint8_t *in;
    uint32_t incnt, inlen;
    uint32_t bitbuf;
    int bitcnt;
    uint8_t *win;
    uint32_t wpos;       /* total bytes produced */
    uint32_t pending;    /* produced but not yet handed to the sink */
    rd_sink_fn sink;
    void *ctx;
};

static const int16_t k_len[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const int16_t k_lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const int16_t k_dist[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const int16_t k_dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
static const uint8_t k_clorder[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};

static int bits(struct st *s, int need)
{
    uint32_t val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->incnt >= s->inlen) return -1;
        val |= (uint32_t)s->in[s->incnt++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = val >> need;
    s->bitcnt -= need;
    return (int)(val & ((1u << need) - 1u));
}

/* >=0 symbol, -1 out of input, -2 invalid code */
static int decode(struct st *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int b = bits(s, 1);
        if (b < 0) return -1;
        code |= b;
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -2;
}

/* 0 = complete code, >0 incomplete, <0 over-subscribed */
static int construct(struct huff *h, const int16_t *length, int n)
{
    int16_t offs[MAXBITS + 1];
    for (int len = 0; len <= MAXBITS; len++) h->count[len] = 0;
    for (int sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int sym = 0; sym < n; sym++)
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (int16_t)sym;
    return left;
}

static int flush(struct st *s)
{
    if (!s->pending) return 0;
    uint32_t start = (s->wpos - s->pending) & WMASK;
    uint32_t end = s->wpos & WMASK;
    int r = 0;
    if (start < end) {
        r = s->sink(s->ctx, s->win + start, s->pending);
    } else {
        r = s->sink(s->ctx, s->win + start, RD_WINDOW_SIZE - start);
        if (!r && end) r = s->sink(s->ctx, s->win, end);
    }
    s->pending = 0;
    return r;
}

static int put(struct st *s, uint8_t b)
{
    s->win[s->wpos & WMASK] = b;
    s->wpos++;
    if (++s->pending >= FLUSH_AT) return flush(s);
    return 0;
}

static int codes(struct st *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0) return sym == -1 ? RD_ERR_TRUNC : RD_ERR_DATA;
        if (sym < 256) {
            if (put(s, (uint8_t)sym)) return RD_ABORTED;
        } else if (sym == 256) {
            return RD_OK;
        } else {
            sym -= 257;
            if (sym >= 29) return RD_ERR_DATA;
            int b = bits(s, k_lext[sym]);
            if (b < 0) return RD_ERR_TRUNC;
            int len = k_len[sym] + b;
            sym = decode(s, dc);
            if (sym < 0) return sym == -1 ? RD_ERR_TRUNC : RD_ERR_DATA;
            if (sym >= 30) return RD_ERR_DATA;
            b = bits(s, k_dext[sym]);
            if (b < 0) return RD_ERR_TRUNC;
            uint32_t dist = (uint32_t)k_dist[sym] + (uint32_t)b;
            if (dist > s->wpos) return RD_ERR_DATA;
            while (len--) {
                if (put(s, s->win[(s->wpos - dist) & WMASK])) return RD_ABORTED;
            }
        }
    }
}

static int stored(struct st *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->incnt + 4 > s->inlen) return RD_ERR_TRUNC;
    uint32_t len = s->in[s->incnt] | ((uint32_t)s->in[s->incnt + 1] << 8);
    uint32_t nlen = s->in[s->incnt + 2] | ((uint32_t)s->in[s->incnt + 3] << 8);
    s->incnt += 4;
    if (len != (~nlen & 0xFFFFu)) return RD_ERR_DATA;
    if (s->incnt + len > s->inlen) return RD_ERR_TRUNC;
    while (len--) {
        if (put(s, s->in[s->incnt++])) return RD_ABORTED;
    }
    return RD_OK;
}

static int fixed(struct st *s)
{
    struct huff lc, dc;
    int16_t lengths[FIXLCODES];
    int sym;
    for (sym = 0; sym < 144; sym++) lengths[sym] = 8;
    for (; sym < 256; sym++) lengths[sym] = 9;
    for (; sym < 280; sym++) lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) lengths[sym] = 8;
    construct(&lc, lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) lengths[sym] = 5;
    construct(&dc, lengths, MAXDCODES);
    return codes(s, &lc, &dc);
}

static int dynamic(struct st *s)
{
    struct huff lc, dc;
    int16_t lengths[MAXLCODES + MAXDCODES];
    int nlen = bits(s, 5);
    int ndist = bits(s, 5);
    int ncode = bits(s, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0) return RD_ERR_TRUNC;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES) return RD_ERR_DATA;

    int index;
    for (index = 0; index < ncode; index++) {
        int b = bits(s, 3);
        if (b < 0) return RD_ERR_TRUNC;
        lengths[k_clorder[index]] = (int16_t)b;
    }
    for (; index < 19; index++) lengths[k_clorder[index]] = 0;
    if (construct(&lc, lengths, 19) != 0) return RD_ERR_DATA;

    index = 0;
    while (index < nlen + ndist) {
        int sym = decode(s, &lc);
        if (sym < 0) return sym == -1 ? RD_ERR_TRUNC : RD_ERR_DATA;
        if (sym < 16) {
            lengths[index++] = (int16_t)sym;
        } else {
            int len = 0, rep;
            if (sym == 16) {
                if (index == 0) return RD_ERR_DATA;
                len = lengths[index - 1];
                rep = bits(s, 2);
                if (rep < 0) return RD_ERR_TRUNC;
                rep += 3;
            } else if (sym == 17) {
                rep = bits(s, 3);
                if (rep < 0) return RD_ERR_TRUNC;
                rep += 3;
            } else {
                rep = bits(s, 7);
                if (rep < 0) return RD_ERR_TRUNC;
                rep += 11;
            }
            if (index + rep > nlen + ndist) return RD_ERR_DATA;
            while (rep--) lengths[index++] = (int16_t)len;
        }
    }
    if (lengths[256] == 0) return RD_ERR_DATA;

    int err = construct(&lc, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lc.count[0] != 1)) return RD_ERR_DATA;
    err = construct(&dc, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - dc.count[0] != 1)) return RD_ERR_DATA;
    return codes(s, &lc, &dc);
}

int rd_inflate(const uint8_t *src, uint32_t src_len, uint8_t *window,
               rd_sink_fn sink, void *ctx)
{
    struct st s;
    s.in = src; s.incnt = 0; s.inlen = src_len;
    s.bitbuf = 0; s.bitcnt = 0;
    s.win = window; s.wpos = 0; s.pending = 0;
    s.sink = sink; s.ctx = ctx;

    int last, err;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (last < 0 || type < 0) return RD_ERR_TRUNC;
        if (type == 0)      err = stored(&s);
        else if (type == 1) err = fixed(&s);
        else if (type == 2) err = dynamic(&s);
        else                err = RD_ERR_DATA;
        if (err) return err;
    } while (!last);
    return flush(&s) ? RD_ABORTED : RD_OK;
}

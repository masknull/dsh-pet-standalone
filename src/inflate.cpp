// inflate.cpp — raw DEFLATE decompressor (RFC 1951), independent implementation.
#include "inflate.h"
#include <cstring>

namespace {

constexpr int kMaxBits = 15;
constexpr int kMaxLcodes = 286;   // 257..285 + literals 0..255 + end-of-block 256
constexpr int kMaxDcodes = 30;
constexpr int kMaxCodes = kMaxLcodes + kMaxDcodes + 1;

// Length code table (codes 257..285): base lengths and extra bits.
const uint16_t kLenBase[29] = {3,   4,   5,   6,   7,   8,   9,   10,  11,  13,
                               15,  17,  19,  23,  27,  31,  35,  43,  51,  59,
                               67,  83,  99,  115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                               2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};

// Distance codes 0..29.
const uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,
                                17,   25,   33,   49,   65,   97,   129,  193,
                                257,  385,  513,  769,  1025, 1537, 2049, 3073,
                                4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0,  0,  0,  0,  1,  1,  2,  2,  3,  3,  4,  4,
                                5,  5,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10,
                                11, 11, 12, 12, 13, 13};

// Code-length repeat codes during dynamic header construction.
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct BitReader {
    const uint8_t* d;
    size_t len;
    size_t pos = 0;  // byte position
    int bit = 0;     // next bit within d[pos] (0 = LSB of current byte)

    int need(int n) { return pos < len || (pos == len && bit == 0); }
    // Read up to 16 bits, LSB-first. Returns -1 on EOF.
    long getBits(int n) {
        long v = 0;
        for (int i = 0; i < n; i++) {
            if (pos >= len) return -1;
            v |= (long)((d[pos] >> bit) & 1) << i;
            if (++bit == 8) {
                bit = 0;
                pos++;
            }
        }
        return v;
    }
    void alignByte() {
        if (bit != 0) {
            bit = 0;
            pos++;
        }
    }
};

struct Huffman {
    uint16_t count[kMaxBits + 1] = {0};  // count[len] = number of codes of that length
    uint16_t symbol[kMaxCodes] = {0};    // symbols sorted by (len, symbol)
    int maxLen = 0;

    // Build from lens[], n symbols. Returns 0 OK, -1 over-subscribed, -2 incomplete/empty.
    int build(const uint8_t* lens, int n) {
        for (int len = 1; len <= kMaxBits; len++) count[len] = 0;
        for (int i = 0; i < n; i++) {
            if (lens[i] > kMaxBits) return -1;
            if (lens[i] > 0) count[lens[i]]++;
        }
        // Validate: canonical codes must not be over-subscribed.
        int left = 1;
        maxLen = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) return -1;  // over-subscribed
            if (count[len]) maxLen = len;
        }
        if (left > 0 && n == 0) return -2;
        // Order symbols by length (stable insertion in symbol[] slots).
        uint16_t offs[kMaxBits + 2];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; len++) offs[len + 1] = offs[len] + count[len];
        for (int sym = 0; sym < n; sym++) {
            if (lens[sym]) symbol[offs[lens[sym]]++] = (uint16_t)sym;
        }
        return 0;
    }

    // Decode one symbol. Returns symbol or -1 on error/EOF.
    int decode(BitReader& br) const {
        if (maxLen == 0) return -1;
        long code = 0;
        int first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            long b = br.getBits(1);
            if (b < 0) return -1;
            code |= b;
            int cnt = count[len];
            if (code - first < cnt) {
                if (len > maxLen) return -1;
                return symbol[index + (int)(code - first)];
            }
            index += cnt;
            first = first + cnt;
            first <<= 1;
            code <<= 1;
        }
        return -1;
    }
};

}  // namespace

int inflateRaw(const uint8_t* in, size_t inLen, uint8_t* out, size_t expectedLen, size_t* actualOut) {
    BitReader br{in, inLen};
    size_t outPos = 0;
    int last = 0;
    // Fixed-code tables (built once per stream — tiny).
    Huffman litFixed, distFixed;
    {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        uint8_t d[30];
        for (int i = 0; i < 30; i++) d[i] = 5;
        if (litFixed.build(l, 288) != 0) return -3;
        if (distFixed.build(d, 30) != 0) return -3;
    }

    auto ensure = [&](size_t need) -> bool {
        if (outPos + need > expectedLen) return false;
        return true;
    };

    while (!last) {
        long bfinal = br.getBits(1);
        if (bfinal < 0) return -1;
        long btype = br.getBits(2);
        if (btype < 0) return -2;
        last = (int)bfinal;

        if (btype == 0) {
            // Stored block: byte-aligned LEN/NLEN.
            br.alignByte();
            long len = br.getBits(16);
            long nlen = br.getBits(16);
            if (len < 0 || nlen < 0) return -1;
            if ((len ^ nlen) != 0xFFFF) return -2;
            if (!ensure((size_t)len)) return -4;
            for (long i = 0; i < len; i++) {
                long b = br.getBits(8);
                if (b < 0) return -1;
                out[outPos++] = (uint8_t)b;
            }
        } else if (btype == 1) {
            // Fixed Huffman.
            const Huffman* lit = &litFixed;
            const Huffman* dist = &distFixed;
            for (;;) {
                int sym = lit->decode(br);
                if (sym < 0) return -3;
                if (sym < 256) {
                    if (!ensure(1)) return -4;
                    out[outPos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li >= 29) return -3;
                    long extra = br.getBits(kLenExtra[li]);
                    if (extra < 0) return -1;
                    size_t length = (size_t)kLenBase[li] + (size_t)extra;
                    int dsym = dist->decode(br);
                    if (dsym < 0 || dsym > 29) return -3;
                    long dextra = br.getBits(kDistExtra[dsym]);
                    if (dextra < 0) return -1;
                    size_t dist = (size_t)kDistBase[dsym] + (size_t)dextra;
                    if (dist > outPos) return -3;  // distance before start of output
                    if (!ensure(length)) return -4;
                    // Copy (may overlap when dist < length).
                    size_t src = outPos - dist;
                    for (size_t i = 0; i < length; i++) out[outPos++] = out[src + i];
                }
            }
        } else if (btype == 2) {
            // Dynamic Huffman.
            long hlit = br.getBits(5);
            long hdist = br.getBits(5);
            long hclen = br.getBits(4);
            if (hlit < 0 || hdist < 0 || hclen < 0) return -1;
            int nlit = (int)hlit + 257;
            int ndist = (int)hdist + 1;
            int nclen = (int)hclen + 4;
            uint8_t clLens[19] = {0};
            for (int i = 0; i < nclen; i++) {
                long v = br.getBits(3);
                if (v < 0) return -1;
                clLens[kClOrder[i]] = (uint8_t)v;
            }
            Huffman cl;
            if (cl.build(clLens, 19) != 0) return -3;
            // Decode the combined literal+length and distance code lengths.
            uint8_t lens[kMaxCodes];
            memset(lens, 0, sizeof(lens));
            int total = nlit + ndist;
            int i = 0;
            while (i < total) {
                int sym = cl.decode(br);
                if (sym < 0) return -3;
                if (sym < 16) {
                    lens[i++] = (uint8_t)sym;
                } else {
                    long repeat = 0;
                    long extra = 0;
                    if (sym == 16) {
                        extra = br.getBits(2);
                        if (extra < 0) return -1;
                        if (i == 0) return -3;  // repeat of previous with none
                        repeat = 3 + extra;
                        if (i + (int)repeat > total) return -3;
                        uint8_t v = lens[i - 1];
                        for (int r = 0; r < repeat; r++) lens[i++] = v;
                    } else if (sym == 17) {
                        extra = br.getBits(3);
                        if (extra < 0) return -1;
                        repeat = 3 + extra;
                        if (i + (int)repeat > total) return -3;
                        i += (int)repeat;  // zeros
                    } else {  // 18
                        extra = br.getBits(7);
                        if (extra < 0) return -1;
                        repeat = 11 + extra;
                        if (i + (int)repeat > total) return -3;
                        i += (int)repeat;
                    }
                }
            }
            Huffman lit, dist;
            if (lit.build(lens, nlit) != 0) return -3;
            if (dist.build(lens + nlit, ndist) != 0) return -3;
            if (lit.maxLen == 0) return -3;
            for (;;) {
                int sym = lit.decode(br);
                if (sym < 0) return -3;
                if (sym < 256) {
                    if (!ensure(1)) return -4;
                    out[outPos++] = (uint8_t)sym;
                } else if (sym == 256) {
                    break;
                } else {
                    int li = sym - 257;
                    if (li >= 29) return -3;
                    long extra = br.getBits(kLenExtra[li]);
                    if (extra < 0) return -1;
                    size_t length = (size_t)kLenBase[li] + (size_t)extra;
                    int dsym = dist.decode(br);
                    if (dsym < 0 || dsym > 29) return -3;
                    long dextra = br.getBits(kDistExtra[dsym]);
                    if (dextra < 0) return -1;
                    size_t distv = (size_t)kDistBase[dsym] + (size_t)dextra;
                    if (distv > outPos) return -3;
                    if (!ensure(length)) return -4;
                    size_t src = outPos - distv;
                    for (size_t i = 0; i < length; i++) out[outPos++] = out[src + i];
                }
            }
        } else {
            return -2;  // reserved block type
        }
    }
    if (actualOut) *actualOut = outPos;
    return 0;
}
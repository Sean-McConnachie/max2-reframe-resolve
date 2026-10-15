// Deflate decoder (RFC 1951), after Mark Adler's "puff". Small and slow, which is fine for timeline files.
#include "inflate.h"

#include <cstring>

namespace {

struct State
{
    const uint8_t* in;
    size_t inLen;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCnt = 0;
    std::vector<uint8_t>* out;
    bool err = false;

    int bits(int need)
    {
        uint32_t v = bitBuf;
        while (bitCnt < need)
        {
            if (pos >= inLen)
            {
                err = true;
                return 0;
            }
            v |= uint32_t(in[pos++]) << bitCnt;
            bitCnt += 8;
        }
        bitBuf = v >> need;
        bitCnt -= need;
        return int(v & ((1u << need) - 1));
    }
};

struct Huffman
{
    short count[16];
    short symbol[288];
};

constexpr int kMaxBits = 15;

// Returns 0 for a complete code, > 0 for an incomplete one, < 0 for an over-subscribed one.
int construct(Huffman& h, const short* length, int n)
{
    memset(h.count, 0, sizeof(h.count));
    for (int s = 0; s < n; ++s) h.count[length[s]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= kMaxBits; ++len)
    {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[kMaxBits + 1];
    offs[1] = 0;
    for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = short(offs[len] + h.count[len]);
    for (int s = 0; s < n; ++s)
        if (length[s] != 0) h.symbol[offs[length[s]]++] = short(s);
    return left;
}

int decode(State& s, const Huffman& h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= kMaxBits; ++len)
    {
        code |= s.bits(1);
        if (s.err) return -1;
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073,
                             4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool codes(State& s, const Huffman& lencode, const Huffman& distcode)
{
    for (;;)
    {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 256)
        {
            s.out->push_back(uint8_t(symbol));
            continue;
        }
        if (symbol == 256) return true;
        symbol -= 257;
        if (symbol >= 29) return false;
        int len = kLenBase[symbol] + s.bits(kLenExtra[symbol]);
        symbol = decode(s, distcode);
        if (symbol < 0 || symbol >= 30) return false;
        size_t dist = size_t(kDistBase[symbol]) + s.bits(kDistExtra[symbol]);
        if (s.err || dist > s.out->size()) return false;
        size_t from = s.out->size() - dist;
        for (int i = 0; i < len; ++i) s.out->push_back((*s.out)[from + i]);
    }
}

bool stored(State& s)
{
    s.bitBuf = 0;
    s.bitCnt = 0;
    if (s.pos + 4 > s.inLen) return false;
    unsigned len = s.in[s.pos] | (s.in[s.pos + 1] << 8);
    unsigned nlen = s.in[s.pos + 2] | (s.in[s.pos + 3] << 8);
    s.pos += 4;
    if (len != (~nlen & 0xffff) || s.pos + len > s.inLen) return false;
    s.out->insert(s.out->end(), s.in + s.pos, s.in + s.pos + len);
    s.pos += len;
    return true;
}

bool fixedBlock(State& s)
{
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built)
    {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < 288; ++sym) lengths[sym] = 8;
        construct(lencode, lengths, 288);
        for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
        construct(distcode, lengths, 30);
        built = true;
    }
    return codes(s, lencode, distcode);
}

bool dynamicBlock(State& s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    int nlen = s.bits(5) + 257;
    int ndist = s.bits(5) + 1;
    int ncode = s.bits(4) + 4;
    if (s.err || nlen > 286 || ndist > 30) return false;
    int index = 0;
    for (; index < ncode; ++index) lengths[order[index]] = short(s.bits(3));
    for (; index < 19; ++index) lengths[order[index]] = 0;
    Huffman lencode, distcode;
    if (construct(lencode, lengths, 19) != 0) return false;
    index = 0;
    while (index < nlen + ndist)
    {
        int symbol = decode(s, lencode);
        if (symbol < 0) return false;
        if (symbol < 16)
        {
            lengths[index++] = short(symbol);
            continue;
        }
        short len = 0;
        int rep;
        if (symbol == 16)
        {
            if (index == 0) return false;
            len = lengths[index - 1];
            rep = 3 + s.bits(2);
        }
        else if (symbol == 17) rep = 3 + s.bits(3);
        else rep = 11 + s.bits(7);
        if (index + rep > nlen + ndist) return false;
        while (rep--) lengths[index++] = len;
    }
    if (lengths[256] == 0) return false;
    int err = construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return false;
    err = construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) return false;
    return codes(s, lencode, distcode);
}

} // namespace

bool inflateRaw(const uint8_t* data, size_t size, std::vector<uint8_t>& out, size_t* used)
{
    State s;
    s.in = data;
    s.inLen = size;
    s.out = &out;
    int last;
    do
    {
        last = s.bits(1);
        int type = s.bits(2);
        if (s.err) return false;
        bool ok = type == 0 ? stored(s) : type == 1 ? fixedBlock(s) : type == 2 ? dynamicBlock(s) : false;
        if (!ok || s.err) return false;
    } while (!last);
    if (used) *used = s.pos;
    return true;
}

bool inflateZlib(const uint8_t* data, size_t size, std::vector<uint8_t>& out)
{
    if (size < 6 || (data[0] & 0x0f) != 8 || ((data[0] << 8) | data[1]) % 31 != 0 || (data[1] & 0x20)) return false;
    size_t used = 0;
    if (!inflateRaw(data + 2, size - 2, out, &used)) return false;
    if (2 + used + 4 > size) return false;
    const uint8_t* a = data + 2 + used;
    uint32_t want = uint32_t(a[0]) << 24 | uint32_t(a[1]) << 16 | uint32_t(a[2]) << 8 | a[3];
    uint32_t s1 = 1, s2 = 0;
    for (uint8_t b : out)
    {
        s1 = (s1 + b) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    return ((s2 << 16) | s1) == want;
}

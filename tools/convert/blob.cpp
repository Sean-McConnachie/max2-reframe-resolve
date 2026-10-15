#include "blob.h"

#include <cstring>

#include "../../third_party/zstd/zstd.h"
#include "util.h"

bool decodeResolveBlob(const std::string& hex, std::vector<uint8_t>& data, std::string* err)
{
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    data.clear();
    if (hex.empty()) return true;
    std::vector<uint8_t> b;
    if (!fromHex(hex, b) || b.size() < 9 || be32(&b[0]) != 2) return fail("unknown effect data format");
    uint32_t len = be32(&b[4]);
    if (len + 8 != b.size()) return fail("the effect data has a bad length");
    if (b[8] == 0x80)
    {
        data.assign(b.begin() + 9, b.end());
        return true;
    }
    if (b[8] != 0x81) return fail("unknown effect data compression");
    unsigned long long n = ZSTD_getFrameContentSize(b.data() + 9, b.size() - 9);
    if (n == ZSTD_CONTENTSIZE_ERROR || n == ZSTD_CONTENTSIZE_UNKNOWN || n > (64u << 20)) return fail("the effect data cannot be decompressed");
    data.resize(size_t(n));
    size_t r = ZSTD_decompress(data.data(), data.size(), b.data() + 9, b.size() - 9);
    if (ZSTD_isError(r) || r != n) return fail("the effect data cannot be decompressed");
    return true;
}

std::string encodeResolveBlob(const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> b{0, 0, 0, 2};
    uint32_t len = uint32_t(data.size() + 1);
    for (int s = 24; s >= 0; s -= 8) b.push_back(uint8_t(len >> s));
    b.push_back(0x80);
    b.insert(b.end(), data.begin(), data.end());
    return toHex(b);
}

bool parsePb(const uint8_t* d, size_t n, std::vector<PbField>& out)
{
    size_t i = 0;
    auto varint = [&](uint64_t& v) {
        v = 0;
        for (int s = 0; s < 64; s += 7)
        {
            if (i >= n) return false;
            uint8_t c = d[i++];
            v |= uint64_t(c & 0x7f) << s;
            if (!(c & 0x80)) return true;
        }
        return false;
    };
    out.clear();
    while (i < n)
    {
        uint64_t k;
        if (!varint(k)) return false;
        PbField f;
        f.num = uint32_t(k >> 3);
        f.wire = int(k & 7);
        if (f.wire == 0)
        {
            if (!varint(f.value)) return false;
        }
        else if (f.wire == 1 || f.wire == 5)
        {
            size_t sz = f.wire == 1 ? 8 : 4;
            if (i + sz > n) return false;
            for (size_t b = 0; b < sz; ++b) f.value |= uint64_t(d[i + b]) << (8 * b);
            i += sz;
        }
        else if (f.wire == 2)
        {
            uint64_t len;
            if (!varint(len) || len > n - i) return false;
            f.bytes.assign(d + i, d + i + len);
            i += size_t(len);
        }
        else return false;
        out.push_back(std::move(f));
    }
    return true;
}

void PbWriter::raw(uint64_t v)
{
    while (v >= 0x80)
    {
        m_Data.push_back(uint8_t(v | 0x80));
        v >>= 7;
    }
    m_Data.push_back(uint8_t(v));
}

PbWriter& PbWriter::varint(uint32_t num, uint64_t v)
{
    key(num, 0);
    raw(v);
    return *this;
}

PbWriter& PbWriter::dbl(uint32_t num, double v)
{
    key(num, 1);
    uint64_t u;
    memcpy(&u, &v, 8);
    for (int b = 0; b < 8; ++b) m_Data.push_back(uint8_t(u >> (8 * b)));
    return *this;
}

PbWriter& PbWriter::bytes(uint32_t num, const std::vector<uint8_t>& v)
{
    key(num, 2);
    raw(v.size());
    m_Data.insert(m_Data.end(), v.begin(), v.end());
    return *this;
}

PbWriter& PbWriter::str(uint32_t num, const std::string& v)
{
    return bytes(num, std::vector<uint8_t>(v.begin(), v.end()));
}

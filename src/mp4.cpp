#include "mp4.h"

#include <cstdio>
#include <memory>
#include <tuple>

namespace {

struct FileCloser { void operator()(FILE* f) const { if (f) fclose(f); } };
using FilePtr = std::unique_ptr<FILE, FileCloser>;

uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint64_t be64(const uint8_t* p) { return (uint64_t(be32(p)) << 32) | be32(p + 4); }

struct Box
{
    std::string type;
    const uint8_t* data;  // payload
    size_t size;          // payload size
};

// Iterate child boxes inside a buffer.
template <class F>
void forBoxes(const uint8_t* p, size_t n, F f)
{
    size_t off = 0;
    while (off + 8 <= n)
    {
        uint64_t size = be32(p + off);
        std::string type(reinterpret_cast<const char*>(p + off + 4), 4);
        size_t hdr = 8;
        if (size == 1)
        {
            if (off + 16 > n) return;
            size = be64(p + off + 8);
            hdr = 16;
        }
        else if (size == 0)
        {
            size = n - off;
        }
        if (size < hdr || off + size > n) return;
        f(Box{type, p + off + hdr, size_t(size - hdr)});
        off += size_t(size);
    }
}

struct SampleTables
{
    std::vector<std::pair<uint32_t, uint32_t>> stts;           // count, delta
    std::vector<std::tuple<uint32_t, uint32_t>> stsc;          // first chunk, samples per chunk
    std::vector<uint64_t> chunkOffsets;
    uint32_t fixedSize = 0;
    std::vector<uint32_t> sizes;
    uint32_t sampleCount = 0;
};

void parseStbl(const Box& stbl, Mp4Track& t)
{
    SampleTables st;
    forBoxes(stbl.data, stbl.size, [&](const Box& b) {
        const uint8_t* p = b.data;
        if (b.type == "stsd" && b.size >= 16)
        {
            // first entry
            const uint8_t* e = p + 8;
            uint32_t esize = be32(e);
            t.codec.assign(reinterpret_cast<const char*>(e + 4), 4);
            if (t.codec == "tmcd" && esize >= 8 + 26)
            {
                // reserved(6) dref(2) reserved(4) flags(4) timescale(4) frameDuration(4) numFrames(1)
                const uint8_t* q = e + 8;
                t.tmcdTimescale = be32(q + 16);
                t.tmcdFrameDuration = be32(q + 20);
                t.tmcdFrames = q[24];
            }
            else if ((t.codec == "hvc1" || t.codec == "hev1") && esize > 86 && 8 + size_t(esize) <= b.size)
            {
                // the visual sample entry is 86 bytes; hvcC keeps bitDepthLumaMinus8 in byte 17
                forBoxes(e + 86, esize - 86, [&](const Box& c) {
                    if (c.type == "hvcC" && c.size >= 18) t.bitDepth = 8 + (c.data[17] & 7);
                });
            }
        }
        else if (b.type == "stts" && b.size >= 8)
        {
            uint32_t n = be32(p + 4);
            for (uint32_t i = 0; i < n && 8 + i * 8 + 8 <= b.size; ++i)
                st.stts.emplace_back(be32(p + 8 + i * 8), be32(p + 12 + i * 8));
        }
        else if (b.type == "stsc" && b.size >= 8)
        {
            uint32_t n = be32(p + 4);
            for (uint32_t i = 0; i < n && 8 + i * 12 + 12 <= b.size; ++i)
                st.stsc.emplace_back(be32(p + 8 + i * 12), be32(p + 12 + i * 12));
        }
        else if (b.type == "stsz" && b.size >= 12)
        {
            st.fixedSize = be32(p + 4);
            st.sampleCount = be32(p + 8);
            if (st.fixedSize == 0)
                for (uint32_t i = 0; i < st.sampleCount && 12 + i * 4 + 4 <= b.size; ++i)
                    st.sizes.push_back(be32(p + 12 + i * 4));
        }
        else if (b.type == "stco" && b.size >= 8)
        {
            uint32_t n = be32(p + 4);
            for (uint32_t i = 0; i < n && 8 + i * 4 + 4 <= b.size; ++i)
                st.chunkOffsets.push_back(be32(p + 8 + i * 4));
        }
        else if (b.type == "co64" && b.size >= 8)
        {
            uint32_t n = be32(p + 4);
            for (uint32_t i = 0; i < n && 8 + i * 8 + 8 <= b.size; ++i)
                st.chunkOffsets.push_back(be64(p + 8 + i * 8));
        }
        else if (b.type == "stss" && b.size >= 8)
        {
            uint32_t n = be32(p + 4);
            for (uint32_t i = 0; i < n && 8 + i * 4 + 4 <= b.size; ++i)
                t.sync.push_back(be32(p + 8 + i * 4));
        }
    });

    uint32_t n = st.sampleCount;
    t.sizes.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        t.sizes[i] = st.fixedSize ? st.fixedSize : (i < st.sizes.size() ? st.sizes[i] : 0);

    // sample offsets from stsc + chunk offsets
    t.offsets.resize(n);
    uint32_t sample = 0;
    for (size_t c = 0; c < st.chunkOffsets.size() && sample < n; ++c)
    {
        uint32_t chunkNo = uint32_t(c + 1);
        uint32_t perChunk = 0;
        for (auto& e : st.stsc)
            if (std::get<0>(e) <= chunkNo) perChunk = std::get<1>(e);
        uint64_t off = st.chunkOffsets[c];
        for (uint32_t k = 0; k < perChunk && sample < n; ++k)
        {
            t.offsets[sample] = off;
            off += t.sizes[sample];
            ++sample;
        }
    }

    t.times.resize(n);
    int64_t tm = 0;
    uint32_t s = 0;
    for (auto& e : st.stts)
        for (uint32_t k = 0; k < e.first && s < n; ++k)
        {
            t.times[s++] = tm;
            tm += e.second;
        }
}

void parseTrak(const Box& trak, Mp4Track& t)
{
    forBoxes(trak.data, trak.size, [&](const Box& b) {
        if (b.type == "tkhd" && b.size >= 84)
        {
            int v = b.data[0];
            t.id = be32(b.data + (v == 1 ? 20 : 12));
            const uint8_t* wh = b.data + b.size - 8;
            t.width = int(be32(wh) >> 16);
            t.height = int(be32(wh + 4) >> 16);
        }
        else if (b.type == "mdia")
        {
            forBoxes(b.data, b.size, [&](const Box& m) {
                if (m.type == "mdhd" && m.size >= 24)
                {
                    int v = m.data[0];
                    if (v == 1)
                    {
                        t.timescale = be32(m.data + 20);
                        t.duration = be64(m.data + 24);
                    }
                    else
                    {
                        t.timescale = be32(m.data + 12);
                        t.duration = be32(m.data + 16);
                    }
                }
                else if (m.type == "hdlr" && m.size >= 12)
                {
                    t.handler.assign(reinterpret_cast<const char*>(m.data + 8), 4);
                }
                else if (m.type == "minf")
                {
                    forBoxes(m.data, m.size, [&](const Box& mi) {
                        if (mi.type == "stbl") parseStbl(mi, t);
                    });
                }
            });
        }
    });
}

} // namespace

bool Mp4File::open(const std::wstring& path, std::string* err)
{
    m_Path = path;
    m_Tracks.clear();
    FilePtr f(_wfopen(path.c_str(), L"rb"));
    if (!f)
    {
        if (err) *err = "cannot open file";
        return false;
    }
    // walk top-level boxes to find moov
    uint64_t off = 0;
    std::vector<uint8_t> moov;
    for (;;)
    {
        uint8_t hdr[16];
        if (_fseeki64(f.get(), int64_t(off), SEEK_SET) != 0 || fread(hdr, 1, 8, f.get()) != 8) break;
        uint64_t size = be32(hdr);
        std::string type(reinterpret_cast<char*>(hdr + 4), 4);
        uint64_t hlen = 8;
        if (size == 1)
        {
            if (fread(hdr + 8, 1, 8, f.get()) != 8) break;
            size = be64(hdr + 8);
            hlen = 16;
        }
        else if (size == 0)
        {
            _fseeki64(f.get(), 0, SEEK_END);
            size = uint64_t(_ftelli64(f.get())) - off;
        }
        if (size < hlen) break;
        if (type == "moov")
        {
            moov.resize(size_t(size - hlen));
            _fseeki64(f.get(), int64_t(off + hlen), SEEK_SET);
            if (fread(moov.data(), 1, moov.size(), f.get()) != moov.size())
            {
                if (err) *err = "truncated moov";
                return false;
            }
            break;
        }
        off += size;
    }
    if (moov.empty())
    {
        if (err) *err = "no moov box (not an MP4/MOV file?)";
        return false;
    }
    forBoxes(moov.data(), moov.size(), [&](const Box& b) {
        if (b.type == "trak")
        {
            Mp4Track t;
            parseTrak(b, t);
            m_Tracks.push_back(std::move(t));
        }
    });
    return true;
}

bool Mp4File::readSample(const Mp4Track& t, size_t i, std::vector<uint8_t>& out) const
{
    if (i >= t.sizes.size()) return false;
    FilePtr f(_wfopen(m_Path.c_str(), L"rb"));
    if (!f) return false;
    out.resize(t.sizes[i]);
    if (_fseeki64(f.get(), int64_t(t.offsets[i]), SEEK_SET) != 0) return false;
    return fread(out.data(), 1, out.size(), f.get()) == out.size();
}

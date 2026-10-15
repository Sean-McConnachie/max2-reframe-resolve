#include "zip.h"

#include <windows.h>

#include <cstdio>
#include <ctime>

#include "inflate.h"

namespace {

uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void wr16(std::vector<uint8_t>& v, uint32_t x) { v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void wr32(std::vector<uint8_t>& v, uint32_t x) { wr16(v, x & 0xffff); wr16(v, x >> 16); }

bool readFile(const std::wstring& path, std::vector<uint8_t>& out)
{
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    _fseeki64(f, 0, SEEK_END);
    long long n = _ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    out.resize(size_t(n));
    bool ok = n == 0 || fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

} // namespace

uint32_t crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256];
    static bool init = false;
    if (!init)
    {
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < size; ++i) c = table[(c ^ data[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

bool readZip(const std::wstring& path, std::vector<ZipEntry>& entries, std::string* err)
{
    auto fail = [&](const char* m) {
        if (err) *err = m;
        return false;
    };
    std::vector<uint8_t> f;
    if (!readFile(path, f)) return fail("cannot read the file");
    if (f.size() < 22) return fail("the file is not a zip archive");
    size_t eocd = std::string::npos;
    for (size_t i = f.size() - 22 + 1; i-- > 0 && f.size() - i < 22 + 65536;)
        if (rd32(&f[i]) == 0x06054b50)
        {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos) return fail("the file is not a zip archive");
    size_t count = rd16(&f[eocd + 10]);
    size_t cd = rd32(&f[eocd + 16]);
    entries.clear();
    for (size_t k = 0; k < count; ++k)
    {
        if (cd + 46 > f.size() || rd32(&f[cd]) != 0x02014b50) return fail("the zip directory is damaged");
        uint16_t method = rd16(&f[cd + 10]);
        uint32_t crc = rd32(&f[cd + 16]);
        size_t csize = rd32(&f[cd + 20]), usize = rd32(&f[cd + 24]);
        size_t nlen = rd16(&f[cd + 28]), xlen = rd16(&f[cd + 30]), clen = rd16(&f[cd + 32]);
        size_t lho = rd32(&f[cd + 42]);
        if (cd + 46 + nlen > f.size()) return fail("the zip directory is damaged");
        ZipEntry e;
        e.name.assign(reinterpret_cast<const char*>(&f[cd + 46]), nlen);
        cd += 46 + nlen + xlen + clen;
        if (lho + 30 > f.size() || rd32(&f[lho]) != 0x04034b50) return fail("a zip entry is damaged");
        size_t data = lho + 30 + rd16(&f[lho + 26]) + rd16(&f[lho + 28]);
        if (data + csize > f.size()) return fail("a zip entry is damaged");
        if (method == 0) e.data.assign(f.begin() + data, f.begin() + data + csize);
        else if (method == 8)
        {
            e.data.reserve(usize);
            if (!inflateRaw(&f[data], csize, e.data)) return fail("a zip entry cannot be decompressed");
        }
        else return fail("a zip entry uses an unknown compression method");
        if (e.data.size() != usize || crc32(e.data.data(), e.data.size()) != crc) return fail("a zip entry has a bad checksum");
        entries.push_back(std::move(e));
    }
    return true;
}

bool writeZip(const std::wstring& path, const std::vector<ZipEntry>& entries, std::string* err)
{
    time_t now = time(nullptr);
    tm t;
    localtime_s(&t, &now);
    uint16_t dosTime = uint16_t(t.tm_hour << 11 | t.tm_min << 5 | t.tm_sec / 2);
    uint16_t dosDate = uint16_t((t.tm_year - 80) << 9 | (t.tm_mon + 1) << 5 | t.tm_mday);

    std::vector<uint8_t> out, dir;
    for (const auto& e : entries)
    {
        bool utf8 = false;
        for (unsigned char c : e.name) utf8 |= c >= 0x80;
        uint16_t flags = utf8 ? 0x0800 : 0;
        uint32_t crc = crc32(e.data.data(), e.data.size());
        uint32_t offset = uint32_t(out.size());
        auto header = [&](std::vector<uint8_t>& v, bool central) {
            wr32(v, central ? 0x02014b50 : 0x04034b50);
            if (central) wr16(v, 20);  // made by
            wr16(v, 20);               // needed to extract
            wr16(v, flags);
            wr16(v, 0);  // stored
            wr16(v, dosTime);
            wr16(v, dosDate);
            wr32(v, crc);
            wr32(v, uint32_t(e.data.size()));
            wr32(v, uint32_t(e.data.size()));
            wr16(v, uint32_t(e.name.size()));
            wr16(v, 0);  // extra
            if (central)
            {
                wr16(v, 0);  // comment
                wr16(v, 0);  // disk
                wr16(v, 0);  // internal attributes
                wr32(v, 0);  // external attributes
                wr32(v, offset);
            }
            v.insert(v.end(), e.name.begin(), e.name.end());
        };
        header(out, false);
        out.insert(out.end(), e.data.begin(), e.data.end());
        header(dir, true);
    }
    uint32_t cdOffset = uint32_t(out.size());
    out.insert(out.end(), dir.begin(), dir.end());
    wr32(out, 0x06054b50);
    wr16(out, 0);
    wr16(out, 0);
    wr16(out, uint32_t(entries.size()));
    wr16(out, uint32_t(entries.size()));
    wr32(out, uint32_t(dir.size()));
    wr32(out, cdOffset);
    wr16(out, 0);

    // Write next to the target, then replace it, so a failure never leaves half a file.
    std::wstring tmp = path + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (!f)
    {
        if (err) *err = "cannot write the new file";
        return false;
    }
    bool ok = fwrite(out.data(), 1, out.size(), f) == out.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        DeleteFileW(tmp.c_str());
        if (err) *err = "cannot write the new file";
        return false;
    }
    return true;
}

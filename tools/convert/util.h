// Small helpers shared by the timeline converter.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

inline int hexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool fromHex(const std::string& s, std::vector<uint8_t>& out)
{
    if (s.size() % 2) return false;
    out.resize(s.size() / 2);
    for (size_t i = 0; i < out.size(); ++i)
    {
        int a = hexDigit(s[2 * i]), b = hexDigit(s[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a << 4 | b);
    }
    return true;
}

inline std::string toHex(const std::vector<uint8_t>& v)
{
    static const char* d = "0123456789abcdef";
    std::string s(v.size() * 2, '0');
    for (size_t i = 0; i < v.size(); ++i)
    {
        s[2 * i] = d[v[i] >> 4];
        s[2 * i + 1] = d[v[i] & 15];
    }
    return s;
}

inline uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }

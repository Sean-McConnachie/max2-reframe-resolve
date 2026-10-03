// Minimal MP4/MOV demuxer: just enough to read sample tables and pull metadata samples.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct Mp4Track
{
    uint32_t id = 0;
    std::string handler;      // "vide", "soun", "meta", "tmcd", ...
    std::string codec;        // sample entry fourcc: "hvc1", "gpmd", "tmcd", ...
    uint32_t timescale = 0;
    uint64_t duration = 0;
    int width = 0, height = 0;
    int bitDepth = 8;         // HEVC luma bit depth (from hvcC)
    std::vector<uint64_t> offsets;  // per sample
    std::vector<uint32_t> sizes;    // per sample
    std::vector<int64_t> times;     // per sample decode time, in timescale units
    std::vector<uint32_t> sync;     // 1-based sync sample numbers; empty means every sample is sync
    // tmcd sample entry
    uint32_t tmcdTimescale = 0, tmcdFrameDuration = 0;
    uint8_t tmcdFrames = 0;
};

class Mp4File
{
public:
    bool open(const std::wstring& path, std::string* err = nullptr);
    const std::vector<Mp4Track>& tracks() const { return m_Tracks; }
    // Read raw bytes of sample i of a track.
    bool readSample(const Mp4Track& t, size_t i, std::vector<uint8_t>& out) const;
    const std::wstring& path() const { return m_Path; }

private:
    std::wstring m_Path;
    std::vector<Mp4Track> m_Tracks;
};

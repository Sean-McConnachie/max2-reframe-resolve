// Hardware (DXVA via Media Foundation) HEVC decoding of one video track of a file, with a small
// frame cache and read-ahead on a worker thread.
#pragma once

#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

struct Nv12Frame
{
    int index = -1;
    int width = 0, height = 0;
    int bytesPerSample = 1;      // 1: NV12 (8-bit). 2: P010 (10-bit, in the high bits of little-endian 16-bit words)
    int pitch = 0;               // bytes per row, both planes
    std::vector<uint8_t> data;   // Y plane (height rows) followed by interleaved UV plane (height/2 rows)
    const uint8_t* y() const { return data.data(); }
    const uint8_t* uv() const { return data.data() + size_t(pitch) * height; }
};

class StreamDecoder
{
public:
    // trackId / firstSampleSize identify the MP4 video track; videoOrdinal (0 = first video stream Media
    // Foundation reports) is the fallback. A bitDepth above 8 asks the decoder for P010 instead of NV12.
    StreamDecoder(const std::wstring& path, uint32_t trackId, uint32_t firstSampleSize, int videoOrdinal, int fpsNum,
                  int fpsDen, int frameCount, int bitDepth);
    ~StreamDecoder();

    // Request a frame and start decoding it (non-blocking).
    void request(int frame);
    // Blocking fetch. Returns null on failure (see error()).
    std::shared_ptr<const Nv12Frame> get(int frame);
    std::string error();
    bool hardware() const { return m_Hardware; }

private:
    void run();
    bool openReader();
    bool findStream(unsigned long& stream);
    bool seek(int frame);
    std::shared_ptr<Nv12Frame> readFrame(bool& eos);

    std::wstring m_Path;
    uint32_t m_TrackId;
    uint32_t m_FirstSampleSize;
    int m_Ordinal;
    int m_FpsNum, m_FpsDen, m_Frames;
    int m_BitDepth;

    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::multiset<int> m_Wanted;
    int m_LastWanted = -1;
    std::map<int, std::shared_ptr<const Nv12Frame>> m_Cache;
    std::string m_Error;
    bool m_Failed = false;
    bool m_Stop = false;
    bool m_Hardware = false;

    // worker-thread state
    struct Impl;
    std::unique_ptr<Impl> m_Impl;
    int m_Next = 0;  // index of the frame the reader will return next (-1 unknown)
    std::thread m_Thread;
};

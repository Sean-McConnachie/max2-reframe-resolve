#include <cuda_runtime.h>

#include <map>
#include <mutex>

#include "gpu.h"
#include "reframe_kernel.h"

namespace {

__global__ void ReframeKernel(RfParams p, const unsigned char* Y0, const unsigned char* Y1, float* out)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= p.outW || y >= p.outH) return;
    float px[4];
    rf_shade(&p, Y0, Y0 + p.uvOffset, Y1, Y1 + p.uvOffset, x, p.outH - 1 - y, px);
    float* o = out + (size_t)y * p.outStride + (size_t)x * 4;
    o[0] = px[0]; o[1] = px[1]; o[2] = px[2]; o[3] = px[3];
}

struct DeviceState
{
    unsigned char* buf[2] = {nullptr, nullptr};
    size_t size[2] = {0, 0};
    std::shared_ptr<const Nv12Frame> last[2];  // frames currently on the device (kept alive so pointers stay unique)
};

std::mutex g_Mutex;
std::map<int, DeviceState> g_Devices;

bool check(cudaError_t e, const char* what, std::string* err)
{
    if (e == cudaSuccess) return true;
    if (err) *err = std::string(what) + ": " + cudaGetErrorString(e);
    return false;
}

} // namespace

bool cudaRender(void* streamPtr, const RfParams& p, const std::shared_ptr<const Nv12Frame>& s0,
                const std::shared_ptr<const Nv12Frame>& s1, float* dst, std::string* err)
{
    cudaStream_t stream = static_cast<cudaStream_t>(streamPtr);
    int dev = 0;
    if (!check(cudaGetDevice(&dev), "cudaGetDevice", err)) return false;

    std::lock_guard<std::mutex> lock(g_Mutex);
    DeviceState& st = g_Devices[dev];
    const std::shared_ptr<const Nv12Frame>* frames[2] = {&s0, &s1};
    for (int i = 0; i < 2; ++i)
    {
        const Nv12Frame& f = **frames[i];
        size_t bytes = f.data.size();
        if (st.size[i] < bytes)
        {
            if (st.buf[i]) cudaFree(st.buf[i]);
            st.buf[i] = nullptr;
            st.size[i] = 0;
            st.last[i].reset();
            if (!check(cudaMalloc(&st.buf[i], bytes), "cudaMalloc", err)) return false;
            st.size[i] = bytes;
        }
        if (st.last[i] != *frames[i])
        {
            if (!check(cudaMemcpyAsync(st.buf[i], f.data.data(), bytes, cudaMemcpyHostToDevice, stream), "upload", err))
                return false;
            st.last[i] = *frames[i];
        }
    }
    dim3 threads(16, 16, 1);
    dim3 blocks((p.outW + 15) / 16, (p.outH + 15) / 16, 1);
    ReframeKernel<<<blocks, threads, 0, stream>>>(p, st.buf[0], st.buf[1], dst);
    if (!check(cudaGetLastError(), "kernel launch", err)) return false;
    // The device buffers are shared between renders that may use different streams: finish before unlocking.
    return check(cudaStreamSynchronize(stream), "kernel", err);
}

bool cudaCopy(void* streamPtr, void* dst, const void* src, size_t bytes, bool fromHost, std::string* err)
{
    cudaStream_t stream = static_cast<cudaStream_t>(streamPtr);
    if (!check(cudaMemcpyAsync(dst, src, bytes, fromHost ? cudaMemcpyHostToDevice : cudaMemcpyDeviceToDevice, stream), "copy", err))
        return false;
    return check(cudaStreamSynchronize(stream), "copy", err);
}

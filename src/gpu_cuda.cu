#include <cuda_runtime.h>

#include <map>
#include <mutex>

#include "gpu.h"
#include "reframe_kernel.h"

namespace {

// steps: one parameter set per motion blur time sample (n >= 1); the result is their average.
__global__ void ReframeKernel(const RfParams* steps, int n, const unsigned char* Y0, const unsigned char* Y1, float* out)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    const int outW = steps[0].outW, outH = steps[0].outH, uvOff = steps[0].uvOffset;
    if (x >= outW || y >= outH) return;
    float acc0 = 0.0f, acc1 = 0.0f, acc2 = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        RfParams q = steps[i];
        float px[4];
        rf_shade(&q, Y0, Y0 + uvOff, Y1, Y1 + uvOff, x, outH - 1 - y, px);
        acc0 += px[0]; acc1 += px[1]; acc2 += px[2];
    }
    const float inv = 1.0f / (float)n;
    float* o = out + (size_t)y * steps[0].outStride + (size_t)x * 4;
    o[0] = acc0 * inv; o[1] = acc1 * inv; o[2] = acc2 * inv; o[3] = 1.0f;
}

struct DeviceState
{
    unsigned char* buf[2] = {nullptr, nullptr};
    size_t size[2] = {0, 0};
    RfParams* steps = nullptr;
    int stepCap = 0;
    uint64_t last[2] = {0, 0};  // serials of the frames currently on the device
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

bool cudaRender(void* streamPtr, const RfParams* p, int n, const std::shared_ptr<const Nv12Frame>& s0,
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
            st.last[i] = 0;
            if (!check(cudaMalloc(&st.buf[i], bytes), "cudaMalloc", err)) return false;
            st.size[i] = bytes;
        }
        if (st.last[i] != f.serial)
        {
            if (!check(cudaMemcpyAsync(st.buf[i], f.data.data(), bytes, cudaMemcpyHostToDevice, stream), "upload", err))
                return false;
            st.last[i] = f.serial;
        }
    }
    if (st.stepCap < n)
    {
        if (st.steps) cudaFree(st.steps);
        st.steps = nullptr;
        st.stepCap = 0;
        if (!check(cudaMalloc(&st.steps, sizeof(RfParams) * n), "cudaMalloc", err)) return false;
        st.stepCap = n;
    }
    if (!check(cudaMemcpyAsync(st.steps, p, sizeof(RfParams) * n, cudaMemcpyHostToDevice, stream), "upload params", err))
        return false;
    dim3 threads(16, 16, 1);
    dim3 blocks((p[0].outW + 15) / 16, (p[0].outH + 15) / 16, 1);
    ReframeKernel<<<blocks, threads, 0, stream>>>(st.steps, n, st.buf[0], st.buf[1], dst);
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

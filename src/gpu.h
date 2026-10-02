// GPU render paths. Both render into buffers owned by the host (Resolve) on the host's stream / queue.
// The two NV12 source frames are uploaded only when they change (e.g. not while tweaking parameters).
#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "decoder.h"
#include "reframe_kernel.h"

// CUDA: stream is a cudaStream_t, dst a device pointer.
bool cudaRender(void* stream, const RfParams& p, const std::shared_ptr<const Nv12Frame>& s0,
                const std::shared_ptr<const Nv12Frame>& s1, float* dst, std::string* err);
bool cudaCopy(void* stream, void* dst, const void* src, size_t bytes, bool fromHost, std::string* err);

// OpenCL: queue is a cl_command_queue, dst a cl_mem.
bool openclRender(void* queue, const RfParams& p, const std::shared_ptr<const Nv12Frame>& s0,
                  const std::shared_ptr<const Nv12Frame>& s1, void* dst, std::string* err);
bool openclCopy(void* queue, void* dst, const void* src, size_t bytes, bool fromHost, std::string* err);

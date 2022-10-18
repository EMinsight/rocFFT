// Copyright (C) 2021 - 2022 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#ifndef ROCFFT_RTC_H
#define ROCFFT_RTC_H

#include "rocfft.h"
#include <hip/hip_runtime_api.h>

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "rtc_generator.h"

struct DeviceCallIn;
class TreeNode;
struct GridParam;

// Helper class that handles alignment of kernel arguments
class RTCKernelArgs
{
public:
    RTCKernelArgs() = default;
    void append_ptr(void* ptr)
    {
        append(&ptr, sizeof(void*));
    }
    void append_size_t(size_t s)
    {
        append(&s, sizeof(size_t));
    }
    void append_unsigned_int(unsigned int i)
    {
        append(&i, sizeof(unsigned int));
    }
    void append_double(double d)
    {
        append(&d, sizeof(double));
    }
    void append_float(float f)
    {
        append(&f, sizeof(float));
    }

    size_t size_bytes() const
    {
        return buf.size();
    }
    void* data()
    {
        return buf.data();
    }

private:
    void append(void* src, size_t nbytes)
    {
        // values need to be aligned to their width (i.e. 8-byte values
        // need 8-byte alignment, 4-byte needs 4-byte alignment)
        size_t oldsize = buf.size();
        size_t padding = oldsize % nbytes ? nbytes - (oldsize % nbytes) : 0;
        buf.resize(oldsize + padding + nbytes);
        std::copy_n(static_cast<const char*>(src), nbytes, buf.begin() + oldsize + padding);
    }
    std::vector<char> buf;
};

// Base class for a runtime compiled kernel.  Subclassed for
// different kernel types that each have their own details about how
// to be launched.
struct RTCKernel
{
    // try to compile kernel for node, and attach compiled kernel to
    // node if successful.  returns nullptr if there is no matching
    // supported scheme + problem size.  throws runtime_error on
    // error.
    static std::shared_future<std::unique_ptr<RTCKernel>> runtime_compile(
        const TreeNode& node, const std::string& gpu_arch, bool enable_callbacks = false);

    virtual ~RTCKernel()
    {
        kernel = nullptr;
        (void)hipModuleUnload(module);
        module = nullptr;
    }

    // disallow copies, since we expect this to be managed by smart ptr
    RTCKernel(const RTCKernel&) = delete;
    RTCKernel(RTCKernel&&)      = delete;
    void operator=(const RTCKernel&) = delete;

    // normal launch from within rocFFT execution plan
    void launch(DeviceCallIn& data);
    // direct launch with kernel args
    void launch(RTCKernelArgs& kargs,
                dim3           gridDim,
                dim3           blockDim,
                unsigned int   lds_bytes,
                hipStream_t    stream = nullptr);

    // Subclasses implement this - each kernel type has different
    // parameters
    virtual RTCKernelArgs get_launch_args(DeviceCallIn& data) = 0;

    // function to construct the correct RTCKernel object, given a kernel name and its compiled code
    using rtckernel_construct_t = std::function<std::unique_ptr<RTCKernel>(
        const std::string&, const std::vector<char>&, dim3, dim3)>;

    // grid parameters for this kernel.  may be set by runtime
    // compilation, if compilation of this kernel type knows how to.
    // Otherwise, TreeNode::SetupGPAndFnPtr_internal will do it
    // later.
    dim3 gridDim;
    dim3 blockDim;

protected:
    // protected ctor, use "runtime_compile" to build kernel for a node
    RTCKernel(const std::string&       kernel_name,
              const std::vector<char>& code,
              dim3                     gridDim  = {},
              dim3                     blockDim = {});

    struct RTCGenerator
    {
        kernel_name_gen_t     generate_name;
        kernel_src_gen_t      generate_src;
        rtckernel_construct_t construct_rtckernel;
        bool                  valid() const
        {
            return generate_name && generate_src && construct_rtckernel;
        }
        // if known at compile time, the grid parameters of the kernel
        // to launch with
        dim3 gridDim;
        dim3 blockDim;
    };

    hipModule_t   module = nullptr;
    hipFunction_t kernel = nullptr;

private:
};

// helper functions to construct pieces of RTC kernel names
static const char* rtc_array_type_name(rocfft_array_type type)
{
    // hermitian is the same as complex in terms of generated code,
    // so give them the same names in kernels
    switch(type)
    {
    case rocfft_array_type_complex_interleaved:
    case rocfft_array_type_hermitian_interleaved:
        return "_CI";
    case rocfft_array_type_complex_planar:
    case rocfft_array_type_hermitian_planar:
        return "_CP";
    case rocfft_array_type_real:
        return "_R";
    default:
        return "_UN";
    }
}

static const char* rtc_precision_name(rocfft_precision precision)
{
    return precision == rocfft_precision_single ? "_sp" : "_dp";
}

static const char* rtc_precision_type_decl(rocfft_precision precision)
{
    switch(precision)
    {
    case rocfft_precision_single:
        return "typedef float2 scalar_type;\n";
    case rocfft_precision_double:
        return "typedef double2 scalar_type;\n";
    }
}

static const std::string rtc_const_cbtype_decl(bool enable_callbacks)
{
    if(enable_callbacks)
        return "static const CallbackType cbtype = CallbackType::USER_LOAD_STORE;\n";
    else
        return "static const CallbackType cbtype = CallbackType::NONE;\n";
}

#endif
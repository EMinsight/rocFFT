// Copyright (C) 2020 - 2023 Advanced Micro Devices, Inc. All rights reserved.
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

#include "tree_node.h"
#include "../../shared/precision_type.h"
#include "function_pool.h"
#include "kernel_launch.h"
#include "logging.h"
#include "repo.h"
#include "twiddles.h"

TreeNode::~TreeNode()
{
    if(twiddles)
    {
        if(scheme == CS_KERNEL_2D_SINGLE)
            Repo::ReleaseTwiddle2D(twiddles);
        else
            Repo::ReleaseTwiddle1D(twiddles);
        twiddles = nullptr;
    }
    if(twiddles_large)
    {
        Repo::ReleaseTwiddle1D(twiddles_large);
        twiddles_large = nullptr;
    }
}

NodeMetaData::NodeMetaData(TreeNode* refNode)
{
    if(refNode != nullptr)
    {
        precision  = refNode->precision;
        batch      = refNode->batch;
        direction  = refNode->direction;
        rootIsC2C  = refNode->IsRootPlanC2CTransform();
        deviceProp = refNode->deviceProp;
    }
}

bool LeafNode::CreateLargeTwdTable()
{
    if(large1D != 0)
    {
        std::tie(twiddles_large, twiddles_large_size) = Repo::GetTwiddles1D(
            large1D, 0, precision, deviceProp.gcnArchName, largeTwdBase, false, {});
    }

    return true;
}

size_t LeafNode::GetTwiddleTableLength()
{
    // length used by twiddle table is length[0] by default
    // could be override by some special schemes
    return length[0];
}

FMKey LeafNode::GetKernelKey() const
{
    if(!externalKernel)
        return EmptyFMKey;

    return TreeNode::GetKernelKey();
}

void LeafNode::GetKernelFactors()
{
    FMKey key     = GetKernelKey();
    kernelFactors = function_pool::get_kernel(key).factors;
}

bool LeafNode::KernelCheck(std::vector<FMKey>& kernel_keys)
{
    if(!externalKernel)
    {
        // such as solutions kernels for 2D_RTRT or 1D_CRT, the "T" kernel is not an external one
        // so in the solution map we will keep it as a empty key. By storing and checking the emptykey,
        // we can increase the reilability of solution map.
        if(!kernel_keys.empty())
        {
            if(LOG_TRACE_ENABLED())
                (*LogSingleton::GetInstance().GetTraceOS())
                    << "solution kernel is an built-in kernel" << std::endl;

            // kernel_key from solution map should be an EmptyFMKey for a built-in kernel
            if(kernel_keys.front() != EmptyFMKey)
                return false;
            kernel_keys.erase(kernel_keys.begin());
        }
        return true;
    }

    specified_key = nullptr;
    if(!kernel_keys.empty())
    {
        FMKey assignedKey = kernel_keys.front();
        kernel_keys.erase(kernel_keys.begin());

        // check if the assigned key is consistent with the node information
        const auto&            key_lengths   = std::get<0>(assignedKey);
        const rocfft_precision key_precision = std::get<1>(assignedKey);
        const ComputeScheme    key_scheme    = std::get<2>(assignedKey);
        if((length[0] != key_lengths[0]) || (dimension == 2 && length[1] != key_lengths[1])
           || (precision != key_precision) || (scheme != key_scheme))
        {
            if(LOG_TRACE_ENABLED())
                (*LogSingleton::GetInstance().GetTraceOS())
                    << "solution kernel keys are invalid" << std::endl;
            return false;
        }
        else
        {
            // get the sbrc_trans_type from assignedKey (for sbrc)
            sbrcTranstype = std::get<3>(assignedKey);

            function_pool::add_new_kernel(assignedKey);
            specified_key = std::make_unique<FMKey>(assignedKey);
        }
    }

    // get the final key and check if we have the kernel.
    // Note that the check is trivial if we are using "specified_key"
    // since we definitly have the kernel, but not trivial if it's the auto-gen key
    FMKey key = GetKernelKey();
    if(!function_pool::has_function(key))
    {
        if(LOG_TRACE_ENABLED())
            (*LogSingleton::GetInstance().GetTraceOS()) << PrintMissingKernelInfo(key);

        return false;
    }

    dir2regMode = (function_pool::get_kernel(key).direct_to_from_reg)
                      ? DirectRegType::TRY_ENABLE_IF_SUPPORT
                      : DirectRegType::FORCE_OFF_OR_NOT_SUPPORT;

    GetKernelFactors();
    return true;
}

void LeafNode::SanityCheck(SchemeTree* solution_scheme, std::vector<FMKey>& kernels_keys)
{
    if(!KernelCheck(kernels_keys))
        throw std::runtime_error("Kernel not found or mismatches node (solution map issue)");

    TreeNode::SanityCheck(solution_scheme, kernels_keys);
}

void LeafNode::Print(rocfft_ostream& os, int indent) const
{
    TreeNode::Print(os, indent);

    std::string indentStr;
    while(indent--)
        indentStr += "    ";

    os << indentStr.c_str() << "Leaf-Node: external-kernel configuration: ";
    indentStr += "    ";
    os << "\n" << indentStr.c_str() << "workgroup_size: " << wgs;
    os << "\n" << indentStr.c_str() << "trans_per_block: " << bwd;
    os << "\n" << indentStr.c_str() << "radices: [ ";
    for(size_t i = 0; i < kernelFactors.size(); i++)
    {
        os << kernelFactors[i] << " ";
    }
    os << "]\n";
}

bool LeafNode::CreateDevKernelArgs()
{
    devKernArg = kargs_create(length, inStride, outStride, iDist, oDist);
    return (devKernArg != nullptr);
}

bool LeafNode::CreateTwiddleTableResource()
{
    if(need_twd_table)
    {
        if(!twd_no_radices)
            GetKernelFactors();
        size_t twd_len                    = GetTwiddleTableLength();
        std::tie(twiddles, twiddles_size) = Repo::GetTwiddles1D(twd_len,
                                                                GetTwiddleTableLengthLimit(),
                                                                precision,
                                                                deviceProp.gcnArchName,
                                                                0,
                                                                twd_attach_halfN,
                                                                kernelFactors);
    }

    return CreateLargeTwdTable();
}

void LeafNode::SetupGridParamAndFuncPtr(DevFnCall& fnPtr, GridParam& gp)
{
    // derived classes setup the gp (bwd, wgs, lds, padding), funPtr
    SetupGPAndFnPtr_internal(fnPtr, gp);

    auto key = GetKernelKey();

    // common: sum up the value;
    gp.lds_bytes = (lds + lds_padding * bwd) * complex_type_size(precision);
    if(scheme == CS_KERNEL_STOCKHAM && ebtype == EmbeddedType::NONE)
    {
        if(function_pool::has_function(key))
        {
            auto kernel = function_pool::get_kernel(key);

            // NB:
            // Special case on specific arch:
            // For some cases using hald_lds, finer tuning(enlarge) dynamic
            // lds allocation size affects occupancy without changing the
            // kernel code. It is a middle solution between perf and code
            // consistency. Eventually, we need better solution arch
            // specific.
            bool double_half_lds_alloc = false;
            if(is_device_gcn_arch(deviceProp, "gfx90a") && (length[0] == 343 || length[0] == 49))
            {
                double_half_lds_alloc = true;
            }

            if(kernel.half_lds && (!double_half_lds_alloc))
                gp.lds_bytes /= 2;
        }
    }
    if((scheme == CS_KERNEL_STOCKHAM_BLOCK_CC || scheme == CS_KERNEL_STOCKHAM_BLOCK_CR)
       && (dir2regMode == DirectRegType::TRY_ENABLE_IF_SUPPORT) && (ebtype == EmbeddedType::NONE))
    {
        if(function_pool::has_function(key))
        {
            auto kernel = function_pool::get_kernel(key);
            if(kernel.half_lds)
                gp.lds_bytes /= 2;
        }
    }
    return;
}

/*****************************************************
 * CS_KERNEL_TRANSPOSE
 * CS_KERNEL_TRANSPOSE_XY_Z
 * CS_KERNEL_TRANSPOSE_Z_XY
 *****************************************************/

// grid params are set up by RTC
void TransposeNode::SetupGPAndFnPtr_internal(DevFnCall& fnPtr, GridParam& gp) {}

void TreeNode::SetTransposeOutputLength()
{
    switch(scheme)
    {
    case CS_KERNEL_TRANSPOSE:
    {
        outputLength = length;
        std::swap(outputLength[0], outputLength[1]);
        break;
    }
    case CS_KERNEL_TRANSPOSE_XY_Z:
    case CS_KERNEL_STOCKHAM_TRANSPOSE_XY_Z:
    {
        outputLength = length;
        std::swap(outputLength[1], outputLength[2]);
        std::swap(outputLength[0], outputLength[1]);
        break;
    }
    case CS_KERNEL_TRANSPOSE_Z_XY:
    case CS_KERNEL_STOCKHAM_TRANSPOSE_Z_XY:
    {
        outputLength = length;
        std::swap(outputLength[0], outputLength[1]);
        std::swap(outputLength[1], outputLength[2]);
        break;
    }
    default:
        throw std::runtime_error("can't set transpose output length on non-transpose node");
    }
}

void TreeNode::CollapseContiguousDims()
{
    // collapse children
    for(auto& child : childNodes)
        child->CollapseContiguousDims();

    const auto collapsibleDims = CollapsibleDims();
    if(collapsibleDims.empty())
        return;

    // utility function to collect the dims to collapse
    auto collectCollapse = [&collapsibleDims](const size_t               dist,
                                              size_t&                    newBatch,
                                              const std::vector<size_t>& length,
                                              const std::vector<size_t>& stride) {
        std::vector<size_t> dimsToCollapse;
        // start with batch dim and go backwards through collapsible dims
        // so we can collapse them without invalidating remaining indexes
        auto curStride = dist;
        for(auto i = collapsibleDims.rbegin(); i != collapsibleDims.rend(); ++i)
        {
            if(curStride % stride[*i] != 0)
                break;
            if(curStride / stride[*i] != length[*i])
                break;
            dimsToCollapse.push_back(*i);
            newBatch *= length[*i];
            curStride = stride[*i];
        }
        return dimsToCollapse;
    };

    // utility function to actually do the collapsing -
    // dimsToCollapse must be in reverse order so we erase dims from
    // highest to lowest
    auto doCollapse = [](size_t&                    dist,
                         const std::vector<size_t>& dimsToCollapse,
                         std::vector<size_t>&       lengthToCollapse,
                         std::vector<size_t>&       strideToCollapse) {
        for(auto i : dimsToCollapse)
        {
            dist /= lengthToCollapse[i];
            lengthToCollapse.erase(lengthToCollapse.begin() + i);
            strideToCollapse.erase(strideToCollapse.begin() + i);
        }
    };

    size_t              newInputBatch = batch;
    std::vector<size_t> inputDimsToCollapse
        = collectCollapse(iDist, newInputBatch, length, inStride);
    auto                outputLengthTemp = GetOutputLength();
    size_t              newOutputBatch   = batch;
    std::vector<size_t> outputDimsToCollapse
        = collectCollapse(oDist, newOutputBatch, outputLengthTemp, outStride);
    if(inputDimsToCollapse != outputDimsToCollapse || newInputBatch != newOutputBatch)
        return;

    if(!inputDimsToCollapse.empty())
    {
        std::stringstream msg;
        msg << "collapsed contiguous high length(s)";
        for(auto i = inputDimsToCollapse.rbegin(); i != inputDimsToCollapse.rend(); ++i)
            msg << " " << length[*i];
        msg << " into batch";
        comments.push_back(msg.str());
    }

    doCollapse(iDist, inputDimsToCollapse, length, inStride);
    doCollapse(oDist, outputDimsToCollapse, outputLengthTemp, outStride);
    batch = newInputBatch;

    if(!outputLength.empty())
        outputLength = outputLengthTemp;
}

bool TreeNode::IsBluesteinChirpSetup()
{
    // setup nodes must be under a bluestein parent
    if(parent == nullptr || parent->scheme != CS_BLUESTEIN)
        return false;
    // bluestein could either be 3-kernel plan, meaning the first two
    // are setup kernels, or a 6 kernel plan where only the first is
    // setup
    if(parent->childNodes.size() == 3)
        return this == parent->childNodes[0].get() || this == parent->childNodes[1].get();
    else if(parent->childNodes.size() == 6)
        return this == parent->childNodes[0].get();

    throw std::runtime_error("unexpected bluestein plan shape");
}

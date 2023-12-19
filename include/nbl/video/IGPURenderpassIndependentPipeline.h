// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h
#ifndef _NBL_VIDEO_I_GPU_RENDERPASS_INDEPENDENT_PIPELINE_H_INCLUDED_
#define _NBL_VIDEO_I_GPU_RENDERPASS_INDEPENDENT_PIPELINE_H_INCLUDED_


#include "nbl/asset/IRenderpassIndependentPipeline.h"

#include "nbl/video/IGPUShader.h"
#include "nbl/video/IPipeline.h"


namespace nbl::video
{

//! GPU Version of Renderpass Independent Pipeline
/*
	@see IRenderpassIndependentPipeline
*/
class IGPURenderpassIndependentPipeline : public IPipeline<IGPURenderpassIndependentPipeline>, public asset::IRenderpassIndependentPipeline<IGPUShader>
{
		using pipeline_t = IPipeline<IGPURenderpassIndependentPipeline>;
		using base_t = asset::IRenderpassIndependentPipeline<IGPUShader>;

	public:
		struct SCreationParams final : pipeline_t::SCreationParams, base_t::SCreationParams
		{
            public:
            #define base_flag(F) static_cast<uint64_t>(pipeline_t::SCreationParams::FLAGS::F)
            enum class FLAGS : uint64_t
            {
                NONE = base_flag(NONE),
                DISABLE_OPTIMIZATIONS = base_flag(DISABLE_OPTIMIZATIONS),
                ALLOW_DERIVATIVES = base_flag(ALLOW_DERIVATIVES),
                VIEW_INDEX_FROM_DEVICE_INDEX = 1<<3,
                CAPTURE_STATISTICS = base_flag(CAPTURE_STATISTICS),
                CAPTURE_INTERNAL_REPRESENTATIONS = base_flag(CAPTURE_INTERNAL_REPRESENTATIONS),
                FAIL_ON_PIPELINE_COMPILE_REQUIRED = base_flag(FAIL_ON_PIPELINE_COMPILE_REQUIRED),
                EARLY_RETURN_ON_FAILURE = base_flag(EARLY_RETURN_ON_FAILURE),
                LINK_TIME_OPTIMIZATION = base_flag(LINK_TIME_OPTIMIZATION),
                RETAIN_LINK_TIME_OPTIMIZATION_INFO = base_flag(RETAIN_LINK_TIME_OPTIMIZATION_INFO)
            };
            #undef base_flag

            inline SSpecializationValidationResult valid() const
            {
                if (!layout)
                    return {};
                SSpecializationValidationResult retval = {.count=0,.dataSize=0};
                const bool valid = base_t::SCreationParams::impl_valid([&retval](const IGPUShader::SSpecInfo& info)->bool
                {
                    const auto dataSize = info.valid();
                    if (dataSize<0)
                        return false;
                    const auto count = info.entries->size();
                    if (count>0x7fffffff)
                        return {};
                    retval += {.count=dataSize ? static_cast<uint32_t>(count):0,.dataSize=static_cast<uint32_t>(dataSize)};
                    return retval;
                });
                if (!valid)
                    return {};
                return retval;
            }

            inline std::span<const IGPUShader::SSpecInfo> getShaders() const {return shaders;}

            // TODO: Could guess the required flags from SPIR-V introspection of declared caps
            core::bitflag<FLAGS> flags = FLAGS::NONE;
		};

        inline core::bitflag<SCreationParams::FLAGS> getCreationFlags() const {return m_flags;}

	protected:
		IGPURenderpassIndependentPipeline(core::smart_refctd_ptr<const ILogicalDevice>&& dev, const SCreationParams& params) :
            pipeline_t(std::move(dev)), base_t(params.cached), m_flags(params.flags) {}
		virtual ~IGPURenderpassIndependentPipeline() = default;

        const core::bitflag<SCreationParams::FLAGS> m_flags;
};

}

#endif
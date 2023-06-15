#ifndef _NBL_VIDEO_I_GPU_QUEUE_H_INCLUDED_
#define _NBL_VIDEO_I_GPU_QUEUE_H_INCLUDED_

#include "nbl/video/decl/IBackendObject.h"

namespace nbl::video
{

class IGPUFence;
class IGPUSemaphore;
class IGPUCommandBuffer;

class IGPUQueue : public core::Interface, public core::Unmovable
{
    public:
        enum class FAMILY_FLAGS : uint8_t
        {
            NONE = 0,
            GRAPHICS_BIT = 0x01,
            COMPUTE_BIT = 0x02,
            TRANSFER_BIT = 0x04,
            SPARSE_BINDING_BIT = 0x08,
            PROTECTED_BIT = 0x10
        };
        enum class CREATE_FLAGS : uint32_t
        {
            PROTECTED_BIT = 0x01
        };


        // for renderdoc and friends
        virtual bool startCapture() = 0;
        virtual bool endCapture() = 0;

        //
        struct SSubmitInfo
        {
            uint32_t waitSemaphoreCount = 0u;
            IGPUSemaphore*const * pWaitSemaphores = nullptr;
            const asset::PIPELINE_STAGE_FLAGS* pWaitDstStageMask = nullptr;
            uint32_t signalSemaphoreCount = 0u;
            IGPUSemaphore*const * pSignalSemaphores = nullptr;
            uint32_t commandBufferCount = 0u;
            IGPUCommandBuffer*const * commandBuffers = nullptr;

            inline bool isValid() const
            {
                if (waitSemaphoreCount > 0u && (pWaitSemaphores == nullptr || pWaitDstStageMask == nullptr))
                    return false;
                if (signalSemaphoreCount > 0u && pSignalSemaphores == nullptr)
                    return false;
                if (commandBufferCount > 0u && commandBuffers == nullptr)
                    return false;
                return true;
            }
        };
        bool submit(const uint32_t _count, const SSubmitInfo* _submits, IGPUFence* const _fence);

        // getters
        float getPriority() const { return m_priority; }
        uint32_t getFamilyIndex() const { return m_familyIndex; }
        CREATE_FLAGS getFlags() const { return m_flags; }

        inline constexpr static float DEFAULT_QUEUE_PRIORITY = 1.f;

        // OpenGL: const egl::CEGL::Context*
        // Vulkan: const VkQueue*
        virtual const void* getNativeHandle() const = 0;

    protected:
        //! `flags` takes bits from E_CREATE_FLAGS
        inline IGPUQueue(ILogicalDevice* originDevice, uint32_t _famIx, CREATE_FLAGS _flags, float _priority)
            : m_originDevice(originDevice), m_flags(_flags), m_familyIndex(_famIx), m_priority(_priority)
        {
        }

        virtual bool submit_impl(const uint32_t _count, const SSubmitInfo* _submits, IGPUFence* const _fence) = 0;

        inline bool markCommandBuffersAsPending(const uint32_t _count, const SSubmitInfo* _submits);
        bool markCommandBuffersAsDone(const uint32_t _count, const SSubmitInfo* _submits);

        const uint32_t m_familyIndex;
        const CREATE_FLAGS m_flags;
        const float m_priority;
        const ILogicalDevice* m_originDevice;
};

NBL_ENUM_ADD_BITWISE_OPERATORS(IGPUQueue::FAMILY_FLAGS)

}

#endif
#include "nbl/video/IGPUDescriptorSet.h"

#include "nbl/video/IDescriptorPool.h"

namespace nbl::video
{

IGPUDescriptorSet::IGPUDescriptorSet(core::smart_refctd_ptr<const IGPUDescriptorSetLayout>&& layout, core::smart_refctd_ptr<IDescriptorPool>&& pool, IDescriptorPool::SStorageOffsets&& offsets)
    : base_t(std::move(layout)), IBackendObject(std::move(core::smart_refctd_ptr<const ILogicalDevice>(pool->getOriginDevice()))), m_version(0ull), m_pool(std::move(pool)), m_storageOffsets(std::move(offsets))
{
    for (auto i = 0u; i < static_cast<uint32_t>(asset::IDescriptor::E_TYPE::ET_COUNT); ++i)
    {
        // There is no descriptor of such type in the set.
        if (m_storageOffsets.data[i] == ~0u)
            continue;

        const auto type = static_cast<asset::IDescriptor::E_TYPE>(i);

        // Default-construct the core::smart_refctd_ptr<IDescriptor>s because even if the user didn't update the descriptor set with ILogicalDevice::updateDescriptorSet we
        // won't have uninitialized memory and destruction wouldn't crash in ~IGPUDescriptorSet.
        std::uninitialized_default_construct_n(m_pool->getDescriptorStorage(type) + m_storageOffsets.data[i], m_layout->getTotalDescriptorCount(type));
    }

    const auto mutableSamplerCount = m_layout->getTotalMutableSamplerCount();
    if (mutableSamplerCount > 0)
        std::uninitialized_default_construct_n(m_pool->getMutableSamplerStorage() + m_storageOffsets.data[static_cast<uint32_t>(asset::IDescriptor::E_TYPE::ET_COUNT)], mutableSamplerCount);
}

IGPUDescriptorSet::~IGPUDescriptorSet()
{
    if (!isZombie())
    {
        auto dummy = this;
        m_pool->deleteSetStorage(dummy);
    }
}

bool IGPUDescriptorSet::processWrite(const IGPUDescriptorSet::SWriteDescriptorSet& write)
{
    assert(write.dstSet == this);

    system::ILogger* logger = nullptr;
    {
        auto debugCallback = getOriginDevice()->getPhysicalDevice()->getDebugCallback();
        if (debugCallback)
            logger = debugCallback->getLogger();
    }

    auto* descriptors = getDescriptors(write.descriptorType, write.binding);
    if (!descriptors)
    {
        if (logger)
            logger->log("Descriptor set layout doesn't allow descriptor of such type at binding %u.", system::ILogger::ELL_ERROR, write.binding);
        return false;
    }

    core::smart_refctd_ptr<video::IGPUSampler>* mutableSamplers = nullptr;

    if (write.descriptorType == asset::IDescriptor::E_TYPE::ET_COMBINED_IMAGE_SAMPLER && write.info->info.image.sampler)
    {
        mutableSamplers = getMutableSamplers(write.binding);
        if (!mutableSamplers)
        {
            if (logger)
                logger->log("Descriptor set layout doesn't allow mutable samplers at binding %u.", system::ILogger::ELL_ERROR, write.binding);
            return false;
        }
    }

    for (auto j = 0; j < write.count; ++j)
    {
        descriptors[j] = write.info[j].desc;

        if (mutableSamplers)
            mutableSamplers[j] = write.info[j].info.image.sampler;
    }

    incrementVersion();

    return true;
}

bool IGPUDescriptorSet::processCopy(const IGPUDescriptorSet::SCopyDescriptorSet& copy)
{
    assert(copy.dstSet == this);

    system::ILogger* logger = nullptr;
    {
        auto debugCallback = getOriginDevice()->getPhysicalDevice()->getDebugCallback();
        if (debugCallback)
            logger = debugCallback->getLogger();
    }

    for (uint32_t t = 0; t < static_cast<uint32_t>(asset::IDescriptor::E_TYPE::ET_COUNT); ++t)
    {
        const auto type = static_cast<asset::IDescriptor::E_TYPE>(t);

        auto* srcDescriptors = copy.srcSet->getDescriptors(type, copy.srcBinding);
        if (!srcDescriptors)
        {
            if (logger)
                logger->log("Expected descriptors of given type at binding %u for the src descriptor set but none found.", system::ILogger::ELL_ERROR, copy.srcBinding);
            return false;
        }

        auto* srcSamplers = copy.srcSet->getMutableSamplers(copy.srcBinding);
        if (!srcSamplers)
        {
            if (logger)
                logger->log("Expected mutable samplers at binding %u for the src descriptor set, but none found", system::ILogger::ELL_ERROR, copy.srcBinding);
            return false;
        }

        auto* dstDescriptors = copy.dstSet->getDescriptors(type, copy.dstBinding);
        if (!dstDescriptors)
        {
            if (logger)
                logger->log("Expected descriptors of given type at binding %u for the dst descriptor set but none found.", system::ILogger::ELL_ERROR, copy.dstBinding);
            return false;
        }

        auto* dstSamplers = copy.dstSet->getMutableSamplers(copy.dstBinding);
        if (!dstSamplers)
        {
            if (logger)
                logger->log("Expected mutable samplers at binding %u for the dst descriptor set, but none found", system::ILogger::ELL_ERROR, copy.dstBinding);
            return false;
        }

        std::copy_n(srcDescriptors, copy.count, dstDescriptors);
        std::copy_n(srcSamplers, copy.count, dstSamplers);
    }

    incrementVersion();

    return true;
}

}
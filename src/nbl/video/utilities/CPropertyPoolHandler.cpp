#include "nbl/video/utilities/CPropertyPoolHandler.h"
#include "nbl/video/ILogicalDevice.h"
#include "nbl/video/IPhysicalDevice.h"

using namespace nbl;
using namespace video;

//
CPropertyPoolHandler::CPropertyPoolHandler(core::smart_refctd_ptr<ILogicalDevice>&& device) : m_device(std::move(device)), m_dsCache()
{
	const auto& deviceLimits = m_device->getPhysicalDevice()->getLimits();
	m_maxPropertiesPerPass = core::min<uint32_t>((deviceLimits.maxPerStageSSBOs-2u)/2u,MaxPropertiesPerDispatch);
	m_alignment = core::max(deviceLimits.SSBOAlignment,256u/*TODO: deviceLimits.nonCoherentAtomSize*/);

	auto system = m_device->getPhysicalDevice()->getSystem();
	auto glsl = system->loadBuiltinData<NBL_CORE_UNIQUE_STRING_LITERAL_TYPE("nbl/builtin/glsl/property_pool/copy.comp")>();
	auto cpushader = core::make_smart_refctd_ptr<asset::ICPUShader>(std::move(glsl),asset::ICPUShader::buffer_contains_glsl);
	auto gpushader = m_device->createGPUShader(asset::IGLSLCompiler::createOverridenCopy(cpushader.get(),"\n#define NBL_BUILTIN_MAX_PROPERTIES_PER_PASS %d\n",m_maxPropertiesPerPass));
	auto specshader = m_device->createGPUSpecializedShader(gpushader.get(),{nullptr,nullptr,"main",asset::ISpecializedShader::ESS_COMPUTE});

	const auto maxStreamingAllocations = 2u*m_maxPropertiesPerPass+2u;
	//m_tmpAddressRanges = reinterpret_cast<AddressUploadRange*>(malloc((sizeof(AddressUploadRange)+sizeof(uint32_t)*3u)*maxStreamingAllocations));
	
	IGPUDescriptorSetLayout::SBinding bindings[4];
	for (auto j=0; j<4; j++)
	{
		bindings[j].binding = j;
		bindings[j].type = asset::EDT_STORAGE_BUFFER;
		bindings[j].count = j<2u ? 1u:m_maxPropertiesPerPass;
		bindings[j].stageFlags = asset::ISpecializedShader::ESS_COMPUTE;
		bindings[j].samplers = nullptr;
	}
	auto dsLayout = m_device->createGPUDescriptorSetLayout(bindings,bindings+4);
	// TODO: if we decide to invalidate all cmdbuffs used for updates (make them non reusable), then we can use the ECF_NONE flag
	auto descPool = m_device->createDescriptorPoolForDSLayouts(IDescriptorPool::ECF_UPDATE_AFTER_BIND_BIT,&dsLayout.get(),&dsLayout.get()+1u,&CPropertyPoolHandler::DescriptorCacheSize);
	m_dsCache = core::make_smart_refctd_ptr<TransferDescriptorSetCache>(m_device.get(),std::move(descPool),core::smart_refctd_ptr(dsLayout));
	
	const asset::SPushConstantRange baseDWORD = {asset::ISpecializedShader::ESS_COMPUTE,0u,sizeof(uint32_t)*2u};
	auto layout = m_device->createGPUPipelineLayout(&baseDWORD,&baseDWORD+1u,std::move(dsLayout));
	m_pipeline = m_device->createGPUComputePipeline(nullptr,std::move(layout),std::move(specshader));
}


bool CPropertyPoolHandler::transferProperties(
	IGPUCommandBuffer* const cmdbuf, IGPUFence* const fence,
	const asset::SBufferBinding<video::IGPUBuffer>& scratch, const asset::SBufferBinding<video::IGPUBuffer>& addresses,
	const TransferRequest* const requestsBegin, const TransferRequest* const requestsEnd,
	system::logger_opt_ptr logger, const uint32_t baseDWORD, const uint32_t endDWORD
)
{
	if (requestsBegin==requestsEnd)
		return true;
	if (!scratch.buffer || !scratch.buffer->getCachedCreationParams().canUpdateSubRange)
	{
		logger.log("CPropertyPoolHandler: Need a valid scratch buffer which can have updates staged from the commandbuffer!",system::ILogger::ELL_ERROR);
		return false;
	}
	// TODO: validate usage flags
	if (scratch.offset+getMaxScratchSize()>scratch.buffer->getSize())
		logger.log("CPropertyPoolHandler: The scratch buffer binding provided might not be big enough in the worst case!",system::ILogger::ELL_WARNING);

	const auto totalProps = std::distance(requestsBegin,requestsEnd);
	const auto fullPasses = totalProps/m_maxPropertiesPerPass;
				
	nbl_glsl_property_pool_transfer_t transferData[MaxPropertiesPerDispatch];
	auto copyPass = [&](const TransferRequest* localRequests, uint32_t propertiesThisPass) -> bool
	{
		const auto scratchSize = sizeof(nbl_glsl_property_pool_transfer_t)*propertiesThisPass;
		if (scratch.offset+scratchSize>scratch.buffer->getSize())
		{
			logger.log("CPropertyPoolHandler: The scratch buffer binding provided is not big enough!",system::ILogger::ELL_ERROR);
			return false;
		}
		// count max dwords to transfer and check element size divisible by `sizeof(uint)`
		uint32_t maxDWORDs = 0u;
		for (uint32_t i=0; i<propertiesThisPass; i++)
		{
			const auto& request = localRequests[i];
			if (request.elementSize%sizeof(uint32_t))
			{
				logger.log("CPropertyPoolHandler::TransferRequest::elementSize (was %d) must be aligned to 4 bytes!",system::ILogger::ELL_ERROR,request.elementSize);
				assert(false);
				return false;
			}
			const auto elementsByteSize = request.elementCount*request.elementSize;
			maxDWORDs = core::max<uint32_t>(elementsByteSize/sizeof(uint32_t),maxDWORDs);
		}
		maxDWORDs = core::min(maxDWORDs,endDWORD);
		if (maxDWORDs==0u)
			return false;
		
		// update desc sets (TODO: handle acquire failure, by using push descriptors!)
		auto setIx = m_dsCache->acquireSet(this,scratch,addresses,localRequests,propertiesThisPass);
		if (setIx==IDescriptorSetCache::invalid_index)
		{
			logger.log("CPropertyPoolHandler: Failed to acquire descriptor set!",system::ILogger::ELL_ERROR);
			return false;
		}

		// prepare the transfers
		for (uint32_t i=0; i<propertiesThisPass; i++)
		{
			const auto& request = localRequests[i];

			auto& transfer = transferData[i];
			transfer.propertyDWORDsize_flags = request.elementSize/sizeof(uint32_t);
			transfer.propertyDWORDsize_flags |= uint32_t(request.flags)<<(32-TransferRequest::EF_BIT_COUNT);
			transfer.elementCount = request.elementCount;
			//
			transfer.srcIndexOffset = request.srcAddressesOffset;
			transfer.dstIndexOffset = request.dstAddressesOffset;
		}
		cmdbuf->updateBuffer(scratch.buffer.get(),scratch.offset,sizeof(nbl_glsl_property_pool_transfer_t)*propertiesThisPass,transferData);
		video::IGPUCommandBuffer::SBufferMemoryBarrier buffBarrier;
		{
			buffBarrier.barrier.srcAccessMask = asset::EAF_TRANSFER_WRITE_BIT;
			buffBarrier.barrier.dstAccessMask = asset::EAF_SHADER_READ_BIT;
			buffBarrier.dstQueueFamilyIndex = buffBarrier.srcQueueFamilyIndex = cmdbuf->getQueueFamilyIndex();
			buffBarrier.buffer = scratch.buffer;
			buffBarrier.offset = scratch.offset;
			buffBarrier.size = scratchSize;
			cmdbuf->pipelineBarrier(asset::EPSF_TRANSFER_BIT,asset::EPSF_COMPUTE_SHADER_BIT,asset::EDF_NONE,0u,nullptr,1u,&buffBarrier,0u,nullptr);
		}
		cmdbuf->bindComputePipeline(m_pipeline.get());
		// bind desc sets
		auto set = m_dsCache->getSet(setIx);
		cmdbuf->bindDescriptorSets(asset::EPBP_COMPUTE,m_pipeline->getLayout(),0u,1u,&set,nullptr);
		{
			const uint32_t data[] = {baseDWORD,endDWORD};
			cmdbuf->pushConstants(m_pipeline->getLayout(),asset::ISpecializedShader::ESS_COMPUTE,0u,sizeof(data),data);
		}
		// dispatch
		{
			const auto& limits = m_device->getPhysicalDevice()->getLimits();
			const auto invocationCoarseness = limits.maxOptimallyResidentWorkgroupInvocations*propertiesThisPass;
			cmdbuf->dispatch(limits.computeOptimalPersistentWorkgroupDispatchSize(maxDWORDs-baseDWORD,invocationCoarseness),propertiesThisPass,1u);
		}
		{
			buffBarrier.barrier.srcAccessMask = asset::EAF_SHADER_READ_BIT;
			buffBarrier.barrier.dstAccessMask = asset::EAF_TRANSFER_WRITE_BIT;
			cmdbuf->pipelineBarrier(asset::EPSF_COMPUTE_SHADER_BIT,asset::EPSF_TRANSFER_BIT,asset::EDF_NONE,0u,nullptr,1u,&buffBarrier,0u,nullptr);
		}
		// deferred release resources
		m_dsCache->releaseSet(m_device.get(),core::smart_refctd_ptr<IGPUFence>(fence),setIx);

		return true;
	};

	bool result = true;
	auto requests = requestsBegin;
	for (uint32_t i=0; i<fullPasses; i++)
	{
		result = copyPass(requests,m_maxPropertiesPerPass)&&result;
		requests += m_maxPropertiesPerPass;
	}

	const auto leftOverProps = totalProps-fullPasses*m_maxPropertiesPerPass;
	if (leftOverProps)
		return copyPass(requests,leftOverProps)&&result;
}

uint32_t CPropertyPoolHandler::transferProperties(
	StreamingTransientDataBufferMT<>* const upBuff, IGPUCommandBuffer* const cmdbuf, IGPUFence* const fence, IGPUQueue* queue,
	const asset::SBufferBinding<video::IGPUBuffer>& scratch, UpStreamingRequest* &requests, const uint32_t requestCount,
	uint32_t& waitSemaphoreCount, IGPUSemaphore* const*& semaphoresToWaitBeforeOverwrite, const asset::E_PIPELINE_STAGE_FLAGS*& stagesToWaitForPerSemaphore,
	system::logger_opt_ptr logger, const std::chrono::high_resolution_clock::time_point& maxWaitPoint
)
{
	if (!requestCount)
		return 0u;

	// somewhat decent attempt at packing
	std::sort(requests,requests+requestCount,[](const UpStreamingRequest& rhs, const UpStreamingRequest& lhs)->bool{return rhs.getElementDWORDs()<lhs.getElementDWORDs();});

	// TODO: slab sort
	struct AddressUploadRange
	{
		AddressUploadRange() : source{ nullptr,nullptr }, destOff(0xdeadbeefu) {}

		core::SRange<const uint32_t> source;
		uint32_t destOff;
	};


	struct CumulativeHistogram
	{
		uint32_t dwordCount[MaxPropertiesPerDispatch+1u];
		uint32_t memoryConsumed[MaxPropertiesPerDispatch+1u];

		inline uint32_t computeDWORDsToTransfer(const uint32_t propertiesThisPass, uint32_t& allocSize)
		{
			const auto upper_ix = std::distance(memoryConsumed,std::upper_bound(memoryConsumed,memoryConsumed+propertiesThisPass+1,allocSize));
			if (upper_ix==0u) // not enough memory to do anything
				return 0u;
			const auto lower_ix = upper_ix-1u;
			uint32_t DWORDs = dwordCount[lower_ix];
			const uint32_t baseMemory = memoryConsumed[lower_ix];
			if (upper_ix<=MaxPropertiesPerDispatch) // can't do all of them
			{
				const uint32_t bytesPerDWORD = (memoryConsumed[upper_ix]-baseMemory)/(dwordCount[upper_ix]-DWORDs);
				DWORDs += (allocSize-baseMemory)/bytesPerDWORD;
			}
			else
				allocSize = baseMemory;
			return DWORDs;
		}
	} cmHist;
	cmHist.dwordCount[0] = 0u;
	//
	TransferRequest xfers[MaxPropertiesPerDispatch];
	uint8_t* const upBuffPtr = reinterpret_cast<uint8_t*>(upBuff->getBufferPointer());
	const asset::SBufferBinding<video::IGPUBuffer> uploadBuffer = {0ull,core::smart_refctd_ptr<video::IGPUBuffer>(upBuff->getBuffer())};
	auto attempt = [&](const uint32_t baseDWORDs, const uint32_t remainingDWORDs, UpStreamingRequest* &localRequests, uint32_t& propertiesThisPass) -> uint32_t
	{
		// skip requests that won't participate
		while (propertiesThisPass && localRequests->getElementDWORDs()<=baseDWORDs)
		{
			localRequests++;
			propertiesThisPass++;
		}
		// nothing to do
		if (propertiesThisPass==0u)
			return 0u;

		// TODO: compute the histogram
		// 1 invocation transfers 1 DWORD
		uint32_t bytesPerInvocation = 0u;
		cmHist.memoryConsumed[0u] = 0u;/*
		for (auto i=0u; i<propertiesThisPass; i++)
		{
			const auto& request = localRequests[i];
			// needs DWORDs to allocate for source data
			if (!request.source.device2device)
				bytesPerInvocation += sizeof(uint32_t);
			// needs DWORDs to allocate for indices
			if (!request.addresses.device2device)
			{
				if (request.addresses.srcData)
					cmHist.memoryConsumed[request.fill ? 0u:69u] += sizeof(uint32_t);
				if (request.addresses.dstData)
					cmHist.memoryConsumed[] += sizeof(uint32_t);
			}
		}
		*/

        const uint32_t freeSpace = static_cast<uint32_t>(core::alignDown(upBuff->max_size(),m_alignment));
		const uint32_t worstCasePadding = m_alignment*propertiesThisPass-propertiesThisPass;
		if (freeSpace<=worstCasePadding)
			return 0u;
		auto paddedSize = freeSpace-worstCasePadding;
		const uint32_t doneDWORDs = cmHist.computeDWORDsToTransfer(propertiesThisPass,paddedSize);
		paddedSize += worstCasePadding;
		
		constexpr auto invalid_address = std::remove_reference_t<decltype(upBuff->getAllocator())>::invalid_address;
		auto addr = invalid_address;
		upBuff->multi_alloc(maxWaitPoint,1u,&addr,&paddedSize,&m_alignment);
		if (addr!=invalid_address)
		{
			// TODO: fill xfers and addresses
			uint32_t offset = addr;
			for (auto i=0u; i<propertiesThisPass; i++)
			{
				const auto& request = localRequests[i];
				auto& transfer = xfers[i];
				transfer.memblock = request.destination;
				transfer.flags = request.fill ? TransferRequest::EF_FILL:TransferRequest::EF_NONE;
				transfer.elementSize = request.elementSize;
				transfer.elementCount = request.elementCount;

				if (request.source.device2device)
					transfer.buffer = request.source.buffer;
				else
				{
					transfer.buffer = uploadBuffer;
					transfer.buffer.offset = offset;
					// copy
					const auto bytesize = transfer.getSourceElementCount()*transfer.elementSize;
					memcpy(upBuffPtr+offset,request.source.data,bytesize);
					// advance
					offset = core::alignUp(offset+bytesize,m_alignment);
				}
				// if valid, fill them later				
				transfer.srcAddressesOffset = request.srcAddresses ? 0u:IPropertyPool::invalid;
				transfer.dstAddressesOffset = request.dstAddresses ? 0u:IPropertyPool::invalid;
			}
			
			// flush if needed
			if (upBuff->needsManualFlushOrInvalidate())
			{
				IDriverMemoryAllocation::MappedMemoryRange flushRange;
				flushRange.memory = uploadBuffer.buffer->getBoundMemory();
				flushRange.range = {addr,paddedSize};
				m_device->flushMappedMemoryRanges(1u,&flushRange);
			}
			// no pipeline barriers necessary because write and optional flush happens before submit, and memory allocation is reclaimed after fence signal
			if (true)// TODO: transferProperties(cmdbuf,fence,scratch,uploadBuffer,xfers,xfers+propertiesThisPass,logger)) with base and max DWORD
			{
				upBuff->multi_free(1u,&addr,&paddedSize,core::smart_refctd_ptr<IGPUFence>(fence),&cmdbuf);
				return doneDWORDs;
			}
			upBuff->multi_free(1u,&addr,&paddedSize);
		}
		return 0u;
	};
	auto submit = [&]() -> void
	{
		IGPUQueue::SSubmitInfo submit;
		submit.commandBufferCount = 1u;
		submit.commandBuffers = &cmdbuf;
		submit.signalSemaphoreCount = 0u;
		submit.pSignalSemaphores = nullptr;
		assert(!waitSemaphoreCount || semaphoresToWaitBeforeOverwrite && stagesToWaitForPerSemaphore);
		submit.waitSemaphoreCount = waitSemaphoreCount;
		submit.pWaitSemaphores = semaphoresToWaitBeforeOverwrite;
		submit.pWaitDstStageMask = stagesToWaitForPerSemaphore;
		queue->submit(1u,&submit,fence);
		m_device->blockForFences(1u,&fence);
		waitSemaphoreCount = 0u;
		semaphoresToWaitBeforeOverwrite = nullptr;
		stagesToWaitForPerSemaphore = nullptr;
		// before resetting we need poll all events in the allocator's deferred free list
		upBuff->cull_frees();
		// we can reset the fence and commandbuffer because we fully wait for the GPU to finish here
		m_device->resetFences(1u,&fence);
		cmdbuf->reset(IGPUCommandBuffer::ERF_RELEASE_RESOURCES_BIT);
		cmdbuf->begin(IGPUCommandBuffer::EU_ONE_TIME_SUBMIT_BIT);
	};
	// return remaining DWORDs
	auto copyPass = [&](UpStreamingRequest* &localRequests, uint32_t propertiesThisPass) -> uint32_t
	{
		const auto localRequestsEnd = localRequests+propertiesThisPass;
		// figure out how much work we have to do
		uint32_t maxDWORDs = 0u;
		for (uint32_t i=0; i<propertiesThisPass; i++)
		{
			const auto& request = localRequests[i];
			maxDWORDs = core::max<uint32_t>(request.getElementDWORDs(),maxDWORDs);
		}
		// do the transfers
		uint32_t doneDWORDs=0u;
		for (uint32_t submitDWORDs=~0u; doneDWORDs<maxDWORDs;)
		{
			const auto thisDWORDs = attempt(doneDWORDs,maxDWORDs-doneDWORDs,localRequests,propertiesThisPass);
			if (thisDWORDs==0u)
			{
				if (submitDWORDs)
				{
					submit();
					submitDWORDs = 0u;
					continue; // try again
				}
				else // second fail in a row
					break;
			}
			submitDWORDs += thisDWORDs;
			doneDWORDs += thisDWORDs;
		}
		if (doneDWORDs!=maxDWORDs)
			return maxDWORDs-doneDWORDs;
		requests = localRequestsEnd;
		return 0u;
	};

	const auto fullPasses = requestCount/m_maxPropertiesPerPass;
	// transfer as many properties at once as possible
	for (uint32_t i=0; i<fullPasses; i++)
	{
		const auto remainingDWORDs = copyPass(requests,m_maxPropertiesPerPass);
		if (remainingDWORDs)
			return remainingDWORDs;
	}

	const auto leftOverProps = requestCount-fullPasses*m_maxPropertiesPerPass;
	if (leftOverProps)
		return copyPass(requests,leftOverProps);
	
	return 0u;
#if 0
			uint32_t slabCount = 0u;
			auto SlabComparator = [](auto lhs, auto rhs) -> bool
			{
				return lhs.source.begin()<rhs.source.begin();
			};
			// figure out the sizes to allocate
			{
				m_tmpSizes[0u] = sizeof(nbl_glsl_property_pool_transfer_t)*m_maxPropertiesPerPass;

				uint32_t addressListCount = 0u;
				uint32_t* upSizesIt = upSizes;
				uint32_t* downSizesIt = downSizes;
				for (uint32_t i=0; i<propertiesThisPass; i++)
				{
					const auto& request = localRequests[i];
					if (request.elementSize%sizeof(uint32_t))
					{
						logger.log("CPropertyPoolHandler::TransferRequest::elementSize (was %d) must be aligned to 4 bytes!",system::ILogger::ELL_ERROR,request.elementSize);
						assert(false);
						return false;
					}
					//
					if (!request.device2device)
					{
							*(upSizesIt++) = request.getSourceElementCount()*request.elementSize;
					}
					//
					if (request.srcAddresses)
					{
						m_tmpAddressRanges[addressListCount].source = {request.srcAddresses,request.srcAddresses+request.getSourceElementCount()};
						m_tmpAddressRanges[addressListCount++].destOff = 0u;
					}
					if (request.dstAddresses)
					{
						m_tmpAddressRanges[addressListCount].source = {request.dstAddresses,request.dstAddresses+request.elementCount};
						m_tmpAddressRanges[addressListCount++].destOff = 0u;
					}
				}

				// find slabs = contiguous or repeated ranges of redirection addresses (reduce duplication)
				{
					std::sort(m_tmpAddressRanges,m_tmpAddressRanges+addressListCount,SlabComparator);

					uint32_t addressOffset = 0u;
					auto oit = m_tmpAddressRanges;
					for (auto i=1u; i<addressListCount; i++)
					{
						const auto& inRange = m_tmpAddressRanges[i];

						// check for discontinuity
						auto& outRange = *oit;
						// if two requests have different contiguous pools, they'll have different redirects, so cant merge duplicate address ranges
						if (inRange.source.begin()>outRange.source.end())
						{
							addressOffset += outRange.source.size();
							// begin a new slab
							oit++;
							*oit = inRange;
							oit->destOff = addressOffset;
						}
						else
							outRange.source = {outRange.source.begin(),inRange.source.end()};
					}
					// note the size of the last slab
					addressOffset += oit->source.size();
					slabCount = std::distance(m_tmpAddressRanges,++oit);

					m_tmpSizes[0u] += addressOffset*sizeof(uint32_t);
				}
			}
			// allocate address list and upload/allocate data
			bool retval = true; // success
			std::fill(m_tmpAddresses,m_tmpAddresses+propertiesThisPass+1u,invalid_address);
			std::fill(downAddresses,downAddresses+propertiesThisPass,invalid_address);
			{
				for (auto i=0u; i<upAllocations; i++)
					m_tmpSizes[i] = core::roundUp(m_tmpSizes[i],m_alignments[i]);
				// TODO: handle overflow (chunk the updates with `max_size()` on the upload and download allocators)
				const auto unallocatedBytes = upBuff->multi_alloc(maxWaitPoint,upAllocations,m_tmpAddresses,m_tmpSizes,m_alignments);
				if (!(retval=unallocatedBytes==0u))
				{
					logger.log("CPropertyPoolHandler: Timed out during upstream staging allocation, failed to allocate %d bytes!",system::ILogger::ELL_ERROR,unallocatedBytes);
					return retval;
				}
			}
			// allocate and write
			{
				IDriverMemoryAllocation::MappedMemoryRange flushRanges[MaxPropertyTransfers+1u];
				flushRanges[0].memory = upBuff->getBuffer()->getBoundMemory();
				flushRanges[0].range = {m_tmpAddresses[0],m_tmpSizes[0]};

				uint32_t* addressBufferPtr = reinterpret_cast<uint32_t*>(upBuffPtr+m_tmpAddresses[0u]);
				// write header
				auto remapAddressList = [&](const uint32_t* originalRange) -> uint32_t
				{
					if (!originalRange)
						return IPropertyPool::invalid;

					// find the slab
					AddressUploadRange dummy;
					dummy.source = {originalRange,nullptr};
					dummy.destOff = 0xdeadbeefu;
					auto aboveOrEqual = std::lower_bound(m_tmpAddressRanges,m_tmpAddressRanges+slabCount,dummy,SlabComparator);
					auto containing = aboveOrEqual->source.begin()!=originalRange ? (aboveOrEqual-1):aboveOrEqual;
					//
					return containing->destOff+(originalRange-containing->source.begin());
				};
				for (uint32_t i=0; i<propertiesThisPass; i++)
				{
					const auto& request = localRequests[i];

					auto& transfer = reinterpret_cast<nbl_glsl_property_pool_transfer_t*>(addressBufferPtr)[i];
					transfer.propertyDWORDsize_flags = request.elementSize/sizeof(uint32_t);
					transfer.propertyDWORDsize_flags |= uint32_t(request.flags)<<(32-TransferRequest::EF_BIT_COUNT);
					transfer.elementCount = request.elementCount;
					//
					transfer.srcIndexOffset = remapAddressList(request.srcAddresses);
					transfer.dstIndexOffset = remapAddressList(request.dstAddresses);
				}
				addressBufferPtr += (sizeof(nbl_glsl_property_pool_transfer_t)/sizeof(uint32_t))*m_maxPropertiesPerPass;
				// write the addresses
				for (auto i=0u; i<slabCount; i++)
				{
					const auto& range = m_tmpAddressRanges[i];
					std::copy(range.source.begin(),range.source.end(),addressBufferPtr);
					addressBufferPtr += range.source.size();
				}
	
				// upload
				auto flushRangesIt = flushRanges+1u;
				auto upAddrIt = m_tmpAddresses+1u;
				uint32_t* upSizesIt = upSizes;
				for (uint32_t i=0u; i<propertiesThisPass; i++)
				{
					const auto& request = localRequests[i];
					if (request.device2device || request.isDownload())
						continue;

					const auto addr = *(upAddrIt++);
				
					*flushRangesIt = flushRanges[0];
					(flushRangesIt++)->range = {addr,*(upSizesIt++)};

					assert(addr!=invalid_address);
					memcpy(upBuffPtr+addr,request.source,request.getSourceElementCount()*request.elementSize);
				}

			}
#endif
}

uint32_t CPropertyPoolHandler::TransferDescriptorSetCache::acquireSet(
	CPropertyPoolHandler* handler, const asset::SBufferBinding<video::IGPUBuffer>& scratch, const asset::SBufferBinding<video::IGPUBuffer>& addresses,
	const TransferRequest* requests, const uint32_t propertyCount
)
{
	auto retval = IDescriptorSetCache::acquireSet();
	if (retval==IDescriptorSetCache::invalid_index)
		return IDescriptorSetCache::invalid_index;
	

	auto device = handler->getDevice();
	const auto maxPropertiesPerPass = handler->getMaxPropertiesPerTransferDispatch();


	IGPUDescriptorSet::SDescriptorInfo infos[MaxPropertiesPerDispatch*2u+2u];
	infos[0].assign(scratch,asset::EDT_STORAGE_BUFFER);
	infos[0].buffer.size = sizeof(nbl_glsl_property_pool_transfer_t)*propertyCount;
	infos[1].assign(addresses,asset::EDT_STORAGE_BUFFER);
	auto* inDescInfo = infos+2;
	auto* outDescInfo = infos+2+propertyCount;
	for (uint32_t i=0u; i<propertyCount; i++)
	{
		const auto& request = requests[i];
			
		const auto& memblock = request.memblock;
		const uint32_t transferPropertySize = request.elementCount*request.elementSize;

		if (request.isDownload())
		{
			inDescInfo[i].assign(memblock,asset::EDT_STORAGE_BUFFER);
			outDescInfo[i].assign(request.buffer,asset::EDT_STORAGE_BUFFER);
			outDescInfo[i].buffer.size = transferPropertySize;
		}
		else
		{
			inDescInfo[i].assign(request.buffer,asset::EDT_STORAGE_BUFFER);
			inDescInfo[i].buffer.size = transferPropertySize;
			outDescInfo[i].assign(memblock,asset::EDT_STORAGE_BUFFER);
		}
	}
	// just to make Vulkan shut up
	for (uint32_t i=propertyCount; i<maxPropertiesPerPass; i++)
	{
		inDescInfo[i].assign(scratch,asset::EDT_STORAGE_BUFFER);
		outDescInfo[i].assign(scratch,asset::EDT_STORAGE_BUFFER);
	}
	IGPUDescriptorSet::SWriteDescriptorSet writes[4u];
	IGPUDescriptorSet* const set = IDescriptorSetCache::getSet(retval);
	for (auto i=0u; i<4u; i++)
	{
		writes[i].dstSet = set;
		writes[i].binding = i;
		writes[i].arrayElement = 0u;
		writes[i].count = i<2u ? 1u:propertyCount;
		writes[i].descriptorType = asset::EDT_STORAGE_BUFFER;
		writes[i].info = i ? (writes[i-1u].info+writes[i-1u].count):infos;
	}
	device->updateDescriptorSets(4u,writes,0u,nullptr);

	return retval;
}
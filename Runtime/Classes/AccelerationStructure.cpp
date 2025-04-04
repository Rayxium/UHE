#include "AccelerationStructure.h"
#include "../CoreGlobals.h"
#include "Mesh.h"
#include "../Engine/Graphic.h"
#include "../Components/MeshRenderer.h"
#include "Types.h"

UHAccelerationStructure::UHAccelerationStructure()
    : AccelerationStructureBuffer(nullptr)
	, ScratchBuffer(nullptr)
	, ASInstanceBuffer(nullptr)
    , AccelerationStructure(nullptr)
	, GeometryKHRCache(VkAccelerationStructureGeometryKHR())
	, GeometryInfoCache(VkAccelerationStructureBuildGeometryInfoKHR())
	, RangeInfoCache(VkAccelerationStructureBuildRangeInfoKHR())
{

}

VkDeviceAddress UHAccelerationStructure::GetDeviceAddress(VkBuffer InBuffer)
{
	VkBufferDeviceAddressInfo AddressInfo{};
	AddressInfo.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO;
	AddressInfo.buffer = InBuffer;

	return vkGetBufferDeviceAddress(LogicalDevice, &AddressInfo);
}

VkDeviceAddress UHAccelerationStructure::GetDeviceAddress(VkAccelerationStructureKHR InAS)
{
	VkAccelerationStructureDeviceAddressInfoKHR AddressInfo{};
	AddressInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR;
	AddressInfo.accelerationStructure = InAS;

	return GVkGetAccelerationStructureDeviceAddressKHR(LogicalDevice, &AddressInfo);
}

// this should called by meshes
void UHAccelerationStructure::CreaetBottomAS(UHMesh* InMesh, VkCommandBuffer InBuffer)
{
	// prevent duplicate builds
	if (!GfxCache->IsRayTracingEnabled() || AccelerationStructure != nullptr)
	{
		return;
	}

	// filling geometry info, always assume Opaque bit here, I'll override it in top-level AS when necessary
	uint32_t MaxPrimitiveCounts = InMesh->GetIndicesCount() / 3;
	VkAccelerationStructureGeometryKHR GeometryKHR{};
	GeometryKHR.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	GeometryKHR.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
	GeometryKHR.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;

	// filling triangles VB/IB infos
	GeometryKHR.geometry.triangles = VkAccelerationStructureGeometryTrianglesDataKHR{};
	GeometryKHR.geometry.triangles.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;

	// set format for Vertex position, which is float3
	// with proper stride, system should fetch vertex pos properly
	GeometryKHR.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
	GeometryKHR.geometry.triangles.vertexStride = InMesh->GetPositionBuffer()->GetBufferStride();
	GeometryKHR.geometry.triangles.vertexData.deviceAddress = GetDeviceAddress(InMesh->GetPositionBuffer()->GetBuffer());
	GeometryKHR.geometry.triangles.maxVertex = InMesh->GetHighestIndex();

	if (InMesh->IsIndexBufer32Bit())
	{
		GeometryKHR.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
		GeometryKHR.geometry.triangles.indexData.deviceAddress = GetDeviceAddress(InMesh->GetIndexBuffer()->GetBuffer());
	}
	else
	{
		GeometryKHR.geometry.triangles.indexType = VK_INDEX_TYPE_UINT16;
		GeometryKHR.geometry.triangles.indexData.deviceAddress = GetDeviceAddress(InMesh->GetIndexBuffer16()->GetBuffer());
	}

	// filling geometry info
	VkAccelerationStructureBuildGeometryInfoKHR GeometryInfo{};
	GeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	GeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	GeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	GeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	GeometryInfo.geometryCount = 1;
	GeometryInfo.pGeometries = &GeometryKHR;

	// fetch the size info before creating AS based on geometry info
	VkAccelerationStructureBuildSizesInfoKHR SizeInfo{};
	SizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	GVkGetAccelerationStructureBuildSizesKHR(LogicalDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &GeometryInfo, &MaxPrimitiveCounts, &SizeInfo);

	// build bottom-level AS after getting proper sizes
	AccelerationStructureBuffer = GfxCache->RequestRenderBuffer<BYTE>(SizeInfo.accelerationStructureSize
		, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		, InMesh->GetName() + "_BottomLevelAS_Buffer");

	VkAccelerationStructureCreateInfoKHR CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	CreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
	CreateInfo.buffer = AccelerationStructureBuffer->GetBuffer();
	CreateInfo.size = SizeInfo.accelerationStructureSize;

	if (GVkCreateAccelerationStructureKHR(LogicalDevice, &CreateInfo, nullptr, &AccelerationStructure) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create bottom level AS!\n");
	}

#if WITH_EDITOR
	std::string ObjName = InMesh->GetName() + "_BottomLevelAS";
	GfxCache->SetDebugUtilsObjectName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)AccelerationStructure, ObjName);
#endif

	// allocate scratch buffer as well, this buffer is for initialization
	ScratchBuffer = GfxCache->RequestRenderBuffer<BYTE>(SizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		, "BottomLevelAS_ScratchBuffer");
	GeometryInfo.scratchData.deviceAddress = GetDeviceAddress(ScratchBuffer->GetBuffer());

	// actually build AS, this needs to push command
	VkAccelerationStructureBuildRangeInfoKHR RangeInfo{};
	RangeInfo.primitiveCount = MaxPrimitiveCounts;
	const VkAccelerationStructureBuildRangeInfoKHR* RangeInfos[1] = { &RangeInfo };
	GeometryInfo.dstAccelerationStructure = AccelerationStructure;

	GVkCmdBuildAccelerationStructuresKHR(InBuffer, 1, &GeometryInfo, RangeInfos);
}

// this should be called by renderer
uint32_t UHAccelerationStructure::CreateTopAS(const std::vector<UHMeshRendererComponent*>& InRenderers, VkCommandBuffer InBuffer)
{
	// prevent duplicate builds
	// to update Top AS, call UpdateTopAS instead
	if (!GfxCache->IsRayTracingEnabled() || AccelerationStructure)
	{
		return 0;
	}

	InstanceKHRs.resize(InRenderers.size());
	RendererCache.resize(InRenderers.size());

	// add top-level instance per-renderer
	uint32_t InstanceCount = 0;
	for (size_t Idx = 0; Idx < InRenderers.size(); Idx++)
	{
		// refresh transform once
		InRenderers[Idx]->Update();
		UHMaterial* Mat = InRenderers[Idx]->GetMaterial();

		// hit every thing for now
		VkAccelerationStructureInstanceKHR InstanceKHR{};
		InstanceKHR.mask = 0xff;

		// set bottom level address
		VkAccelerationStructureKHR BottomLevelAS = InRenderers[Idx]->GetMesh()->GetBottomLevelAS()->GetAS();
		InstanceKHR.accelerationStructureReference = GetDeviceAddress(BottomLevelAS);

		// copy transform3x4
		XMFLOAT3X4 Transform3x4 = MathHelpers::MatrixTo3x4(InRenderers[Idx]->GetWorldMatrix());
		std::copy(&Transform3x4.m[0][0], &Transform3x4.m[0][0] + 12, &InstanceKHR.transform.matrix[0][0]);

		// cull mode flag, in DXR system, it's default cull back, here just to check the other two modes
		if (Mat->GetCullMode() == UHCullMode::CullNone)
		{
			InstanceKHR.flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		}
		else if (Mat->GetCullMode() == UHCullMode::CullFront)
		{
			InstanceKHR.flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
		}

		// non-opaque flag, cutoff is treated as translucent as well so I can ignore the hit on culled pixel
		if (Mat->GetBlendMode() > UHBlendMode::Opaque)
		{
			InstanceKHR.flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
		}

		// set material buffer data index as SBT index, each material has an unique hitgroup shader
		InstanceKHR.instanceShaderBindingTableRecordOffset = Mat->GetBufferDataIndex();
		InstanceKHR.instanceCustomIndex = Mat->GetBufferDataIndex();

		// cache the instance KHRs and renderers for later use
		const int32_t RendererIdx = InRenderers[Idx]->GetBufferDataIndex();
		InstanceKHRs[RendererIdx] = InstanceKHR;
		RendererCache[RendererIdx] = InRenderers[Idx];
		InstanceCount++;
	}

	// don't create if there is no instance
	if (InstanceCount == 0)
	{
		return 0;
	}
	
	// create instance KHR buffer for later use
	ASInstanceBuffer = GfxCache->RequestRenderBuffer<VkAccelerationStructureInstanceKHR>(InstanceCount
		, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		, "Scene_TopLevelAS_InstanceBuffer");
	ASInstanceBuffer->UploadAllData(InstanceKHRs.data());

	// setup instance type
	VkAccelerationStructureGeometryKHR GeometryKHR{};
	GeometryKHR.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR;
	GeometryKHR.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	GeometryKHR.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{};
	GeometryKHR.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	GeometryKHR.geometry.instances.data.deviceAddress = GetDeviceAddress(ASInstanceBuffer->GetBuffer());

	// geometry count must be 1 when it's top level
	VkAccelerationStructureBuildGeometryInfoKHR GeometryInfo{};
	GeometryInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR;
	GeometryInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	GeometryInfo.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	GeometryInfo.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR | VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	GeometryInfo.geometryCount = 1;
	GeometryInfo.pGeometries = &GeometryKHR;

	// fetch the size info before creating AS based on geometry info
	VkAccelerationStructureBuildSizesInfoKHR SizeInfo{};
	SizeInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
	GVkGetAccelerationStructureBuildSizesKHR(LogicalDevice, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &GeometryInfo, &InstanceCount, &SizeInfo);

	// build bottom-level AS after getting proper sizes
	AccelerationStructureBuffer = GfxCache->RequestRenderBuffer<BYTE>(SizeInfo.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR
		, "Scene_TopLevelAS_Buffer");

	VkAccelerationStructureCreateInfoKHR CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR;
	CreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	CreateInfo.buffer = AccelerationStructureBuffer->GetBuffer();
	CreateInfo.size = SizeInfo.accelerationStructureSize;

	if (GVkCreateAccelerationStructureKHR(LogicalDevice, &CreateInfo, nullptr, &AccelerationStructure) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create top level AS!\n");
	}

#if WITH_EDITOR
	std::string ObjName = "Scene_TopLevelAS";
	GfxCache->SetDebugUtilsObjectName(VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR, (uint64_t)AccelerationStructure, ObjName);
#endif

	// allocate scratch buffer as well, this buffer is for initialization
	ScratchBuffer = GfxCache->RequestRenderBuffer<BYTE>(SizeInfo.buildScratchSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
		, "TopLevelAS_ScratchBuffer");
	GeometryInfo.scratchData.deviceAddress = GetDeviceAddress(ScratchBuffer->GetBuffer());

	// actually build AS, this needs to push command, primitive count is used as instance count in Vulkan spec if it's VK_GEOMETRY_TYPE_INSTANCES_KHR
	VkAccelerationStructureBuildRangeInfoKHR RangeInfo{};
	RangeInfo.primitiveCount = InstanceCount;
	const VkAccelerationStructureBuildRangeInfoKHR* RangeInfos[1] = { &RangeInfo };
	GeometryInfo.dstAccelerationStructure = AccelerationStructure;

	GVkCmdBuildAccelerationStructuresKHR(InBuffer, 1, &GeometryInfo, RangeInfos);

	GeometryKHRCache = GeometryKHR;
	GeometryInfoCache = GeometryInfo;
	RangeInfoCache = RangeInfo;

	// set geometry info as mode update for later use
	GeometryInfoCache.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
	GeometryInfoCache.srcAccelerationStructure = GeometryInfoCache.dstAccelerationStructure;
	GeometryInfoCache.pGeometries = &GeometryKHRCache;

	return InstanceCount;
}

// update top AS
void UHAccelerationStructure::UpdateTopAS(VkCommandBuffer InBuffer, const int32_t CurrentFrameRT, const float RTCullingDistance)
{
	for (size_t Idx = 0; Idx < InstanceKHRs.size(); Idx++)
	{
		UHMeshRendererComponent* Renderer = RendererCache[Idx];

		// copy transform3x4 when it's dirty
		if (Renderer->IsTransformChanged())
		{
			XMFLOAT3X4 Transform3x4 = MathHelpers::MatrixTo3x4(Renderer->GetWorldMatrix());
			std::copy(&Transform3x4.m[0][0], &Transform3x4.m[0][0] + 12, &InstanceKHRs[Idx].transform.matrix[0][0]);

			// refresh bottom level address
			VkAccelerationStructureKHR BottomLevelAS = Renderer->GetMesh()->GetBottomLevelAS()->GetAS();
			InstanceKHRs[Idx].accelerationStructureReference = GetDeviceAddress(BottomLevelAS);
		}

		// check visibility, can't use IsVisible() as it's set by frustum culling
		bool bIsVisible = Renderer->IsEnabled()
#if WITH_EDITOR
			&& Renderer->IsVisibleInEditor()
#endif
			;

		if (bIsVisible)
		{
			// only check culling distance when the component is visible
			bIsVisible &= (Renderer->GetSquareDistanceToMainCam() < RTCullingDistance * RTCullingDistance);
		}
		InstanceKHRs[Idx].mask = bIsVisible ? 0xff : 0;

		// check material state
		UHMaterial* Mat = RendererCache[Idx]->GetMaterial();
		InstanceKHRs[Idx].flags = 0;

		// cull mode flag, in DXR system, it's default cull back, here just to check the other two modes
		if (Mat->GetCullMode() == UHCullMode::CullNone)
		{
			InstanceKHRs[Idx].flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
		}
		else if (Mat->GetCullMode() == UHCullMode::CullFront)
		{
			InstanceKHRs[Idx].flags |= VK_GEOMETRY_INSTANCE_TRIANGLE_FLIP_FACING_BIT_KHR;
		}

		// non-opaque flag, cutoff is treated as translucent as well so I can ignore the hit on culled pixel
		if (Mat->GetBlendMode() > UHBlendMode::Opaque)
		{
			InstanceKHRs[Idx].flags |= VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR;
		}

		// set material buffer data index as SBT index, each material has an unique hitgroup shader
		InstanceKHRs[Idx].instanceShaderBindingTableRecordOffset = Mat->GetBufferDataIndex();
		InstanceKHRs[Idx].instanceCustomIndex = Mat->GetBufferDataIndex();
	}

	// upload all data in one call
	ASInstanceBuffer->UploadAllData(InstanceKHRs.data());

	// update it 
	const VkAccelerationStructureBuildRangeInfoKHR* RangeInfos[1] = { &RangeInfoCache };
	GVkCmdBuildAccelerationStructuresKHR(InBuffer, 1, &GeometryInfoCache, RangeInfos);
}

void UHAccelerationStructure::Release()
{
    if (GfxCache->IsRayTracingEnabled())
    {
		UH_SAFE_RELEASE(ScratchBuffer);
		UH_SAFE_RELEASE(ASInstanceBuffer);
		UH_SAFE_RELEASE(AccelerationStructureBuffer);

		if (AccelerationStructure)
		{
			GVkDestroyAccelerationStructureKHR(LogicalDevice, AccelerationStructure, nullptr);
			AccelerationStructure = nullptr;
		}
    }
}

// release scratch buffer only, this can be cleared after initialization
void UHAccelerationStructure::ReleaseScratch()
{
	if (GfxCache->IsRayTracingEnabled())
	{
		UH_SAFE_RELEASE(ScratchBuffer);
		// release temp AS instance buffer as well
		UH_SAFE_RELEASE(ASInstanceBuffer);
	}
}

VkAccelerationStructureKHR UHAccelerationStructure::GetAS() const
{
	return AccelerationStructure;
}
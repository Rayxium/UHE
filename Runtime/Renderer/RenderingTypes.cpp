#include "RenderingTypes.h"

// ---------------------------------------------------- UHDepthInfo
UHDepthInfo::UHDepthInfo()
	: UHDepthInfo(true, true, VK_COMPARE_OP_GREATER)
{

}

UHDepthInfo::UHDepthInfo(bool bInEnableDepthTest, bool bInEnableDepthWrite, VkCompareOp InDepthFunc)
	: bEnableDepthTest(bInEnableDepthTest)
	, bEnableDepthWrite(bInEnableDepthWrite)
	, DepthFunc(InDepthFunc)
{

}


// ---------------------------------------------------- UHRenderPassInfo
UHRenderPassInfo::UHRenderPassInfo()
	: UHRenderPassInfo(VK_NULL_HANDLE, UHDepthInfo(), UHCullMode::CullNone, UHBlendMode::Opaque, nullptr, nullptr, 1, VK_NULL_HANDLE)
{

}

// value for cullmode and blend mode is from different objects, don't set them in constructor for flexible usage
UHRenderPassInfo::UHRenderPassInfo(VkRenderPass InRenderPass, UHDepthInfo InDepthInfo, UHCullMode InCullInfo, UHBlendMode InBlendMode
	, UHShader* InVS, UHShader* InPS, int32_t InRTCount, VkPipelineLayout InPipelineLayout)
	: CullMode(InCullInfo)
	, BlendMode(InBlendMode)
	, RenderPass(InRenderPass)
	, DepthInfo(InDepthInfo)
	, VS(InVS)
	, PS(InPS)
	, GS(nullptr)
	, RTCount(InRTCount)
	, PipelineLayout(InPipelineLayout)
{

}

bool UHRenderPassInfo::operator==(const UHRenderPassInfo& InInfo)
{
	bool bVSEqual = true;
	if (InInfo.VS && VS)
	{
		bVSEqual = (*InInfo.VS == *VS);
	}

	bool bPSEqual = true;
	if (InInfo.PS && PS)
	{
		bPSEqual = (*InInfo.PS == *PS);
	}

	bool bGSEqual = true;
	if (InInfo.GS && GS)
	{
		bGSEqual = (*InInfo.GS == *GS);
	}

	return InInfo.CullMode == CullMode
		&& InInfo.BlendMode == BlendMode
		&& InInfo.RenderPass == RenderPass
		&& bVSEqual
		&& bPSEqual
		&& bGSEqual
		&& InInfo.RTCount == RTCount
		&& InInfo.PipelineLayout == PipelineLayout;
}


// ---------------------------------------------------- UHComputePassInfo
UHComputePassInfo::UHComputePassInfo()
	: CS(nullptr)
	, PipelineLayout(VK_NULL_HANDLE)
{

}

UHComputePassInfo::UHComputePassInfo(VkPipelineLayout InPipelineLayout)
	: CS(nullptr)
	, PipelineLayout(InPipelineLayout)
{

}

bool UHComputePassInfo::operator==(const UHComputePassInfo& InInfo)
{
	bool bCSEqual = true;
	if (InInfo.CS && CS)
	{
		bCSEqual = (*InInfo.CS == *CS);
	}

	return bCSEqual && InInfo.PipelineLayout == PipelineLayout;
}


// ---------------------------------------------------- UHRayTracingInfo
UHRayTracingInfo::UHRayTracingInfo()
	: PipelineLayout(VK_NULL_HANDLE)
	, MaxRecursionDepth(1)
	, RayGenShader(nullptr)
	, ClosestHitShader(nullptr)
	, MissShader(nullptr)
	, PayloadSize(4)
	, AttributeSize(8)
{

}

bool UHRayTracingInfo::operator==(const UHRayTracingInfo& InInfo)
{
	if (InInfo.RayGenShader != RayGenShader)
	{
		return false;
	}

	if (InInfo.ClosestHitShader != ClosestHitShader)
	{
		return false;
	}

	if (InInfo.AnyHitShaders.size() != AnyHitShaders.size())
	{
		return false;
	}
	else
	{
		for (size_t Idx = 0; Idx < AnyHitShaders.size(); Idx++)
		{
			if (AnyHitShaders[Idx] != InInfo.AnyHitShaders[Idx])
			{
				return false;
			}
		}
	}

	if (InInfo.MissShader != MissShader)
	{
		return false;
	}

	return InInfo.PipelineLayout == PipelineLayout
		&& InInfo.MaxRecursionDepth == MaxRecursionDepth
		&& InInfo.PayloadSize == PayloadSize
		&& InInfo.AttributeSize == AttributeSize;
}


// ---------------------------------------------------- UHRenderPassObject
UHRenderPassObject::UHRenderPassObject()
	: FrameBuffer(VK_NULL_HANDLE)
	, RenderPass(VK_NULL_HANDLE)
{

}

void UHRenderPassObject::Release(VkDevice LogicalDevice)
{
	vkDestroyFramebuffer(LogicalDevice, FrameBuffer, nullptr);
	vkDestroyRenderPass(LogicalDevice, RenderPass, nullptr);
}

void UHRenderPassObject::ReleaseRenderPass(VkDevice LogicalDevice)
{
	vkDestroyRenderPass(LogicalDevice, RenderPass, nullptr);
}

void UHRenderPassObject::ReleaseFrameBuffer(VkDevice LogicalDevice)
{
	// release frame buffer only, used for resizing
	vkDestroyFramebuffer(LogicalDevice, FrameBuffer, nullptr);
}


// ---------------------------------------------------- UHRenderState
UHRenderState::UHRenderState()
	: BufferDataIndex(0)
{
	// always dirty at the beginning
	for (int32_t Idx = 0; Idx < GMaxFrameInFlight; Idx++)
	{
		bIsRenderDirties[Idx] = true;
		bIsRayTracingDirties[Idx] = false;
		bIsMotionDirties[Idx] = false;
	}
}

void UHRenderState::SetRenderDirties(bool bIsDirty)
{
	for (int32_t Idx = 0; Idx < GMaxFrameInFlight; Idx++)
	{
		bIsRenderDirties[Idx] = bIsDirty;
	}
}

void UHRenderState::SetRenderDirty(bool bIsDirty, int32_t FrameIdx)
{
	bIsRenderDirties[FrameIdx] = bIsDirty;
}

void UHRenderState::SetRayTracingDirties(bool bIsDirty)
{
	for (int32_t Idx = 0; Idx < GMaxFrameInFlight; Idx++)
	{
		bIsRayTracingDirties[Idx] = bIsDirty;
	}
}

void UHRenderState::SetRayTracingDirty(bool bIsDirty, int32_t FrameIdx)
{
	bIsRayTracingDirties[FrameIdx] = bIsDirty;
}

void UHRenderState::SetMotionDirties(bool bIsDirty)
{
	for (int32_t Idx = 0; Idx < GMaxFrameInFlight; Idx++)
	{
		bIsMotionDirties[Idx] = bIsDirty;
	}
}

void UHRenderState::SetMotionDirty(bool bIsDirty, int32_t FrameIdx)
{
	bIsMotionDirties[FrameIdx] = bIsDirty;
}

bool UHRenderState::IsRenderDirty(int32_t FrameIdx)
{
	return bIsRenderDirties[FrameIdx];
}

bool UHRenderState::IsRayTracingDirty(int32_t FrameIdx)
{
	return bIsRayTracingDirties[FrameIdx];
}

bool UHRenderState::IsMotionDirty(int32_t FrameIdx)
{
	return bIsMotionDirties[FrameIdx];
}

void UHRenderState::SetBufferDataIndex(int32_t InIndex)
{
	BufferDataIndex = InIndex;
}

int32_t UHRenderState::GetBufferDataIndex() const
{
	return BufferDataIndex;
}
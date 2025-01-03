#include "LightPassShader.h"
#include "../RendererShared.h"

UHLightPassShader::UHLightPassShader(UHGraphic* InGfx, std::string Name)
	: UHShaderClass(InGfx, Name, typeid(UHLightPassShader), nullptr)
{
	// Lighting pass: bind system, light buffer, GBuffers, and samplers, all fragment only since it's a full screen quad draw
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);

	// if multiple descriptor count is used here, I can declare things like: Texture2D GBuffers[4]; in the shader
	// which acts like a "descriptor array"
	AddLayoutBinding(GNumOfGBuffersSRV, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);

	// scene output + shadow result + point light list + sampler
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_SAMPLER);
	AddLayoutBinding(1, VK_SHADER_STAGE_COMPUTE_BIT, VK_DESCRIPTOR_TYPE_SAMPLER);

	CreateLayoutAndDescriptor();
	OnCompile();
}

void UHLightPassShader::OnCompile()
{
	ShaderCS = Gfx->RequestShader("LightComputeShader", "Shaders/LightComputeShader.hlsl", "LightCS", "cs_6_0");

	// state
	UHComputePassInfo Info(PipelineLayout);
	Info.CS = ShaderCS;

	CreateComputeState(Info);
}

void UHLightPassShader::BindParameters(const bool bIsRaytracingEnableRT)
{
	BindConstant(GSystemConstantBuffer, 0, 0);
	BindStorage(GDirectionalLightBuffer, 1, 0, true);
	BindStorage(GPointLightBuffer, 2, 0, true);
	BindStorage(GSpotLightBuffer, 3, 0, true);

	BindImage(GetGBuffersSRV(), 4);
	BindRWImage(GSceneResult, 5);

	if (bIsRaytracingEnableRT)
	{
		BindImage(GRTShadowResult, 6);
	}
	else
	{
		BindImage(GWhiteTexture, 6);
	}

	BindStorage(GPointLightListBuffer.get(), 7, 0, true);
	BindStorage(GSpotLightListBuffer.get(), 8, 0, true);
	BindStorage(GSH9Data.get(), 9, 0, true);
	BindSampler(GPointClampedSampler, 10);
	BindSampler(GLinearClampedSampler, 11);
}
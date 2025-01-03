#include "TextureFormat.h"
#include <unordered_map>

VkFormat GetVulkanFormat(UHTextureFormat InUHFormat)
{
	static std::unordered_map<UHTextureFormat, VkFormat> FormatCache;
	if (FormatCache.size() == 0)
	{
		FormatCache[UHTextureFormat::UH_FORMAT_RGBA8_UNORM] = VK_FORMAT_R8G8B8A8_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_RGBA8_SRGB] = VK_FORMAT_R8G8B8A8_SRGB;
		FormatCache[UHTextureFormat::UH_FORMAT_RGBA16F] = VK_FORMAT_R16G16B16A16_SFLOAT;
		FormatCache[UHTextureFormat::UH_FORMAT_BGRA8_SRGB] = VK_FORMAT_B8G8R8A8_SRGB;
		FormatCache[UHTextureFormat::UH_FORMAT_BGRA8_UNORM] = VK_FORMAT_B8G8R8A8_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_RGB32F] = VK_FORMAT_R32G32B32_SFLOAT;
		FormatCache[UHTextureFormat::UH_FORMAT_D16] = VK_FORMAT_D16_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_D24_S8] = VK_FORMAT_D24_UNORM_S8_UINT;
		FormatCache[UHTextureFormat::UH_FORMAT_D32F] = VK_FORMAT_D32_SFLOAT;
		FormatCache[UHTextureFormat::UH_FORMAT_D32F_S8] = VK_FORMAT_D32_SFLOAT_S8_UINT;
		FormatCache[UHTextureFormat::UH_FORMAT_X8_D24] = VK_FORMAT_X8_D24_UNORM_PACK32;
		FormatCache[UHTextureFormat::UH_FORMAT_A2B10G10R10] = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
		FormatCache[UHTextureFormat::UH_FORMAT_A2R10G10B10] = VK_FORMAT_A2R10G10B10_UNORM_PACK32;
		FormatCache[UHTextureFormat::UH_FORMAT_RG16F] = VK_FORMAT_R16G16_SFLOAT;
		FormatCache[UHTextureFormat::UH_FORMAT_R8_UNORM] = VK_FORMAT_R8_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_R16F] = VK_FORMAT_R16_SFLOAT;
		FormatCache[UHTextureFormat::UH_FORMAT_R16_UNORM] = VK_FORMAT_R16_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_RG16_UNORM] = VK_FORMAT_R16G16_UNORM;
		FormatCache[UHTextureFormat::UH_FORMAT_R11G11B10] = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
		FormatCache[UHTextureFormat::UH_FORMAT_R32F] = VK_FORMAT_R32_SFLOAT;

		FormatCache[UHTextureFormat::UH_FORMAT_BC1_UNORM] = VK_FORMAT_BC1_RGB_UNORM_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC1_SRGB] = VK_FORMAT_BC1_RGB_SRGB_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC3_UNORM] = VK_FORMAT_BC3_UNORM_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC3_SRGB] = VK_FORMAT_BC3_SRGB_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC4] = VK_FORMAT_BC4_UNORM_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC5] = VK_FORMAT_BC5_UNORM_BLOCK;
		FormatCache[UHTextureFormat::UH_FORMAT_BC6H] = VK_FORMAT_BC6H_SFLOAT_BLOCK;
	}

	return FormatCache[InUHFormat];
}
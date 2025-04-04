#include "../../framework.h"
#include "Graphic.h"
#include <sstream>	// file stream
#include <limits>	// std::numeric_limits
#include <algorithm> // for clamp
#include "../Classes/Utility.h"
#include "../Classes/AssetPath.h"

UHGraphic::UHGraphic(UHAssetManager* InAssetManager, UHConfigManager* InConfig)
	: GraphicsQueue(nullptr)
	, CreationCommandPool(nullptr)
	, LogicalDevice(nullptr)
	, SwapChainRenderPass(nullptr)
	, MainSurface(nullptr)
	, PhysicalDevice(nullptr)
	, PhysicalDeviceMemoryProperties(VkPhysicalDeviceMemoryProperties())
	, SwapChain(nullptr)
	, VulkanInstance(nullptr)
	, WindowCache(nullptr)
	, bIsFullScreen(false)
	, bUseValidationLayers(false)
	, AssetManagerInterface(InAssetManager)
	, ConfigInterface(InConfig)
	, bEnableDepthPrePass(InConfig->RenderingSetting().bEnableDepthPrePass)
	, bEnableRayTracing(InConfig->RenderingSetting().bEnableRayTracing)
	, bSupportHDR(false)
	, bSupport24BitDepth(true)
	, bSupportMeshShader(false)
	, MeshBufferSharedMemory(nullptr)
	, ImageSharedMemory(nullptr)
#if WITH_EDITOR
	, ImGuiDescriptorPool(nullptr)
	, ImGuiPipeline(nullptr)
#endif
{
	// extension defines, hard code for now
	InstanceExtensions = { "VK_KHR_surface"
		, "VK_KHR_win32_surface"
		, "VK_KHR_get_surface_capabilities2"
		, "VK_KHR_get_physical_device_properties2"
		, "VK_EXT_swapchain_colorspace" };

	if (GIsEditor)
	{
		InstanceExtensions.push_back("VK_EXT_debug_utils");
	}

	DeviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME
		, "VK_EXT_full_screen_exclusive"
		, "VK_KHR_spirv_1_4"
		, "VK_KHR_shader_float_controls"
		, "VK_EXT_robustness2"
		, "VK_EXT_hdr_metadata"
		, "VK_KHR_dynamic_rendering"
		, "VK_KHR_synchronization2"
		, "VK_KHR_push_descriptor"
		, "VK_EXT_conditional_rendering"
		, "VK_EXT_descriptor_indexing"
		, "VK_EXT_mesh_shader" };

	RayTracingExtensions = { "VK_KHR_deferred_host_operations"
		, "VK_KHR_acceleration_structure"
		, "VK_KHR_ray_tracing_pipeline"
		, "VK_KHR_ray_query"
		, "VK_KHR_pipeline_library" };

	// push ray tracing extension
	DeviceExtensions.insert(DeviceExtensions.end(), RayTracingExtensions.begin(), RayTracingExtensions.end());
}

// init graphics
bool UHGraphic::InitGraphics(HWND Hwnd)
{
	bUseValidationLayers = ConfigInterface->RenderingSetting().bEnableLayerValidation && GIsEditor;

	// variable setting
	WindowCache = Hwnd;

	bool bInitSuccess = CreateInstance()
		&& CreatePhysicalDevice()
		&& CreateWindowSurface()
		&& CreateQueueFamily()
		&& CreateLogicalDevice()
		&& CreateSwapChain();

	if (bInitSuccess)
	{
		// allocate shared GPU memory if initialization succeed
		ImageSharedMemory = MakeUnique<UHGPUMemory>();
		MeshBufferSharedMemory = MakeUnique<UHGPUMemory>();

		ImageSharedMemory->SetGfxCache(this);
		MeshBufferSharedMemory->SetGfxCache(this);

		DeviceMemoryTypeIndices = GetMemoryTypeIndices(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		HostMemoryTypeIndex = GetMemoryTypeIndices(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)[0];

		// use the first heap for shared image memory anyway, it's rare to have multiple heaps from a single GPU
		ImageSharedMemory->AllocateMemory(static_cast<uint64_t>(ConfigInterface->EngineSetting().ImageMemoryBudgetMB) * 1048576, DeviceMemoryTypeIndices[0]);
		MeshBufferSharedMemory->AllocateMemory(static_cast<uint64_t>(ConfigInterface->EngineSetting().MeshBufferMemoryBudgetMB) * 1048576, HostMemoryTypeIndex);

		// reserve pools for faster allocation
		ShaderPools.reserve(std::numeric_limits<int16_t>::max());
		StatePools.reserve(1024);
		RTPools.reserve(64);
		SamplerPools.reserve(64);
		Texture2DPools.reserve(1024);
		TextureCubePools.reserve(1024);
		MaterialPools.reserve(1024);
		QueryPools.reserve(std::numeric_limits<int16_t>::max());
	}

	return bInitSuccess;
}

// release graphics
void UHGraphic::Release()
{
	// wait device to finish before release
	WaitGPU();

	if (bIsFullScreen)
	{
		GLeaveFullScreenCallback(LogicalDevice, SwapChain);
		bIsFullScreen = false;
	}

	WindowCache = nullptr;
	GraphicsQueue = nullptr;

	// release all shaders
	ClearContainer(ShaderPools);

	// release all states
	ClearContainer(StatePools);

	// release all RTs
	ClearSwapChain();
	ClearContainer(RTPools);

	// release all samplers
	ClearContainer(SamplerPools);

	// relase all textures
	ClearContainer(Texture2DPools);
	ClearContainer(TextureCubePools);

	// release all materials
	for (auto& Mat : MaterialPools)
	{
		Mat.reset();
	}
	MaterialPools.clear();

	// release all queries
	ClearContainer(QueryPools);

	// release GPU memory pool
	ImageSharedMemory->Release();
	ImageSharedMemory.reset();
	MeshBufferSharedMemory->Release();
	MeshBufferSharedMemory.reset();

#if WITH_EDITOR
	ImGui_ImplVulkan_Shutdown();
	ImGui_ImplWin32_Shutdown();
	ImGui::DestroyContext();
	vkDestroyDescriptorPool(LogicalDevice, ImGuiDescriptorPool, nullptr);
	if (ImGuiPipeline)
	{
		vkDestroyPipeline(LogicalDevice, ImGuiPipeline, nullptr);
	}
#endif

	vkDestroyCommandPool(LogicalDevice, CreationCommandPool, nullptr);
	vkDestroySurfaceKHR(VulkanInstance, MainSurface, nullptr);
	vkDestroyDevice(LogicalDevice, nullptr);
	vkDestroyInstance(VulkanInstance, nullptr);
}

// debug only functions
#if WITH_EDITOR

// check validation layer support
bool UHGraphic::CheckValidationLayerSupport()
{
	uint32_t LayerCount;
	vkEnumerateInstanceLayerProperties(&LayerCount, nullptr);

	std::vector<VkLayerProperties> AvailableLayers(LayerCount);
	vkEnumerateInstanceLayerProperties(&LayerCount, AvailableLayers.data());

	bool LayerFound = false;
	for (const char* LayerName : ValidationLayers)
	{
		for (const auto& LayerProperties : AvailableLayers)
		{
			if (strcmp(LayerName, LayerProperties.layerName) == 0)
			{
				LayerFound = true;
				break;
			}
		}
	}

	if (!LayerFound) 
	{
		return false;
	}
	return true;
}

#endif

bool CheckInstanceExtension(const std::vector<const char*>& RequiredExtensions)
{
	uint32_t ExtensionCount = 0;
	vkEnumerateInstanceExtensionProperties(nullptr, &ExtensionCount, nullptr);
	std::vector<VkExtensionProperties> Extensions(ExtensionCount);
	vkEnumerateInstanceExtensionProperties(nullptr, &ExtensionCount, Extensions.data());

	// count every extensions
	uint32_t InExtensionCount = static_cast<uint32_t>(RequiredExtensions.size());
	uint32_t Count = 0;

	for (uint32_t Idx = 0; Idx < InExtensionCount; Idx++)
	{
		bool bSupported = false;
		for (uint32_t Jdx = 0; Jdx < ExtensionCount; Jdx++)
		{
			if (strcmp(RequiredExtensions[Idx], Extensions[Jdx].extensionName) == 0)
			{
				Count++;
				bSupported = true;
				break;
			}
		}

		if (!bSupported)
		{
			UHE_LOG(L"Unsupport instance extension detected: " + UHUtilities::ToStringW(RequiredExtensions[Idx]) + L"\n");
		}
	}

	// only return true if required extension is supported
	if (InExtensionCount == Count)
	{
		return true;
	}

	UHE_LOG(L"Unsupport instance extension detected!\n");
	return false;
}

bool UHGraphic::CreateInstance()
{
	// prepare app info
	VkApplicationInfo AppInfo{};
	AppInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	AppInfo.pApplicationName = ENGINE_NAME;
	AppInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	AppInfo.pEngineName = ENGINE_NAME;
	AppInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	AppInfo.apiVersion = VK_API_VERSION_1_3;

	// create vk instance
	VkInstanceCreateInfo CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	CreateInfo.pApplicationInfo = &AppInfo;

	// set up validation layer if it's debugging
#if WITH_EDITOR
	if (bUseValidationLayers && CheckValidationLayerSupport())
	{
		CreateInfo.enabledLayerCount = static_cast<uint32_t>(ValidationLayers.size());
		CreateInfo.ppEnabledLayerNames = ValidationLayers.data();
	}
#endif

	if (GIsEditor)
	{
		InstanceExtensions.push_back("VK_EXT_debug_utils");
	}

	if (CheckInstanceExtension(InstanceExtensions))
	{
		CreateInfo.enabledExtensionCount = static_cast<uint32_t>(InstanceExtensions.size());
		CreateInfo.ppEnabledExtensionNames = InstanceExtensions.data();
	}

	VkResult CreateResult = vkCreateInstance(&CreateInfo, nullptr, &VulkanInstance);

	// print the failure reason for instance creation
	if (CreateResult != VK_SUCCESS)
	{
		UHE_LOG(L"Vulkan instance creation failed!\n");
		return false;
	}

	// get necessary function callback after instance is created
	GEnterFullScreenCallback = (PFN_vkAcquireFullScreenExclusiveModeEXT)vkGetInstanceProcAddr(VulkanInstance, "vkAcquireFullScreenExclusiveModeEXT");
	GLeaveFullScreenCallback = (PFN_vkReleaseFullScreenExclusiveModeEXT)vkGetInstanceProcAddr(VulkanInstance, "vkReleaseFullScreenExclusiveModeEXT");
	GGetSurfacePresentModes2Callback = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)vkGetInstanceProcAddr(VulkanInstance, "vkGetPhysicalDeviceSurfacePresentModes2EXT");

#if WITH_EDITOR
	GBeginCmdDebugLabelCallback = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(VulkanInstance, "vkCmdBeginDebugUtilsLabelEXT");
	GEndCmdDebugLabelCallback = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(VulkanInstance, "vkCmdEndDebugUtilsLabelEXT");
	GSetDebugUtilsObjectNameEXT = (PFN_vkSetDebugUtilsObjectNameEXT)vkGetInstanceProcAddr(VulkanInstance, "vkSetDebugUtilsObjectNameEXT");
#endif

	GVkGetAccelerationStructureDeviceAddressKHR = (PFN_vkGetAccelerationStructureDeviceAddressKHR)vkGetInstanceProcAddr(VulkanInstance, "vkGetAccelerationStructureDeviceAddressKHR");
	GVkGetAccelerationStructureBuildSizesKHR = (PFN_vkGetAccelerationStructureBuildSizesKHR)vkGetInstanceProcAddr(VulkanInstance, "vkGetAccelerationStructureBuildSizesKHR");
	GVkCreateAccelerationStructureKHR = (PFN_vkCreateAccelerationStructureKHR)vkGetInstanceProcAddr(VulkanInstance, "vkCreateAccelerationStructureKHR");
	GVkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)vkGetInstanceProcAddr(VulkanInstance, "vkCmdBuildAccelerationStructuresKHR");
	GVkDestroyAccelerationStructureKHR = (PFN_vkDestroyAccelerationStructureKHR)vkGetInstanceProcAddr(VulkanInstance, "vkDestroyAccelerationStructureKHR");
	GVkCreateRayTracingPipelinesKHR = (PFN_vkCreateRayTracingPipelinesKHR)vkGetInstanceProcAddr(VulkanInstance, "vkCreateRayTracingPipelinesKHR");
	GVkCmdTraceRaysKHR = (PFN_vkCmdTraceRaysKHR)vkGetInstanceProcAddr(VulkanInstance, "vkCmdTraceRaysKHR");
	GVkGetRayTracingShaderGroupHandlesKHR = (PFN_vkGetRayTracingShaderGroupHandlesKHR)vkGetInstanceProcAddr(VulkanInstance, "vkGetRayTracingShaderGroupHandlesKHR");
	GVkSetHdrMetadataEXT = (PFN_vkSetHdrMetadataEXT)vkGetInstanceProcAddr(VulkanInstance, "vkSetHdrMetadataEXT");
	GVkCmdPushDescriptorSetKHR = (PFN_vkCmdPushDescriptorSetKHR)vkGetInstanceProcAddr(VulkanInstance, "vkCmdPushDescriptorSetKHR");
	GVkCmdBeginConditionalRenderingEXT = (PFN_vkCmdBeginConditionalRenderingEXT)vkGetInstanceProcAddr(VulkanInstance, "vkCmdBeginConditionalRenderingEXT");
	GVkCmdEndConditionalRenderingEXT = (PFN_vkCmdEndConditionalRenderingEXT)vkGetInstanceProcAddr(VulkanInstance, "vkCmdEndConditionalRenderingEXT");
	GVkCmdDrawMeshTasksEXT = (PFN_vkCmdDrawMeshTasksEXT)vkGetInstanceProcAddr(VulkanInstance, "vkCmdDrawMeshTasksEXT");

	return true;
}

bool UHGraphic::CheckDeviceExtension(VkPhysicalDevice InDevice, std::vector<const char*>& RequiredExtensions)
{
	uint32_t ExtensionCount;
	vkEnumerateDeviceExtensionProperties(InDevice, nullptr, &ExtensionCount, nullptr);

	std::vector<VkExtensionProperties> AvailableExtensions(ExtensionCount);
	vkEnumerateDeviceExtensionProperties(InDevice, nullptr, &ExtensionCount, AvailableExtensions.data());

	// count every extensions
	uint32_t InExtensionCount = static_cast<uint32_t>(RequiredExtensions.size());

	std::vector<const char*> ValidExtensions;
	for (uint32_t Idx = 0; Idx < InExtensionCount; Idx++)
	{
		bool bSupported = false;
		for (uint32_t Jdx = 0; Jdx < ExtensionCount; Jdx++)
		{
			if (strcmp(RequiredExtensions[Idx], AvailableExtensions[Jdx].extensionName) == 0)
			{
				bSupported = true;
				ValidExtensions.push_back(RequiredExtensions[Idx]);
				break;
			}
		}

		if (!bSupported)
		{
			UHE_LOG(L"Unsupport device extension detected: " + UHUtilities::ToStringW(RequiredExtensions[Idx]) + L"\n");
			if (UHUtilities::FindByElement(RayTracingExtensions, RequiredExtensions[Idx]))
			{
				UHE_LOG(L"Ray tracing not supported!\n");
				bEnableRayTracing = false;
			}
		}
	}

	if (RequiredExtensions.size() == ValidExtensions.size())
	{
		return true;
	}

	RequiredExtensions = ValidExtensions;
	UHE_LOG(L"Unsupport device extension automatically removed.\n");
	return true;
}

bool UHGraphic::CreatePhysicalDevice()
{
	// enum devices
	uint32_t DeviceCount = 0;
	vkEnumeratePhysicalDevices(VulkanInstance, &DeviceCount, nullptr);
	if (DeviceCount == 0)
	{
		UHE_LOG(L"Failed to find GPUs with Vulkan support!\n");
		return false;
	}

	// actually collect devices
	std::vector<VkPhysicalDevice> Devices(DeviceCount);
	vkEnumeratePhysicalDevices(VulkanInstance, &DeviceCount, Devices.data());

	// choose a suitable device
	const VkPhysicalDeviceType TestDeviceType = VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
	std::string SelectedDeviceName;
	for (uint32_t Idx = 0; Idx < DeviceCount; Idx++)
	{
		// use device properties 2
		VkPhysicalDeviceProperties2 DeviceProperties{};
		DeviceProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
		vkGetPhysicalDeviceProperties2(Devices[Idx], &DeviceProperties);
		UHE_LOG(L"Trying GPU device: " + UHUtilities::ToStringW(DeviceProperties.properties.deviceName) + L"\n");

		// choose 1st available GPU for use
		if ((DeviceProperties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
			|| DeviceProperties.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU)
			&& CheckDeviceExtension(Devices[Idx], DeviceExtensions))
		{
			PhysicalDevice = Devices[Idx];
			SelectedDeviceName = DeviceProperties.properties.deviceName;
			if (TestDeviceType == DeviceProperties.properties.deviceType)
			{
				break;
			}
		}
	}

	if (PhysicalDevice == nullptr)
	{
		UHE_LOG(L"Failed to find a suitable GPU!\n");
		return false;
	}

	std::wostringstream Msg;
	Msg << L"Selected device: " << SelectedDeviceName.c_str() << std::endl;
	UHE_LOG(Msg.str());

	// request memory props after creation
	vkGetPhysicalDeviceMemoryProperties(PhysicalDevice, &PhysicalDeviceMemoryProperties);

	return true;
}

bool UHGraphic::CreateQueueFamily()
{
	uint32_t QueueFamilyCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &QueueFamilyCount, nullptr);

	std::vector<VkQueueFamilyProperties> QueueFamilies(QueueFamilyCount);
	vkGetPhysicalDeviceQueueFamilyProperties(PhysicalDevice, &QueueFamilyCount, QueueFamilies.data());

	// choose queue family, find both graphic queue and compute queue for now
	for (uint32_t Idx = 0; Idx < QueueFamilyCount; Idx++)
	{
		// consider present support
		VkBool32 PresentSupport = false;
		vkGetPhysicalDeviceSurfaceSupportKHR(PhysicalDevice, Idx, MainSurface, &PresentSupport);

		// consider swap chain support
		bool SwapChainAdequate = false;
		UHSwapChainDetails SwapChainSupport = QuerySwapChainSupport(PhysicalDevice);
		SwapChainAdequate = !SwapChainSupport.Formats2.empty() && !SwapChainSupport.PresentModes.empty();

		if (PresentSupport && SwapChainAdequate)
		{
			if (QueueFamilies[Idx].queueFlags & VK_QUEUE_GRAPHICS_BIT)
			{
				QueueFamily.GraphicsFamily = Idx;
			}
			else if (QueueFamilies[Idx].queueFlags & VK_QUEUE_COMPUTE_BIT)
			{
				QueueFamily.ComputesFamily = Idx;
			}
		}
	}

	if (!QueueFamily.GraphicsFamily.has_value())
	{
		UHE_LOG(L"Failed to create graphic queue!\n");
		return false;
	}

	if (!QueueFamily.ComputesFamily.has_value())
	{
		UHE_LOG(L"Failed to create compute queue!\n");
		return false;
	}

	return true;
}

bool UHGraphic::CreateLogicalDevice()
{
	// graphic queue
	float QueuePriority = 1.0f;
	VkDeviceQueueCreateInfo GraphicQueueCreateInfo{};
	GraphicQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	GraphicQueueCreateInfo.queueFamilyIndex = QueueFamily.GraphicsFamily.value();
	GraphicQueueCreateInfo.queueCount = 1;
	GraphicQueueCreateInfo.pQueuePriorities = &QueuePriority;

	// compute queue
	VkDeviceQueueCreateInfo ComputeQueueCreateInfo{};
	ComputeQueueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
	ComputeQueueCreateInfo.queueFamilyIndex = QueueFamily.ComputesFamily.value();
	ComputeQueueCreateInfo.queueCount = 1;
	ComputeQueueCreateInfo.pQueuePriorities = &QueuePriority;

	std::vector<VkDeviceQueueCreateInfo> QueueCreateInfo = { GraphicQueueCreateInfo, ComputeQueueCreateInfo };

	// define features, enable what I need in UH
	VkPhysicalDeviceFeatures DeviceFeatures{};
	DeviceFeatures.samplerAnisotropy = true;
	DeviceFeatures.fullDrawIndexUint32 = true;
	DeviceFeatures.textureCompressionBC = true;

	// check ray tracing & AS & ray query feature
	VkPhysicalDeviceAccelerationStructureFeaturesKHR ASFeatures{};
	ASFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR;

	VkPhysicalDeviceRayQueryFeaturesKHR RQFeatures{};
	RQFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR;
	RQFeatures.pNext = &ASFeatures;

	VkPhysicalDeviceRayTracingPipelineFeaturesKHR RTFeatures{};
	RTFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR;
	RTFeatures.pNext = &RQFeatures;

	// 1_2 runtime features
	VkPhysicalDeviceVulkan12Features Vk12Features{};
	Vk12Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
	Vk12Features.pNext = &RTFeatures;

	// 1_3 features
	VkPhysicalDeviceVulkan13Features VK13Features{};
	VK13Features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
	VK13Features.pNext = &Vk12Features;

	VkPhysicalDeviceRobustness2FeaturesEXT RobustnessFeatures{};
	RobustnessFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT;
	RobustnessFeatures.pNext = &VK13Features;

	// mesh shader feature check
	VkPhysicalDeviceMeshShaderFeaturesEXT MeshShaderFeatures{};
	MeshShaderFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_FEATURES_EXT;
	MeshShaderFeatures.pNext = &RobustnessFeatures;

	// predication feature check
	VkPhysicalDeviceConditionalRenderingFeaturesEXT ConditionalRenderingFeatures{};
	ConditionalRenderingFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CONDITIONAL_RENDERING_FEATURES_EXT;
	ConditionalRenderingFeatures.pNext = &MeshShaderFeatures;

	// device feature needs to assign in fature 2
	VkPhysicalDeviceFeatures2 PhyFeatures{};
	PhyFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
	PhyFeatures.features = DeviceFeatures;
	PhyFeatures.pNext = &ConditionalRenderingFeatures;

	vkGetPhysicalDeviceFeatures2(PhysicalDevice, &PhyFeatures);

	// feature support check
	{
		if (!RTFeatures.rayTracingPipeline)
		{
			UHE_LOG(L"Ray tracing pipeline not supported. System won't render ray tracing effects.\n");
			bEnableRayTracing = false;
		}

		// check 24-bit depth format
		VkFormatProperties FormatProps{};
		vkGetPhysicalDeviceFormatProperties(PhysicalDevice, VK_FORMAT_X8_D24_UNORM_PACK32, &FormatProps);
		bSupport24BitDepth = FormatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;

		// mesh shader support, disable others usage for now
		bSupportMeshShader = MeshShaderFeatures.meshShader;
		MeshShaderFeatures.multiviewMeshShader = false;
		MeshShaderFeatures.primitiveFragmentShadingRateMeshShader = false;
	}

	// get RT feature props
	VkPhysicalDeviceRayTracingPipelinePropertiesKHR RTPropsFeatures{};
	RTPropsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR;
	
	// get mesh shader props
	VkPhysicalDeviceMeshShaderPropertiesEXT MeshPropsFeatures{};
	MeshPropsFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MESH_SHADER_PROPERTIES_EXT;
	RTPropsFeatures.pNext = &MeshPropsFeatures;

	VkPhysicalDeviceProperties2 Props2{};
	Props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
	Props2.pNext = &RTPropsFeatures;

	vkGetPhysicalDeviceProperties2(PhysicalDevice, &Props2);
	ShaderRecordSize = RTPropsFeatures.shaderGroupHandleSize;
	GPUTimeStampPeriod = Props2.properties.limits.timestampPeriod;

	// device create info, pass raytracing feature to pNext of create info
	VkDeviceCreateInfo CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	CreateInfo.pQueueCreateInfos = QueueCreateInfo.data();
	CreateInfo.queueCreateInfoCount = static_cast<uint32_t>(QueueCreateInfo.size());
	CreateInfo.pEnabledFeatures = nullptr;
	CreateInfo.pNext = &PhyFeatures;

	uint32_t ExtensionCount = static_cast<uint32_t>(DeviceExtensions.size());
	if (ExtensionCount > 0)
	{
		CreateInfo.enabledExtensionCount = static_cast<uint32_t>(DeviceExtensions.size());
		CreateInfo.ppEnabledExtensionNames = DeviceExtensions.data();
	}

	// set up validation layer if it's debugging
#if WITH_EDITOR
	if (bUseValidationLayers && CheckValidationLayerSupport())
	{
		CreateInfo.enabledLayerCount = static_cast<uint32_t>(ValidationLayers.size());
		CreateInfo.ppEnabledLayerNames = ValidationLayers.data();
	}
#endif

	VkResult CreateResult = vkCreateDevice(PhysicalDevice, &CreateInfo, nullptr, &LogicalDevice);
	if (CreateResult != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create Vulkan device!\n");
		return false;
	}

#if WITH_EDITOR
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_DEVICE, (uint64_t)LogicalDevice, "MainLogicalDevice");
	// some debug name must be set after logical device creation
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_INSTANCE, (uint64_t)VulkanInstance, "MainVulkanInstance");
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_SURFACE_KHR, (uint64_t)MainSurface, "MainWindowSurface");
#endif

	// finally, get both graphics and computes queue
	vkGetDeviceQueue(LogicalDevice, QueueFamily.GraphicsFamily.value(), 0, &GraphicsQueue);

	return true;
}

bool UHGraphic::CreateWindowSurface()
{
	// pass window handle and create surface
	VkWin32SurfaceCreateInfoKHR CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
	CreateInfo.hwnd = WindowCache;
	CreateInfo.hinstance = GetModuleHandle(nullptr);

	if (vkCreateWin32SurfaceKHR(VulkanInstance, &CreateInfo, nullptr, &MainSurface) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create window surface!.\n");
		return false;
	}

	return true;
}

UHSwapChainDetails UHGraphic::QuerySwapChainSupport(VkPhysicalDevice InDevice) const
{
	// query swap chain details from chosen physical device, so we can know what format is supported
	UHSwapChainDetails Details;

	VkSurfaceFullScreenExclusiveInfoEXT FullScreenInfo{};
	FullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;

	// prepare win32 full screen for capabilities
	VkSurfaceFullScreenExclusiveWin32InfoEXT Win32FullScreenInfo{};
	Win32FullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
	Win32FullScreenInfo.hmonitor = MonitorFromWindow(WindowCache, MONITOR_DEFAULTTOPRIMARY);

	// try to get surface 2
	VkPhysicalDeviceSurfaceInfo2KHR Surface2Info{};
	Surface2Info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR;
	Surface2Info.surface = MainSurface;
	Surface2Info.pNext = &Win32FullScreenInfo;
	
	Details.Capabilities2.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR;
	vkGetPhysicalDeviceSurfaceCapabilities2KHR(InDevice, &Surface2Info, &Details.Capabilities2);

	// find format
	uint32_t FormatCount;
	vkGetPhysicalDeviceSurfaceFormats2KHR(InDevice, &Surface2Info, &FormatCount, nullptr);

	if (FormatCount != 0)
	{
		Details.Formats2.resize(FormatCount);
		for (uint32_t Idx = 0; Idx < FormatCount; Idx++)
		{
			Details.Formats2[Idx].sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
		}

		vkGetPhysicalDeviceSurfaceFormats2KHR(InDevice, &Surface2Info, &FormatCount, Details.Formats2.data());
	}

	// find present mode
	uint32_t PresentModeCount;
	GGetSurfacePresentModes2Callback(InDevice, &Surface2Info, &PresentModeCount, nullptr);

	if (PresentModeCount != 0)
	{
		Details.PresentModes.resize(PresentModeCount);
		GGetSurfacePresentModes2Callback(InDevice, &Surface2Info, &PresentModeCount, Details.PresentModes.data());
	}

	return Details;
}

VkSurfaceFormatKHR ChooseSwapChainFormat(const UHSwapChainDetails& Details, bool bEnableHDR, bool& bSupportHDR)
{
	std::optional<VkSurfaceFormatKHR> HDR10Format;
	VkSurfaceFormatKHR DesiredFormat{};

	// for now, choose non linear SRGB format
	// even use R10G10B10A2_UNORM, I need linear to gamma conversion, so just let it be converted by hardware
	for (const auto& AvailableFormat : Details.Formats2)
	{
		if (AvailableFormat.surfaceFormat.format == VK_FORMAT_B8G8R8A8_UNORM && AvailableFormat.surfaceFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			DesiredFormat = AvailableFormat.surfaceFormat;
		}
		else if (AvailableFormat.surfaceFormat.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && AvailableFormat.surfaceFormat.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT)
		{
			HDR10Format = AvailableFormat.surfaceFormat;
			bSupportHDR = true;
		}
	}

	// return hdr format if it's supported and enabled
	if (HDR10Format.has_value() && bEnableHDR)
	{
		return HDR10Format.value();
	}

	return DesiredFormat;
}

VkPresentModeKHR ChooseSwapChainMode(const UHSwapChainDetails& Details, bool bUseVsync)
{
	// VK_PRESENT_MODE_IMMEDIATE_KHR: Fastest but might have screen tearing
	// VK_PRESENT_MODE_FIFO_KHR: vertical blank

	// select the mode based on Vsync setting and go for mailbox if possible
	bool bVsyncSupported = false;

	for (const auto& AvailablePresentMode : Details.PresentModes)
	{
		if (AvailablePresentMode == VK_PRESENT_MODE_FIFO_KHR)
		{
			bVsyncSupported = true;
		}
	}

	if (bUseVsync && bVsyncSupported)
	{
		return VK_PRESENT_MODE_FIFO_KHR;
	}

	return VK_PRESENT_MODE_IMMEDIATE_KHR;
}

VkExtent2D ChooseSwapChainExtent(const UHSwapChainDetails& Details, HWND WindowCache)
{
	// return size directly if it's already acquired by Vulkan
	if (Details.Capabilities2.surfaceCapabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
	{
		return Details.Capabilities2.surfaceCapabilities.currentExtent;
	}

	RECT Rect;
	int32_t Width = 0;
	int32_t Height = 0;

	if (GetWindowRect(WindowCache, &Rect))
	{
		Width = Rect.right - Rect.left;
		Height = Rect.bottom - Rect.top;
	}

	// ensure to clamp the resolution size
	VkExtent2D ActualExtent = { static_cast<uint32_t>(Width), static_cast<uint32_t>(Height) };
	ActualExtent.width = std::clamp(ActualExtent.width, Details.Capabilities2.surfaceCapabilities.minImageExtent.width, Details.Capabilities2.surfaceCapabilities.maxImageExtent.width);
	ActualExtent.height = std::clamp(ActualExtent.height, Details.Capabilities2.surfaceCapabilities.minImageExtent.height, Details.Capabilities2.surfaceCapabilities.maxImageExtent.height);

	return ActualExtent;
}

void UHGraphic::ClearSwapChain()
{
	for (size_t Idx = 0; Idx < SwapChainFrameBuffer.size(); Idx++)
	{
		RequestReleaseRT(SwapChainRT[Idx]);
		vkDestroyFramebuffer(LogicalDevice, SwapChainFrameBuffer[Idx], nullptr);
	}

	vkDestroyRenderPass(LogicalDevice, SwapChainRenderPass, nullptr);
	vkDestroySwapchainKHR(LogicalDevice, SwapChain, nullptr);
	SwapChainRT.clear();
	SwapChainFrameBuffer.clear();
}

bool UHGraphic::ResizeSwapChain()
{
	// wait device before resize
	WaitGPU();
	ClearSwapChain();

	return CreateSwapChain();
}

void UHGraphic::ToggleFullScreen(bool InFullScreenState)
{
	if (bIsFullScreen == InFullScreenState)
	{
		return;
	}

	// wait before toggling
	WaitGPU();
	bIsFullScreen = !bIsFullScreen;
}

void UHGraphic::WaitGPU()
{
	vkDeviceWaitIdle(LogicalDevice);
}

// create render pass, imageless
UHRenderPassObject UHGraphic::CreateRenderPass(UHTransitionInfo InTransitionInfo) const
{
	std::vector<UHTexture*> Texture{};
	return CreateRenderPass(Texture, InTransitionInfo);
}

// create render pass, single Texture
UHRenderPassObject UHGraphic::CreateRenderPass(UHTexture* InTexture, UHTransitionInfo InTransitionInfo, UHTexture* InDepth) const
{
	std::vector<UHTexture*> Texture{ InTexture };
	return CreateRenderPass(Texture, InTransitionInfo, InDepth);
}

// create render pass, depth only
UHRenderPassObject UHGraphic::CreateRenderPass(UHTransitionInfo InTransitionInfo, UHTexture* InDepthTexture) const
{
	return CreateRenderPass(std::vector<UHTexture*>(), InTransitionInfo, InDepthTexture);
}

// create render pass, multiple formats are possible
UHRenderPassObject UHGraphic::CreateRenderPass(std::vector<UHTexture*> InTextures, UHTransitionInfo InTransitionInfo, UHTexture* InDepth) const
{
	UHRenderPassObject ResultRenderPass{};
	VkRenderPass NewRenderPass = nullptr;
	uint32_t RTCount = static_cast<uint32_t>(InTextures.size());
	bool bHasDepthAttachment = (InDepth != nullptr);

	std::vector<VkAttachmentDescription> ColorAttachments;
	std::vector<VkAttachmentReference> ColorAttachmentRefs;

	if (RTCount > 0)
	{
		ColorAttachments.resize(RTCount);
		ColorAttachmentRefs.resize(RTCount);

		for (size_t Idx = 0; Idx < RTCount; Idx++)
		{
			// create color attachment, this part desides how RT is going to be used
			VkAttachmentDescription ColorAttachment{};
			ColorAttachment.format = GetVulkanFormat(InTextures[Idx]->GetFormat());
			ColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
			ColorAttachment.loadOp = InTransitionInfo.LoadOp;
			ColorAttachment.storeOp = InTransitionInfo.StoreOp;
			ColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			ColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
			ColorAttachment.initialLayout = InTransitionInfo.InitialLayout;
			ColorAttachment.finalLayout = InTransitionInfo.FinalLayout;
			ColorAttachments[Idx] = ColorAttachment;

			// define attachment ref cor color attachment
			VkAttachmentReference ColorAttachmentRef{};
			ColorAttachmentRef.attachment = static_cast<uint32_t>(Idx);
			ColorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
			ColorAttachmentRefs[Idx] = ColorAttachmentRef;
			ResultRenderPass.ColorTextures.push_back(InTextures[Idx]);
		}
	}

	// subpass desc
	VkSubpassDescription Subpass{};
	Subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	Subpass.colorAttachmentCount = RTCount;
	Subpass.pColorAttachments = (RTCount > 0) ? ColorAttachmentRefs.data() : nullptr;

	// consider depth attachment
	VkAttachmentDescription DepthAttachment{};
	VkAttachmentReference DepthAttachmentRef{};
	if (bHasDepthAttachment)
	{
		DepthAttachment.format = GetVulkanFormat(InDepth->GetFormat());
		DepthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
		DepthAttachment.loadOp = InTransitionInfo.DepthLoadOp;
		DepthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		DepthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		DepthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		DepthAttachment.initialLayout = (DepthAttachment.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
		DepthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
		
		DepthAttachmentRef.attachment = RTCount;
		DepthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		Subpass.pDepthStencilAttachment = &DepthAttachmentRef;
		ColorAttachments.push_back(DepthAttachment);
		ResultRenderPass.DepthTexture = InDepth;
		ResultRenderPass.FinalDepthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
	}

	// setup subpass dependency, similar to resource transition
	VkSubpassDependency Dependency{};
	Dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
	Dependency.dstSubpass = 0;
	Dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	Dependency.srcAccessMask = 0;
	Dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	Dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

	if (bHasDepthAttachment)
	{
		// adjust dependency
		Dependency.srcStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		Dependency.dstStageMask |= VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
		Dependency.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
	}

	// collect the desc above 
	VkRenderPassCreateInfo RenderPassInfo{};
	RenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
	RenderPassInfo.attachmentCount = (bHasDepthAttachment) ? RTCount + 1 : RTCount;
	RenderPassInfo.pAttachments = ColorAttachments.data();
	RenderPassInfo.subpassCount = 1;
	RenderPassInfo.pSubpasses = &Subpass;
	RenderPassInfo.dependencyCount = 1;
	RenderPassInfo.pDependencies = &Dependency;

	if (vkCreateRenderPass(LogicalDevice, &RenderPassInfo, nullptr, &NewRenderPass) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create render pass\n");
	}

	ResultRenderPass.RenderPass = NewRenderPass;
	ResultRenderPass.FinalLayout = InTransitionInfo.FinalLayout;

#if WITH_EDITOR
	std::string ObjName;
	if (InTextures.size() > 0)
	{
		ObjName = InTextures[0]->GetName();
	}
	else if (InDepth != nullptr)
	{
		ObjName = InDepth->GetName();
	}
	ObjName += "_RenderPass";

	SetDebugUtilsObjectName(VK_OBJECT_TYPE_RENDER_PASS, (uint64_t)NewRenderPass, ObjName);
#endif

	return ResultRenderPass;
}

VkFramebuffer UHGraphic::CreateFrameBuffer(UHRenderTexture* InRT, VkRenderPass InRenderPass, VkExtent2D InExtent, int32_t Layers) const
{
	std::vector<UHRenderTexture*> RTs{ InRT };
	return CreateFrameBuffer(RTs, InRenderPass, InExtent, Layers);
}

VkFramebuffer UHGraphic::CreateFrameBuffer(std::vector<UHRenderTexture*> InRTs, VkRenderPass InRenderPass, VkExtent2D InExtent, int32_t Layers) const
{
	VkFramebuffer NewFrameBuffer = nullptr;

	// create frame buffer
	VkFramebufferCreateInfo FramebufferInfo{};
	FramebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
	FramebufferInfo.renderPass = InRenderPass;
	FramebufferInfo.attachmentCount = static_cast<uint32_t>(InRTs.size());

	std::string DebugName;
	std::vector<VkImageView> Views(InRTs.size());
	for (size_t Idx = 0; Idx < InRTs.size(); Idx++)
	{
		Views[Idx] = InRTs[Idx]->GetImageView();
		DebugName += "_" + InRTs[Idx]->GetName();
	}

	FramebufferInfo.pAttachments = Views.data();
	FramebufferInfo.width = InExtent.width;
	FramebufferInfo.height = InExtent.height;
	FramebufferInfo.layers = Layers;

	if (vkCreateFramebuffer(LogicalDevice, &FramebufferInfo, nullptr, &NewFrameBuffer) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create framebuffer!\n");
	}

#if WITH_EDITOR
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_FRAMEBUFFER, (uint64_t)NewFrameBuffer, "FrameBuffer" + DebugName);
#endif

	return NewFrameBuffer;
}

UHGPUQuery* UHGraphic::RequestGPUQuery(uint32_t Count, VkQueryType QueueType)
{
	UniquePtr<UHGPUQuery> NewQuery = MakeUnique<UHGPUQuery>();
	NewQuery->SetGfxCache(this);
	NewQuery->CreateQueryPool(Count, QueueType);

	QueryPools.push_back(std::move(NewQuery));
	return QueryPools.back().get();
}

void UHGraphic::RequestReleaseGPUQuery(UHGPUQuery* InQuery)
{
	int32_t Idx = UHUtilities::FindIndex<UHGPUQuery>(QueryPools, *InQuery);
	if (Idx == UHINDEXNONE)
	{
		return;
	}

	QueryPools[Idx]->Release();
	QueryPools[Idx].reset();
	UHUtilities::RemoveByIndex(QueryPools, Idx);
}

// request render texture, this also sets device info to it
UHRenderTexture* UHGraphic::RequestRenderTexture(std::string InName, VkExtent2D InExtent, UHTextureFormat InFormat, bool bIsReadWrite, bool bUseMipmap)
{
	return RequestRenderTexture(InName, nullptr, InExtent, InFormat, bIsReadWrite, bUseMipmap);
}

// request render texture, this also sets device info to it
UHRenderTexture* UHGraphic::RequestRenderTexture(std::string InName, VkImage InImage, VkExtent2D InExtent
	, UHTextureFormat InFormat, bool bIsReadWrite, bool bUseMipmap)
{
	// return cached if there is already one
	UniquePtr<UHRenderTexture> NewRT = MakeUnique<UHRenderTexture>(InName, InExtent, InFormat, bIsReadWrite, bUseMipmap);
	NewRT->SetImage(InImage);

	int32_t Idx = UHUtilities::FindIndex<UHRenderTexture>(RTPools, *NewRT.get());
	if (Idx != UHINDEXNONE)
	{
		return RTPools[Idx].get();
	}

	NewRT->SetGfxCache(this);
	NewRT->SetImage(InImage);

	if (NewRT->CreateRT())
	{
		RTPools.push_back(std::move(NewRT));
		return RTPools.back().get();
	}

	return nullptr;
}

// request release RT, this could be used during resizing
void UHGraphic::RequestReleaseRT(UHRenderTexture* InRT)
{
	int32_t Idx = UHUtilities::FindIndex<UHRenderTexture>(RTPools, *InRT);
	if (Idx == UHINDEXNONE)
	{
		return;
	}

	RTPools[Idx]->Release();
	RTPools[Idx].reset();
	UHUtilities::RemoveByIndex(RTPools, Idx);
}

UHTexture2D* UHGraphic::RequestTexture2D(UniquePtr<UHTexture2D>& LoadedTex, bool bUseSharedMemory)
{
	// return cached if there is already one
	int32_t Idx = UHUtilities::FindIndex<UHTexture2D>(Texture2DPools, *LoadedTex.get());
	if (Idx != UHINDEXNONE)
	{
		return Texture2DPools[Idx].get();
	}

	LoadedTex->SetGfxCache(this);

	if (LoadedTex->CreateTexture(bUseSharedMemory))
	{
		Texture2DPools.push_back(std::move(LoadedTex));
		return Texture2DPools.back().get();
	}

	return nullptr;
}

void UHGraphic::RequestReleaseTexture2D(UHTexture2D* InTex)
{
	int32_t Idx = UHUtilities::FindIndex<UHTexture2D>(Texture2DPools, *InTex);
	if (Idx == UHINDEXNONE)
	{
		return;
	}

	Texture2DPools[Idx]->ReleaseCPUTextureData();
	Texture2DPools[Idx]->Release();
	Texture2DPools[Idx].reset();
	UHUtilities::RemoveByIndex(Texture2DPools, Idx);
}

bool AreTextureSliceConsistent(std::string InArrayName, std::vector<UHTexture2D*> InTextures)
{
	if (InTextures.size() == 0)
	{
		return false;
	}

	bool bIsConsistent = true;
	for (size_t Idx = 0; Idx < InTextures.size(); Idx++)
	{
		for (size_t Jdx = 0; Jdx < InTextures.size(); Jdx++)
		{
			if (Idx != Jdx)
			{
				bool bIsFormatMatched = (InTextures[Idx]->GetFormat() == InTextures[Jdx]->GetFormat());
				bool bIsExtentMatched = (InTextures[Idx]->GetExtent().width == InTextures[Jdx]->GetExtent().width
					&& InTextures[Idx]->GetExtent().height == InTextures[Jdx]->GetExtent().height);

				if (!bIsFormatMatched)
				{
					UHE_LOG(L"Inconsistent texture slice format detected in array " + UHUtilities::ToStringW(InArrayName) + L"\n");
				}

				if (!bIsExtentMatched)
				{
					UHE_LOG(L"Inconsistent texture slice extent detected in array " + UHUtilities::ToStringW(InArrayName) + L"\n");
				}

				bIsConsistent &= bIsFormatMatched;
				bIsConsistent &= bIsExtentMatched;
			}
		}
	}

	return bIsConsistent;
}

UHTextureCube* UHGraphic::RequestTextureCube(std::string InName, std::vector<UHTexture2D*> InTextures)
{
	if (InTextures.size() != 6)
	{
		// for now the cube is consisted by texture slices, can't do it without any slices
		UHE_LOG(L"Number of texture slices is not 6!\n");
		return nullptr;
	}

	// consistent check between slices
	bool bConsistent = AreTextureSliceConsistent(InName, InTextures);
	if (!bConsistent)
	{
		return nullptr;
	}

	UniquePtr<UHTextureCube> NewCube = MakeUnique<UHTextureCube>(InName, InTextures[0]->GetExtent(), InTextures[0]->GetFormat(), InTextures[0]->GetTextureSettings());
	int32_t Idx = UHUtilities::FindIndex<UHTextureCube>(TextureCubePools, *NewCube.get());
	if (Idx != UHINDEXNONE)
	{
		return TextureCubePools[Idx].get();
	}

	NewCube->SetGfxCache(this);

	if (NewCube->CreateCube(InTextures))
	{
		TextureCubePools.push_back(std::move(NewCube));
		return TextureCubePools.back().get();
	}

	return nullptr;
}

// light version of texture cube request, usually called when an existed asset is imported
UHTextureCube* UHGraphic::RequestTextureCube(UniquePtr<UHTextureCube>& LoadedCube)
{
	int32_t Idx = UHUtilities::FindIndex<UHTextureCube>(TextureCubePools, *LoadedCube.get());
	if (Idx != UHINDEXNONE)
	{
		return TextureCubePools[Idx].get();
	}

	LoadedCube->SetGfxCache(this);

	if (LoadedCube->CreateCube())
	{
		TextureCubePools.push_back(std::move(LoadedCube));
		return TextureCubePools.back().get();
	}

	return nullptr;
}

void UHGraphic::RequestReleaseTextureCube(UHTextureCube* InCube)
{
	int32_t Idx = UHUtilities::FindIndex<UHTextureCube>(TextureCubePools, *InCube);
	if (Idx == UHINDEXNONE)
	{
		return;
	}

	TextureCubePools[Idx]->ReleaseCPUData();
	TextureCubePools[Idx]->Release();
	TextureCubePools[Idx].reset();
	UHUtilities::RemoveByIndex(TextureCubePools, Idx);
}

// request material without any import
// mostly used for engine materials
UHMaterial* UHGraphic::RequestMaterial()
{
	UniquePtr<UHMaterial> NewMat = MakeUnique<UHMaterial>();
	MaterialPools.push_back(std::move(NewMat));
	return MaterialPools.back().get();
}

UHMaterial* UHGraphic::RequestMaterial(std::filesystem::path InPath)
{
	// for now this function just allocate a new material
	// might have other usage in the future
	UniquePtr<UHMaterial> NewMat = MakeUnique<UHMaterial>();
	if (NewMat->Import(InPath))
	{
		NewMat->SetGfxCache(this);
		NewMat->PostImport();
		MaterialPools.push_back(std::move(NewMat));
		return MaterialPools.back().get();
	}

	return nullptr;
}

void UHGraphic::RequestReleaseMaterial(UHMaterial* InMat)
{
	int32_t Idx = UHUtilities::FindIndex<UHMaterial>(MaterialPools, *InMat);
	if (Idx == UHINDEXNONE)
	{
		return;
	}

	MaterialPools[Idx].reset();
	UHUtilities::RemoveByIndex(MaterialPools, Idx);
}

UniquePtr<UHAccelerationStructure> UHGraphic::RequestAccelerationStructure()
{
	UniquePtr<UHAccelerationStructure> NewAS = MakeUnique<UHAccelerationStructure>();
	NewAS->SetGfxCache(this);

	return std::move(NewAS);
}

bool UHGraphic::CreateShaderModule(UniquePtr<UHShader>& NewShader, std::filesystem::path OutputShaderPath)
{
	// setup input shader path, read from compiled shader
	if (!std::filesystem::exists(OutputShaderPath))
	{
		UHE_LOG(L"Failed to load shader " + OutputShaderPath.wstring() + L"!\n");
		return false;
	}

	// load shader code
	std::ifstream FileIn(OutputShaderPath.string(), std::ios::ate | std::ios::binary);

	// get file size
	size_t FileSize = static_cast<size_t>(FileIn.tellg());
	std::vector<char> ShaderCode(FileSize);

	// read data
	FileIn.seekg(0);
	FileIn.read(ShaderCode.data(), FileSize);

	FileIn.close();

	// start to create shader module
	VkShaderModuleCreateInfo CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
	CreateInfo.codeSize = ShaderCode.size();
	CreateInfo.pCode = reinterpret_cast<const uint32_t*>(ShaderCode.data());

	if (!NewShader->Create(CreateInfo))
	{
		return false;
	}

	return true;
}

uint32_t UHGraphic::RequestShader(std::string InShaderName, std::filesystem::path InSource, std::string EntryName, std::string ProfileName
	, std::vector<std::string> InMacro)
{
	UniquePtr<UHShader> NewShader = MakeUnique<UHShader>(InShaderName, InSource, EntryName, ProfileName, InMacro);
	NewShader->SetGfxCache(this);

	// early return if it's exist in pool
	int32_t PoolIdx = UHUtilities::FindIndex<UHShader>(ShaderPools, *NewShader.get());
	if (PoolIdx != UHINDEXNONE)
	{
		return ShaderPools[PoolIdx]->GetId();
	}

	// ensure the shader is compiled (debug only)
#if WITH_EDITOR
	AssetManagerInterface->CompileShader(InShaderName, InSource, EntryName, ProfileName, InMacro);
#endif

	// get macro hash name
	size_t MacroHash = UHUtilities::ShaderDefinesToHash(InMacro);
	std::string MacroHashName = (MacroHash != 0) ? "_" + std::to_string(MacroHash) : "";

	// find origin path and try to preserve file structure
	std::string OriginSubpath = UHAssetPath::GetShaderOriginSubpath(InSource);

	// setup input shader path, read from compiled shader
	std::filesystem::path OutputShaderPath = GShaderAssetFolder + OriginSubpath + InShaderName + MacroHashName + GShaderAssetExtension;
	if (!CreateShaderModule(NewShader, OutputShaderPath))
	{
		return -1;
	}

	ShaderPools.push_back(std::move(NewShader));
	return ShaderPools.back()->GetId();
}

// request shader for material
uint32_t UHGraphic::RequestMaterialShader(std::string InShaderName, std::filesystem::path InSource, std::string EntryName, std::string ProfileName
	, UHMaterialCompileData InData, std::vector<std::string> InMacro)
{
	// macro hash
	size_t MacroHash = UHUtilities::ShaderDefinesToHash(InMacro);
	std::string MacroHashName = (MacroHash != 0) ? "_" + std::to_string(MacroHash) : "";

	UHMaterial* InMat = InData.MaterialCache;
	const std::string OriginSubpath = UHAssetPath::GetMaterialOriginSubpath(InData.MaterialCache->GetPath());
	std::string OutName = UHAssetPath::FormatMaterialShaderOutputPath("", InData.MaterialCache->GetSourcePath(), InShaderName, MacroHashName);
	std::filesystem::path OutputShaderPath = GShaderAssetFolder + OutName + GShaderAssetExtension;

	// if it's a release build, and there is no material shader for it, use a fallback one
	if (GIsShipping && !std::filesystem::exists(OutputShaderPath))
	{
		InShaderName = "FallbackPixelShader";
		EntryName = "FallbackPS";
		InSource = GRawShaderPath + InShaderName;
	}

	UniquePtr<UHShader> NewShader = MakeUnique<UHShader>(OutName, InSource, EntryName, ProfileName, true, InMacro);
	NewShader->SetGfxCache(this);

	// early return if it's exist in pool and does not need recompile
	int32_t PoolIdx = UHUtilities::FindIndex<UHShader>(ShaderPools, *NewShader.get());
	if (PoolIdx != UHINDEXNONE)
	{
		return ShaderPools[PoolIdx]->GetId();
	}

	// almost the same as common shader flow, but this will go through HLSL translator instead
	// only compile it when the compile flag or version is matched
#if WITH_EDITOR
	AssetManagerInterface->TranslateHLSL(InShaderName, InSource, EntryName, ProfileName, InData, InMacro, OutputShaderPath);
#endif

	if (!CreateShaderModule(NewShader, OutputShaderPath))
	{
		return -1;
	}

	ShaderPools.push_back(std::move(NewShader));
	return ShaderPools.back()->GetId();
}

void UHGraphic::RequestReleaseShader(uint32_t InShaderID)
{
	// check if the object still exists before release
	if (const UHShader* InShader = SafeGetObjectFromTable<const UHShader>(InShaderID))
	{
		int32_t Idx = UHUtilities::FindIndex(ShaderPools, *InShader);
		if (Idx != UHINDEXNONE)
		{
			ShaderPools[Idx]->Release();
			ShaderPools[Idx].reset();
			UHUtilities::RemoveByIndex(ShaderPools, Idx);
		}
	}
}

// request a Graphic State object and return
UHGraphicState* UHGraphic::RequestGraphicState(UHRenderPassInfo InInfo)
{
	std::unique_lock<std::mutex> Lock(Mutex);
	UniquePtr<UHGraphicState> NewState = MakeUnique<UHGraphicState>(InInfo);

	// check cached state first
	int32_t Idx = UHUtilities::FindIndex(StatePools, *NewState.get());
	if (Idx != UHINDEXNONE)
	{
		StatePools[Idx]->IncreaseRefCount();
		return StatePools[Idx].get();
	}

	NewState->SetGfxCache(this);
	
	if (!NewState->CreateState(InInfo))
	{
		return nullptr;
	}

	NewState->IncreaseRefCount();
	StatePools.push_back(std::move(NewState));
	return StatePools.back().get();
}

void UHGraphic::RequestReleaseGraphicState(UHGraphicState* InState)
{
	std::unique_lock<std::mutex> Lock(Mutex);
	if (InState == nullptr)
	{
		return;
	}

	int32_t Idx = UHUtilities::FindIndex(StatePools, *InState);
	if (Idx != UHINDEXNONE)
	{
		// since a graphic state could be referenced by multiple shader record
		// only release and remove it from the pool when ref count = 0
		InState->DecreaseRefCount();
		if (InState->GetRefCount() == 0)
		{
			StatePools[Idx]->Release();
			StatePools[Idx].reset();
			UHUtilities::RemoveByIndex(StatePools, Idx);
		}
	}
}

UHGraphicState* UHGraphic::RequestRTState(UHRayTracingInfo InInfo)
{
	std::unique_lock<std::mutex> Lock(Mutex);
	UniquePtr<UHGraphicState> NewState = MakeUnique<UHGraphicState>(InInfo);

	// check cached state first
	int32_t Idx = UHUtilities::FindIndex(StatePools, *NewState.get());
	if (Idx != UHINDEXNONE)
	{
		StatePools[Idx]->IncreaseRefCount();
		return StatePools[Idx].get();
	}

	NewState->SetGfxCache(this);

	if (!NewState->CreateState(InInfo))
	{
		return nullptr;
	}

	NewState->IncreaseRefCount();
	StatePools.push_back(std::move(NewState));
	return StatePools.back().get();
}

UHComputeState* UHGraphic::RequestComputeState(UHComputePassInfo InInfo)
{
	std::unique_lock<std::mutex> Lock(Mutex);
	UniquePtr<UHComputeState> NewState = MakeUnique<UHComputeState>(InInfo);

	// check cached state first
	int32_t Idx = UHUtilities::FindIndex(StatePools, *NewState.get());
	if (Idx != UHINDEXNONE)
	{
		StatePools[Idx]->IncreaseRefCount();
		return StatePools[Idx].get();
	}

	NewState->SetGfxCache(this);

	if (!NewState->CreateState(InInfo))
	{
		return nullptr;
	}

	NewState->IncreaseRefCount();
	StatePools.push_back(std::move(NewState));
	return StatePools.back().get();
}

UHSampler* UHGraphic::RequestTextureSampler(UHSamplerInfo InInfo)
{
	UniquePtr<UHSampler> NewSampler = MakeUnique<UHSampler>(InInfo);

	int32_t Idx = UHUtilities::FindIndex(SamplerPools, *NewSampler.get());
	if (Idx != UHINDEXNONE)
	{
		return SamplerPools[Idx].get();
	}

	// create new one if cache fails
	NewSampler->SetGfxCache(this);

	if (!NewSampler->Create())
	{
		return nullptr;
	}

	SamplerPools.push_back(std::move(NewSampler));
	return SamplerPools.back().get();
}

VkInstance UHGraphic::GetInstance() const
{
	return VulkanInstance;
}

VkPhysicalDevice UHGraphic::GetPhysicalDevice() const
{
	return PhysicalDevice;
}

VkDevice UHGraphic::GetLogicalDevice() const
{
	return LogicalDevice;
}

UHQueueFamily UHGraphic::GetQueueFamily() const
{
	return QueueFamily;
}

VkSwapchainKHR UHGraphic::GetSwapChain() const
{
	return SwapChain;
}

UHRenderTexture* UHGraphic::GetSwapChainRT(int32_t ImageIdx) const
{
	return SwapChainRT[ImageIdx];
}

VkFramebuffer UHGraphic::GetSwapChainBuffer(int32_t ImageIdx) const
{
	return SwapChainFrameBuffer[ImageIdx];
}

uint32_t UHGraphic::GetSwapChainCount() const
{
	return static_cast<uint32_t>(SwapChainRT.size());
}

VkExtent2D UHGraphic::GetSwapChainExtent() const
{
	return SwapChainRT[0]->GetExtent();
}

VkFormat UHGraphic::GetSwapChainFormat() const
{
	return GetVulkanFormat(SwapChainRT[0]->GetFormat());
}

VkRenderPass UHGraphic::GetSwapChainRenderPass() const
{
	return SwapChainRenderPass;
}

VkPhysicalDeviceMemoryProperties UHGraphic::GetDeviceMemProps() const
{
	return PhysicalDeviceMemoryProperties;
}

uint32_t UHGraphic::GetShaderRecordSize() const
{
	return ShaderRecordSize;
}

float UHGraphic::GetGPUTimeStampPeriod() const
{
	return GPUTimeStampPeriod;
}

bool UHGraphic::IsDepthPrePassEnabled() const
{
	return bEnableDepthPrePass;
}

bool UHGraphic::IsRayTracingEnabled() const
{
	return bEnableRayTracing;
}

bool UHGraphic::IsDebugLayerEnabled() const
{
	return bUseValidationLayers;
}

bool UHGraphic::IsHDRAvailable() const
{
	return bSupportHDR && ConfigInterface->RenderingSetting().bEnableHDR;
}

bool UHGraphic::Is24BitDepthSupported() const
{
	return bSupport24BitDepth;
}

bool UHGraphic::IsMeshShaderSupported() const
{
	return bSupportMeshShader;
}

std::vector<UHSampler*> UHGraphic::GetSamplers() const
{
	std::vector<UHSampler*> Samplers(SamplerPools.size());
	for (size_t Idx = 0; Idx < Samplers.size(); Idx++)
	{
		Samplers[Idx] = SamplerPools[Idx].get();
	}

	return Samplers;
}

UHGPUMemory* UHGraphic::GetMeshSharedMemory() const
{
	return MeshBufferSharedMemory.get();
}

UHGPUMemory* UHGraphic::GetImageSharedMemory() const
{
	return ImageSharedMemory.get();
}

void UHGraphic::BeginCmdDebug(VkCommandBuffer InBuffer, std::string InName)
{
#if WITH_EDITOR
	if (ConfigInterface->RenderingSetting().bEnableGPULabeling)
	{
		VkDebugUtilsLabelEXT LabelInfo{};
		LabelInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT;
		LabelInfo.pLabelName = InName.c_str();

		GBeginCmdDebugLabelCallback(InBuffer, &LabelInfo);
	}
#endif
}

void UHGraphic::EndCmdDebug(VkCommandBuffer InBuffer)
{
#if WITH_EDITOR
	if (ConfigInterface->RenderingSetting().bEnableGPULabeling)
	{
		GEndCmdDebugLabelCallback(InBuffer);
	}
#endif
}

VkCommandBuffer UHGraphic::BeginOneTimeCmd()
{
	// allocate command pool
	VkCommandPoolCreateInfo PoolInfo{};
	PoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;

	// I'd like to reset and record every frame
	PoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	PoolInfo.queueFamilyIndex = QueueFamily.GraphicsFamily.value();

	vkCreateCommandPool(LogicalDevice, &PoolInfo, nullptr, &CreationCommandPool);

	// allocate cmd and kick off it after creation
	VkCommandBufferAllocateInfo AllocInfo{};
	AllocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
	AllocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	AllocInfo.commandPool = CreationCommandPool;
	AllocInfo.commandBufferCount = 1;

	VkCommandBuffer CommandBuffer;
	vkAllocateCommandBuffers(LogicalDevice, &AllocInfo, &CommandBuffer);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(CommandBuffer, &beginInfo);

#if WITH_EDITOR
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_COMMAND_POOL, (uint64_t)CreationCommandPool, "OneTimeCommandPool");
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_COMMAND_BUFFER, (uint64_t)CommandBuffer, "OneTimeCommandBuffer");
#endif

	return CommandBuffer;
}

void UHGraphic::EndOneTimeCmd(VkCommandBuffer InBuffer)
{
	// end the one time cmd and submit it to GPU
	vkEndCommandBuffer(InBuffer);

	VkSubmitInfo SubmitInfo{};
	SubmitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	SubmitInfo.commandBufferCount = 1;
	SubmitInfo.pCommandBuffers = &InBuffer;

	vkQueueSubmit(GraphicsQueue, 1, &SubmitInfo, nullptr);
	vkQueueWaitIdle(GraphicsQueue);

	vkFreeCommandBuffers(LogicalDevice, CreationCommandPool, 1, &InBuffer);
	vkDestroyCommandPool(LogicalDevice, CreationCommandPool, nullptr);
	CreationCommandPool = nullptr;
}

VkQueue UHGraphic::GetGraphicsQueue() const
{
	return GraphicsQueue;
}

std::vector<uint32_t> UHGraphic::GetDeviceMemoryTypeIndices() const
{
	return DeviceMemoryTypeIndices;
}

uint32_t UHGraphic::GetHostMemoryTypeIndex() const
{
	return HostMemoryTypeIndex;
}

#if WITH_EDITOR
uint32_t UHGraphic::GetMinImageCount() const
{
	return MinImageCount;
}

bool UHGraphic::RecreateImGui()
{
	bool bImGuiSucceed = true;
	static ImGui_ImplVulkan_InitInfo InitInfo{};

	if (ImGui::GetCurrentContext() != nullptr)
	{
		// recreate the pipeline for ImGui use
		if (ImGuiPipeline)
		{
			vkDestroyPipeline(LogicalDevice, ImGuiPipeline, nullptr);
		}
		ImGui_ImplVulkan_CreatePipeline(LogicalDevice, nullptr, nullptr, SwapChainRenderPass, VK_SAMPLE_COUNT_1_BIT, &ImGuiPipeline, 0);

		// update swapchain info
		InitInfo.SwapChainFormat = GetSwapChainFormat();
		InitInfo.SwapChainColorSpace = IsHDRAvailable() ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
		ImGui_ImplVulkan_UpdateInitInfo(InitInfo);

		return true;
	}

	// Create ImGui stuffs after engine is initialized (editor only)
	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& ImGuiIO = ImGui::GetIO(); (void)ImGuiIO;
	ImGuiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
	ImGuiIO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls
	ImGuiIO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;         // Enable Docking
	ImGuiIO.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;       // Enable Multi-Viewport / Platform Windows
	//io.ConfigViewportsNoAutoMerge = true; // optional
	//io.ConfigViewportsNoTaskBarIcon = true; // optional

	// Setup Dear ImGui style
	ImGui::StyleColorsDark();

	// When viewports are enabled we tweak WindowRounding/WindowBg so platform windows can look identical to regular ones.
	ImGuiStyle& style = ImGui::GetStyle();
	if (ImGuiIO.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		style.WindowRounding = 0.0f;
		style.Colors[ImGuiCol_WindowBg].w = 1.0f;
	}

	// Setup Platform/Renderer backends
	bImGuiSucceed &= ImGui_ImplWin32_Init(WindowCache);

	InitInfo.Instance = GetInstance();
	InitInfo.PhysicalDevice = GetPhysicalDevice();
	InitInfo.Device = GetLogicalDevice();
	InitInfo.QueueFamily = GetQueueFamily().GraphicsFamily.value();
	InitInfo.Queue = GetGraphicsQueue();
	InitInfo.PipelineCache = nullptr;
	InitInfo.DescriptorPool = ImGuiDescriptorPool;
	InitInfo.Subpass = 0;
	InitInfo.MinImageCount = GetMinImageCount();
	InitInfo.ImageCount = GetSwapChainCount();
	InitInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	InitInfo.Allocator = nullptr;
	InitInfo.CheckVkResultFn = nullptr;
	InitInfo.SwapChainFormat = GetSwapChainFormat();
	InitInfo.SwapChainColorSpace = IsHDRAvailable() ? VK_COLOR_SPACE_HDR10_ST2084_EXT : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;

	// init Vulkan ImGui
	bImGuiSucceed &= ImGui_ImplVulkan_Init(&InitInfo, GetSwapChainRenderPass());
	if (!bImGuiSucceed)
	{
		UHE_LOG("Failed to init ImGui context!\n");
	}

	// init Vulkan font
	VkCommandBuffer CmdBuffer = BeginOneTimeCmd();
	ImGui_ImplVulkan_CreateFontsTexture(CmdBuffer);
	EndOneTimeCmd(CmdBuffer);
	ImGui_ImplVulkan_DestroyFontUploadObjects();

	return bImGuiSucceed;
}

VkPipeline UHGraphic::GetImGuiPipeline() const
{
	return ImGuiPipeline;
}

void UHGraphic::SetDepthPrepassActive(bool bInFlag)
{
	bEnableDepthPrePass = bInFlag;
}

void UHGraphic::SetDebugUtilsObjectName(VkObjectType InObjType, uint64_t InObjHandle, std::string InObjName) const
{
	VkDebugUtilsObjectNameInfoEXT DebugInfo{};
	DebugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
	DebugInfo.objectType = InObjType;
	DebugInfo.objectHandle = InObjHandle;
	DebugInfo.pObjectName = InObjName.c_str();

	GSetDebugUtilsObjectNameEXT(LogicalDevice, &DebugInfo);
}
#endif

bool UHGraphic::CreateSwapChain()
{
	// create swap chain officially!
	UHSwapChainDetails SwapChainSupport = QuerySwapChainSupport(PhysicalDevice);

	VkSurfaceFormatKHR Format = ChooseSwapChainFormat(SwapChainSupport, ConfigInterface->RenderingSetting().bEnableHDR, bSupportHDR);
	VkPresentModeKHR PresentMode = ChooseSwapChainMode(SwapChainSupport, ConfigInterface->PresentationSetting().bVsync);
	VkExtent2D Extent = ChooseSwapChainExtent(SwapChainSupport, WindowCache);

	// Follow GMaxFrameInFlight for image counts
	uint32_t ImageCount = GMaxFrameInFlight;
	if (SwapChainSupport.Capabilities2.surfaceCapabilities.maxImageCount > 0 && ImageCount > SwapChainSupport.Capabilities2.surfaceCapabilities.maxImageCount)
	{
		ImageCount = SwapChainSupport.Capabilities2.surfaceCapabilities.maxImageCount;
	}
#if WITH_EDITOR
	MinImageCount = ImageCount;
#endif

	// create info for swap chain
	VkSwapchainCreateInfoKHR CreateInfo{};
	CreateInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	CreateInfo.surface = MainSurface;
	CreateInfo.minImageCount = ImageCount;
	CreateInfo.imageFormat = Format.format;
	CreateInfo.imageColorSpace = Format.colorSpace;
	CreateInfo.imageExtent = Extent;

	// for VR app, this can be above 1
	CreateInfo.imageArrayLayers = 1;

	// use VK_IMAGE_USAGE_TRANSFER_DST_BIT if I'm rendering on another image and transfer to swapchain
	// use VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT if I want to render to swapchain directly
	// combine both usages so I can create imageview for swapchain but can also transfer to dst bit
	CreateInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

	// in this engine, graphic family is used as present family
	CreateInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	CreateInfo.queueFamilyIndexCount = 0; // Optional
	CreateInfo.pQueueFamilyIndices = nullptr; // Optional

	CreateInfo.preTransform = SwapChainSupport.Capabilities2.surfaceCapabilities.currentTransform;
	CreateInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	CreateInfo.presentMode = PresentMode;
	CreateInfo.clipped = VK_TRUE;
	CreateInfo.oldSwapchain = nullptr;

	// prepare win32 fullscreen ext
	VkSurfaceFullScreenExclusiveWin32InfoEXT Win32FullScreenInfo{};
	Win32FullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT;
	Win32FullScreenInfo.hmonitor = MonitorFromWindow(WindowCache, MONITOR_DEFAULTTOPRIMARY);

	// prepare fullscreen stuff, set to VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT and let the driver do the work
	// at the beginning it was controlled by app, but it could cause initialization failed for 4070 Ti 
	VkSurfaceFullScreenExclusiveInfoEXT FullScreenInfo{};
	FullScreenInfo.sType = VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT;
	FullScreenInfo.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_ALLOWED_EXT;
	FullScreenInfo.pNext = &Win32FullScreenInfo;
	CreateInfo.pNext = &FullScreenInfo;

	if (vkCreateSwapchainKHR(LogicalDevice, &CreateInfo, nullptr, &SwapChain) != VK_SUCCESS)
	{
		UHE_LOG(L"Failed to create swap chain!\n");
		return false;
	}

#if WITH_EDITOR
	SetDebugUtilsObjectName(VK_OBJECT_TYPE_SWAPCHAIN_KHR, (uint64_t)SwapChain, "SwapChain");
#endif

	// store swap chain image
	vkGetSwapchainImagesKHR(LogicalDevice, SwapChain, &ImageCount, nullptr);

	std::vector<VkImage> SwapChainImages;
	SwapChainImages.resize(ImageCount);
	vkGetSwapchainImagesKHR(LogicalDevice, SwapChain, &ImageCount, SwapChainImages.data());

	// create render pass for swap chain, it will be blit from other source, so transfer to drc_bit first
	UHTransitionInfo SwapChainTransition(VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	UHTextureFormat TargetFormat = IsHDRAvailable() ? UHTextureFormat::UH_FORMAT_A2B10G10R10 : UHTextureFormat::UH_FORMAT_BGRA8_UNORM;

	// create swap chain RTs
	SwapChainRT.resize(ImageCount);
	SwapChainFrameBuffer.resize(ImageCount);
	for (size_t Idx = 0; Idx < ImageCount; Idx++)
	{
		SwapChainRT[Idx] = RequestRenderTexture("SwapChain" + std::to_string(Idx), SwapChainImages[Idx], Extent, TargetFormat);
	}
	SwapChainRenderPass = CreateRenderPass(SwapChainRT[0], SwapChainTransition).RenderPass;

	for (size_t Idx = 0; Idx < ImageCount; Idx++)
	{
		SwapChainFrameBuffer[Idx] = CreateFrameBuffer(SwapChainRT[Idx], SwapChainRenderPass, Extent);
	}

#if WITH_EDITOR
	// init shared descriptor pool for editor use, hard-code size should suffice now
	if (ImGuiDescriptorPool == nullptr)
	{
		VkDescriptorPoolSize DescriptorPoolSize;
		DescriptorPoolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		DescriptorPoolSize.descriptorCount = 1024;

		VkDescriptorPoolCreateInfo PoolInfo = {};
		PoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		PoolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		PoolInfo.maxSets = 1024;
		PoolInfo.poolSizeCount = 1;
		PoolInfo.pPoolSizes = &DescriptorPoolSize;
		vkCreateDescriptorPool(LogicalDevice, &PoolInfo, nullptr, &ImGuiDescriptorPool);

#if WITH_EDITOR
		SetDebugUtilsObjectName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, (uint64_t)ImGuiDescriptorPool, "ImGuiDescriptorPool");
#endif
	}
	RecreateImGui();
#endif

	return true;
}

std::vector<uint32_t> UHGraphic::GetMemoryTypeIndices(VkMemoryPropertyFlags InFlags) const
{
	std::vector<uint32_t> OutTypes;

	for (uint32_t Idx = 0; Idx < PhysicalDeviceMemoryProperties.memoryTypeCount; Idx++)
	{
		if ((PhysicalDeviceMemoryProperties.memoryTypes[Idx].propertyFlags & InFlags) == InFlags)
		{
			OutTypes.push_back(Idx);
		}
	}

	return OutTypes;
}
//Instance Extension
static std::vector<const char*> LayerNames;

#ifdef _WIN32

PFN_vkCreateDebugUtilsMessengerEXT  __CreateDebugUtilsMessenger     = nullptr;
PFN_vkDestroyDebugUtilsMessengerEXT __DestroyDebugUtilsMessenger    = nullptr;
PFN_vkCmdBeginDebugUtilsLabelEXT    __CmdBeginDebugUtilsLabel       = nullptr;
PFN_vkCmdEndDebugUtilsLabelEXT      __CmdEndDebugUtilsLabel         = nullptr;
PFN_vkCmdInsertDebugUtilsLabelEXT   __CmdInsertDebugUtilsLabel      = nullptr;

VkDebugUtilsMessengerEXT gDebugMessenger = VK_NULL_HANDLE;
#endif // _WIN32



static const char* EnableExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
  VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#elif defined(__ANDROID__)
  // Android 的呈现 surface：ANativeWindow → VkSurfaceKHR（句柄见 RHIDisplay.h 的 AndroidDisplayHandle）
  VK_KHR_ANDROID_SURFACE_EXTENSION_NAME,
#endif
#if defined(_DEBUG) && !defined(__ANDROID__)
    // 注意：Android 上系统不带 VK_EXT_debug_utils（要自带校验层 .so 才有），
    // 无条件请求它会让 vkCreateInstance 直接失败 —— 所以这里显式排除。
    VK_EXT_DEBUG_UTILS_EXTENSION_NAME,
#endif
};



//Device
static VkPhysicalDevice SelectDevice(VkInstance instance) {
    uint32_t DeviceCount = 0;
    vkEnumeratePhysicalDevices(instance, &DeviceCount, nullptr);
    struct GpuInfo {
        GpuInfo(uint32_t Index, VkPhysicalDevice PhysicalDevice) :
            mIndex(Index), mPhysicalDevice(PhysicalDevice) {
            vkGetPhysicalDeviceProperties2(mPhysicalDevice, &mPhysicalDeviceProperties2);
        }

        // 注意 const：std::sort 比较的是 const GpuInfo&，参数少了 const 在 libc++（Android）
        // 上直接编不过，MSVC 却能通过 —— 属于典型的"只在 Windows 上能编"的写法。
        bool operator< (const GpuInfo& info) const {
            if (mPhysicalDeviceProperties2.properties.deviceType == info.mPhysicalDeviceProperties2.properties.deviceType) {
                return mIndex < info.mIndex;
            }
            return mPhysicalDeviceProperties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU || info.mPhysicalDeviceProperties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
        }

        uint32_t mIndex;
        VkPhysicalDevice mPhysicalDevice;
        VkPhysicalDeviceIDProperties   mPhysicalDeviceIDProperties = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 mPhysicalDeviceProperties2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &mPhysicalDeviceIDProperties
        };
    };

    std::vector<GpuInfo> Infos;
    VkPhysicalDevice* devices = new VkPhysicalDevice[DeviceCount];
    vkEnumeratePhysicalDevices(instance, &DeviceCount, devices);
    for (uint32_t i = 0; i < DeviceCount; i++)
        Infos.emplace_back(i, devices[i]);
    std::sort(Infos.begin(), Infos.end());
    VkPhysicalDevice gpu = Infos[0].mPhysicalDevice;
    delete[] devices;
    return gpu;
}



//Instance
static VkInstance gInstance = VK_NULL_HANDLE;
static bool gIsShuttingDown = 0;
static bool MakeVkInstance() {
    // 请求的实例版本必须 ≤ loader 支持的版本：否则 vkCreateInstance 直接返回
    // VK_ERROR_INCOMPATIBLE_DRIVER。桌面 LunarG loader 通常到 1.4，而手机系统 loader 常见 1.1/1.3
    // （Vulkan 1.4 是 2024 底才出的），所以这里先问 loader 再取 min。
    uint32_t loaderVersion = VK_API_VERSION_1_0;
    auto vkEnumerateInstanceVersionPtr =
        reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion"));
    if (vkEnumerateInstanceVersionPtr) vkEnumerateInstanceVersionPtr(&loaderVersion);
    const uint32_t requestVersion = loaderVersion < VK_API_VERSION_1_4 ? loaderVersion : VK_API_VERSION_1_4;
    Info("Vulkan loader version {}.{}.{}, requesting instance apiVersion {}.{}.{}",
         VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion), VK_VERSION_PATCH(loaderVersion),
         VK_VERSION_MAJOR(requestVersion), VK_VERSION_MINOR(requestVersion), VK_VERSION_PATCH(requestVersion));

    VkApplicationInfo vkApplicationInfo = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "FISIR Engine APP",
      .applicationVersion = VK_MAKE_VERSION(1,0,0),
      .pEngineName = "FISIR Engine",
      .engineVersion = VK_MAKE_VERSION(1,0,0),
      .apiVersion = requestVersion
    };
    //Init Layer
    uint32_t layerCount;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layerProperties(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layerProperties.data());
#ifdef _DEBUG
    for (const auto& layerP : layerProperties) {
        if (strstr(layerP.layerName, "validation")) LayerNames.push_back(layerP.layerName);
    }
#endif // DEBUG

    //CreateInstance
    VkInstanceCreateInfo vkInstanceCreateInfo = {
       .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
       .pApplicationInfo = &vkApplicationInfo,
       .enabledLayerCount = (uint32_t)LayerNames.size(),
       .ppEnabledLayerNames = LayerNames.data(),
       .enabledExtensionCount = sizeof(EnableExtensions) / sizeof(EnableExtensions[0]),
       .ppEnabledExtensionNames = EnableExtensions,
    };
    if (vkCreateInstance(&vkInstanceCreateInfo, nullptr, &gInstance) != VK_SUCCESS) {
        Error("Failed to create Vulkan instance!");
        return false;
    }
#ifdef _WIN32
    __CreateDebugUtilsMessenger = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(gInstance, "vkCreateDebugUtilsMessengerEXT");
    __DestroyDebugUtilsMessenger = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(gInstance, "vkDestroyDebugUtilsMessengerEXT");
    __CmdBeginDebugUtilsLabel = (PFN_vkCmdBeginDebugUtilsLabelEXT)vkGetInstanceProcAddr(gInstance, "vkCmdBeginDebugUtilsLabelEXT");
    __CmdEndDebugUtilsLabel = (PFN_vkCmdEndDebugUtilsLabelEXT)vkGetInstanceProcAddr(gInstance, "vkCmdEndDebugUtilsLabelEXT");
    __CmdInsertDebugUtilsLabel = (PFN_vkCmdInsertDebugUtilsLabelEXT)vkGetInstanceProcAddr(gInstance, "vkCmdInsertDebugUtilsLabelEXT");
#endif

    return true;
}

//Debug Callback
static VKAPI_ATTR VkBool32 VKAPI_CALL debugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
    void* pUserData)
{
    if (gIsShuttingDown) {
        return VK_FALSE;
    }

    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        Warn("{}", pCallbackData->pMessage);
    }
    if (messageSeverity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        Error("{}", pCallbackData->pMessage);
    }

    return VK_FALSE;
}

static bool MakeDebugReportCallback() {
#if defined(_DEBUG) && defined(_WIN32)
    VkDebugUtilsMessageSeverityFlagsEXT msgSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT;

    VkDebugUtilsMessageTypeFlagsEXT msgType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;

    VkDebugUtilsMessengerCreateInfoEXT debugUtilsCreateInfo = {
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = msgSeverity,
            .messageType = msgType,
            .pfnUserCallback = debugUtilsCallback,
            .pUserData = nullptr
    };

    if (__CreateDebugUtilsMessenger(gInstance, &debugUtilsCreateInfo, nullptr, &gDebugMessenger) != VK_SUCCESS) {
        Error("Failed to create debug utils messenger!");
        return false;
    }

    Info("Debug Utils Messenger registered successfully");
#endif
    return true;
}

static void DestroyDebugReportCallback() {
#ifdef _WIN32
    if (gDebugMessenger) {
        __DestroyDebugUtilsMessenger(gInstance, gDebugMessenger, nullptr);
    }
#endif
}
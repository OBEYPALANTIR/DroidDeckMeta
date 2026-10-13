// See xr_session.h.
#include "xr_session.h"

#include <android/log.h>
#include <dlfcn.h>
#include <openxr/openxr_reflection.h>
#include <pthread.h>  // pthread_gettid_np: gettid() itself needs API 29, the app targets 26

#include <algorithm>
#include <atomic>  // one-line extension-refusal logs
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

#define TAG "xrbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace xrbridge {
namespace {

constexpr XrViewConfigurationType kViewType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

// Enum names for the log, from the registry's reflection lists, the way hello_xr prints them.
#define XRB_CASE(name, value) case name: return #name;
#define XRB_TO_STRING(type) \
    const char *toString(type value) { switch (value) { XR_LIST_ENUM_##type(XRB_CASE) default: return "unknown " #type; } }
XRB_TO_STRING(XrResult)
XRB_TO_STRING(XrSessionState)
XRB_TO_STRING(XrReferenceSpaceType)
XRB_TO_STRING(XrEnvironmentBlendMode)
XRB_TO_STRING(XrAndroidThreadTypeKHR)
XRB_TO_STRING(XrPerfSettingsDomainEXT)
XRB_TO_STRING(XrPerfSettingsSubDomainEXT)
XRB_TO_STRING(XrPerfSettingsLevelEXT)
XRB_TO_STRING(XrPerfSettingsNotificationLevelEXT)
#undef XRB_TO_STRING
#undef XRB_CASE

template <typename T, typename Format>
std::string join(const std::vector<T> &items, Format format) {
    std::string out;
    for (const T &item : items) out += (out.empty() ? "" : ", ") + format(item);
    return out.empty() ? "none" : out;
}

// One boolean action per enum qb_button bit, in bit order (Session::buttons_). A null path leaves
// that hand unbound: the menu button is on the left Touch only, the right one's belongs to the system.
struct ButtonPaths {
    const char *name;
    const char *localized;
    const char *left;
    const char *right;
};
constexpr ButtonPaths kButtonPaths[] = {
        {"primary", "A or X", "/user/hand/left/input/x/click", "/user/hand/right/input/a/click"},
        {"primary_touch", "A or X touch", "/user/hand/left/input/x/touch", "/user/hand/right/input/a/touch"},
        {"secondary", "B or Y", "/user/hand/left/input/y/click", "/user/hand/right/input/b/click"},
        {"secondary_touch", "B or Y touch", "/user/hand/left/input/y/touch", "/user/hand/right/input/b/touch"},
        {"menu", "Menu", "/user/hand/left/input/menu/click", nullptr},
        {"thumbstick_click", "Thumbstick click", "/user/hand/left/input/thumbstick/click", "/user/hand/right/input/thumbstick/click"},
        {"thumbstick_touch", "Thumbstick touch", "/user/hand/left/input/thumbstick/touch", "/user/hand/right/input/thumbstick/touch"},
        {"trigger_touch", "Trigger touch", "/user/hand/left/input/trigger/touch", "/user/hand/right/input/trigger/touch"},
        {"thumbrest_touch", "Thumb rest touch", "/user/hand/left/input/thumbrest/touch", "/user/hand/right/input/thumbrest/touch"},
};
static_assert(QB_BTN_PRIMARY_CLICK == 1 << 0 && QB_BTN_MENU_CLICK == 1 << 4 && QB_BTN_THUMBREST_TOUCH == 1 << 8,
              "kButtonPaths follows enum qb_button");

}  // namespace

thread_local XrResult Session::last_ = XR_SUCCESS;

bool Session::check(XrResult result, const char *what) {
    if (XR_SUCCEEDED(result)) return true;
    last_ = result;
    LOGE("%s failed: %s (%d)", what, toString(result), static_cast<int>(result));
    return false;
}

bool Session::checkVk(VkResult result, const char *what) {
    if (result >= VK_SUCCESS) return true;  // positive codes are statuses, not failures
    // Surfaced as the nearest OpenXR result; the log keeps the VkResult.
    last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
    LOGE("%s failed: VkResult %d", what, static_cast<int>(result));
    return false;
}

template <typename Fn>
bool Session::load(const char *name, Fn &fn) {
    return check(xrGetInstanceProcAddr(instance_, name, reinterpret_cast<PFN_xrVoidFunction *>(&fn)), name);
}

bool Session::init(JavaVM *vm, jobject activity) {
    shutdown();
    last_ = XR_SUCCESS;
    return createInstance(vm, activity) && createSystem() && createVulkan() && createSession() && createActions();
}

bool Session::createInstance(JavaVM *vm, jobject activity) {
    vm_ = vm;  // createSurfaceSwapchain hands its Surface over through JNI
    // On Android the loader needs the VM and a context before any other call.
    PFN_xrInitializeLoaderKHR initializeLoader = nullptr;
    if (!load("xrInitializeLoaderKHR", initializeLoader)) return false;
    XrLoaderInitInfoAndroidKHR loaderInfo{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loaderInfo.applicationVM = vm;
    loaderInfo.applicationContext = activity;
    if (!check(initializeLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&loaderInfo)), "xrInitializeLoaderKHR"))
        return false;

    uint32_t count = 0;
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr), "xrEnumerateInstanceExtensionProperties"))
        return false;
    std::vector<XrExtensionProperties> offered(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (!check(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, offered.data()), "xrEnumerateInstanceExtensionProperties"))
        return false;
    offered.resize(count);
    bool vulkanEnable2 = false;
    bool androidCreateInstance = false;
    const struct { const char *name; bool *present; } watched[] = {
            {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, &vulkanEnable2},
            {XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, &androidCreateInstance},
            {XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME, &runtimeExtensions_.androidSurfaceSwapchain},
            {XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME, &runtimeExtensions_.compositionLayerDepth},
            {XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, &runtimeExtensions_.displayRefreshRate},
            {XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME, &runtimeExtensions_.compositionLayerImageLayout},
            {XR_EXT_HAND_TRACKING_EXTENSION_NAME, &runtimeExtensions_.handTracking},
            {XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME, &runtimeExtensions_.androidThreadSettings},
            {XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME, &runtimeExtensions_.performanceSettings},
    };
    for (const XrExtensionProperties &extension : offered) {
        LOGI("runtime extension %s v%u", extension.extensionName, extension.extensionVersion);
        for (const auto &w : watched)
            if (!strcmp(extension.extensionName, w.name)) *w.present = true;
    }
    if (!vulkanEnable2 || !androidCreateInstance) {
        LOGE("the runtime lacks %s", vulkanEnable2 ? XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME : XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
        last_ = XR_ERROR_EXTENSION_NOT_PRESENT;
        return false;
    }
    std::vector<const char *> enabled = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME};
    if (runtimeExtensions_.displayRefreshRate) enabled.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (runtimeExtensions_.androidSurfaceSwapchain) enabled.push_back(XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME);
    if (runtimeExtensions_.androidThreadSettings) enabled.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
    if (runtimeExtensions_.performanceSettings) enabled.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = vm;
    androidInfo.applicationActivity = activity;
    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.next = &androidInfo;
    snprintf(info.applicationInfo.applicationName, sizeof info.applicationInfo.applicationName, "%s", "DroidDeck");
    info.applicationInfo.applicationVersion = 1;
    snprintf(info.applicationInfo.engineName, sizeof info.applicationInfo.engineName, "%s", "DroidDeck XR bridge");
    info.applicationInfo.apiVersion = XR_API_VERSION_1_0;  // everything used is 1.0 plus extensions
    info.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    info.enabledExtensionNames = enabled.data();
    if (!check(xrCreateInstance(&info, &instance_), "xrCreateInstance")) return false;
    // Optional: without it there is no Steam panel, and everything else still works.
    if (runtimeExtensions_.androidSurfaceSwapchain) load("xrCreateSwapchainAndroidSurfaceKHR", createSurfaceSwapchain_);
    if (runtimeExtensions_.androidThreadSettings) load("xrSetAndroidApplicationThreadKHR", setApplicationThread_);
    if (runtimeExtensions_.performanceSettings) load("xrPerfSettingsSetPerformanceLevelEXT", setPerformanceLevel_);

    if (!check(xrGetInstanceProperties(instance_, &instanceProperties_), "xrGetInstanceProperties")) return false;
    const XrVersion version = instanceProperties_.runtimeVersion;
    LOGI("runtime %s %u.%u.%u", instanceProperties_.runtimeName, unsigned(XR_VERSION_MAJOR(version)),
         unsigned(XR_VERSION_MINOR(version)), unsigned(XR_VERSION_PATCH(version)));
    return true;
}

bool Session::createSystem() {
    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!check(xrGetSystem(instance_, &info, &systemId_), "xrGetSystem")) return false;
    if (!check(xrGetSystemProperties(instance_, systemId_, &systemProperties_), "xrGetSystemProperties")) return false;
    const XrSystemProperties &system = systemProperties_;
    LOGI("system %s, vendor 0x%x, swapchains up to %ux%u, %u layers, orientation tracking %u, position tracking %u",
         system.systemName, system.vendorId, system.graphicsProperties.maxSwapchainImageWidth,
         system.graphicsProperties.maxSwapchainImageHeight, system.graphicsProperties.maxLayerCount,
         system.trackingProperties.orientationTracking, system.trackingProperties.positionTracking);

    uint32_t count = 0;
    if (!check(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewType, 0, &count, nullptr), "xrEnumerateViewConfigurationViews"))
        return false;
    viewConfigs_.assign(count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (!check(xrEnumerateViewConfigurationViews(instance_, systemId_, kViewType, count, &count, viewConfigs_.data()),
               "xrEnumerateViewConfigurationViews"))
        return false;
    viewConfigs_.resize(count);
    if (count != 2) {
        LOGE("primary stereo has %u views, not 2", count);
        last_ = XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        const XrViewConfigurationView &view = viewConfigs_[i];
        LOGI("view %u: recommended %ux%u (max %ux%u), %u samples (max %u)", i, view.recommendedImageRectWidth,
             view.recommendedImageRectHeight, view.maxImageRectWidth, view.maxImageRectHeight,
             view.recommendedSwapchainSampleCount, view.maxSwapchainSampleCount);
    }

    if (!check(xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewType, 0, &count, nullptr), "xrEnumerateEnvironmentBlendModes"))
        return false;
    std::vector<XrEnvironmentBlendMode> modes(count);
    if (!check(xrEnumerateEnvironmentBlendModes(instance_, systemId_, kViewType, count, &count, modes.data()),
               "xrEnumerateEnvironmentBlendModes"))
        return false;
    modes.resize(count);
    if (modes.empty()) {
        LOGE("the runtime lists no environment blend mode");
        last_ = XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
        return false;
    }
    blendMode_ = modes[0];  // listed in the runtime's order of preference
    LOGI("environment blend modes: %s", join(modes, [](XrEnvironmentBlendMode mode) { return std::string(toString(mode)); }).c_str());
    return true;
}

bool Session::createVulkan() {
    PFN_xrGetVulkanGraphicsRequirements2KHR getRequirements = nullptr;
    PFN_xrCreateVulkanInstanceKHR createVulkanInstance = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR getVulkanDevice = nullptr;
    PFN_xrCreateVulkanDeviceKHR createVulkanDevice = nullptr;
    if (!load("xrGetVulkanGraphicsRequirements2KHR", getRequirements) || !load("xrCreateVulkanInstanceKHR", createVulkanInstance) ||
        !load("xrGetVulkanGraphicsDevice2KHR", getVulkanDevice) || !load("xrCreateVulkanDeviceKHR", createVulkanDevice))
        return false;

    // Asked for 1.1: it makes external memory, dedicated allocation and YCbCr conversion core,
    // which the image-sharing extensions below build on.
    XrGraphicsRequirementsVulkan2KHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
    if (!check(getRequirements(instance_, systemId_, &requirements), "xrGetVulkanGraphicsRequirements2KHR")) return false;
    const XrVersion lowest = requirements.minApiVersionSupported;
    const XrVersion highest = requirements.maxApiVersionSupported;
    LOGI("Vulkan: the runtime takes API %u.%u to %u.%u", unsigned(XR_VERSION_MAJOR(lowest)), unsigned(XR_VERSION_MINOR(lowest)),
         unsigned(XR_VERSION_MAJOR(highest)), unsigned(XR_VERSION_MINOR(highest)));
    if (XR_MAKE_VERSION(XR_VERSION_MAJOR(lowest), XR_VERSION_MINOR(lowest), 0) > XR_MAKE_VERSION(1, 1, 0)) {
        LOGE("Vulkan: the runtime needs more than API 1.1");
        last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }

    vulkan_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (vulkan_) getInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(vulkan_, "vkGetInstanceProcAddr"));
    if (!getInstanceProcAddr_) {
        const char *error = dlerror();
        LOGE("Vulkan: no system loader (%s)", error ? error : "no vkGetInstanceProcAddr");
        last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "DroidDeck XR bridge";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    instanceInfo.pApplicationInfo = &app;
    XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xrInstanceInfo.systemId = systemId_;
    xrInstanceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
    VkResult vk = VK_SUCCESS;
    if (!check(createVulkanInstance(instance_, &xrInstanceInfo, &vkInstance_, &vk), "xrCreateVulkanInstanceKHR") ||
        !checkVk(vk, "vkCreateInstance")) {
        vkInstance_ = VK_NULL_HANDLE;
        return false;
    }
    destroyInstance_ = reinterpret_cast<PFN_vkDestroyInstance>(getInstanceProcAddr_(vkInstance_, "vkDestroyInstance"));
    getDeviceProcAddr_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(getInstanceProcAddr_(vkInstance_, "vkGetDeviceProcAddr"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            getInstanceProcAddr_(vkInstance_, "vkGetPhysicalDeviceProperties"));
    const auto getQueueFamilies = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            getInstanceProcAddr_(vkInstance_, "vkGetPhysicalDeviceQueueFamilyProperties"));
    const auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            getInstanceProcAddr_(vkInstance_, "vkEnumerateDeviceExtensionProperties"));
    if (!destroyInstance_ || !getDeviceProcAddr_ || !getProperties || !getQueueFamilies || !enumerateExtensions) {
        LOGE("Vulkan: the loader lacks core instance functions");
        last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }

    XrVulkanGraphicsDeviceGetInfoKHR deviceGet{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    deviceGet.systemId = systemId_;
    deviceGet.vulkanInstance = vkInstance_;
    if (!check(getVulkanDevice(instance_, &deviceGet, &physicalDevice_), "xrGetVulkanGraphicsDevice2KHR")) return false;
    VkPhysicalDeviceProperties properties;
    getProperties(physicalDevice_, &properties);
    uint32_t count = 0;
    getQueueFamilies(physicalDevice_, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    getQueueFamilies(physicalDevice_, &count, families.data());
    const auto graphics = std::find_if(families.begin(), families.end(),
                                       [](const VkQueueFamilyProperties &family) { return (family.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0; });
    if (graphics == families.end()) {
        LOGE("Vulkan: %s has no graphics queue", properties.deviceName);
        last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }
    queueFamily_ = static_cast<uint32_t>(graphics - families.begin());
    LOGI("Vulkan: %s, API %u.%u.%u, graphics queue family %u", properties.deviceName, VK_API_VERSION_MAJOR(properties.apiVersion),
         VK_API_VERSION_MINOR(properties.apiVersion), VK_API_VERSION_PATCH(properties.apiVersion), queueFamily_);

    if (!checkVk(enumerateExtensions(physicalDevice_, nullptr, &count, nullptr), "vkEnumerateDeviceExtensionProperties")) return false;
    std::vector<VkExtensionProperties> offered(count);
    if (!checkVk(enumerateExtensions(physicalDevice_, nullptr, &count, offered.data()), "vkEnumerateDeviceExtensionProperties"))
        return false;
    offered.resize(count);
    const auto has = [&offered](const char *name) {
        return std::any_of(offered.begin(), offered.end(), [name](const VkExtensionProperties &e) { return !strcmp(e.extensionName, name); });
    };
    // Each needs a 1.1 device and, where named, an earlier entry (image_format_list is core only from 1.2).
    DeviceExtensions &ext = deviceExtensions_;
    const bool core11 = properties.apiVersion >= VK_API_VERSION_1_1;
    const struct { const char *name; const bool *needs; bool *enabled; } sharing[] = {
            {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, nullptr, &ext.externalMemoryFd},
            {VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME, &ext.externalMemoryFd, &ext.externalMemoryDmaBuf},
            {VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME, nullptr, &ext.queueFamilyForeign},
            {VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME, nullptr, &ext.imageFormatList},
            {VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME, &ext.imageFormatList, &ext.imageDrmFormatModifier},
            {VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME, &ext.queueFamilyForeign, &ext.androidHardwareBuffer},
            {VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, nullptr, &ext.externalSemaphoreFd},
    };
    for (const auto &s : sharing) {
        *s.enabled = core11 && has(s.name) && (!s.needs || *s.needs);
        if (*s.enabled) enabledDeviceExtensions_.push_back(s.name);
    }

    const float priority = 0.0f;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queueInfo.queueFamilyIndex = queueFamily_;
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(enabledDeviceExtensions_.size());
    deviceInfo.ppEnabledExtensionNames = enabledDeviceExtensions_.data();
    XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xrDeviceInfo.systemId = systemId_;
    xrDeviceInfo.pfnGetInstanceProcAddr = getInstanceProcAddr_;
    xrDeviceInfo.vulkanPhysicalDevice = physicalDevice_;
    xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
    if (!check(createVulkanDevice(instance_, &xrDeviceInfo, &device_, &vk), "xrCreateVulkanDeviceKHR") || !checkVk(vk, "vkCreateDevice")) {
        device_ = VK_NULL_HANDLE;
        return false;
    }
    const auto device = [this](const char *name) { return getDeviceProcAddr_(device_, name); };
    destroyDevice_ = reinterpret_cast<PFN_vkDestroyDevice>(device("vkDestroyDevice"));
    deviceWaitIdle_ = reinterpret_cast<PFN_vkDeviceWaitIdle>(device("vkDeviceWaitIdle"));
    const auto getQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(device("vkGetDeviceQueue"));
    createImage_ = reinterpret_cast<PFN_vkCreateImage>(device("vkCreateImage"));
    destroyImage_ = reinterpret_cast<PFN_vkDestroyImage>(device("vkDestroyImage"));
    allocateMemory_ = reinterpret_cast<PFN_vkAllocateMemory>(device("vkAllocateMemory"));
    freeMemory_ = reinterpret_cast<PFN_vkFreeMemory>(device("vkFreeMemory"));
    bindImageMemory_ = reinterpret_cast<PFN_vkBindImageMemory>(device("vkBindImageMemory"));
    createSemaphore_ = reinterpret_cast<PFN_vkCreateSemaphore>(device("vkCreateSemaphore"));
    if (!destroyDevice_ || !deviceWaitIdle_ || !getQueue || !createImage_ || !destroyImage_ || !allocateMemory_ || !freeMemory_ ||
        !bindImageMemory_ || !createSemaphore_) {
        LOGE("Vulkan: the device lacks core functions");
        last_ = XR_ERROR_GRAPHICS_DEVICE_INVALID;
        return false;
    }
    // An enabled extension's own entry points; importAhb and importSyncFd refuse without them.
    if (ext.androidHardwareBuffer)
        getAhbProperties_ = reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
                device("vkGetAndroidHardwareBufferPropertiesANDROID"));
    if (ext.externalSemaphoreFd) importSemaphoreFd_ = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(device("vkImportSemaphoreFdKHR"));
    getQueue(device_, queueFamily_, 0, &queue_);
    return true;
}

bool Session::createSession() {
    XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
    binding.instance = vkInstance_;
    binding.physicalDevice = physicalDevice_;
    binding.device = device_;
    binding.queueFamilyIndex = queueFamily_;
    binding.queueIndex = 0;
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = systemId_;
    if (!check(xrCreateSession(instance_, &info, &session_), "xrCreateSession")) return false;

    uint32_t count = 0;
    if (!check(xrEnumerateReferenceSpaces(session_, 0, &count, nullptr), "xrEnumerateReferenceSpaces")) return false;
    std::vector<XrReferenceSpaceType> spaces(count);
    if (!check(xrEnumerateReferenceSpaces(session_, count, &count, spaces.data()), "xrEnumerateReferenceSpaces")) return false;
    spaces.resize(count);
    LOGI("reference spaces: %s", join(spaces, [](XrReferenceSpaceType type) { return std::string(toString(type)); }).c_str());
    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!check(xrCreateReferenceSpace(session_, &spaceInfo, &local_), "xrCreateReferenceSpace(LOCAL)")) return false;
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;  // every runtime has it
    if (!check(xrCreateReferenceSpace(session_, &spaceInfo, &view_), "xrCreateReferenceSpace(VIEW)")) return false;
    if (std::find(spaces.begin(), spaces.end(), XR_REFERENCE_SPACE_TYPE_STAGE) != spaces.end()) {
        spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
        if (!check(xrCreateReferenceSpace(session_, &spaceInfo, &stage_), "xrCreateReferenceSpace(STAGE)")) return false;
    }

    if (!check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "xrEnumerateSwapchainFormats")) return false;
    formats_.resize(count);
    if (!check(xrEnumerateSwapchainFormats(session_, count, &count, formats_.data()), "xrEnumerateSwapchainFormats")) return false;
    formats_.resize(count);
    LOGI("swapchain formats (VkFormat): %s", join(formats_, [](int64_t format) { return std::to_string(format); }).c_str());

    if (runtimeExtensions_.displayRefreshRate) {
        // Informational: a failure here is logged and never fails the session.
        load("xrRequestDisplayRefreshRateFB", requestDisplayRefreshRate_);
        PFN_xrEnumerateDisplayRefreshRatesFB enumerateRates = nullptr;
        uint32_t rates = 0;
        if (load("xrEnumerateDisplayRefreshRatesFB", enumerateRates) && load("xrGetDisplayRefreshRateFB", getDisplayRefreshRate_) &&
            check(enumerateRates(session_, 0, &rates, nullptr), "xrEnumerateDisplayRefreshRatesFB")) {
            refreshRates_.resize(rates);
            if (!check(enumerateRates(session_, rates, &rates, refreshRates_.data()), "xrEnumerateDisplayRefreshRatesFB")) rates = 0;
        } else {
            rates = 0;
        }
        refreshRates_.resize(rates);
        const auto hz = [](float rate) {
            char text[16];
            snprintf(text, sizeof text, "%.1f", rate);
            return std::string(text);
        };
        LOGI("display refresh rates: %s Hz, now %.1f", join(refreshRates_, hz).c_str(), refreshRate());
    }
    applyThreadHints();  // best-effort: without the extension it logs once and the session still works
    return true;
}

bool Session::createActions() {
    if (!check(xrStringToPath(instance_, "/user/hand/left", &hands_[kLeft].path), "xrStringToPath") ||
        !check(xrStringToPath(instance_, "/user/hand/right", &hands_[kRight].path), "xrStringToPath"))
        return false;
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(setInfo.actionSetName, sizeof setInfo.actionSetName, "%s", "droiddeck");
    snprintf(setInfo.localizedActionSetName, sizeof setInfo.localizedActionSetName, "%s", "DroidDeck");
    if (!check(xrCreateActionSet(instance_, &setInfo, &actionSet_), "xrCreateActionSet")) return false;

    // Every action exists per hand (subaction paths) and is suggested for both Touch controllers;
    // a null path leaves that hand unbound.
    static_assert(sizeof kButtonPaths / sizeof kButtonPaths[0] == kButtons, "one action per enum qb_button bit");
    const XrPath handPaths[2] = {hands_[kLeft].path, hands_[kRight].path};
    struct ActionInfo { XrAction *action; XrActionType type; const char *name; const char *localized; const char *left; const char *right; };
    std::vector<ActionInfo> actions = {
            {&aim_, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim pose", "/user/hand/left/input/aim/pose", "/user/hand/right/input/aim/pose"},
            {&grip_, XR_ACTION_TYPE_POSE_INPUT, "grip", "Grip pose", "/user/hand/left/input/grip/pose", "/user/hand/right/input/grip/pose"},
            {&trigger_, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", "/user/hand/left/input/trigger/value", "/user/hand/right/input/trigger/value"},
            {&squeeze_, XR_ACTION_TYPE_FLOAT_INPUT, "squeeze", "Grip button", "/user/hand/left/input/squeeze/value", "/user/hand/right/input/squeeze/value"},
            {&thumbstick_, XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick", "Thumbstick", "/user/hand/left/input/thumbstick", "/user/hand/right/input/thumbstick"},
            {&haptic_, XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic", "Vibration", "/user/hand/left/output/haptic", "/user/hand/right/output/haptic"},
    };
    for (int i = 0; i < kButtons; i++) {
        const ButtonPaths &b = kButtonPaths[i];
        actions.push_back({&buttons_[i], XR_ACTION_TYPE_BOOLEAN_INPUT, b.name, b.localized, b.left, b.right});
    }
    std::vector<XrActionSuggestedBinding> bindings;
    for (const ActionInfo &a : actions) {
        XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
        info.actionType = a.type;
        snprintf(info.actionName, sizeof info.actionName, "%s", a.name);
        snprintf(info.localizedActionName, sizeof info.localizedActionName, "%s", a.localized);
        info.countSubactionPaths = 2;
        info.subactionPaths = handPaths;
        if (!check(xrCreateAction(actionSet_, &info, a.action), "xrCreateAction")) return false;
        for (const char *path : {a.left, a.right}) {
            if (!path) continue;
            XrPath binding = XR_NULL_PATH;
            if (!check(xrStringToPath(instance_, path, &binding), "xrStringToPath")) return false;
            bindings.push_back({*a.action, binding});
        }
    }
    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    if (!check(xrStringToPath(instance_, "/interaction_profiles/oculus/touch_controller", &suggested.interactionProfile), "xrStringToPath"))
        return false;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    if (!check(xrSuggestInteractionProfileBindings(instance_, &suggested), "xrSuggestInteractionProfileBindings")) return false;

    for (Hand &hand : hands_) {
        XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        spaceInfo.subactionPath = hand.path;
        spaceInfo.poseInActionSpace.orientation.w = 1.0f;
        spaceInfo.action = aim_;
        if (!check(xrCreateActionSpace(session_, &spaceInfo, &hand.aim), "xrCreateActionSpace(aim)")) return false;
        spaceInfo.action = grip_;
        if (!check(xrCreateActionSpace(session_, &spaceInfo, &hand.grip), "xrCreateActionSpace(grip)")) return false;
    }
    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &actionSet_;
    return check(xrAttachSessionActionSets(session_, &attach), "xrAttachSessionActionSets");
}

void Session::shutdown() {
    // Nothing of ours may still be running on the swapchain images when they go.
    if (device_ != VK_NULL_HANDLE && deviceWaitIdle_) deviceWaitIdle_(device_);
    for (Hand &hand : hands_) {
        if (XrSpace space = std::exchange(hand.aim, XR_NULL_HANDLE)) xrDestroySpace(space);
        if (XrSpace space = std::exchange(hand.grip, XR_NULL_HANDLE)) xrDestroySpace(space);
        hand = Hand{};
    }
    // The set takes its actions with it.
    if (XrActionSet set = std::exchange(actionSet_, XR_NULL_HANDLE)) xrDestroyActionSet(set);
    aim_ = grip_ = trigger_ = squeeze_ = thumbstick_ = haptic_ = XR_NULL_HANDLE;
    for (XrAction &button : buttons_) button = XR_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(swapchainLock_);
        for (XrSwapchain swapchain : swapchains_) xrDestroySwapchain(swapchain);
        swapchains_.clear();
    }
    if (XrSpace space = std::exchange(view_, XR_NULL_HANDLE)) xrDestroySpace(space);
    if (XrSpace space = std::exchange(local_, XR_NULL_HANDLE)) xrDestroySpace(space);
    if (XrSpace space = std::exchange(stage_, XR_NULL_HANDLE)) xrDestroySpace(space);
    if (XrSession session = std::exchange(session_, XR_NULL_HANDLE)) xrDestroySession(session);
    if (XrInstance instance = std::exchange(instance_, XR_NULL_HANDLE)) xrDestroyInstance(instance);
    // The runtime made the Vulkan device and instance for us; they are ours to destroy, after it.
    if (VkDevice device = std::exchange(device_, VK_NULL_HANDLE); device && destroyDevice_) destroyDevice_(device, nullptr);
    if (VkInstance instance = std::exchange(vkInstance_, VK_NULL_HANDLE); instance && destroyInstance_) destroyInstance_(instance, nullptr);
    if (void *library = std::exchange(vulkan_, nullptr)) dlclose(library);

    getInstanceProcAddr_ = nullptr;
    getDeviceProcAddr_ = nullptr;
    destroyInstance_ = nullptr;
    destroyDevice_ = nullptr;
    deviceWaitIdle_ = nullptr;
    createImage_ = nullptr;
    destroyImage_ = nullptr;
    allocateMemory_ = nullptr;
    freeMemory_ = nullptr;
    bindImageMemory_ = nullptr;
    createSemaphore_ = nullptr;
    getAhbProperties_ = nullptr;
    importSemaphoreFd_ = nullptr;
    ahbRefusalLogged_ = false;
    syncFdRefusalLogged_ = false;
    physicalDevice_ = VK_NULL_HANDLE;
    queueFamily_ = 0;
    queue_ = VK_NULL_HANDLE;
    deviceExtensions_ = {};
    enabledDeviceExtensions_.clear();
    systemId_ = XR_NULL_SYSTEM_ID;
    state_ = XR_SESSION_STATE_UNKNOWN;
    running_ = false;
    exiting_ = false;
    perfLevelsApplied_ = false;
    instanceProperties_ = {XR_TYPE_INSTANCE_PROPERTIES};
    systemProperties_ = {XR_TYPE_SYSTEM_PROPERTIES};
    runtimeExtensions_ = {};
    viewConfigs_.clear();
    blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    formats_.clear();
    refreshRates_.clear();
    getDisplayRefreshRate_ = nullptr;
    requestDisplayRefreshRate_ = nullptr;
    createSurfaceSwapchain_ = nullptr;
    setApplicationThread_ = nullptr;
    setPerformanceLevel_ = nullptr;
    vm_ = nullptr;
}

bool Session::pollEvents(const std::function<void(const XrEventDataBuffer &)> &observer) {
    if (instance_ == XR_NULL_HANDLE) return false;
    for (;;) {
        XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
        const XrResult result = xrPollEvent(instance_, &event);
        if (result == XR_EVENT_UNAVAILABLE) return !exiting_;
        if (!check(result, "xrPollEvent")) return false;
        switch (event.type) {
            case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
                const auto &changed = *reinterpret_cast<const XrEventDataSessionStateChanged *>(&event);
                LOGI("session %s -> %s", toString(state_), toString(changed.state));
                state_ = changed.state;
                if (state_ == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = kViewType;
                    if (!check(xrBeginSession(session_, &begin), "xrBeginSession")) return false;
                    running_ = true;
                    // Once per session: SUSTAINED_HIGH is WinlatorXR's Quest 3 choice; BOOST is for short bursts.
                    if (!perfLevelsApplied_)
                        perfLevelsApplied_ = setPerformanceLevels(XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT,
                                                                  XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                } else if (state_ == XR_SESSION_STATE_STOPPING) {
                    running_ = false;
                    perfLevelsApplied_ = false;  // the next READY re-applies the levels
                    if (!check(xrEndSession(session_), "xrEndSession")) return false;
                } else if (state_ == XR_SESSION_STATE_EXITING || state_ == XR_SESSION_STATE_LOSS_PENDING) {
                    exiting_ = true;
                }
                break;
            }
            case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
                LOGE("instance loss pending");
                exiting_ = true;
                break;
            case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
                const auto &change = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending *>(&event);
                LOGI("reference space %s changes at %lld, pose valid %u", toString(change.referenceSpaceType),
                     static_cast<long long>(change.changeTime), change.poseValid);
                break;
            }
            case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
                logInteractionProfiles();
                break;
            case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
                const auto &rate = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB *>(&event);
                LOGI("display refresh rate %.1f -> %.1f Hz", rate.fromDisplayRefreshRate, rate.toDisplayRefreshRate);
                break;
            }
            case XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT: {
                const auto &perf = *reinterpret_cast<const XrEventDataPerfSettingsEXT *>(&event);
                LOGI("performance %s %s %s -> %s", toString(perf.domain), toString(perf.subDomain), toString(perf.fromLevel),
                     toString(perf.toLevel));
                break;
            }
            case XR_TYPE_EVENT_DATA_EVENTS_LOST:
                LOGE("%u events lost", reinterpret_cast<const XrEventDataEventsLost *>(&event)->lostEventCount);
                break;
            default:
                LOGI("event type %d", static_cast<int>(event.type));
                break;
        }
        if (observer) observer(event);
    }
}

void Session::logInteractionProfiles() {
    for (int hand = kLeft; hand <= kRight; hand++) {
        XrInteractionProfileState state{XR_TYPE_INTERACTION_PROFILE_STATE};
        if (!check(xrGetCurrentInteractionProfile(session_, hands_[hand].path, &state), "xrGetCurrentInteractionProfile")) continue;
        char profile[XR_MAX_PATH_LENGTH] = "none";
        uint32_t length = 0;
        if (state.interactionProfile != XR_NULL_PATH &&
            !check(xrPathToString(instance_, state.interactionProfile, static_cast<uint32_t>(sizeof profile), &length, profile), "xrPathToString"))
            snprintf(profile, sizeof profile, "%s", "unknown");
        LOGI("%s hand interaction profile: %s", hand == kLeft ? "left" : "right", profile);
    }
}

bool Session::waitFrame(XrFrameState &frame) {
    XrFrameWaitInfo info{XR_TYPE_FRAME_WAIT_INFO};
    return check(xrWaitFrame(session_, &info, &frame), "xrWaitFrame");
}

bool Session::beginFrame() {
    // XR_FRAME_DISCARDED is a success code: the previous frame was dropped, this one goes ahead.
    XrFrameBeginInfo info{XR_TYPE_FRAME_BEGIN_INFO};
    return check(xrBeginFrame(session_, &info), "xrBeginFrame");
}

bool Session::locateViews(XrSpace base, XrTime time, XrView out[2], XrViewStateFlags &flags) {
    XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO};
    info.viewConfigurationType = kViewType;
    info.displayTime = time;
    info.space = base;
    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t count = 0;
    if (!check(xrLocateViews(session_, &info, &state, 2, &count, out), "xrLocateViews")) return false;
    flags = state.viewStateFlags;
    if (count == 2) return true;
    LOGE("xrLocateViews returned %u views, not 2", count);
    last_ = XR_ERROR_RUNTIME_FAILURE;
    return false;
}

bool Session::endFrame(XrTime displayTime, const XrCompositionLayerBaseHeader *const *layers, uint32_t layerCount) {
    XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO};
    info.displayTime = displayTime;
    info.environmentBlendMode = blendMode_;
    info.layerCount = layerCount;
    info.layers = layers;
    return check(xrEndFrame(session_, &info), "xrEndFrame");
}

bool Session::syncActions() {
    const XrActiveActionSet active{actionSet_, XR_NULL_PATH};
    XrActionsSyncInfo info{XR_TYPE_ACTIONS_SYNC_INFO};
    info.countActiveActionSets = 1;
    info.activeActionSets = &active;
    // XR_SESSION_NOT_FOCUSED is a success code; every state then reads inactive.
    if (!check(xrSyncActions(session_, &info), "xrSyncActions")) return false;
    for (Hand &hand : hands_) {
        XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO};
        get.subactionPath = hand.path;
        XrActionStatePose pose{XR_TYPE_ACTION_STATE_POSE};
        XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
        XrActionStateFloat squeeze{XR_TYPE_ACTION_STATE_FLOAT};
        XrActionStateVector2f thumbstick{XR_TYPE_ACTION_STATE_VECTOR2F};
        get.action = grip_;
        if (!check(xrGetActionStatePose(session_, &get, &pose), "xrGetActionStatePose")) return false;
        get.action = trigger_;
        if (!check(xrGetActionStateFloat(session_, &get, &trigger), "xrGetActionStateFloat")) return false;
        get.action = squeeze_;
        if (!check(xrGetActionStateFloat(session_, &get, &squeeze), "xrGetActionStateFloat")) return false;
        get.action = thumbstick_;
        if (!check(xrGetActionStateVector2f(session_, &get, &thumbstick), "xrGetActionStateVector2f")) return false;
        uint32_t buttons = 0;
        for (int i = 0; i < kButtons; i++) {
            XrActionStateBoolean button{XR_TYPE_ACTION_STATE_BOOLEAN};
            get.action = buttons_[i];
            if (!check(xrGetActionStateBoolean(session_, &get, &button), "xrGetActionStateBoolean")) return false;
            if (button.isActive && button.currentState) buttons |= 1u << i;
        }
        HandInput &in = hand.input;
        in.active = pose.isActive;
        in.buttons = buttons;
        in.trigger = trigger.isActive ? trigger.currentState : 0.0f;
        in.squeeze = squeeze.isActive ? squeeze.currentState : 0.0f;
        in.thumbstick = thumbstick.isActive ? thumbstick.currentState : XrVector2f{0.0f, 0.0f};
    }
    return true;
}

bool Session::locateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation &out) {
    return check(xrLocateSpace(space, base, time, &out), "xrLocateSpace");
}

bool Session::locateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation &out, XrSpaceVelocity *velocity) {
    if (!velocity) return locateSpace(space, base, time, out);
    velocity->type = XR_TYPE_SPACE_VELOCITY;
    velocity->next = nullptr;
    velocity->velocityFlags = 0;
    void *saved = const_cast<void *>(out.next);
    out.next = velocity;
    const bool ok = check(xrLocateSpace(space, base, time, &out), "xrLocateSpace");
    out.next = saved;
    return ok;
}

bool Session::applyHaptic(int hand, XrDuration duration, float frequency, float amplitude) {
    XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
    vibration.duration = duration;
    vibration.frequency = frequency;
    vibration.amplitude = amplitude;
    XrHapticActionInfo info{XR_TYPE_HAPTIC_ACTION_INFO};
    info.action = haptic_;
    info.subactionPath = hands_[hand].path;
    return check(xrApplyHapticFeedback(session_, &info, reinterpret_cast<const XrHapticBaseHeader *>(&vibration)), "xrApplyHapticFeedback");
}

XrSpace Session::referenceSpace(XrReferenceSpaceType type) const {
    if (type == XR_REFERENCE_SPACE_TYPE_VIEW) return view_;
    if (type == XR_REFERENCE_SPACE_TYPE_LOCAL) return local_;
    if (type == XR_REFERENCE_SPACE_TYPE_STAGE) return stage_;
    return XR_NULL_HANDLE;
}

VkFormat Session::colorFormat() const {
    // Meta documents only B8G8R8A8_SRGB for Vulkan, so the BGRA twins back up each RGBA pick.
    for (const VkFormat format : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
        if (std::find(formats_.begin(), formats_.end(), static_cast<int64_t>(format)) != formats_.end()) return format;
    return VK_FORMAT_UNDEFINED;
}

XrSwapchain Session::createSwapchain(VkFormat format, uint32_t width, uint32_t height, std::vector<VkImage> &images) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    info.format = format;
    info.sampleCount = 1;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    if (!check(xrCreateSwapchain(session_, &info, &swapchain), "xrCreateSwapchain")) return XR_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(swapchainLock_);
        swapchains_.push_back(swapchain);
    }
    uint32_t count = 0;
    if (check(xrEnumerateSwapchainImages(swapchain, 0, &count, nullptr), "xrEnumerateSwapchainImages")) {
        std::vector<XrSwapchainImageVulkan2KHR> vulkanImages(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (check(xrEnumerateSwapchainImages(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader *>(vulkanImages.data())),
                  "xrEnumerateSwapchainImages")) {
            images.clear();
            for (uint32_t i = 0; i < count; i++) images.push_back(vulkanImages[i].image);
            return swapchain;
        }
    }
    destroySwapchain(swapchain);
    return XR_NULL_HANDLE;
}

void Session::destroySwapchain(XrSwapchain swapchain) {
    {
        std::lock_guard<std::mutex> lock(swapchainLock_);
        const auto found = std::find(swapchains_.begin(), swapchains_.end(), swapchain);
        if (swapchain == XR_NULL_HANDLE || found == swapchains_.end()) return;  // not ours, or already gone
        swapchains_.erase(found);
    }
    check(xrDestroySwapchain(swapchain), "xrDestroySwapchain");
}

bool Session::acquire(XrSwapchain swapchain, uint32_t &index) {
    XrSwapchainImageAcquireInfo info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    return check(xrAcquireSwapchainImage(swapchain, &info, &index), "xrAcquireSwapchainImage");
}

bool Session::wait(XrSwapchain swapchain) {
    XrSwapchainImageWaitInfo info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    info.timeout = XR_INFINITE_DURATION;
    return check(xrWaitSwapchainImage(swapchain, &info), "xrWaitSwapchainImage");
}

bool Session::release(XrSwapchain swapchain) {
    XrSwapchainImageReleaseInfo info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    return check(xrReleaseSwapchainImage(swapchain, &info), "xrReleaseSwapchainImage");
}

float Session::refreshRate() {
    float rate = 0.0f;
    if (getDisplayRefreshRate_ && !check(getDisplayRefreshRate_(session_, &rate), "xrGetDisplayRefreshRateFB")) rate = 0.0f;
    return rate;
}

bool Session::stageBounds(XrExtent2Df &bounds) {
    bounds = {0.0f, 0.0f};
    // XR_SPACE_BOUNDS_UNAVAILABLE is a success code that leaves 0 x 0.
    return stage_ == XR_NULL_HANDLE ||
           check(xrGetReferenceSpaceBoundsRect(session_, XR_REFERENCE_SPACE_TYPE_STAGE, &bounds), "xrGetReferenceSpaceBoundsRect");
}

bool Session::requestRefreshRate(float rate) {
    if (!requestDisplayRefreshRate_) {
        LOGE("xrRequestDisplayRefreshRateFB: the runtime has no XR_FB_display_refresh_rate");
        last_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        return false;
    }
    return check(requestDisplayRefreshRate_(session_, rate), "xrRequestDisplayRefreshRateFB");
}

bool Session::applyThreadHints() {
    // The frame thread drives the session, so it is the renderer main thread.
    return markApplicationThread(XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR);
}

bool Session::markApplicationThread(XrAndroidThreadTypeKHR type) {
    if (!setApplicationThread_) {
        // One line per process: without the extension every marked thread would log this.
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true)) LOGE("xrSetAndroidApplicationThreadKHR: the runtime has no XR_KHR_android_thread_settings");
        last_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        return false;
    }
    return check(setApplicationThread_(session_, type, static_cast<uint32_t>(pthread_gettid_np(pthread_self()))), "xrSetAndroidApplicationThreadKHR");
}

bool Session::setPerformanceLevels(XrPerfSettingsLevelEXT cpu, XrPerfSettingsLevelEXT gpu) {
    if (!setPerformanceLevel_) {
        // Same one-line refusal as above, for the CPU/GPU pair.
        static std::atomic<bool> refused{false};
        if (!refused.exchange(true)) LOGE("xrPerfSettingsSetPerformanceLevelEXT: the runtime has no XR_EXT_performance_settings");
        last_ = XR_ERROR_FUNCTION_UNSUPPORTED;
        return false;
    }
    // CPU then GPU, one log line for the pair the way the runtime reports them.
    const bool ok =
            check(setPerformanceLevel_(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, cpu), "xrPerfSettingsSetPerformanceLevelEXT(CPU)") &&
            check(setPerformanceLevel_(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, gpu), "xrPerfSettingsSetPerformanceLevelEXT(GPU)");
    LOGI("performance levels CPU %s GPU %s: %s", toString(cpu), toString(gpu), ok ? "applied" : "failed");
    return ok;
}

XrSwapchain Session::createSurfaceSwapchain(uint32_t width, uint32_t height, jobject &surfaceOut) {
    surfaceOut = nullptr;
    if (!createSurfaceSwapchain_) {
        LOGE("xrCreateSwapchainAndroidSurfaceKHR: the runtime has no XR_KHR_android_surface_swapchain");
        last_ = XR_ERROR_EXTENSION_NOT_PRESENT;
        return XR_NULL_HANDLE;
    }
    JNIEnv *env = nullptr;
    if (!vm_ || vm_->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        LOGE("xrCreateSwapchainAndroidSurfaceKHR: this thread is not attached to the VM");
        last_ = XR_ERROR_RUNTIME_FAILURE;
        return XR_NULL_HANDLE;
    }
    // The producer picks format, samples and layers: the extension wants those fields zero.
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.width = width;
    info.height = height;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    jobject surface = nullptr;
    if (!check(createSurfaceSwapchain_(session_, &info, &swapchain, &surface), "xrCreateSwapchainAndroidSurfaceKHR"))
        return XR_NULL_HANDLE;
    {
        std::lock_guard<std::mutex> lock(swapchainLock_);
        swapchains_.push_back(swapchain);
    }
    if (surface) {
        surfaceOut = env->NewGlobalRef(surface);
        // The spec leaves the reference kind open; a local one would pin the Surface for this thread's life.
        if (env->GetObjectRefType(surface) == JNILocalRefType) env->DeleteLocalRef(surface);
    }
    if (surfaceOut) return swapchain;
    LOGE("xrCreateSwapchainAndroidSurfaceKHR returned no Surface");
    last_ = XR_ERROR_RUNTIME_FAILURE;
    destroySwapchain(swapchain);
    return XR_NULL_HANDLE;
}

bool Session::importAhb(AHardwareBuffer *buffer, VkFormat format, uint32_t width, uint32_t height, VkImage &image,
                        VkDeviceMemory &memory) {
    image = VK_NULL_HANDLE;
    memory = VK_NULL_HANDLE;
    if (!getAhbProperties_) {
        if (!ahbRefusalLogged_.exchange(true))
            LOGE("Vulkan: no VK_ANDROID_external_memory_android_hardware_buffer, gralloc buffers cannot be imported");
        last_ = XR_ERROR_FEATURE_UNSUPPORTED;
        return false;
    }
    VkAndroidHardwareBufferFormatPropertiesANDROID formatProperties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
    VkAndroidHardwareBufferPropertiesANDROID properties{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, &formatProperties};
    if (!checkVk(getAhbProperties_(device_, buffer, &properties), "vkGetAndroidHardwareBufferPropertiesANDROID")) return false;
    // A dedicated import takes the buffer's own format, never a view of it.
    if (formatProperties.format != format || !properties.memoryTypeBits) {
        LOGE("Vulkan: the gralloc buffer imports as VkFormat %d (memory types 0x%x), not %d", static_cast<int>(formatProperties.format),
             properties.memoryTypeBits, static_cast<int>(format));
        last_ = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
        return false;
    }
    VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = format;
    imageInfo.extent = {width, height, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;  // the only tiling a gralloc image takes: gralloc's own layout
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!checkVk(createImage_(device_, &imageInfo, nullptr, &image), "vkCreateImage(AHardwareBuffer)")) {
        image = VK_NULL_HANDLE;
        return false;
    }
    VkImportAndroidHardwareBufferInfoANDROID import{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    import.buffer = buffer;
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import};
    dedicated.image = image;
    VkMemoryAllocateInfo allocInfo{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
    allocInfo.allocationSize = properties.allocationSize;
    allocInfo.memoryTypeIndex = static_cast<uint32_t>(__builtin_ctz(properties.memoryTypeBits));  // any type the buffer allows
    const VkResult allocated = allocateMemory_(device_, &allocInfo, nullptr, &memory);
    if (allocated < VK_SUCCESS) memory = VK_NULL_HANDLE;
    if (checkVk(allocated, "vkAllocateMemory(AHardwareBuffer)") && checkVk(bindImageMemory_(device_, image, memory, 0), "vkBindImageMemory"))
        return true;
    if (memory != VK_NULL_HANDLE) freeMemory_(device_, std::exchange(memory, VK_NULL_HANDLE), nullptr);
    destroyImage_(device_, std::exchange(image, VK_NULL_HANDLE), nullptr);
    return false;
}

bool Session::importSyncFd(int fd, VkSemaphore &semaphore) {
    if (!importSemaphoreFd_) {
        if (!syncFdRefusalLogged_.exchange(true)) LOGE("Vulkan: no VK_KHR_external_semaphore_fd, sync_file fences cannot be waited for on the GPU");
        last_ = XR_ERROR_FEATURE_UNSUPPORTED;
        return false;
    }
    if (semaphore == VK_NULL_HANDLE) {
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (!checkVk(createSemaphore_(device_, &info, nullptr, &semaphore), "vkCreateSemaphore")) {
            semaphore = VK_NULL_HANDLE;
            return false;
        }
    }
    VkImportSemaphoreFdInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
    import.semaphore = semaphore;
    import.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;  // a sync_file payload only imports temporarily
    import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    import.fd = fd;
    return checkVk(importSemaphoreFd_(device_, &import), "vkImportSemaphoreFdKHR");
}

}  // namespace xrbridge

// The app side of the XR bridge (PLAN.md section 3): one OpenXR instance and session on the
// headset's runtime, reached through the Khronos loader, with the Vulkan device the runtime picks,
// the VIEW, LOCAL and STAGE spaces, the Touch controller action set and swapchains. The hidden
// sample (sample_main.cpp) drives it, and so does the bridge service. Failures are logged under
// "xrbridge" and return false (or XR_NULL_HANDLE) with the XrResult in lastResult(); nothing
// aborts. One thread runs the frame loop and everything that reaches queue(), which the runtime
// shares: init, pollEvents, the frame calls, syncActions, acquire/wait/release, importSyncFd and
// shutdown. While init() and shutdown() are not running, one other thread may use the const getters,
// locate spaces and views, apply haptics, read or request the refresh rate, read the stage bounds,
// import gralloc buffers, and create swapchains (or destroy them while no frame references them).
#pragma once

#include <android/hardware_buffer.h>
#include <jni.h>

#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#ifndef VK_USE_PLATFORM_ANDROID_KHR
#define VK_USE_PLATFORM_ANDROID_KHR
#endif
#include <vulkan/vulkan.h>

#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#ifndef XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "questbridge_protocol.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace xrbridge {

// Runtime extensions the bridge plans around: XR_KHR_vulkan_enable2 and
// XR_KHR_android_create_instance (both required), the rest each enabled when present.
struct RuntimeExtensions {
    bool androidSurfaceSwapchain = false;      // XR_KHR_android_surface_swapchain, enabled when present
    bool compositionLayerDepth = false;        // XR_KHR_composition_layer_depth
    bool displayRefreshRate = false;           // XR_FB_display_refresh_rate, enabled when present
    bool compositionLayerImageLayout = false;  // XR_FB_composition_layer_image_layout
    bool handTracking = false;                 // XR_EXT_hand_tracking
    bool androidThreadSettings = false;        // XR_KHR_android_thread_settings, enabled when present
    bool performanceSettings = false;          // XR_EXT_performance_settings, enabled when present
};

// Device extensions for sharing images and fences with the guest, each enabled when the device
// offers it and what it builds on.
struct DeviceExtensions {
    bool externalMemoryFd = false;        // VK_KHR_external_memory_fd
    bool externalMemoryDmaBuf = false;    // VK_EXT_external_memory_dma_buf
    bool queueFamilyForeign = false;      // VK_EXT_queue_family_foreign
    bool imageFormatList = false;         // VK_KHR_image_format_list
    bool imageDrmFormatModifier = false;  // VK_EXT_image_drm_format_modifier
    bool androidHardwareBuffer = false;   // VK_ANDROID_external_memory_android_hardware_buffer
    bool externalSemaphoreFd = false;     // VK_KHR_external_semaphore_fd
};

// One Touch controller as of the last syncActions(); all false and zero while unfocused.
struct HandInput {
    bool active = false;   // bound, with its grip pose tracked
    uint32_t buttons = 0;  // enum qb_button bits: A/X, B/Y, menu, thumbstick, trigger and thumb rest
    float trigger = 0.0f;
    float squeeze = 0.0f;
    XrVector2f thumbstick{0.0f, 0.0f};
};

class Session {
public:
    static constexpr int kLeft = 0;
    static constexpr int kRight = 1;

    Session() = default;
    ~Session() { shutdown(); }
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    // Loader, instance, HMD system, Vulkan instance and device, session, reference spaces, view
    // configuration, swapchain formats and the Touch action set. activity must be a global
    // reference that outlives the session. After a false return shutdown() frees the partial state.
    bool init(JavaVM *vm, jobject activity);
    // Destroys everything, children first. The caller's own Vulkan objects on device() go before.
    void shutdown();

    // Drains the event queue, beginning the session on READY and ending it on STOPPING; observer,
    // when given, sees each event after that. False once the session is exiting or lost, or a call
    // failed: the caller stops and calls shutdown().
    bool pollEvents(const std::function<void(const XrEventDataBuffer &)> &observer = {});
    bool running() const { return running_; }  // between xrBeginSession and xrEndSession
    bool exiting() const { return exiting_; }  // the runtime ended it: EXITING, LOSS_PENDING or instance loss
    XrSessionState state() const { return state_; }

    // One frame, only while running(): waitFrame, beginFrame, render into acquired images,
    // endFrame. Output structs (frame, out[]) carry their XR_TYPE_* as in any OpenXR call.
    bool waitFrame(XrFrameState &frame);
    bool beginFrame();
    bool locateViews(XrSpace base, XrTime time, XrView out[2], XrViewStateFlags &flags);
    bool endFrame(XrTime displayTime, const XrCompositionLayerBaseHeader *const *layers, uint32_t layerCount);

    // Touch controllers; hand is kLeft or kRight. syncActions() refreshes input() once a frame;
    // the pose spaces are located on demand at the frame's display time.
    bool syncActions();
    const HandInput &input(int hand) const { return hands_[hand].input; }
    XrSpace gripSpace(int hand) const { return hands_[hand].grip; }
    XrSpace aimSpace(int hand) const { return hands_[hand].aim; }
    bool locateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation &out);
    // Same, chaining velocity (XR_TYPE_SPACE_VELOCITY) onto out.next for the call so the runtime
    // fills it; null chains nothing. out.next is restored before returning.
    bool locateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation &out, XrSpaceVelocity *velocity);
    bool applyHaptic(int hand, XrDuration duration, float frequency, float amplitude);

    // VIEW and LOCAL always exist after init(); STAGE only when the runtime offers it, else XR_NULL_HANDLE.
    XrSpace referenceSpace(XrReferenceSpaceType type) const;
    // The STAGE bounds; 0 x 0 when the runtime does not know them or has no STAGE.
    bool stageBounds(XrExtent2Df &bounds);
    // Through XR_FB_display_refresh_rate (0 = the runtime's choice); false without the extension.
    bool requestRefreshRate(float rate);
    // XR_KHR_android_thread_settings; false without the extension. applyThreadHints marks the
    // calling (frame) thread XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR; the session calls it once
    // after creating the session. markApplicationThread marks the calling thread as type, so the
    // service can later mark its own threads (e.g. its socket thread APPLICATION_WORKER_KHR).
    bool applyThreadHints();
    bool markApplicationThread(XrAndroidThreadTypeKHR type);
    // XR_EXT_performance_settings; false without the extension. The session calls it once after
    // xrBeginSession with SUSTAINED_HIGH for both (WinlatorXR's Quest 3 choice; BOOST is reserved
    // for short bursts); later calls re-apply, and the runtime reports changes through pollEvents.
    bool setPerformanceLevels(XrPerfSettingsLevelEXT cpu, XrPerfSettingsLevelEXT gpu);

    // Swapchains whose images take colour-attachment writes and transfers (clears, copies). An
    // acquired image arrives in COLOR_ATTACHMENT_OPTIMAL and must be released in it, with the
    // writes already submitted to queue().
    const std::vector<int64_t> &swapchainFormats() const { return formats_; }
    VkFormat colorFormat() const;  // 8-bit sRGB (RGBA, then BGRA), else UNORM, else VK_FORMAT_UNDEFINED
    XrSwapchain createSwapchain(VkFormat format, uint32_t width, uint32_t height, std::vector<VkImage> &images);
    // XR_KHR_android_surface_swapchain: a swapchain whose images a producer queues to surfaceOut, a
    // global reference the caller deletes before destroySwapchain, the only call that applies to it.
    // XR_NULL_HANDLE without the extension.
    XrSwapchain createSurfaceSwapchain(uint32_t width, uint32_t height, jobject &surfaceOut);
    void destroySwapchain(XrSwapchain swapchain);
    bool acquire(XrSwapchain swapchain, uint32_t &index);
    bool wait(XrSwapchain swapchain);
    bool release(XrSwapchain swapchain);

    // Images and fences from outside the runtime, each false (logged once) without its device
    // extension. importAhb: a transfer-source image bound to a gralloc buffer as a dedicated
    // allocation; format is the buffer's own (R8G8B8A8_UNORM for an RGBA8 buffer, whatever format
    // it is copied into). The caller destroys image and memory before shutdown(). importSyncFd:
    // semaphore (created when VK_NULL_HANDLE, so a caller can reuse one) makes its next wait wait for
    // the sync_file fd, which it owns from a true return on; after false the caller still does.
    bool importAhb(AHardwareBuffer *buffer, VkFormat format, uint32_t width, uint32_t height, VkImage &image,
                   VkDeviceMemory &memory);
    bool importSyncFd(int fd, VkSemaphore &semaphore);

    // Vulkan, as the runtime created it on the system driver.
    PFN_vkGetInstanceProcAddr instanceProcAddr() const { return getInstanceProcAddr_; }
    PFN_vkGetDeviceProcAddr deviceProcAddr() const { return getDeviceProcAddr_; }
    VkInstance vulkanInstance() const { return vkInstance_; }
    VkPhysicalDevice physicalDevice() const { return physicalDevice_; }
    VkDevice device() const { return device_; }
    uint32_t queueFamily() const { return queueFamily_; }
    VkQueue queue() const { return queue_; }
    const DeviceExtensions &deviceExtensions() const { return deviceExtensions_; }
    // What this class asked for; the runtime adds its own, which vulkan_enable2 does not report.
    const std::vector<const char *> &enabledDeviceExtensions() const { return enabledDeviceExtensions_; }

    // The runtime and the headset.
    XrInstance instance() const { return instance_; }
    XrSystemId systemId() const { return systemId_; }
    XrSession handle() const { return session_; }
    const XrInstanceProperties &instanceProperties() const { return instanceProperties_; }
    const XrSystemProperties &systemProperties() const { return systemProperties_; }
    const RuntimeExtensions &runtimeExtensions() const { return runtimeExtensions_; }
    const std::vector<XrViewConfigurationView> &viewConfigs() const { return viewConfigs_; }  // left, right
    XrEnvironmentBlendMode blendMode() const { return blendMode_; }
    const std::vector<float> &refreshRates() const { return refreshRates_; }
    float refreshRate();  // Hz, through XR_FB_display_refresh_rate; 0 when unavailable
    XrResult lastResult() const { return last_; }  // the calling thread's last failure, like errno

private:
    static constexpr int kButtons = 9;  // one boolean action per enum qb_button bit

    struct Hand {
        XrPath path = XR_NULL_PATH;
        XrSpace aim = XR_NULL_HANDLE;
        XrSpace grip = XR_NULL_HANDLE;
        HandInput input;
    };

    bool check(XrResult result, const char *what);
    bool checkVk(VkResult result, const char *what);
    template <typename Fn> bool load(const char *name, Fn &fn);
    bool createInstance(JavaVM *vm, jobject activity);
    bool createSystem();
    bool createVulkan();
    bool createSession();
    bool createActions();
    void logInteractionProfiles();

    static thread_local XrResult last_;
    JavaVM *vm_ = nullptr;
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
    bool running_ = false;
    bool exiting_ = false;
    XrInstanceProperties instanceProperties_{XR_TYPE_INSTANCE_PROPERTIES};
    XrSystemProperties systemProperties_{XR_TYPE_SYSTEM_PROPERTIES};
    RuntimeExtensions runtimeExtensions_;
    std::vector<XrViewConfigurationView> viewConfigs_;
    XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    std::vector<int64_t> formats_;
    std::mutex swapchainLock_;  // swapchains_: two threads create and destroy
    std::vector<XrSwapchain> swapchains_;
    std::vector<float> refreshRates_;
    PFN_xrGetDisplayRefreshRateFB getDisplayRefreshRate_ = nullptr;
    PFN_xrRequestDisplayRefreshRateFB requestDisplayRefreshRate_ = nullptr;
    PFN_xrCreateSwapchainAndroidSurfaceKHR createSurfaceSwapchain_ = nullptr;
    PFN_xrSetAndroidApplicationThreadKHR setApplicationThread_ = nullptr;
    PFN_xrPerfSettingsSetPerformanceLevelEXT setPerformanceLevel_ = nullptr;
    bool perfLevelsApplied_ = false;
    XrSpace view_ = XR_NULL_HANDLE;
    XrSpace local_ = XR_NULL_HANDLE;
    XrSpace stage_ = XR_NULL_HANDLE;

    XrActionSet actionSet_ = XR_NULL_HANDLE;
    XrAction aim_ = XR_NULL_HANDLE;
    XrAction grip_ = XR_NULL_HANDLE;
    XrAction trigger_ = XR_NULL_HANDLE;
    XrAction squeeze_ = XR_NULL_HANDLE;
    XrAction thumbstick_ = XR_NULL_HANDLE;
    XrAction haptic_ = XR_NULL_HANDLE;
    XrAction buttons_[kButtons] = {};  // bit i of HandInput::buttons
    Hand hands_[2];

    void *vulkan_ = nullptr;  // the system libvulkan.so
    PFN_vkGetInstanceProcAddr getInstanceProcAddr_ = nullptr;
    PFN_vkGetDeviceProcAddr getDeviceProcAddr_ = nullptr;
    PFN_vkDestroyInstance destroyInstance_ = nullptr;
    PFN_vkDestroyDevice destroyDevice_ = nullptr;
    PFN_vkDeviceWaitIdle deviceWaitIdle_ = nullptr;
    PFN_vkCreateImage createImage_ = nullptr;
    PFN_vkDestroyImage destroyImage_ = nullptr;
    PFN_vkAllocateMemory allocateMemory_ = nullptr;
    PFN_vkFreeMemory freeMemory_ = nullptr;
    PFN_vkBindImageMemory bindImageMemory_ = nullptr;
    PFN_vkCreateSemaphore createSemaphore_ = nullptr;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID getAhbProperties_ = nullptr;
    PFN_vkImportSemaphoreFdKHR importSemaphoreFd_ = nullptr;
    std::atomic<bool> ahbRefusalLogged_{false};
    std::atomic<bool> syncFdRefusalLogged_{false};
    VkInstance vkInstance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    DeviceExtensions deviceExtensions_;
    std::vector<const char *> enabledDeviceExtensions_;
};

}  // namespace xrbridge

// The hidden XR sample's native half (XrSampleActivity, PLAN.md Loop 2 item 1). A thread drives an
// xrbridge::Session: each eye is cleared to its own colour, a trigger pull buzzes that controller,
// and once a second the view poses, frame timing and Touch state are logged under "xrbridge".
#include "xr_session.h"

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#define TAG "xrbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

using xrbridge::Session;

std::mutex g_lock;  // nativeStart and nativeStop
std::thread g_thread;
jobject g_activity = nullptr;  // global reference, owned by nativeStart/nativeStop
std::atomic<bool> g_stop{false};

// Left eye red, right eye blue: a swapped or missing eye shows at a glance.
const VkClearColorValue kEyeColor[2] = {{{0.6f, 0.1f, 0.1f, 1.0f}}, {{0.1f, 0.1f, 0.6f, 1.0f}}};

struct Eye {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<VkImage> images;
};

#define CLEARER_FUNCTIONS(X) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCreateFence) X(vkDestroyFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle)

// Clears swapchain images on the session's queue: one command buffer and fence per eye.
struct Clearer {
#define CLEARER_DECLARE(name) PFN_##name name = nullptr;
    CLEARER_FUNCTIONS(CLEARER_DECLARE)
#undef CLEARER_DECLARE
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands[2] = {};
    VkFence fences[2] = {};

    bool init(const Session &xr) {
#define CLEARER_LOAD(name)                                                              \
        name = reinterpret_cast<PFN_##name>(xr.deviceProcAddr()(xr.device(), #name)); \
        if (!name) {                                                                    \
            LOGE("sample: the device lacks " #name);                                   \
            return false;                                                               \
        }
        CLEARER_FUNCTIONS(CLEARER_LOAD)
#undef CLEARER_LOAD
        device = xr.device();
        queue = xr.queue();
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = xr.queueFamily();
        VkResult result = vkCreateCommandPool(device, &poolInfo, nullptr, &pool);
        VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        allocInfo.commandPool = pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = 2;
        if (result == VK_SUCCESS) result = vkAllocateCommandBuffers(device, &allocInfo, commands);
        // Signalled, so each eye's first clear does not wait.
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        for (VkFence &fence : fences)
            if (result == VK_SUCCESS) result = vkCreateFence(device, &fenceInfo, nullptr, &fence);
        if (result != VK_SUCCESS) LOGE("sample: Vulkan setup failed: VkResult %d", result);
        return result == VK_SUCCESS;
    }

    bool clear(int eye, VkImage image, const VkClearColorValue &color) {
        // This eye's command buffer may still be in flight from the previous frame.
        VkResult result = vkWaitForFences(device, 1, &fences[eye], VK_TRUE, UINT64_MAX);
        if (result == VK_SUCCESS) result = vkResetFences(device, 1, &fences[eye]);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (result == VK_SUCCESS) result = vkBeginCommandBuffer(commands[eye], &begin);
        if (result != VK_SUCCESS) {
            LOGE("sample: clearing eye %d failed: VkResult %d", eye, result);
            return false;
        }
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        // The runtime hands the image over as a colour attachment and takes it back as one.
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // every pixel is overwritten
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = range;
        vkCmdPipelineBarrier(commands[eye], VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);
        vkCmdClearColorImage(commands[eye], image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(commands[eye], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);
        result = vkEndCommandBuffer(commands[eye]);
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands[eye];
        if (result == VK_SUCCESS) result = vkQueueSubmit(queue, 1, &submit, fences[eye]);
        if (result != VK_SUCCESS) LOGE("sample: clearing eye %d failed: VkResult %d", eye, result);
        return result == VK_SUCCESS;
    }

    void destroy() {
        if (device == VK_NULL_HANDLE) return;
        // Waits on the device, not the fences: a failed frame can leave a fence reset and never submitted.
        vkDeviceWaitIdle(device);
        for (VkFence &fence : fences)
            if (fence != VK_NULL_HANDLE) vkDestroyFence(device, std::exchange(fence, VK_NULL_HANDLE), nullptr);
        // The pool frees its command buffers.
        if (pool != VK_NULL_HANDLE) vkDestroyCommandPool(device, std::exchange(pool, VK_NULL_HANDLE), nullptr);
        device = VK_NULL_HANDLE;
    }
};

std::string pose(const XrPosef &p) {
    char text[128];
    snprintf(text, sizeof text, "pos %.3f %.3f %.3f rot %.3f %.3f %.3f %.3f", p.position.x, p.position.y, p.position.z,
             p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
    return text;
}

// One swapchain per eye at the runtime's recommended size.
bool createEyes(Session &xr, Eye (&eyes)[2]) {
    const VkFormat format = xr.colorFormat();
    if (format == VK_FORMAT_UNDEFINED) {
        LOGE("sample: the runtime offers no 8-bit RGBA or BGRA swapchain format");
        return false;
    }
    for (int i = 0; i < 2; i++) {
        Eye &eye = eyes[i];
        eye.width = xr.viewConfigs()[i].recommendedImageRectWidth;
        eye.height = xr.viewConfigs()[i].recommendedImageRectHeight;
        eye.swapchain = xr.createSwapchain(format, eye.width, eye.height, eye.images);
        if (eye.swapchain == XR_NULL_HANDLE) return false;
        LOGI("sample: eye %d swapchain %ux%u, VkFormat %d, %zu images", i, eye.width, eye.height, static_cast<int>(format),
             eye.images.size());
    }
    return true;
}

void logSetup(const Session &xr) {
    const xrbridge::RuntimeExtensions &r = xr.runtimeExtensions();
    LOGI("sample: runtime offers android_surface_swapchain %d, composition_layer_depth %d, display_refresh_rate %d, "
         "composition_layer_image_layout %d, hand_tracking %d",
         r.androidSurfaceSwapchain, r.compositionLayerDepth, r.displayRefreshRate, r.compositionLayerImageLayout, r.handTracking);
    std::string enabled;
    for (const char *name : xr.enabledDeviceExtensions()) enabled += (enabled.empty() ? "" : " ") + std::string(name);
    LOGI("sample: Vulkan device extensions enabled: %s", enabled.empty() ? "none" : enabled.c_str());
}

void logFrame(Session &xr, const XrFrameState &frame, const XrView (&views)[2], XrViewStateFlags viewFlags, VkFormat format) {
    LOGI("frame: display period %.2f ms, refresh %.1f Hz, should render %u, swapchain VkFormat %d, view flags 0x%llx",
         frame.predictedDisplayPeriod / 1e6, xr.refreshRate(), frame.shouldRender, static_cast<int>(format),
         static_cast<unsigned long long>(viewFlags));
    for (int i = 0; i < 2; i++) LOGI("view %d: %s", i, pose(views[i].pose).c_str());
    const XrSpace local = xr.referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
    for (int hand : {Session::kLeft, Session::kRight}) {
        const bool left = hand == Session::kLeft;
        XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
        const bool located = xr.locateSpace(xr.aimSpace(hand), local, frame.predictedDisplayTime, aim);
        const xrbridge::HandInput &in = xr.input(hand);
        LOGI("%s: active %d, aim %s (flags 0x%llx), %s %d, %s %d, trigger %.2f, squeeze %.2f, thumbstick %.2f %.2f",
             left ? "left" : "right", in.active, located ? pose(aim.pose).c_str() : "not located",
             static_cast<unsigned long long>(aim.locationFlags), left ? "X" : "A", (in.buttons & QB_BTN_PRIMARY_CLICK) != 0,
             left ? "Y" : "B", (in.buttons & QB_BTN_SECONDARY_CLICK) != 0, in.trigger, in.squeeze, in.thumbstick.x, in.thumbstick.y);
    }
}

// Acquire, wait, clear, release. The image goes back even when the clear failed.
bool clearEye(Session &xr, Clearer &clearer, const Eye &eye, int index) {
    uint32_t image = 0;
    if (!xr.acquire(eye.swapchain, image) || !xr.wait(eye.swapchain)) return false;
    const bool cleared = clearer.clear(index, eye.images[image], kEyeColor[index]);
    return xr.release(eye.swapchain) && cleared;
}

void runFrames(Session &xr, Clearer &clearer, const Eye (&eyes)[2], VkFormat format) {
    using Clock = std::chrono::steady_clock;
    Clock::time_point nextLog = Clock::now();
    bool triggerHeld[2] = {false, false};
    const XrSpace local = xr.referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
    while (!g_stop.load()) {
        if (!xr.pollEvents()) return;
        if (!xr.running()) {
            // xrWaitFrame is only valid in a running session: idle until the runtime says READY.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        if (!xr.waitFrame(frame) || !xr.beginFrame() || !xr.syncActions()) return;

        XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        XrViewStateFlags viewFlags = 0;
        XrCompositionLayerProjectionView projected[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
                                                         {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        const XrCompositionLayerBaseHeader *layers[] = {reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer)};
        uint32_t layerCount = 0;
        if (frame.shouldRender && xr.locateViews(local, frame.predictedDisplayTime, views, viewFlags) &&
            (viewFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) {
            for (int i = 0; i < 2; i++) {
                if (!clearEye(xr, clearer, eyes[i], i)) return;
                projected[i].pose = views[i].pose;
                projected[i].fov = views[i].fov;
                projected[i].subImage.swapchain = eyes[i].swapchain;
                projected[i].subImage.imageRect.extent = {static_cast<int32_t>(eyes[i].width), static_cast<int32_t>(eyes[i].height)};
            }
            layer.space = local;
            layer.viewCount = 2;
            layer.views = projected;
            layerCount = 1;
        }
        if (!xr.endFrame(frame.predictedDisplayTime, layers, layerCount)) return;

        // A short pulse on the controller whose trigger was just pulled: the haptic output path.
        for (int hand : {Session::kLeft, Session::kRight}) {
            const bool held = xr.input(hand).trigger > 0.5f;
            if (held && !triggerHeld[hand]) xr.applyHaptic(hand, 50000000 /* 50 ms */, XR_FREQUENCY_UNSPECIFIED, 0.5f);
            triggerHeld[hand] = held;
        }
        if (Clock::now() >= nextLog) {
            nextLog = Clock::now() + std::chrono::seconds(1);
            logFrame(xr, frame, views, viewFlags, format);
        }
    }
}

void runSession(JavaVM *vm, jobject activity) {
    Session xr;
    Clearer clearer;
    Eye eyes[2];
    if (xr.init(vm, activity) && clearer.init(xr) && createEyes(xr, eyes)) {
        logSetup(xr);
        runFrames(xr, clearer, eyes, xr.colorFormat());
    } else {
        LOGE("sample: setup failed, see above");
    }
    // Ours before the runtime's.
    clearer.destroy();
    for (Eye &eye : eyes) xr.destroySwapchain(eye.swapchain);
    xr.shutdown();
}

void run(JavaVM *vm, jobject activity) {
    JNIEnv *env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("sample: the XR thread cannot attach to the VM");
        return;
    }
    LOGI("sample: started");
    runSession(vm, activity);
    // An end nativeStop did not ask for (the runtime exiting, or a failure) finishes the activity,
    // as hello_xr does, rather than leave the headset on an empty immersive view.
    if (!g_stop.load()) {
        jclass activityClass = env->GetObjectClass(activity);
        jmethodID finish = env->GetMethodID(activityClass, "finish", "()V");
        if (finish) env->CallVoidMethod(activity, finish);
        if (env->ExceptionCheck()) {
            env->ExceptionDescribe();
            env->ExceptionClear();
        }
        env->DeleteLocalRef(activityClass);
    }
    LOGI("sample: stopped");
    vm->DetachCurrentThread();
}

}  // namespace

extern "C" JNIEXPORT void JNICALL
Java_com_droiddeck_launcher_xr_XrSampleActivity_nativeStart(JNIEnv *env, jobject, jobject activity) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_thread.joinable()) return;  // still running: a resume without the pause in between
    JavaVM *vm = nullptr;
    if (env->GetJavaVM(&vm) != JNI_OK) {
        LOGE("sample: no JavaVM");
        return;
    }
    g_activity = env->NewGlobalRef(activity);
    g_stop = false;
    try {
        g_thread = std::thread(run, vm, g_activity);
    } catch (const std::system_error &e) {
        LOGE("sample: no XR thread: %s", e.what());
        env->DeleteGlobalRef(std::exchange(g_activity, nullptr));
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_droiddeck_launcher_xr_XrSampleActivity_nativeStop(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_thread.joinable()) return;
    g_stop = true;
    g_thread.join();
    env->DeleteGlobalRef(std::exchange(g_activity, nullptr));
}

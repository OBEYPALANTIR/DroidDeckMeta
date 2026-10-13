// See bridge_service.h. Frame pipelining, image ownership and fences follow the top comment of
// questbridge_protocol.h: the guest's END_FRAME becomes the pending frame, the next real frame
// copies its images into the runtime's own swapchains (the mirrors) and submits its layers, and an
// image goes back to the guest (ACQUIRE_IMAGE) once the copy that read it has finished.
#include "bridge_service.h"

#include <android/log.h>
#include <dlfcn.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <new>
#include <system_error>
#include <utility>

#define TAG "xrbridge"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace xrbridge {
namespace {

constexpr uint64_t kModLinear = 0;                              // DRM_FORMAT_MOD_LINEAR
constexpr uint64_t kModQcomCompressed = 0x0500000000000001ULL;  // DRM_FORMAT_MOD_QCOM_COMPRESSED (UBWC)
constexpr int64_t kDefaultPeriod = 1000000000 / 72;             // until the runtime has named one
constexpr size_t kMaxEvents = 64;

// The gralloc handle behind an AHardwareBuffer (AOSP cutils/native_handle.h, which the NDK does not
// ship; a stable ABI: fds first, then ints), read as sc_layer.c reads it.
struct NativeHandle {
    int version;
    int numFds;
    int numInts;
    int data[];
};
using GetNativeHandle = const NativeHandle *(*)(const AHardwareBuffer *);

// AHardwareBuffer_getNativeHandle is exported by libnativewindow.so on every device but missing from
// the NDK headers (vndk/hardware_buffer.h), so it comes through dlsym, as in sc_layer.c.
GetNativeHandle nativeHandleApi() {
    static const GetNativeHandle get = [] {
        void *library = dlopen("libnativewindow.so", RTLD_NOW | RTLD_NOLOAD);
        if (!library) library = dlopen("libnativewindow.so", RTLD_NOW);
        const auto found = library ? reinterpret_cast<GetNativeHandle>(dlsym(library, "AHardwareBuffer_getNativeHandle")) : nullptr;
        if (!found) LOGE("bridge: libnativewindow.so has no AHardwareBuffer_getNativeHandle, gralloc buffers cannot be shared");
        return found;
    }();
    return get;
}

// Mesa's u_gralloc fallback sniff, as in sc_layer.c: a QTI gralloc private_handle_t carries 'gmsm' as
// its first int and the UBWC flag (PRIV_FLAGS_UBWC_ALIGNED, 0x08000000) in the next one. False when
// the layout is not QTI's.
bool sniffModifier(const NativeHandle *handle, uint64_t &modifier) {
    const uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
    if (!handle || handle->numFds < 1 || handle->numInts < 2 || static_cast<uint32_t>(handle->data[handle->numFds]) != gmsm)
        return false;
    modifier = (handle->data[handle->numFds + 1] & 0x08000000) ? kModQcomCompressed : kModLinear;
    return true;
}

// DROIDDECK_XR_UBWC=1 or `setprop debug.droiddeck.xr.ubwc 1`: gralloc may compress (UBWC). Off by
// default because the guest Turnip reading QTI's UBWC is unproven on Quest, and QTI gralloc never
// compresses a buffer asked for with a CPU usage bit.
bool ubwcRequested() {
    const char *env = getenv("DROIDDECK_XR_UBWC");
    char property[PROP_VALUE_MAX] = "";
    return (env && !strcmp(env, "1")) || (__system_property_get("debug.droiddeck.xr.ubwc", property) > 0 && !strcmp(property, "1"));
}

// What ENUM_FORMATS can offer: the gralloc format behind each and that buffer's own VkFormat, the one
// it imports as (an sRGB swapchain's bytes are the UNORM buffer's, copied unchanged).
struct FormatInfo {
    VkFormat format;
    uint32_t ahbFormat;
    VkFormat bufferFormat;
    uint32_t bytesPerPixel;
};
constexpr FormatInfo kFormats[] = {
        {VK_FORMAT_R8G8B8A8_SRGB, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4},
        {VK_FORMAT_R8G8B8A8_UNORM, AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4},
        {VK_FORMAT_R16G16B16A16_SFLOAT, AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8},
};
static_assert(sizeof kFormats / sizeof kFormats[0] <= QB_MAX_FORMATS, "qb_formats holds them all");

const FormatInfo *formatInfo(VkFormat format) {
    for (const FormatInfo &info : kFormats)
        if (info.format == format) return &info;
    return nullptr;
}

int64_t monotonicNs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;
}

void closeFd(int &fd) {
    if (fd >= 0) close(std::exchange(fd, -1));
}

// A sync_file polls readable once it has signalled (or failed: either way nothing renders any more).
bool signalled(int fd) {
    pollfd p{fd, POLLIN, 0};
    return poll(&p, 1, 0) > 0;
}

XrPosef toXr(const qb_posef &p) { return {{p.qx, p.qy, p.qz, p.qw}, {p.x, p.y, p.z}}; }

qb_posef toWire(const XrPosef &p) {
    return {p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y, p.position.z, 0.0f};
}

// A unit orientation within 1% and a finite position: the guest runtime's own test (NaN fails).
bool poseValid(const qb_posef &p) {
    const float n = p.qx * p.qx + p.qy * p.qy + p.qz * p.qz + p.qw * p.qw;
    return n > 0.98f && n < 1.02f && std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
}

// b given in a's frame, expressed in a's parent.
constexpr XrPosef multiply(const XrPosef &a, const XrPosef &b) {
    const XrQuaternionf &q = a.orientation;
    const XrVector3f &v = b.position;
    // q v q* as v + w t + u x t, with u = q.xyz and t = 2 u x v.
    const XrVector3f t{2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
    const XrVector3f r{v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
                       v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
    const XrQuaternionf &o = b.orientation;
    return {{q.w * o.x + q.x * o.w + q.y * o.z - q.z * o.y, q.w * o.y - q.x * o.z + q.y * o.w + q.z * o.x,
             q.w * o.z + q.x * o.y - q.y * o.x + q.z * o.w, q.w * o.w - q.x * o.x - q.y * o.y - q.z * o.z},
            {a.position.x + r.x, a.position.y + r.y, a.position.z + r.z}};
}
// Half turns, exact in floats: about Y the point (1,0,0) lands on (-1,0,0), then the offset adds
// (1,2,3); a half turn about X then one about Y is one about Z (i j = k).
constexpr XrPosef kHalfTurnY{{0.0f, 1.0f, 0.0f, 0.0f}, {1.0f, 2.0f, 3.0f}};
constexpr XrPosef kComposed = multiply(kHalfTurnY, XrPosef{{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}});
static_assert(kComposed.position.x == 0.0f && kComposed.position.y == 2.0f && kComposed.position.z == 3.0f &&
                      kComposed.orientation.y == 1.0f && kComposed.orientation.w == 0.0f,
              "multiply rotates the child's position, then offsets it");
constexpr XrPosef kHalfTurnsXY = multiply(XrPosef{{1.0f, 0.0f, 0.0f, 0.0f}, {}}, XrPosef{{0.0f, 1.0f, 0.0f, 0.0f}, {}});
static_assert(kHalfTurnsXY.orientation.x == 0.0f && kHalfTurnsXY.orientation.y == 0.0f && kHalfTurnsXY.orientation.z == 1.0f &&
                      kHalfTurnsXY.orientation.w == 0.0f,
              "multiply takes the parent's orientation first");

}  // namespace

// One request as it arrived: the header, the op's body, and the one fd a request may carry.
struct BridgeService::Request {
    qb_hdr hdr{};
    union {
        qb_hello hello;
        qb_swapchain_create createSwapchain;
        qb_swapchain_ref swapchain;
        qb_end_frame endFrame;
        qb_locate_views locateViews;
        qb_locate_space locateSpace;
        qb_haptic haptic;
        qb_refresh_rate refreshRate;
    } body{};
    int fd = -1;  // END_FRAME's sync_file, owned
};

// One reply: the op's body, or the status alone when it failed, and the fds that go with it.
struct BridgeService::Reply {
    union Any {
        qb_status status;
        qb_hello_reply hello;
        qb_formats formats;
        qb_swapchain_info swapchain;
        qb_image_index image;
        qb_frame_state frame;
        qb_views views;
        qb_space_location location;
        qb_input_state input;
        qb_stage_bounds bounds;
        qb_event event;
    };
    alignas(Any) unsigned char body[sizeof(Any)] = {};
    uint32_t size = 0;
    int fds[QB_MAX_SWAPCHAIN_IMAGES] = {};  // owned by the gralloc handles; the kernel dups them across
    uint32_t fdCount = 0;

    // A zeroed T as the body: status XR_SUCCESS.
    template <typename T> T &as() {
        static_assert(sizeof(T) <= sizeof(Any), "a reply struct");
        size = sizeof(T);
        return *new (body) T{};
    }
    // Anything that is not an error reads as a runtime failure: a failed reply must say why.
    void fail(XrResult result) {
        fdCount = 0;
        as<qb_status>().xr_result = XR_FAILED(result) ? result : XR_ERROR_RUNTIME_FAILURE;
    }
};

// ---- lifetime ------------------------------------------------------------------------------------

bool BridgeService::start(JavaVM *vm, jobject activity, const std::string &socketName, uint32_t panelWidth, uint32_t panelHeight) {
    if (frameThread_.joinable()) return state_ != kFailed;
    socketName_ = socketName;
    panelWidth_ = panelWidth;
    panelHeight_ = panelHeight;
    wakeFd_ = eventfd(0, EFD_CLOEXEC);
    if (wakeFd_ < 0) LOGE("bridge: eventfd failed: %s", strerror(errno));
    if (!panelWidth || !panelHeight || wakeFd_ < 0 || !bindSocket(socketName)) {
        state_ = kFailed;
        closeFds();
        return false;
    }
    state_ = kStarting;
    try {
        frameThread_ = std::thread(&BridgeService::run, this, vm, activity);
    } catch (const std::system_error &e) {
        LOGE("bridge: no frame thread: %s", e.what());
        state_ = kFailed;
        closeFds();
        return false;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return state_ != kStarting; });
    return state_ != kFailed;
}

void BridgeService::stop() {
    if (frameThread_.joinable()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        changed_.notify_all();
        wake();
        frameThread_.join();
    }
    closeFds();
}

jobject BridgeService::panelSurface(JNIEnv *env) {
    std::lock_guard<std::mutex> lock(surfaceLock_);
    return panelSurface_ ? env->NewLocalRef(panelSurface_) : nullptr;
}

void BridgeService::setState(State state) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        state_ = state;
    }
    changed_.notify_all();
}

// ---- frame thread --------------------------------------------------------------------------------

void BridgeService::run(JavaVM *vm, jobject activity) {
    JNIEnv *env = nullptr;
    bool ok = vm->AttachCurrentThread(&env, nullptr) == JNI_OK;
    if (!ok) {
        env = nullptr;
        LOGE("bridge: the frame thread cannot attach to the VM");
    }
    if (ok) ok = xr_.init(vm, activity) && createCopier();
    if (ok) {
        createPanel();
        try {
            socketThread_ = std::thread(&BridgeService::serve, this);
        } catch (const std::system_error &e) {
            LOGE("bridge: no socket thread: %s", e.what());
            ok = false;
        }
    }
    if (ok) LOGI("bridge: running; guests connect to %s", socketName_.c_str());
    else LOGE("bridge: setup failed, see above");
    setState(ok ? kRunning : kFailed);
    if (ok) ok = loop();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        if (state_ != kFailed) state_ = ok ? kExiting : kFailed;
    }
    changed_.notify_all();
    wake();
    if (socketThread_.joinable()) socketThread_.join();
    tearDown(env);
    LOGI("bridge: stopped");
    if (env) vm->DetachCurrentThread();
}

bool BridgeService::createCopier() {
    const VkDevice device = xr_.device();
#define XRBRIDGE_LOAD(name)                                                                    \
    vk_.name = reinterpret_cast<PFN_##name>(xr_.deviceProcAddr()(device, #name));             \
    if (!vk_.name) {                                                                           \
        LOGE("bridge: the device lacks " #name);                                               \
        return false;                                                                          \
    }
    XRBRIDGE_COPY_FUNCTIONS(XRBRIDGE_LOAD)
#undef XRBRIDGE_LOAD
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = xr_.queueFamily();
    VkResult result = vk_.vkCreateCommandPool(device, &poolInfo, nullptr, &pool_);
    if (result != VK_SUCCESS) pool_ = VK_NULL_HANDLE;
    VkCommandBuffer commands[kSlots] = {};
    VkCommandBufferAllocateInfo allocInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocInfo.commandPool = pool_;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = kSlots;
    if (result == VK_SUCCESS) result = vk_.vkAllocateCommandBuffers(device, &allocInfo, commands);
    const VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    for (uint32_t i = 0; i < kSlots && result == VK_SUCCESS; i++) {
        slots_[i].commands = commands[i];
        result = vk_.vkCreateFence(device, &fenceInfo, nullptr, &slots_[i].fence);
        if (result != VK_SUCCESS) slots_[i].fence = VK_NULL_HANDLE;
    }
    if (result != VK_SUCCESS) LOGE("bridge: copy setup failed: VkResult %d", static_cast<int>(result));
    return result == VK_SUCCESS;
}

void BridgeService::createPanel() {
    jobject surface = nullptr;
    panel_ = xr_.createSurfaceSwapchain(panelWidth_, panelHeight_, surface);
    if (panel_ == XR_NULL_HANDLE) {
        LOGE("bridge: no panel Surface, so no Steam panel; guest frames still work");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        panelSurface_ = surface;
    }
    // 1.6 m wide, 1.5 m ahead of where the session started looking.
    panelLayer_.space = xr_.referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
    panelLayer_.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
    panelLayer_.subImage.swapchain = panel_;
    panelLayer_.subImage.imageRect.extent = {static_cast<int32_t>(panelWidth_), static_cast<int32_t>(panelHeight_)};
    panelLayer_.pose.orientation.w = 1.0f;
    panelLayer_.pose.position.z = -1.5f;
    panelLayer_.size = {1.6f, 1.6f * static_cast<float>(panelHeight_) / static_cast<float>(panelWidth_)};
    panelLayers_[0] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&panelLayer_);
    LOGI("bridge: panel %ux%u as a %.2f x %.2f m quad", panelWidth_, panelHeight_, panelLayer_.size.width, panelLayer_.size.height);
}

// True when asked to stop or the runtime ended the session, false on a failure.
bool BridgeService::loop() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point nextLog = Clock::now() + std::chrono::seconds(1);
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return true;
        }
        if (!xr_.pollEvents([this](const XrEventDataBuffer &event) { onEvent(event); })) return xr_.exiting();
        if (!reap()) return false;
        destroyDoomed();
        if (!xr_.running()) {
            // xrWaitFrame is only valid in a running session: idle until the runtime says READY, with
            // no guest frame held back (its images go back to the guest).
            std::unique_lock<std::mutex> lock(mutex_);
            dropPending();
            changed_.notify_all();
            changed_.wait_for(lock, std::chrono::milliseconds(100), [this] { return stopping_; });
            continue;
        }
        XrFrameState frame{XR_TYPE_FRAME_STATE};
        if (!xr_.waitFrame(frame) || !xr_.syncActions()) return false;
        publish(frame);
        if (!xr_.beginFrame()) return false;
        const XrCompositionLayerBaseHeader *const *layers = nullptr;
        uint32_t count = 0;
        if (!compose(frame, layers, count) || !xr_.endFrame(frame.predictedDisplayTime, layers, count)) return false;
        stats_.frames++;
        if (Clock::now() >= nextLog) {
            nextLog = Clock::now() + std::chrono::seconds(1);
            const HandInput &l = xr_.input(Session::kLeft), &r = xr_.input(Session::kRight);
            LOGI("bridge: %u frames, %u guest frames, %u copies, %u dropped; touch L active %d buttons 0x%x trigger %.2f squeeze %.2f, R active %d buttons 0x%x trigger %.2f squeeze %.2f",
                 stats_.frames.exchange(0), stats_.guestFrames.exchange(0), stats_.copies.exchange(0),
                 stats_.dropped.exchange(0), l.active, l.buttons, l.trigger, l.squeeze, r.active, r.buttons,
                 r.trigger, r.squeeze);
        }
    }
}

// The real session's events, as Session::pollEvents saw them, become the guest's.
void BridgeService::onEvent(const XrEventDataBuffer &event) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (event.type) {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED: {
            const auto &changed = reinterpret_cast<const XrEventDataSessionStateChanged &>(event);
            realState_ = changed.state;
            realRunning_ = xr_.running();
            // The guest follows: READY once its own session exists, SYNCHRONIZED to FOCUSED and STOPPING
            // while it runs, the end of the session always. IDLE it reaches by itself after END_SESSION.
            switch (changed.state) {
                case XR_SESSION_STATE_READY:
                    if (guest_ == Guest::kIdle) queueState(changed.state, changed.time);
                    break;
                case XR_SESSION_STATE_SYNCHRONIZED:
                case XR_SESSION_STATE_VISIBLE:
                case XR_SESSION_STATE_FOCUSED:
                case XR_SESSION_STATE_STOPPING:
                    if (guest_ == Guest::kRunning) queueState(changed.state, changed.time);
                    break;
                case XR_SESSION_STATE_LOSS_PENDING:
                case XR_SESSION_STATE_EXITING:
                    if (guest_ != Guest::kNone) queueState(changed.state, changed.time);
                    break;
                default:
                    break;
            }
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: {
            qb_event out{};
            out.type = QB_EVENT_INSTANCE_LOSS_PENDING;
            out.time = reinterpret_cast<const XrEventDataInstanceLossPending &>(event).lossTime;
            if (connected_) queueEvent(out);
            break;
        }
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: {
            const auto &change = reinterpret_cast<const XrEventDataReferenceSpaceChangePending &>(event);
            qb_event out{};
            out.type = QB_EVENT_REFERENCE_SPACE_CHANGE_PENDING;
            out.a = change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW    ? QB_SPACE_VIEW
                    : change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? QB_SPACE_LOCAL
                    : change.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? QB_SPACE_STAGE
                                                                                : QB_SPACE_COUNT;
            out.time = change.changeTime;
            // An all-zero pose is no unit quaternion: how the wire says poseValid is false.
            if (change.poseValid) out.pose = toWire(change.poseInPreviousSpace);
            if (guest_ != Guest::kNone && out.a != QB_SPACE_COUNT) queueEvent(out);
            break;
        }
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: {
            qb_event out{};
            out.type = QB_EVENT_INTERACTION_PROFILE_CHANGED;
            out.time = monotonicNs();
            if (guest_ != Guest::kNone) queueEvent(out);
            break;
        }
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB: {
            qb_event out{};
            out.type = QB_EVENT_REFRESH_RATE_CHANGED;
            out.time = monotonicNs();
            out.f = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB &>(event).toDisplayRefreshRate;
            if (guest_ != Guest::kNone) queueEvent(out);
            break;
        }
        default:
            break;
    }
    changed_.notify_all();  // WAIT_FRAME follows realRunning_
}

// Frees the guest images of every copy that has finished.
bool BridgeService::reap() {
    bool freed = false;
    for (CopySlot &slot : slots_) {
        if (!slot.busy) continue;
        const VkResult status = vk_.vkGetFenceStatus(xr_.device(), slot.fence);
        if (status == VK_NOT_READY) continue;
        if (status != VK_SUCCESS) {
            LOGE("bridge: waiting for a copy failed: VkResult %d", static_cast<int>(status));
            return false;
        }
        slot.busy = false;
        std::lock_guard<std::mutex> lock(mutex_);
        release(slot.refs, true);
        freed = true;
    }
    if (freed) changed_.notify_all();
    return true;
}

void BridgeService::destroyDoomed() {
    std::vector<std::unique_ptr<GuestSwapchain>> dead;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = swapchains_.begin(); it != swapchains_.end();) {
            const GuestSwapchain &sc = *it->second;
            const bool unread = std::all_of(std::begin(sc.images), std::end(sc.images), [](const GuestImage &image) { return image.refs == 0; });
            if (sc.doomed && unread) {
                dead.push_back(std::move(it->second));
                it = swapchains_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (const auto &sc : dead) freeSwapchain(*sc);
}

void BridgeService::publish(const XrFrameState &frame) {
    qb_input_state input{};
    input.time = monotonicNs();
    for (int hand : {Session::kLeft, Session::kRight}) {
        const HandInput &in = xr_.input(hand);
        qb_hand_state &out = input.hands[hand];
        out.active = in.active;
        out.buttons = in.buttons;
        out.trigger = in.trigger;
        out.squeeze = in.squeeze;
        out.thumbstick_x = in.thumbstick.x;
        out.thumbstick_y = in.thumbstick.y;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // The guest's frame reaches the display one real frame after this one (the pipelining in
        // questbridge_protocol.h), so that is the time it renders for.
        frame_.predicted_display_time = frame.predictedDisplayTime + frame.predictedDisplayPeriod;
        frame_.predicted_display_period = frame.predictedDisplayPeriod;
        frame_.should_render = frame.shouldRender;
        frameSerial_++;
        input_ = input;
    }
    changed_.notify_all();
}

// This frame's layers: the newest guest frame (copied now), else the last one again, else the panel.
bool BridgeService::compose(const XrFrameState &frame, const XrCompositionLayerBaseHeader *const *&layers, uint32_t &count) {
    PendingFrame next;
    bool running = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pending_.valid) next = takePending();
        running = guest_ == Guest::kRunning;
    }
    if (next.valid) {
        CopySlot *slot = nullptr;
        // Nothing gets displayed (should_render false) or nobody wants it any more: dropped.
        const Copy copied = frame.shouldRender && running ? copyImages(next, slot) : Copy::kNoSlot;
        if (copied == Copy::kFailed) return false;
        if (copied == Copy::kDone) translate(next);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (copied == Copy::kDone) {
                // Shown again until the next guest frame, unless the guest ends its session or
                // destroys a swapchain the layers use in the meantime.
                shownIds_.clear();
                bool alive = guest_ == Guest::kRunning;
                for (const ImageRef &ref : next.refs) {
                    shownIds_.push_back(ref.swapchain->id);
                    alive = alive && !ref.swapchain->doomed;
                }
                shownValid_ = alive;
                if (slot) slot->refs = std::move(next.refs);  // freed with the copy's fence
                else release(next.refs, true);
                stats_.guestFrames++;
            } else if (copied == Copy::kNotReady && !pending_.valid) {
                pending_ = std::move(next);  // its fence has not signalled: again next frame, unless a newer frame comes
            } else {
                release(next.refs, false);
                closeFd(next.syncFd);
                stats_.dropped++;
            }
        }
        changed_.notify_all();
    }
    layers = nullptr;
    count = 0;
    if (!frame.shouldRender) return true;
    bool guestLayers = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        guestLayers = guest_ == Guest::kRunning && shownValid_;
    }
    if (guestLayers) {
        layers = shown_.layers;
        count = shown_.count;
    } else if (panel_ != XR_NULL_HANDLE) {
        layers = panelLayers_;
        count = 1;
    }
    return true;
}

// Copies the frame's new images into acquired mirror images in one submission on the session's queue.
BridgeService::Copy BridgeService::copyImages(PendingFrame &next, CopySlot *&used) {
    used = nullptr;
    ImageRef *copies[kMaxRefs];
    uint32_t n = 0;
    for (ImageRef &ref : next.refs)
        if (ref.copy) copies[n++] = &ref;
    if (n == 0) {  // only images the mirrors hold already (static swapchains)
        closeFd(next.syncFd);
        return Copy::kDone;
    }
    CopySlot *slot = nullptr;
    for (CopySlot &candidate : slots_)
        if (!candidate.busy) {
            slot = &candidate;
            break;
        }
    if (!slot) return Copy::kNoSlot;

    // The guest's rendering: done already (no fence, or it signalled), else waited for on the GPU at
    // TRANSFER; without GPU waits the frame waits for a later real frame instead.
    bool gpuWait = false;
    if (next.syncFd >= 0) {
        if (signalled(next.syncFd)) {
            closeFd(next.syncFd);
        } else if (gpuWaits_ && xr_.importSyncFd(next.syncFd, slot->wait)) {
            next.syncFd = -1;  // the semaphore owns it now
            gpuWait = true;
        } else {
            gpuWaits_ = false;  // an import that failed once fails again: poll from now on
            return Copy::kNotReady;
        }
    }
    for (uint32_t i = 0; i < n; i++) {
        const XrSwapchain mirror = copies[i]->swapchain->mirror;
        if (!xr_.acquire(mirror, copies[i]->mirrorIndex) || !xr_.wait(mirror)) return Copy::kFailed;
    }

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult result = vk_.vkBeginCommandBuffer(slot->commands, &begin);
    if (result != VK_SUCCESS) {
        LOGE("bridge: vkBeginCommandBuffer failed: VkResult %d", static_cast<int>(result));
        return Copy::kFailed;
    }
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkImageMemoryBarrier barriers[2 * kMaxRefs];
    // In: the guest image from its producer (FOREIGN, left in GENERAL with its contents) to a copy
    // source; the mirror image from the colour attachment it was acquired as to a copy destination.
    for (uint32_t i = 0; i < n; i++) {
        const GuestSwapchain &sc = *copies[i]->swapchain;
        VkImageMemoryBarrier &source = barriers[2 * i];
        source = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        source.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        source.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        source.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source.srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        source.dstQueueFamilyIndex = xr_.queueFamily();
        source.image = sc.images[copies[i]->index].image;
        source.subresourceRange = range;
        VkImageMemoryBarrier &target = barriers[2 * i + 1];
        target = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        target.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        target.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        target.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        target.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        target.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        target.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        target.image = sc.mirrorImages[copies[i]->mirrorIndex];
        target.subresourceRange = range;
    }
    // TRANSFER in the first scope chains the guest images' acquire after the semaphore wait.
    vk_.vkCmdPipelineBarrier(slot->commands, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2 * n, barriers);
    for (uint32_t i = 0; i < n; i++) {
        const GuestSwapchain &sc = *copies[i]->swapchain;
        // Same size, formats of one class: a raw copy, so the UNORM buffer's sRGB bytes stay sRGB.
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {sc.width, sc.height, 1};
        vk_.vkCmdCopyImage(slot->commands, sc.images[copies[i]->index].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           sc.mirrorImages[copies[i]->mirrorIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    }
    // Out: the mirror image back to the colour attachment the runtime takes it back as, the guest
    // image back to its producer.
    for (uint32_t i = 0; i < n; i++) {
        VkImageMemoryBarrier &source = barriers[2 * i];
        source.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        source.dstAccessMask = 0;
        source.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        source.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        source.srcQueueFamilyIndex = xr_.queueFamily();
        source.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        VkImageMemoryBarrier &target = barriers[2 * i + 1];
        target.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        target.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        target.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        target.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    vk_.vkCmdPipelineBarrier(slot->commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0,
                             nullptr, 2 * n, barriers);
    result = vk_.vkEndCommandBuffer(slot->commands);
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = gpuWait ? 1 : 0;
    submit.pWaitSemaphores = &slot->wait;
    submit.pWaitDstStageMask = &waitStage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &slot->commands;
    if (result == VK_SUCCESS) result = vk_.vkResetFences(xr_.device(), 1, &slot->fence);
    if (result == VK_SUCCESS) result = vk_.vkQueueSubmit(xr_.queue(), 1, &submit, slot->fence);
    if (result != VK_SUCCESS) {
        LOGE("bridge: copying the guest's images failed: VkResult %d", static_cast<int>(result));
        return Copy::kFailed;
    }
    slot->busy = true;
    used = slot;
    // The copies are queued ahead of anything the runtime submits after taking the images back.
    for (uint32_t i = 0; i < n; i++) {
        if (!xr_.release(copies[i]->swapchain->mirror)) return Copy::kFailed;
        copies[i]->swapchain->shown = static_cast<int>(copies[i]->index);
    }
    stats_.copies += n;
    return Copy::kDone;
}

// The guest's layers as the runtime's, onto the mirrors, into shown_.
void BridgeService::translate(const PendingFrame &next) {
    const auto subImage = [&next](const qb_sub_image &in) {
        XrSwapchainSubImage out{};
        for (const ImageRef &ref : next.refs)  // collect() put every referenced swapchain there
            if (ref.swapchain->id == in.swapchain_id) out.swapchain = ref.swapchain->mirror;
        out.imageRect = {{in.x, in.y}, {in.width, in.height}};
        return out;
    };
    LayerList &out = shown_;
    out.count = 0;
    for (uint32_t i = 0; i < next.body.layer_count; i++) {
        const qb_layer &in = next.body.layers[i];
        const uint32_t n = out.count++;
        if (in.type == QB_LAYER_PROJECTION) {
            XrCompositionLayerProjection &layer = out.projections[n];
            layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
            layer.layerFlags = in.layer_flags;
            layer.space = spaceOf(in.space);
            layer.viewCount = QB_MAX_VIEWS;
            layer.views = out.views[n];
            // The guest's view poses are in its own XrSpace, offset within `space`.
            const XrPosef offset = toXr(in.space_offset);
            for (uint32_t v = 0; v < QB_MAX_VIEWS; v++) {
                const qb_projection_view &view = in.views[v];
                XrCompositionLayerProjectionView &projected = out.views[n][v];
                projected = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                projected.pose = multiply(offset, toXr(view.pose));
                projected.fov = {view.fov.angle_left, view.fov.angle_right, view.fov.angle_up, view.fov.angle_down};
                projected.subImage = subImage(view.sub_image);
            }
            out.layers[n] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
        } else {
            XrCompositionLayerQuad &layer = out.quads[n];
            layer = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            layer.layerFlags = in.layer_flags;
            layer.space = spaceOf(in.space);
            layer.eyeVisibility = static_cast<XrEyeVisibility>(in.eye_visibility);
            layer.subImage = subImage(in.quad_image);
            layer.pose = toXr(in.quad_pose);  // the guest applied its offset already
            layer.size = {in.quad_width, in.quad_height};
            out.layers[n] = reinterpret_cast<const XrCompositionLayerBaseHeader *>(&layer);
        }
    }
}

void BridgeService::freeSwapchain(GuestSwapchain &swapchain) {
    const VkDevice device = xr_.device();
    for (GuestImage &image : swapchain.images) {
        if (image.image != VK_NULL_HANDLE) vk_.vkDestroyImage(device, image.image, nullptr);
        if (image.memory != VK_NULL_HANDLE) vk_.vkFreeMemory(device, image.memory, nullptr);
        if (image.buffer) AHardwareBuffer_release(image.buffer);
        image = GuestImage{};
    }
    xr_.destroySwapchain(std::exchange(swapchain.mirror, XR_NULL_HANDLE));
}

void BridgeService::tearDown(JNIEnv *env) {
    const VkDevice device = xr_.device();
    if (device != VK_NULL_HANDLE && vk_.vkDeviceWaitIdle) vk_.vkDeviceWaitIdle(device);
    // What the guest left, the copy path, the panel, then the session under them.
    closeFd(pending_.syncFd);
    pending_ = PendingFrame{};
    for (auto &entry : swapchains_) freeSwapchain(*entry.second);
    swapchains_.clear();
    for (CopySlot &slot : slots_) {
        if (slot.fence != VK_NULL_HANDLE) vk_.vkDestroyFence(device, slot.fence, nullptr);
        if (slot.wait != VK_NULL_HANDLE) vk_.vkDestroySemaphore(device, slot.wait, nullptr);
        slot = CopySlot{};
    }
    // The pool frees its command buffers.
    if (pool_ != VK_NULL_HANDLE) vk_.vkDestroyCommandPool(device, std::exchange(pool_, VK_NULL_HANDLE), nullptr);
    {
        std::lock_guard<std::mutex> lock(surfaceLock_);
        if (panelSurface_ && env) env->DeleteGlobalRef(panelSurface_);
        panelSurface_ = nullptr;
    }
    xr_.destroySwapchain(std::exchange(panel_, XR_NULL_HANDLE));
    xr_.shutdown();
}

// ---- socket thread -------------------------------------------------------------------------------

bool BridgeService::bindSocket(const std::string &name) {
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const bool abstract = !name.empty() && name[0] == '@';
    if (name.size() < (abstract ? 2u : 1u) || name.size() >= sizeof addr.sun_path) {
        LOGE("bridge: unusable socket name \"%s\"", name.c_str());
        return false;
    }
    memcpy(addr.sun_path, name.data(), name.size());
    socklen_t length = sizeof addr;
    if (abstract) {
        addr.sun_path[0] = '\0';  // "@name": the abstract namespace, read the way the guest's qb_ipc_connect reads it
        length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + name.size());
    } else {
        unlink(name.c_str());  // a socket file an earlier run left behind
    }
    listenFd_ = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (listenFd_ >= 0 && bind(listenFd_, reinterpret_cast<const sockaddr *>(&addr), length) == 0 && listen(listenFd_, 1) == 0)
        return true;
    LOGE("bridge: cannot listen on %s: %s", name.c_str(), strerror(errno));
    closeFd(listenFd_);
    return false;
}

void BridgeService::wake() {
    const uint64_t one = 1;
    if (wakeFd_ >= 0 && write(wakeFd_, &one, sizeof one) < 0) LOGE("bridge: waking the socket thread failed: %s", strerror(errno));
}

void BridgeService::closeFds() {
    closeFd(listenFd_);
    closeFd(wakeFd_);
}

void BridgeService::serve() {
    int guest = -1;
    for (;;) {
        pollfd fds[3] = {{wakeFd_, POLLIN, 0}, {listenFd_, POLLIN, 0}, {guest, POLLIN, 0}};
        const int ready = poll(fds, guest >= 0 ? 3 : 2, -1);
        const int error = errno;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) break;
        }
        if (ready < 0) {
            if (error == EINTR) continue;
            LOGE("bridge: poll failed, no more guests: %s", strerror(error));
            break;
        }
        if (fds[1].revents & POLLIN) acceptGuest(guest);
        if (guest >= 0 && fds[2].revents && !serveRequest(guest)) disconnect(guest);
    }
    if (guest >= 0) disconnect(guest);
}

void BridgeService::acceptGuest(int &guest) {
    const int fd = accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
        if (errno != EINTR && errno != EAGAIN && errno != ECONNABORTED) LOGE("bridge: accept failed: %s", strerror(errno));
        return;
    }
    // The socket is abstract, so anything on the device could connect: only this app's uid, the one
    // every program in the guest runs as, is served (as BwrapSpawner does).
    ucred peer{};
    socklen_t length = sizeof peer;
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &length) != 0 || peer.uid != getuid()) {
        LOGE("bridge: refused a connection from uid %d (pid %d)", static_cast<int>(peer.uid), static_cast<int>(peer.pid));
        close(fd);
        return;
    }
    if (guest >= 0) {
        refuse(fd);
        return;
    }
    guest = fd;
    std::lock_guard<std::mutex> lock(mutex_);
    connected_ = true;
    events_.clear();
    if (state_ == kRunning) state_ = kGuestConnected;
    LOGI("bridge: guest pid %d connected", static_cast<int>(peer.pid));
}

// One guest at a time: a second one's HELLO is answered with XR_ERROR_LIMIT_REACHED.
void BridgeService::refuse(int fd) {
    pollfd p{fd, POLLIN, 0};
    Request request;
    if (poll(&p, 1, 1000) > 0 && receive(fd, request)) {
        Reply reply;
        reply.fail(XR_ERROR_LIMIT_REACHED);
        sendReply(fd, request.hdr, reply);
        closeFd(request.fd);
    }
    LOGE("bridge: refused a second guest while one is connected");
    close(fd);
}

void BridgeService::disconnect(int &guest) {
    closeFd(guest);
    std::lock_guard<std::mutex> lock(mutex_);
    endGuestSession();
    connected_ = false;
    events_.clear();
    if (state_ == kGuestConnected) state_ = kRunning;
    LOGI("bridge: the guest disconnected");
}

bool BridgeService::serveRequest(int fd) {
    Request request;
    if (!receive(fd, request)) return false;
    Reply reply;
    handle(request, reply);
    closeFd(request.fd);  // unless END_FRAME took it
    return sendReply(fd, request.hdr, reply);
}

// One request datagram; false when the guest hung up or broke the framing, which ends the connection.
bool BridgeService::receive(int fd, Request &request) {
    iovec iov[2] = {{&request.hdr, sizeof request.hdr}, {&request.body, sizeof request.body}};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * QB_MAX_SWAPCHAIN_IMAGES)];
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    ssize_t n;
    while ((n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC)) < 0 && errno == EINTR) {
    }
    const int error = errno;
    int fds[QB_MAX_SWAPCHAIN_IMAGES];
    uint32_t count = 0;
    if (n > 0) {
        for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            const size_t k = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (size_t i = 0; i < k; i++) {
                int received;
                memcpy(&received, CMSG_DATA(c) + i * sizeof(int), sizeof received);
                if (count < QB_MAX_SWAPCHAIN_IMAGES) fds[count++] = received;
                else close(received);
            }
        }
    }
    const bool framed = n >= static_cast<ssize_t>(sizeof request.hdr) && !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) &&
                        request.hdr.body_len == static_cast<size_t>(n) - sizeof request.hdr && request.hdr.fd_count == count &&
                        count <= 1;
    if (framed) {
        request.fd = count ? fds[0] : -1;
        return true;
    }
    for (uint32_t i = 0; i < count; i++) close(fds[i]);
    if (n < 0) LOGE("bridge: reading from the guest failed: %s", strerror(error));
    else if (n > 0) LOGE("bridge: malformed request (%zd bytes, op %u, %u fds): dropping the guest", n, request.hdr.op, count);
    return false;
}

bool BridgeService::sendReply(int fd, const qb_hdr &request, const Reply &reply) {
    qb_hdr hdr{request.op, request.seq, reply.size, reply.fdCount};
    iovec iov[2] = {{&hdr, sizeof hdr}, {const_cast<unsigned char *>(reply.body), reply.size}};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * QB_MAX_SWAPCHAIN_IMAGES)] = {};
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;
    if (reply.fdCount) {
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * reply.fdCount);
        cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * reply.fdCount);
        memcpy(CMSG_DATA(c), reply.fds, sizeof(int) * reply.fdCount);
    }
    ssize_t n;
    // MSG_NOSIGNAL: a guest that died must not take the app down with SIGPIPE.
    while ((n = sendmsg(fd, &msg, MSG_NOSIGNAL)) < 0 && errno == EINTR) {
    }
    if (n == static_cast<ssize_t>(sizeof hdr + reply.size)) return true;
    if (n < 0 && errno != EPIPE && errno != ECONNRESET) LOGE("bridge: replying to the guest failed: %s", strerror(errno));
    return false;
}

void BridgeService::handle(Request &request, Reply &reply) {
    reply.fail(XR_ERROR_VALIDATION_FAILURE);  // a body that is not the op's struct, unless a handler answers
    const qb_hdr &hdr = request.hdr;
    const auto sized = [&hdr](size_t size) { return hdr.body_len == size; };
    if (request.fd >= 0 && hdr.op != QB_OP_END_FRAME) return;
    switch (hdr.op) {
        case QB_OP_HELLO:
            if (sized(sizeof(qb_hello))) onHello(request.body.hello, reply);
            return;
        case QB_OP_CREATE_SESSION:
            if (sized(0)) onCreateSession(reply);
            return;
        case QB_OP_BEGIN_SESSION:
            if (sized(0)) onBeginSession(reply);
            return;
        case QB_OP_END_SESSION:
            if (sized(0)) onEndSession(reply);
            return;
        case QB_OP_DESTROY_SESSION:
            if (sized(0)) onDestroySession(reply);
            return;
        case QB_OP_REQUEST_EXIT:
            if (sized(0)) onRequestExit(reply);
            return;
        case QB_OP_POLL_EVENT:
            if (sized(0)) onPollEvent(reply);
            return;
        case QB_OP_ENUM_FORMATS:
            if (sized(0)) onEnumFormats(reply);
            return;
        case QB_OP_CREATE_SWAPCHAIN:
            if (sized(sizeof(qb_swapchain_create))) onCreateSwapchain(request.body.createSwapchain, reply);
            return;
        case QB_OP_DESTROY_SWAPCHAIN:
            if (sized(sizeof(qb_swapchain_ref))) onDestroySwapchain(request.body.swapchain, reply);
            return;
        case QB_OP_ACQUIRE_IMAGE:
            if (sized(sizeof(qb_swapchain_ref))) onAcquireImage(request.body.swapchain, reply);
            return;
        case QB_OP_WAIT_FRAME:
            if (sized(0)) onWaitFrame(reply);
            return;
        case QB_OP_BEGIN_FRAME:
            if (sized(0)) onBeginFrame(reply);
            return;
        case QB_OP_END_FRAME:
            if (sized(sizeof(qb_end_frame))) onEndFrame(request.body.endFrame, request.fd, reply);
            return;
        case QB_OP_LOCATE_VIEWS:
            if (sized(sizeof(qb_locate_views))) onLocateViews(request.body.locateViews, reply);
            return;
        case QB_OP_LOCATE_SPACE:
            if (sized(sizeof(qb_locate_space))) onLocateSpace(request.body.locateSpace, reply);
            return;
        case QB_OP_SYNC_ACTIONS:
            if (sized(0)) onSyncActions(reply);
            return;
        case QB_OP_APPLY_HAPTIC:
            if (sized(sizeof(qb_haptic))) onApplyHaptic(request.body.haptic, reply);
            return;
        case QB_OP_SET_REFRESH_RATE:
            if (sized(sizeof(qb_refresh_rate))) onSetRefreshRate(request.body.refreshRate, reply);
            return;
        case QB_OP_STAGE_BOUNDS:
            if (sized(0)) onStageBounds(reply);
            return;
        default:
            reply.fail(XR_ERROR_FUNCTION_UNSUPPORTED);
            return;
    }
}

// ---- requests ------------------------------------------------------------------------------------

void BridgeService::onHello(const qb_hello &hello, Reply &reply) {
    char app[QB_NAME_LEN + 1] = {};
    memcpy(app, hello.app_name, QB_NAME_LEN);  // the guest's string, not trusted to be terminated
    LOGI("bridge: hello from \"%s\" (pid %u), protocol %u", app, hello.guest_pid, hello.protocol_version);
    if (hello.protocol_version != QB_PROTOCOL_VERSION) return reply.fail(XR_ERROR_RUNTIME_UNAVAILABLE);
    const XrSystemProperties &system = xr_.systemProperties();
    qb_hello_reply &out = reply.as<qb_hello_reply>();
    out.protocol_version = QB_PROTOCOL_VERSION;
    out.vendor_id = system.vendorId;
    snprintf(out.system_name, sizeof out.system_name, "%s", system.systemName);
    snprintf(out.runtime_name, sizeof out.runtime_name, "%s", xr_.instanceProperties().runtimeName);
    out.runtime_version = xr_.instanceProperties().runtimeVersion;
    out.max_swapchain_width = system.graphicsProperties.maxSwapchainImageWidth;
    out.max_swapchain_height = system.graphicsProperties.maxSwapchainImageHeight;
    out.max_layer_count = system.graphicsProperties.maxLayerCount;
    out.orientation_tracking = system.trackingProperties.orientationTracking;
    out.position_tracking = system.trackingProperties.positionTracking;
    out.view_count = QB_MAX_VIEWS;
    for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
        const XrViewConfigurationView &view = xr_.viewConfigs()[i];
        out.views[i] = {view.recommendedImageRectWidth, view.recommendedImageRectHeight, view.maxImageRectWidth,
                        view.maxImageRectHeight, view.recommendedSwapchainSampleCount, view.maxSwapchainSampleCount};
    }
    for (uint32_t space = 0; space < QB_SPACE_COUNT; space++)
        if (spaceOf(space) != XR_NULL_HANDLE) out.space_mask |= 1u << space;
    const std::vector<float> &rates = xr_.refreshRates();
    out.refresh_rate_count = static_cast<uint32_t>(std::min<size_t>(rates.size(), QB_MAX_REFRESH_RATES));
    std::copy_n(rates.begin(), out.refresh_rate_count, out.refresh_rates);
    out.current_refresh_rate = xr_.refreshRate();
}

void BridgeService::onCreateSession(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ != Guest::kNone) return reply.fail(XR_ERROR_LIMIT_REACHED);  // one session per instance
    guest_ = Guest::kIdle;
    exitRequested_ = false;
    // The guest queued IDLE itself; READY comes once the real session runs, now or later.
    if (realRunning_) queueState(XR_SESSION_STATE_READY, monotonicNs());
    reply.as<qb_status>();
}

void BridgeService::onBeginSession(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ == Guest::kNone) return reply.fail(XR_ERROR_HANDLE_INVALID);
    if (guest_ == Guest::kRunning) return reply.fail(XR_ERROR_SESSION_RUNNING);
    if (!realRunning_) return reply.fail(XR_ERROR_SESSION_NOT_READY);
    guest_ = Guest::kRunning;
    shownValid_ = false;
    // Catch up with the real session, which may be well past READY.
    const int64_t now = monotonicNs();
    for (XrSessionState state : {XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_FOCUSED})
        if (realState_ >= state && realState_ <= XR_SESSION_STATE_FOCUSED) queueState(state, now);
    LOGI("bridge: the guest's session began; its layers replace the panel");
    reply.as<qb_status>();
}

void BridgeService::onEndSession(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ != Guest::kRunning) return reply.fail(XR_ERROR_SESSION_NOT_RUNNING);
    guest_ = Guest::kIdle;
    shownValid_ = false;
    dropPending();
    // The guest queued IDLE itself. After xrRequestExitSession comes EXITING; otherwise the real session
    // stopped it, and READY returns with the real session.
    if (exitRequested_) queueState(XR_SESSION_STATE_EXITING, monotonicNs());
    else if (realRunning_) queueState(XR_SESSION_STATE_READY, monotonicNs());
    LOGI("bridge: the guest's session ended; the panel is back");
    reply.as<qb_status>();
}

void BridgeService::onDestroySession(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ == Guest::kNone) return reply.fail(XR_ERROR_HANDLE_INVALID);
    endGuestSession();
    reply.as<qb_status>();
}

void BridgeService::onRequestExit(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ != Guest::kRunning) return reply.fail(XR_ERROR_SESSION_NOT_RUNNING);
    exitRequested_ = true;
    queueState(XR_SESSION_STATE_STOPPING, monotonicNs());
    reply.as<qb_status>();
}

void BridgeService::onPollEvent(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    qb_event &out = reply.as<qb_event>();  // QB_EVENT_NONE when there is none
    if (events_.empty()) return;
    out = events_.front();
    events_.pop_front();
}

void BridgeService::onEnumFormats(Reply &reply) {
    qb_formats &out = reply.as<qb_formats>();
    for (const FormatInfo &info : kFormats)
        if (offered(info.format)) out.formats[out.count++] = info.format;
}

void BridgeService::onCreateSwapchain(const qb_swapchain_create &create, Reply &reply) {
    const VkFormat format = static_cast<VkFormat>(create.format);
    const FormatInfo *info = formatInfo(format);
    if (!info || !offered(format)) return reply.fail(XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED);
    const GetNativeHandle nativeHandle = nativeHandleApi();
    if (!nativeHandle) return reply.fail(XR_ERROR_FEATURE_UNSUPPORTED);
    auto swapchain = std::make_unique<GuestSwapchain>();
    GuestSwapchain &sc = *swapchain;
    sc.format = format;
    sc.width = create.width;
    sc.height = create.height;
    // The mirror first: the runtime judges size and format, with its own XrResult.
    sc.mirror = xr_.createSwapchain(format, sc.width, sc.height, sc.mirrorImages);
    if (sc.mirror == XR_NULL_HANDLE) return reply.fail(xr_.lastResult());

    const bool ubwc = ubwcRequested();
    AHardwareBuffer_Desc desc{};
    desc.width = sc.width;
    desc.height = sc.height;
    desc.layers = 1;
    desc.format = info->ahbFormat;
    desc.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                 (ubwc ? 0 : AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY);
    int fds[kImages] = {};
    uint32_t stride = 0;
    uint64_t modifier = kModLinear;
    bool sniffed = false;
    off_t size = 0;
    const char *failure = nullptr;
    XrResult result = XR_ERROR_RUNTIME_FAILURE;
    for (uint32_t i = 0; i < kImages && !failure; i++) {
        GuestImage &image = sc.images[i];
        if (AHardwareBuffer_allocate(&desc, &image.buffer) != 0 || !image.buffer) {
            image.buffer = nullptr;
            failure = "AHardwareBuffer_allocate failed";
            result = XR_ERROR_OUT_OF_MEMORY;
            break;
        }
        const NativeHandle *handle = nativeHandle(image.buffer);
        if (!handle || handle->numFds < 1) {
            failure = "the gralloc handle has no dma-buf";
            break;
        }
        AHardwareBuffer_Desc got{};
        AHardwareBuffer_describe(image.buffer, &got);
        uint64_t layout = kModLinear;  // an unreadable handle reads as linear, which the CPU bits ask for
        const bool known = sniffModifier(handle, layout);
        const uint32_t pitch = got.stride * info->bytesPerPixel;
        if (i == 0) {
            stride = pitch;
            modifier = layout;
            sniffed = known;
            size = lseek(handle->data[0], 0, SEEK_END);
        } else if (pitch != stride || layout != modifier) {
            failure = "gralloc gave one swapchain's buffers different layouts";
            break;
        }
        if (!xr_.importAhb(image.buffer, info->bufferFormat, sc.width, sc.height, image.image, image.memory)) {
            failure = "the runtime's Vulkan device cannot import the buffer";
            result = xr_.lastResult();
            break;
        }
        fds[i] = handle->data[0];
    }
    if (!failure && size <= 0) failure = "the dma-buf reports no size";
    if (failure) {
        LOGE("bridge: guest swapchain %ux%u VkFormat %d: %s", sc.width, sc.height, static_cast<int>(format), failure);
        freeSwapchain(sc);
        return reply.fail(result);
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sc.id = nextSwapchainId_++;
        if (nextSwapchainId_ == 0) nextSwapchainId_ = 1;  // ids are non-zero
        swapchains_.emplace(sc.id, std::move(swapchain));
    }
    LOGI("bridge: swapchain %u: %ux%u VkFormat %d, %u gralloc buffers, %s (%s), stride %u bytes, %lld bytes each, mirror of %zu images",
         sc.id, sc.width, sc.height, static_cast<int>(format), kImages, modifier == kModQcomCompressed ? "UBWC" : "linear",
         !sniffed ? "handle not QTI's, assumed" : ubwc ? "GPU-only usage, debug.droiddeck.xr.ubwc" : "CPU usage bits",
         stride, static_cast<long long>(size), sc.mirrorImages.size());
    qb_swapchain_info &out = reply.as<qb_swapchain_info>();
    out.swapchain_id = sc.id;
    out.image_count = kImages;
    out.stride_bytes = stride;
    out.modifier = modifier;
    out.size_bytes = static_cast<uint64_t>(size);
    std::copy(std::begin(fds), std::end(fds), reply.fds);
    reply.fdCount = kImages;
}

void BridgeService::onDestroySwapchain(const qb_swapchain_ref &ref, Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    GuestSwapchain *sc = findSwapchain(ref.swapchain_id);
    if (!sc) return reply.fail(XR_ERROR_HANDLE_INVALID);
    sc->doomed = true;
    if (std::find(shownIds_.begin(), shownIds_.end(), sc->id) != shownIds_.end()) shownValid_ = false;
    reply.as<qb_status>();
}

void BridgeService::onAcquireImage(const qb_swapchain_ref &ref, Reply &reply) {
    std::unique_lock<std::mutex> lock(mutex_);
    GuestSwapchain *sc = findSwapchain(ref.swapchain_id);
    if (!sc) return reply.fail(XR_ERROR_HANDLE_INVALID);
    const uint32_t index = sc->next;
    GuestImage &image = sc->images[index];
    // Handed out again only once no frame still has to read it.
    changed_.wait(lock, [&] { return stopping_ || image.refs == 0; });
    if (stopping_) return reply.fail(XR_ERROR_INSTANCE_LOST);
    sc->next = (index + 1) % kImages;
    image.dirty = true;
    reply.as<qb_image_index>().index = index;
}

void BridgeService::onWaitFrame(Reply &reply) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (guest_ != Guest::kRunning) return reply.fail(XR_ERROR_SESSION_NOT_RUNNING);
    // The newest real frame the guest has not had yet, else the next one.
    while (!stopping_ && frameSerial_ == handedSerial_) {
        if (realRunning_) {
            changed_.wait(lock);
            continue;
        }
        // No real frames while the runtime has its session stopped: the guest keeps looping at the last
        // rate with nothing to render, until the STOPPING it was sent ends its session.
        const int64_t period = frame_.predicted_display_period > 0 ? frame_.predicted_display_period : kDefaultPeriod;
        if (changed_.wait_for(lock, std::chrono::nanoseconds(period)) == std::cv_status::timeout && frameSerial_ == handedSerial_ &&
            !realRunning_) {
            qb_frame_state &out = reply.as<qb_frame_state>();
            out.predicted_display_time = monotonicNs() + period;
            out.predicted_display_period = period;
            out.should_render = 0;
            return;
        }
    }
    if (stopping_) return reply.fail(XR_ERROR_INSTANCE_LOST);
    handedSerial_ = frameSerial_;
    reply.as<qb_frame_state>() = frame_;
}

void BridgeService::onBeginFrame(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ != Guest::kRunning) return reply.fail(XR_ERROR_SESSION_NOT_RUNNING);
    reply.as<qb_status>();
}

void BridgeService::onEndFrame(const qb_end_frame &frame, int &fd, Reply &reply) {
    if (frame.layer_count > QB_MAX_LAYERS) return reply.fail(XR_ERROR_LAYER_LIMIT_EXCEEDED);
    std::lock_guard<std::mutex> lock(mutex_);
    if (guest_ != Guest::kRunning) return reply.fail(XR_ERROR_SESSION_NOT_RUNNING);
    PendingFrame next;
    const XrResult result = collect(frame, next.refs);
    if (XR_FAILED(result)) return reply.fail(result);
    // The newest complete frame wins: one the frame thread has not taken yet is dropped.
    dropPending();
    next.valid = true;
    next.body = frame;
    next.syncFd = std::exchange(fd, -1);
    for (const ImageRef &ref : next.refs) ref.swapchain->images[ref.index].refs++;
    pending_ = std::move(next);
    reply.as<qb_status>();
}

void BridgeService::onLocateViews(const qb_locate_views &locate, Reply &reply) {
    const XrSpace base = spaceOf(locate.base_space);
    if (base == XR_NULL_HANDLE) return reply.fail(XR_ERROR_VALIDATION_FAILURE);
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrViewStateFlags flags = 0;
    if (!xr_.locateViews(base, locate.time, views, flags)) return reply.fail(xr_.lastResult());
    qb_views &out = reply.as<qb_views>();
    out.view_state_flags = flags;
    out.count = QB_MAX_VIEWS;
    for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
        out.poses[i] = toWire(views[i].pose);
        out.fovs[i] = {views[i].fov.angleLeft, views[i].fov.angleRight, views[i].fov.angleUp, views[i].fov.angleDown};
    }
}

void BridgeService::onLocateSpace(const qb_locate_space &locate, Reply &reply) {
    const XrSpace space = spaceOf(locate.space);
    const XrSpace base = spaceOf(locate.base_space);
    if (space == XR_NULL_HANDLE || base == XR_NULL_HANDLE) return reply.fail(XR_ERROR_VALIDATION_FAILURE);
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    if (!xr_.locateSpace(space, base, locate.time, location, &velocity)) return reply.fail(xr_.lastResult());
    qb_space_location &out = reply.as<qb_space_location>();
    out.location_flags = location.locationFlags;
    out.pose = toWire(location.pose);
    out.velocity_flags = velocity.velocityFlags;
    // No velocity: the vectors stay a defined zero rather than whatever the runtime left behind.
    out.linear[0] = velocity.velocityFlags ? velocity.linearVelocity.x : 0.0f;
    out.linear[1] = velocity.velocityFlags ? velocity.linearVelocity.y : 0.0f;
    out.linear[2] = velocity.velocityFlags ? velocity.linearVelocity.z : 0.0f;
    out.angular[0] = velocity.velocityFlags ? velocity.angularVelocity.x : 0.0f;
    out.angular[1] = velocity.velocityFlags ? velocity.angularVelocity.y : 0.0f;
    out.angular[2] = velocity.velocityFlags ? velocity.angularVelocity.z : 0.0f;
}

void BridgeService::onSyncActions(Reply &reply) {
    std::lock_guard<std::mutex> lock(mutex_);
    reply.as<qb_input_state>() = input_;
}

void BridgeService::onApplyHaptic(const qb_haptic &haptic, Reply &reply) {
    if (haptic.hand > 1) return reply.fail(XR_ERROR_VALIDATION_FAILURE);
    if (!xr_.applyHaptic(static_cast<int>(haptic.hand), haptic.duration_ns, haptic.frequency, haptic.amplitude))
        return reply.fail(xr_.lastResult());
    reply.as<qb_status>();
}

void BridgeService::onSetRefreshRate(const qb_refresh_rate &rate, Reply &reply) {
    if (!xr_.requestRefreshRate(rate.rate)) return reply.fail(xr_.lastResult());
    reply.as<qb_status>();
}

void BridgeService::onStageBounds(Reply &reply) {
    XrExtent2Df bounds{};
    if (!xr_.stageBounds(bounds)) return reply.fail(xr_.lastResult());
    qb_stage_bounds &out = reply.as<qb_stage_bounds>();
    out.width = bounds.width;
    out.depth = bounds.height;  // the rect's second axis is the floor's depth
}

// ---- shared helpers ------------------------------------------------------------------------------

XrSpace BridgeService::spaceOf(uint32_t space) const {
    switch (space) {
        case QB_SPACE_VIEW: return xr_.referenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW);
        case QB_SPACE_LOCAL: return xr_.referenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL);
        case QB_SPACE_STAGE: return xr_.referenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE);
        case QB_SPACE_LEFT_GRIP: return xr_.gripSpace(Session::kLeft);
        case QB_SPACE_LEFT_AIM: return xr_.aimSpace(Session::kLeft);
        case QB_SPACE_RIGHT_GRIP: return xr_.gripSpace(Session::kRight);
        case QB_SPACE_RIGHT_AIM: return xr_.aimSpace(Session::kRight);
        default: return XR_NULL_HANDLE;
    }
}

bool BridgeService::offered(VkFormat format) const {
    const std::vector<int64_t> &formats = xr_.swapchainFormats();
    return std::find(formats.begin(), formats.end(), static_cast<int64_t>(format)) != formats.end();
}

BridgeService::GuestSwapchain *BridgeService::findSwapchain(uint32_t id) {
    const auto found = swapchains_.find(id);
    return found != swapchains_.end() && !found->second->doomed ? found->second.get() : nullptr;
}

// The images a frame's layers read, one entry per image, after checking what the runtime would
// refuse in xrEndFrame (which would end the session for everyone): XR_SUCCESS or why the frame is not taken.
XrResult BridgeService::collect(const qb_end_frame &frame, std::vector<ImageRef> &refs) {
    const auto add = [&](const qb_sub_image &sub) {
        GuestSwapchain *sc = findSwapchain(sub.swapchain_id);
        if (!sc || sub.image_index >= kImages || sub.array_index != 0) return XR_ERROR_LAYER_INVALID;
        if (sub.x < 0 || sub.y < 0 || sub.width <= 0 || sub.height <= 0 || static_cast<int64_t>(sub.x) + sub.width > sc->width ||
            static_cast<int64_t>(sub.y) + sub.height > sc->height)
            return XR_ERROR_SWAPCHAIN_RECT_INVALID;
        for (const ImageRef &ref : refs)
            if (ref.swapchain == sc) return ref.index == sub.image_index ? XR_SUCCESS : XR_ERROR_LAYER_INVALID;  // one image per swapchain and frame
        refs.push_back({sc, sub.image_index});
        return XR_SUCCESS;
    };
    for (uint32_t i = 0; i < frame.layer_count; i++) {
        const qb_layer &layer = frame.layers[i];
        if (spaceOf(layer.space) == XR_NULL_HANDLE) return XR_ERROR_LAYER_INVALID;
        if (layer.type == QB_LAYER_PROJECTION) {
            if (layer.view_count != QB_MAX_VIEWS) return XR_ERROR_LAYER_INVALID;
            if (!poseValid(layer.space_offset)) return XR_ERROR_POSE_INVALID;
            for (const qb_projection_view &view : layer.views) {
                if (!poseValid(view.pose)) return XR_ERROR_POSE_INVALID;
                if (const XrResult result = add(view.sub_image); XR_FAILED(result)) return result;
            }
        } else if (layer.type == QB_LAYER_QUAD) {
            if (!poseValid(layer.quad_pose)) return XR_ERROR_POSE_INVALID;
            if (!(layer.quad_width > 0.0f) || !(layer.quad_height > 0.0f) || !std::isfinite(layer.quad_width) ||
                !std::isfinite(layer.quad_height) || layer.eye_visibility > XR_EYE_VISIBILITY_RIGHT)
                return XR_ERROR_VALIDATION_FAILURE;
            if (const XrResult result = add(layer.quad_image); XR_FAILED(result)) return result;
        } else {
            return XR_ERROR_LAYER_INVALID;
        }
    }
    return XR_SUCCESS;
}

// Moves the newest guest frame out and decides, image by image, what the mirrors need copied: what
// the guest drew since its last copy, or anything the mirror does not show right now.
BridgeService::PendingFrame BridgeService::takePending() {
    PendingFrame next = std::move(pending_);
    pending_ = PendingFrame{};
    for (ImageRef &ref : next.refs) {
        GuestImage &image = ref.swapchain->images[ref.index];
        ref.copy = ref.copy || image.dirty || ref.swapchain->shown != static_cast<int>(ref.index);
        image.dirty = false;
    }
    return next;
}

// Frees the images a frame was reading; one it never copied stays marked as drawn.
void BridgeService::release(std::vector<ImageRef> &refs, bool copied) {
    for (const ImageRef &ref : refs) {
        GuestImage &image = ref.swapchain->images[ref.index];
        image.refs--;
        if (!copied && ref.copy) image.dirty = true;
    }
    refs.clear();
}

void BridgeService::dropPending() {
    if (!pending_.valid) return;
    release(pending_.refs, false);
    closeFd(pending_.syncFd);
    pending_ = PendingFrame{};
    stats_.dropped++;
}

// The guest's session is gone with everything it made: its pending frame is dropped, and its
// swapchains go to the frame thread to free once no copy reads them.
void BridgeService::endGuestSession() {
    guest_ = Guest::kNone;
    exitRequested_ = false;
    shownValid_ = false;
    dropPending();
    for (auto &entry : swapchains_) entry.second->doomed = true;
}

void BridgeService::queueState(XrSessionState state, int64_t time) {
    qb_event event{};
    event.type = QB_EVENT_SESSION_STATE;
    event.a = static_cast<uint32_t>(state);
    event.time = time;
    queueEvent(event);
}

void BridgeService::queueEvent(const qb_event &event) {
    if (events_.size() < kMaxEvents) {
        events_.push_back(event);
        return;
    }
    LOGE("bridge: the guest stopped polling events; event type %u dropped", event.type);
}

}  // namespace xrbridge

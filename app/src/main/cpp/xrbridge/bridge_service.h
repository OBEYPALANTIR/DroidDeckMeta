// The xr-bridge-service (PLAN.md section 3 item 2; Loop 2 items 2 to 4): one xrbridge::Session on
// the headset's runtime that shows the Steam panel (a Surface swapchain) as a quad and composites
// the frames of one Linux guest (libopenxr_questbridge.so) connected to an abstract SOCK_SEQPACKET
// socket; the wire is questbridge_protocol.h. The frame thread owns the frame loop, the Vulkan queue
// and every teardown. The socket thread answers the guest against state the two share under one
// mutex, waiting on it where a request blocks (WAIT_FRAME, ACQUIRE_IMAGE): requests do not queue
// for the frame thread, which spends most of each frame inside xrWaitFrame while a guest frame
// takes half a dozen round trips.
#pragma once

#include "xr_session.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xrbridge {

// The copy path's Vulkan entry points, loaded through Session::deviceProcAddr().
#define XRBRIDGE_COPY_FUNCTIONS(X)                                                                              \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)         \
    X(vkEndCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCreateFence) X(vkDestroyFence)          \
    X(vkGetFenceStatus) X(vkResetFences) X(vkQueueSubmit) X(vkDeviceWaitIdle) X(vkDestroyImage) X(vkFreeMemory) \
    X(vkDestroySemaphore)

class BridgeService {
public:
    // What nativeState() reports (XrBridge.kt).
    enum State { kFailed = -1, kIdle = 0, kStarting = 1, kRunning = 2, kGuestConnected = 3, kExiting = 4 };

    BridgeService() = default;
    ~BridgeService() { stop(); }
    BridgeService(const BridgeService &) = delete;
    BridgeService &operator=(const BridgeService &) = delete;

    // Once per object: binds socketName ("@name" = abstract), starts the frame thread and returns once
    // its session runs (true) or could not be created (false). activity: a global reference that
    // outlives the service.
    bool start(JavaVM *vm, jobject activity, const std::string &socketName, uint32_t panelWidth, uint32_t panelHeight);
    // Ends the session and joins both threads; safe to call twice.
    void stop();
    int state() const { return state_.load(); }
    // A new local reference to the Steam panel's Surface; null before the session runs or without
    // XR_KHR_android_surface_swapchain.
    jobject panelSurface(JNIEnv *env);

private:
    static constexpr uint32_t kImages = 3;                              // per guest swapchain
    static constexpr uint32_t kSlots = 3;                               // copy submissions in flight
    static constexpr uint32_t kMaxRefs = QB_MAX_LAYERS * QB_MAX_VIEWS;  // distinct images one frame reads

    struct GuestImage {
        AHardwareBuffer *buffer = nullptr;
        VkImage image = VK_NULL_HANDLE;  // the buffer as a copy source on the runtime's device
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint32_t refs = 0;   // frames (pending or being copied) that read it; ACQUIRE_IMAGE waits for 0
        bool dirty = false;  // acquired by the guest since its last copy
    };
    struct GuestSwapchain {
        uint32_t id = 0;  // set before it is shared, then fixed
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t width = 0;
        uint32_t height = 0;
        GuestImage images[kImages];
        uint32_t next = 0;    // ACQUIRE_IMAGE hands indices out round-robin
        bool doomed = false;  // the guest is done with it; the frame thread frees it once nothing reads it
        XrSwapchain mirror = XR_NULL_HANDLE;  // the runtime's swapchain the layers show, a copy of the guest's
        std::vector<VkImage> mirrorImages;
        int shown = -1;  // the guest image the mirror's last released image holds (frame thread)
    };
    struct ImageRef {
        GuestSwapchain *swapchain = nullptr;
        uint32_t index = 0;
        bool copy = false;         // the mirror has to take this image in
        uint32_t mirrorIndex = 0;  // the mirror image it is copied into (frame thread)
    };
    struct PendingFrame {
        bool valid = false;
        qb_end_frame body{};
        int syncFd = -1;              // owned until closed or imported
        std::vector<ImageRef> refs;  // each distinct image the layers read, counted in its refs
    };
    struct CopySlot {
        VkCommandBuffer commands = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore wait = VK_NULL_HANDLE;  // the guest's sync_file, imported for one submission
        bool busy = false;                  // submitted, fence not seen signalled yet
        std::vector<ImageRef> refs;         // the guest images it reads, freed with the fence
    };
    // The last guest frame shown, kept to submit again until the guest's next one is copied.
    struct LayerList {
        uint32_t count = 0;
        XrCompositionLayerProjection projections[QB_MAX_LAYERS];
        XrCompositionLayerProjectionView views[QB_MAX_LAYERS][QB_MAX_VIEWS];
        XrCompositionLayerQuad quads[QB_MAX_LAYERS];
        const XrCompositionLayerBaseHeader *layers[QB_MAX_LAYERS];
    };
    struct Vk {
#define XRBRIDGE_DECLARE(name) PFN_##name name = nullptr;
        XRBRIDGE_COPY_FUNCTIONS(XRBRIDGE_DECLARE)
#undef XRBRIDGE_DECLARE
    };
    struct Stats {  // the once-a-second line
        std::atomic<uint32_t> frames{0};
        std::atomic<uint32_t> guestFrames{0};
        std::atomic<uint32_t> copies{0};
        std::atomic<uint32_t> dropped{0};
    };
    struct Request;
    struct Reply;
    enum class Guest { kNone, kIdle, kRunning };  // the guest's XrSession: none, created (or ended), begun
    enum class Copy { kDone, kNotReady, kNoSlot, kFailed };

    // Frame thread.
    void run(JavaVM *vm, jobject activity);
    void setState(State state);
    bool createCopier();
    void createPanel();
    bool loop();
    void onEvent(const XrEventDataBuffer &event);
    bool reap();
    void destroyDoomed();
    void publish(const XrFrameState &frame);
    bool compose(const XrFrameState &frame, const XrCompositionLayerBaseHeader *const *&layers, uint32_t &count);
    Copy copyImages(PendingFrame &next, CopySlot *&used);
    void translate(const PendingFrame &next);
    void freeSwapchain(GuestSwapchain &swapchain);
    void tearDown(JNIEnv *env);

    // Socket thread.
    bool bindSocket(const std::string &name);
    void wake();
    void closeFds();
    void serve();
    void acceptGuest(int &guest);
    void refuse(int fd);
    void disconnect(int &guest);
    bool serveRequest(int fd);
    bool receive(int fd, Request &request);
    bool sendReply(int fd, const qb_hdr &request, const Reply &reply);
    void handle(Request &request, Reply &reply);
    void onHello(const qb_hello &hello, Reply &reply);
    void onCreateSession(Reply &reply);
    void onBeginSession(Reply &reply);
    void onEndSession(Reply &reply);
    void onDestroySession(Reply &reply);
    void onRequestExit(Reply &reply);
    void onPollEvent(Reply &reply);
    void onEnumFormats(Reply &reply);
    void onCreateSwapchain(const qb_swapchain_create &create, Reply &reply);
    void onDestroySwapchain(const qb_swapchain_ref &ref, Reply &reply);
    void onAcquireImage(const qb_swapchain_ref &ref, Reply &reply);
    void onWaitFrame(Reply &reply);
    void onBeginFrame(Reply &reply);
    void onEndFrame(const qb_end_frame &frame, int &fd, Reply &reply);
    void onLocateViews(const qb_locate_views &locate, Reply &reply);
    void onLocateSpace(const qb_locate_space &locate, Reply &reply);
    void onSyncActions(Reply &reply);
    void onApplyHaptic(const qb_haptic &haptic, Reply &reply);
    void onSetRefreshRate(const qb_refresh_rate &rate, Reply &reply);
    void onStageBounds(Reply &reply);

    // Either thread; the ones marked so with mutex_ held.
    XrSpace spaceOf(uint32_t space) const;
    bool offered(VkFormat format) const;
    GuestSwapchain *findSwapchain(uint32_t id);                           // mutex_ held; live ones only
    XrResult collect(const qb_end_frame &frame, std::vector<ImageRef> &refs);  // mutex_ held
    PendingFrame takePending();                                           // mutex_ held
    void release(std::vector<ImageRef> &refs, bool copied);               // mutex_ held
    void dropPending();                                                   // mutex_ held
    void endGuestSession();                                               // mutex_ held
    void queueState(XrSessionState state, int64_t time);                  // mutex_ held
    void queueEvent(const qb_event &event);                               // mutex_ held

    Session xr_;  // the frame thread's, apart from what xr_session.h lets a second thread do
    std::string socketName_;
    uint32_t panelWidth_ = 0;
    uint32_t panelHeight_ = 0;
    std::atomic<int> state_{kIdle};  // written under mutex_
    std::thread frameThread_;
    std::thread socketThread_;  // started and joined by the frame thread
    int listenFd_ = -1;
    int wakeFd_ = -1;  // an eventfd that breaks the socket thread's poll when stopping

    // Shared by both threads under mutex_; changed_ announces frames, freed images and state.
    std::mutex mutex_;
    std::condition_variable changed_;
    bool stopping_ = false;
    bool connected_ = false;  // a guest holds the socket
    Guest guest_ = Guest::kNone;
    bool exitRequested_ = false;
    XrSessionState realState_ = XR_SESSION_STATE_UNKNOWN;
    bool realRunning_ = false;
    uint64_t frameSerial_ = 0;   // xrWaitFrame returns so far
    uint64_t handedSerial_ = 0;  // the newest of them a WAIT_FRAME has answered with
    qb_frame_state frame_{};
    qb_input_state input_{};
    std::map<uint32_t, std::unique_ptr<GuestSwapchain>> swapchains_;  // erased by the frame thread only
    uint32_t nextSwapchainId_ = 1;
    PendingFrame pending_;
    std::deque<qb_event> events_;
    bool shownValid_ = false;         // shown_ may be submitted (again)
    std::vector<uint32_t> shownIds_;  // the swapchains shown_ references

    // Frame thread only.
    LayerList shown_;
    CopySlot slots_[kSlots];
    VkCommandPool pool_ = VK_NULL_HANDLE;
    bool gpuWaits_ = true;  // guest fences become semaphore waits, until an import fails
    XrSwapchain panel_ = XR_NULL_HANDLE;
    XrCompositionLayerQuad panelLayer_{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader *panelLayers_[1] = {};
    Vk vk_;
    Stats stats_;

    std::mutex surfaceLock_;  // panelSurface_, which JNI threads read
    jobject panelSurface_ = nullptr;
};

}  // namespace xrbridge

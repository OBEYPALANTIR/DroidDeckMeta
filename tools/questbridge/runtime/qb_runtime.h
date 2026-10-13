/*
 * qb_runtime.h - shared state of libopenxr_questbridge.so: one struct per OpenXR handle (each starts
 * with a magic word every entry point checks), the per-instance lock, the event queue, and the pose
 * math the guest does itself (a guest XrSpace is a service space plus a pose offset, see
 * questbridge_protocol.h).
 *
 * Entry points are named qb_xr*, hidden, and reached only through xrGetInstanceProcAddr: exported xr*
 * names would be interposed by the loader's own symbols of the same name.
 */
#ifndef QB_RUNTIME_H
#define QB_RUNTIME_H

#define XR_NO_PROTOTYPES 1
#define XR_USE_TIMESPEC 1
#include <pthread.h>
#include <stdint.h>
#include <time.h>

#include "qb_vulkan.h"
#include "qb_ipc.h"
#include "qb_math.h"
#include <openxr/openxr_loader_negotiation.h>

#pragma GCC visibility push(hidden)

#define QB_SYSTEM_ID ((XrSystemId)1)
#define QB_EVENT_QUEUE 32u
#define QB_LOCAL_IMAGES 3u       /* guest-only swapchains (depth) */
#define QB_DRIVEN_PROFILES 2     /* bindings resolve against oculus/touch_controller, then khr/simple_controller */

#define QB_MAGIC_INSTANCE 0x514269696e737431ull
#define QB_MAGIC_SESSION 0x5142697365737331ull
#define QB_MAGIC_SPACE 0x5142697370616331ull
#define QB_MAGIC_SWAPCHAIN 0x5142697377617031ull
#define QB_MAGIC_ACTION_SET 0x5142696173657431ull
#define QB_MAGIC_ACTION 0x5142696163746e31ull

enum qb_ext { /* enabled instance extensions; QB_API_1_1 gates the one command OpenXR 1.1 added */
    QB_EXT_VULKAN = 1u << 0,
    QB_EXT_VULKAN2 = 1u << 1,
    QB_EXT_TIMESPEC = 1u << 2,
    QB_EXT_REFRESH_RATE = 1u << 3,
    QB_API_1_1 = 1u << 4,
};

/* Top-level user paths; the first two index qb_input_state.hands. */
enum qb_user { QB_USER_LEFT, QB_USER_RIGHT, QB_USER_HEAD, QB_USER_GAMEPAD, QB_USER_TREADMILL, QB_USER_COUNT };

/* Every event this runtime queues. */
union qb_xr_event {
    XrEventDataBaseHeader base;
    XrEventDataEventsLost lost;
    XrEventDataInstanceLossPending instance_loss;
    XrEventDataSessionStateChanged state;
    XrEventDataReferenceSpaceChangePending space_change;
    XrEventDataInteractionProfileChanged profile;
    XrEventDataDisplayRefreshRateChangedFB refresh;
};

struct qb_action;
struct qb_component; /* qb_input.c's interaction profile tables */

struct qb_suggestion { /* an XrActionSuggestedBinding for one of the driven profiles */
    struct qb_action *action;
    XrPath binding;
};

struct qb_instance {
    uint64_t magic;
    pthread_mutex_t lock; /* guards everything below and the session; dropped only around blocking requests */
    struct qb_ipc ipc;
    struct qb_hello_reply sys; /* the HELLO answer describes the one system */
    uint32_t exts;             /* enum qb_ext */
    int gfx_requirements;      /* an xrGetVulkanGraphicsRequirements*KHR call happened */
    float refresh_rate;
    struct qb_session *session;
    char **paths; /* XrPath n is paths[n - 1]; never shrinks */
    uint32_t path_count, path_cap;
    union qb_xr_event events[QB_EVENT_QUEUE];
    uint32_t event_head, event_count, events_lost;
    int loss_queued;
    struct qb_action_set *action_sets; /* destroyed ones too, until xrDestroyInstance */
    struct qb_suggestion *suggestions[QB_DRIVEN_PROFILES];
    uint32_t suggestion_count[QB_DRIVEN_PROFILES];
    XrPath profile_path[QB_DRIVEN_PROFILES];
};

struct qb_session {
    uint64_t magic;
    struct qb_instance *inst;
    struct qb_vk *vk;
    XrSessionState state; /* as last queued to the app */
    int running;          /* between xrBeginSession and xrEndSession */
    int frame_waiting, frame_waited, frame_begun;
    pthread_cond_t frame_cond; /* frame pacing, on inst->lock */
    int formats_cached;
    uint32_t format_count;
    int64_t formats[QB_MAX_FORMATS]; /* ENUM_FORMATS */
    struct qb_space_handle *spaces;
    struct qb_swapchain *swapchains;
    struct qb_action_set **attached_sets;
    uint32_t attached_count;
    int attached;
    int profile; /* driven profile the bindings resolved against, -1 = none suggested */
    struct qb_input_state input;
    XrPath current_profile[2];
};

struct qb_space_handle { /* an XrSpace; the protocol's enum qb_space owns the shorter tag */
    uint64_t magic;
    struct qb_session *sess;
    struct qb_space_handle *next;
    uint32_t ref;             /* reference space: enum qb_space */
    struct qb_action *action; /* action space: located through its bound grip/aim source */
    int user;                 /* action space subaction path, -1 = XR_NULL_PATH */
    XrPosef offset;
};

struct qb_swapchain {
    uint64_t magic;
    struct qb_session *sess;
    struct qb_swapchain *next;
    struct qb_vk_swapchain *vk;
    uint32_t id; /* service swapchain_id; 0 = guest-only, never composited */
    uint32_t image_count;
    uint32_t width, height, array_size;
    XrSwapchainCreateFlags flags;
    uint32_t acquired[QB_MAX_SWAPCHAIN_IMAGES]; /* ring of acquired indices, oldest first */
    uint32_t acquired_head, acquired_count, acquiring;
    int waited; /* the oldest acquired image has been waited */
    int ever_acquired;
    uint32_t next_local;
    int64_t released; /* last released index (END_FRAME's image_index), -1 = none */
};

struct qb_binding { /* an input source an action resolved to at xrAttachSessionActionSets */
    const struct qb_component *c;
    XrPath path; /* full source path, for xrEnumerateBoundSourcesForAction */
    uint8_t hand;
    uint8_t suppressed; /* a higher-priority active set uses the same source */
};

struct qb_action_value { /* what xrGetActionState* reports, as of the last xrSyncActions */
    int active;
    float v[2];
    XrBool32 changed;
    XrTime changed_at;
};

struct qb_action {
    uint64_t magic;
    struct qb_action_set *set;
    struct qb_action *next;
    XrActionType type;
    uint32_t users; /* declared subaction paths, bit per enum qb_user */
    int destroyed;
    char name[XR_MAX_ACTION_NAME_SIZE];
    char localized[XR_MAX_LOCALIZED_ACTION_NAME_SIZE];
    struct qb_binding *bindings;
    uint32_t binding_count;
    struct qb_action_value value[3]; /* left, right, either */
};

struct qb_action_set {
    uint64_t magic;
    struct qb_instance *inst;
    struct qb_action_set *next;
    struct qb_action *actions;
    uint32_t priority;
    uint32_t sync_users; /* enum qb_user bits active in the last xrSyncActions */
    int destroyed;
    int locked; /* attached to a session once: immutable */
    char name[XR_MAX_ACTION_SET_NAME_SIZE];
    char localized[XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE];
};

#define QB_HANDLE_GET(type, xr_type, magic_value)                       \
    static inline struct type *type##_get(xr_type handle) {             \
        struct type *p = (struct type *)handle;                         \
        return p && p->magic == (magic_value) ? p : NULL;               \
    }
QB_HANDLE_GET(qb_instance, XrInstance, QB_MAGIC_INSTANCE)
QB_HANDLE_GET(qb_session, XrSession, QB_MAGIC_SESSION)
QB_HANDLE_GET(qb_space_handle, XrSpace, QB_MAGIC_SPACE)
QB_HANDLE_GET(qb_swapchain, XrSwapchain, QB_MAGIC_SWAPCHAIN)
QB_HANDLE_GET(qb_action_set, XrActionSet, QB_MAGIC_ACTION_SET)
QB_HANDLE_GET(qb_action, XrAction, QB_MAGIC_ACTION)

static inline pthread_mutex_t *qb_lock_(pthread_mutex_t *m) {
    pthread_mutex_lock(m);
    return m;
}
static inline void qb_unlock_(pthread_mutex_t **m) {
    pthread_mutex_unlock(*m);
}
/* Holds the instance lock to the end of the enclosing block, so every return path releases it. */
#define QB_LOCK(inst) \
    pthread_mutex_t *qb_held_ __attribute__((cleanup(qb_unlock_), unused)) = qb_lock_(&(inst)->lock)

/* The two-call idiom: the count always, XR_ERROR_SIZE_INSUFFICIENT when a non-zero capacity is too
 * small. Callers fill the array only on XR_SUCCESS with cap != 0. */
static inline XrResult qb_count(uint32_t cap, uint32_t *count, uint32_t n) {
    *count = n;
    return cap && cap < n ? XR_ERROR_SIZE_INSUFFICIENT : XR_SUCCESS;
}

/* Session calls succeed with XR_SESSION_LOSS_PENDING once the service has said so. */
static inline XrResult qb_session_result(const struct qb_session *s, XrResult r) {
    return r == XR_SUCCESS && s->state == XR_SESSION_STATE_LOSS_PENDING ? XR_SESSION_LOSS_PENDING : r;
}

static inline uint32_t qb_max_layers(const struct qb_instance *inst) {
    return inst->sys.max_layer_count < QB_MAX_LAYERS ? inst->sys.max_layer_count : QB_MAX_LAYERS;
}

/* XrTime is CLOCK_MONOTONIC in ns on both sides of the socket (one kernel). */
static inline XrTime qb_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (XrTime)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- qb_runtime.c */
XrResult qb_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function);
XrResult qb_xrEnumerateApiLayerProperties(uint32_t propertyCapacityInput, uint32_t *propertyCountOutput,
                                          XrApiLayerProperties *properties);
XrResult qb_xrEnumerateInstanceExtensionProperties(const char *layerName, uint32_t propertyCapacityInput,
                                                   uint32_t *propertyCountOutput, XrExtensionProperties *properties);
XrResult qb_xrCreateInstance(const XrInstanceCreateInfo *createInfo, XrInstance *instance);
XrResult qb_xrDestroyInstance(XrInstance instance);
XrResult qb_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *instanceProperties);
XrResult qb_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData);
XrResult qb_xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]);
XrResult qb_xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]);
XrResult qb_xrGetSystem(XrInstance instance, const XrSystemGetInfo *getInfo, XrSystemId *systemId);
XrResult qb_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties);
XrResult qb_xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId,
                                             XrViewConfigurationType viewConfigurationType,
                                             uint32_t environmentBlendModeCapacityInput,
                                             uint32_t *environmentBlendModeCountOutput,
                                             XrEnvironmentBlendMode *environmentBlendModes);
XrResult qb_xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId,
                                          uint32_t viewConfigurationTypeCapacityInput,
                                          uint32_t *viewConfigurationTypeCountOutput,
                                          XrViewConfigurationType *viewConfigurationTypes);
XrResult qb_xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId,
                                             XrViewConfigurationType viewConfigurationType,
                                             XrViewConfigurationProperties *configurationProperties);
XrResult qb_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId,
                                              XrViewConfigurationType viewConfigurationType, uint32_t viewCapacityInput,
                                              uint32_t *viewCountOutput, XrViewConfigurationView *views);
XrResult qb_xrStringToPath(XrInstance instance, const char *pathString, XrPath *path);
XrResult qb_xrPathToString(XrInstance instance, XrPath path, uint32_t bufferCapacityInput, uint32_t *bufferCountOutput,
                           char *buffer);
XrResult qb_xrConvertTimespecTimeToTimeKHR(XrInstance instance, const struct timespec *timespecTime, XrTime *time);
XrResult qb_xrConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime time, struct timespec *timespecTime);
XrResult qb_xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput,
                                             uint32_t *bufferCountOutput, char *buffer);
XrResult qb_xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t bufferCapacityInput,
                                           uint32_t *bufferCountOutput, char *buffer);
XrResult qb_xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId, VkInstance vkInstance,
                                         VkPhysicalDevice *vkPhysicalDevice);
XrResult qb_xrGetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                               XrGraphicsRequirementsVulkanKHR *graphicsRequirements);
XrResult qb_xrCreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR *createInfo,
                                      VkInstance *vulkanInstance, VkResult *vulkanResult);
XrResult qb_xrCreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR *createInfo,
                                    VkDevice *vulkanDevice, VkResult *vulkanResult);
XrResult qb_xrGetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR *getInfo,
                                          VkPhysicalDevice *vulkanPhysicalDevice);
XrResult qb_xrGetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId,
                                                XrGraphicsRequirementsVulkanKHR *graphicsRequirements);
XrResult qb_xrEnumerateDisplayRefreshRatesFB(XrSession session, uint32_t displayRefreshRateCapacityInput,
                                             uint32_t *displayRefreshRateCountOutput, float *displayRefreshRates);
XrResult qb_xrGetDisplayRefreshRateFB(XrSession session, float *displayRefreshRate);
XrResult qb_xrRequestDisplayRefreshRateFB(XrSession session, float displayRefreshRate);

/* Lock held for all of these. 0 = XR_NULL_PATH on allocation failure; `str` must be well formed. */
XrPath qb_path_intern(struct qb_instance *inst, const char *str);
/* NULL for anything that is not a live XrPath. */
const char *qb_path_string(const struct qb_instance *inst, XrPath path);
/* One well-formed path element: [a-z0-9-_.], not only periods. */
int qb_name_valid(const char *name);
void qb_event_push(struct qb_instance *inst, const union qb_xr_event *event);
void qb_event_drop_session(struct qb_instance *inst, XrSession session);

/* ---- qb_session.c */
XrResult qb_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *createInfo, XrSession *session);
XrResult qb_xrDestroySession(XrSession session);
XrResult qb_xrBeginSession(XrSession session, const XrSessionBeginInfo *beginInfo);
XrResult qb_xrEndSession(XrSession session);
XrResult qb_xrRequestExitSession(XrSession session);
XrResult qb_xrEnumerateReferenceSpaces(XrSession session, uint32_t spaceCapacityInput, uint32_t *spaceCountOutput,
                                       XrReferenceSpaceType *spaces);
XrResult qb_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *createInfo, XrSpace *space);
XrResult qb_xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType referenceSpaceType,
                                          XrExtent2Df *bounds);
XrResult qb_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *createInfo, XrSpace *space);
XrResult qb_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation *location);
XrResult qb_xrDestroySpace(XrSpace space);
XrResult qb_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo *locateInfo, XrSpaceLocations *spaceLocations);
XrResult qb_xrLocateViews(XrSession session, const XrViewLocateInfo *viewLocateInfo, XrViewState *viewState,
                          uint32_t viewCapacityInput, uint32_t *viewCountOutput, XrView *views);
XrResult qb_xrWaitFrame(XrSession session, const XrFrameWaitInfo *frameWaitInfo, XrFrameState *frameState);
XrResult qb_xrBeginFrame(XrSession session, const XrFrameBeginInfo *frameBeginInfo);
XrResult qb_xrEndFrame(XrSession session, const XrFrameEndInfo *frameEndInfo);

/* Lock held: queue XrEventDataSessionStateChanged unless `state` is already current. */
void qb_session_set_state(struct qb_session *s, XrSessionState state, XrTime time);
/* Lock held: everything the session owns, then the session (xrDestroySession, xrDestroyInstance). */
void qb_session_destroy(struct qb_session *s);

/* ---- qb_swapchain.c */
XrResult qb_xrEnumerateSwapchainFormats(XrSession session, uint32_t formatCapacityInput, uint32_t *formatCountOutput,
                                        int64_t *formats);
XrResult qb_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *createInfo, XrSwapchain *swapchain);
XrResult qb_xrDestroySwapchain(XrSwapchain swapchain);
XrResult qb_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t imageCapacityInput, uint32_t *imageCountOutput,
                                       XrSwapchainImageBaseHeader *images);
XrResult qb_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *acquireInfo,
                                    uint32_t *index);
XrResult qb_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *waitInfo);
XrResult qb_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *releaseInfo);

/* Lock held. */
void qb_swapchain_destroy(struct qb_swapchain *sc);

/* ---- qb_input.c */
XrResult qb_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *createInfo, XrActionSet *actionSet);
XrResult qb_xrDestroyActionSet(XrActionSet actionSet);
XrResult qb_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo *createInfo, XrAction *action);
XrResult qb_xrDestroyAction(XrAction action);
XrResult qb_xrSuggestInteractionProfileBindings(XrInstance instance,
                                                const XrInteractionProfileSuggestedBinding *suggestedBindings);
XrResult qb_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *attachInfo);
XrResult qb_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath,
                                           XrInteractionProfileState *interactionProfile);
XrResult qb_xrSyncActions(XrSession session, const XrActionsSyncInfo *syncInfo);
XrResult qb_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateBoolean *state);
XrResult qb_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateFloat *state);
XrResult qb_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *getInfo,
                                     XrActionStateVector2f *state);
XrResult qb_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStatePose *state);
XrResult qb_xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo *enumerateInfo,
                                             uint32_t sourceCapacityInput, uint32_t *sourceCountOutput, XrPath *sources);
XrResult qb_xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo *getInfo,
                                          uint32_t bufferCapacityInput, uint32_t *bufferCountOutput, char *buffer);
XrResult qb_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo,
                                  const XrHapticBaseHeader *hapticFeedback);
XrResult qb_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo);

/* Lock held for all of these. The subaction path as enum qb_user (-1 for XR_NULL_PATH), checked
 * against the action's declared ones. */
XrResult qb_input_subaction(const struct qb_action *a, XrPath path, int *user);
/* The service space a pose action is located through for `user` (-1 = its first bound hand); 0 while
 * the action is inactive or not bound to a grip/aim pose. */
int qb_input_pose_space(const struct qb_session *s, const struct qb_action *a, int user, uint32_t *space);
/* xrDestroySession: forget the attachment and every binding resolved for it. */
void qb_input_detach(struct qb_session *s);
/* xrDestroyInstance: every action set, action and suggestion. */
void qb_input_destroy_all(struct qb_instance *inst);

#pragma GCC visibility pop

#endif

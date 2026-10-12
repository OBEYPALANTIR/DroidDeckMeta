/* qb_runtime.c - loader negotiation, xrGetInstanceProcAddr, the instance and system queries, paths,
 * events, timespec conversion, and the XR_KHR_vulkan_enable(2) and XR_FB_display_refresh_rate entry
 * points of libopenxr_questbridge.so. */
#define _GNU_SOURCE
#include "qb_runtime.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openxr/openxr_reflection.h>

#define QB_COUNT(a) (sizeof(a) / sizeof((a)[0]))
/* Where the app listens when $DROIDDECK_XR_SOCKET is unset. */
#define QB_DEFAULT_SOCKET "@com.droiddeck.launcher.xr-bridge"

static const struct {
    const char *name;
    uint32_t version, bit;
} extensions[] = {
    /* the revisions implemented; the 1.1.63 headers are at 10 and 4 */
    {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME, 8, QB_EXT_VULKAN},
    {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME, 2, QB_EXT_VULKAN2},
    {XR_KHR_CONVERT_TIMESPEC_TIME_EXTENSION_NAME, XR_KHR_convert_timespec_time_SPEC_VERSION, QB_EXT_TIMESPEC},
    {XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME, XR_FB_display_refresh_rate_SPEC_VERSION, QB_EXT_REFRESH_RATE},
};

#define FN(name, ext) {#name, (PFN_xrVoidFunction)qb_##name, ext}
static const struct {
    const char *name;
    PFN_xrVoidFunction fn;
    uint32_t ext; /* enum qb_ext that must be enabled, 0 = core */
} functions[] = {
    FN(xrGetInstanceProcAddr, 0),
    FN(xrEnumerateApiLayerProperties, 0),
    FN(xrEnumerateInstanceExtensionProperties, 0),
    FN(xrCreateInstance, 0),
    FN(xrDestroyInstance, 0),
    FN(xrGetInstanceProperties, 0),
    FN(xrPollEvent, 0),
    FN(xrResultToString, 0),
    FN(xrStructureTypeToString, 0),
    FN(xrGetSystem, 0),
    FN(xrGetSystemProperties, 0),
    FN(xrEnumerateEnvironmentBlendModes, 0),
    FN(xrEnumerateViewConfigurations, 0),
    FN(xrGetViewConfigurationProperties, 0),
    FN(xrEnumerateViewConfigurationViews, 0),
    FN(xrStringToPath, 0),
    FN(xrPathToString, 0),
    FN(xrCreateSession, 0),
    FN(xrDestroySession, 0),
    FN(xrBeginSession, 0),
    FN(xrEndSession, 0),
    FN(xrRequestExitSession, 0),
    FN(xrEnumerateReferenceSpaces, 0),
    FN(xrCreateReferenceSpace, 0),
    FN(xrGetReferenceSpaceBoundsRect, 0),
    FN(xrCreateActionSpace, 0),
    FN(xrLocateSpace, 0),
    FN(xrDestroySpace, 0),
    FN(xrLocateSpaces, QB_API_1_1),
    FN(xrLocateViews, 0),
    FN(xrWaitFrame, 0),
    FN(xrBeginFrame, 0),
    FN(xrEndFrame, 0),
    FN(xrEnumerateSwapchainFormats, 0),
    FN(xrCreateSwapchain, 0),
    FN(xrDestroySwapchain, 0),
    FN(xrEnumerateSwapchainImages, 0),
    FN(xrAcquireSwapchainImage, 0),
    FN(xrWaitSwapchainImage, 0),
    FN(xrReleaseSwapchainImage, 0),
    FN(xrCreateActionSet, 0),
    FN(xrDestroyActionSet, 0),
    FN(xrCreateAction, 0),
    FN(xrDestroyAction, 0),
    FN(xrSuggestInteractionProfileBindings, 0),
    FN(xrAttachSessionActionSets, 0),
    FN(xrGetCurrentInteractionProfile, 0),
    FN(xrSyncActions, 0),
    FN(xrGetActionStateBoolean, 0),
    FN(xrGetActionStateFloat, 0),
    FN(xrGetActionStateVector2f, 0),
    FN(xrGetActionStatePose, 0),
    FN(xrEnumerateBoundSourcesForAction, 0),
    FN(xrGetInputSourceLocalizedName, 0),
    FN(xrApplyHapticFeedback, 0),
    FN(xrStopHapticFeedback, 0),
    FN(xrConvertTimespecTimeToTimeKHR, QB_EXT_TIMESPEC),
    FN(xrConvertTimeToTimespecTimeKHR, QB_EXT_TIMESPEC),
    FN(xrGetVulkanInstanceExtensionsKHR, QB_EXT_VULKAN),
    FN(xrGetVulkanDeviceExtensionsKHR, QB_EXT_VULKAN),
    FN(xrGetVulkanGraphicsDeviceKHR, QB_EXT_VULKAN),
    FN(xrGetVulkanGraphicsRequirementsKHR, QB_EXT_VULKAN),
    FN(xrCreateVulkanInstanceKHR, QB_EXT_VULKAN2),
    FN(xrCreateVulkanDeviceKHR, QB_EXT_VULKAN2),
    FN(xrGetVulkanGraphicsDevice2KHR, QB_EXT_VULKAN2),
    FN(xrGetVulkanGraphicsRequirements2KHR, QB_EXT_VULKAN2),
    FN(xrEnumerateDisplayRefreshRatesFB, QB_EXT_REFRESH_RATE),
    FN(xrGetDisplayRefreshRateFB, QB_EXT_REFRESH_RATE),
    FN(xrRequestDisplayRefreshRateFB, QB_EXT_REFRESH_RATE),
};

/* The one exported symbol (questbridge.json names no other). */
__attribute__((visibility("default"))) XRAPI_ATTR XrResult XRAPI_CALL
xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *info, XrNegotiateRuntimeRequest *req) {
    if (!info || info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        info->structVersion != XR_LOADER_INFO_STRUCT_VERSION || info->structSize != sizeof(*info) || !req ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        req->structVersion != XR_RUNTIME_INFO_STRUCT_VERSION || req->structSize != sizeof(*req) ||
        info->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        info->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION || info->minApiVersion > XR_CURRENT_API_VERSION ||
        info->maxApiVersion < XR_CURRENT_API_VERSION)
        return XR_ERROR_INITIALIZATION_FAILED;
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_CURRENT_API_VERSION;
    req->getInstanceProcAddr = qb_xrGetInstanceProcAddr;
    return XR_SUCCESS;
}

XrResult qb_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    struct qb_instance *inst = NULL;
    if (!function) return XR_ERROR_VALIDATION_FAILURE;
    *function = NULL;
    if (!name) return XR_ERROR_VALIDATION_FAILURE;
    if (instance == XR_NULL_HANDLE) {
        if (strcmp(name, "xrEnumerateInstanceExtensionProperties") && strcmp(name, "xrEnumerateApiLayerProperties") &&
            strcmp(name, "xrCreateInstance"))
            return strcmp(name, "xrInitializeLoaderKHR") ? XR_ERROR_HANDLE_INVALID : XR_ERROR_FUNCTION_UNSUPPORTED;
    } else if (!(inst = qb_instance_get(instance))) {
        return XR_ERROR_HANDLE_INVALID;
    }
    for (size_t i = 0; i < QB_COUNT(functions); i++) {
        if (strcmp(functions[i].name, name)) continue;
        if (functions[i].ext && !(inst && (inst->exts & functions[i].ext))) return XR_ERROR_FUNCTION_UNSUPPORTED;
        *function = functions[i].fn;
        return XR_SUCCESS;
    }
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult qb_xrEnumerateApiLayerProperties(uint32_t cap, uint32_t *count, XrApiLayerProperties *properties) {
    if (!count) return XR_ERROR_VALIDATION_FAILURE;
    *count = 0;
    return XR_SUCCESS;
}

XrResult qb_xrEnumerateInstanceExtensionProperties(const char *layerName, uint32_t cap, uint32_t *count,
                                                   XrExtensionProperties *properties) {
    if (layerName) return XR_ERROR_API_LAYER_NOT_PRESENT;
    if (!count || (cap && !properties)) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = qb_count(cap, count, QB_COUNT(extensions));
    if (r != XR_SUCCESS || !cap) return r;
    for (size_t i = 0; i < QB_COUNT(extensions); i++) {
        if (properties[i].type != XR_TYPE_EXTENSION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
        snprintf(properties[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", extensions[i].name);
        properties[i].extensionVersion = extensions[i].version;
    }
    return XR_SUCCESS;
}

/* $DROIDDECK_XR_SOCKET, else the app's abstract socket, else $XDG_RUNTIME_DIR/xr-bridge.sock. */
static XrResult connect_service(struct qb_ipc *ipc) {
    const char *env = getenv(QB_SOCKET_ENV), *dir = getenv("XDG_RUNTIME_DIR");
    char path[PATH_MAX];
    if (env && *env) return qb_ipc_connect(ipc, env);
    XrResult r = qb_ipc_connect(ipc, QB_DEFAULT_SOCKET);
    if (r != XR_ERROR_RUNTIME_UNAVAILABLE || !dir || !*dir) return r;
    snprintf(path, sizeof(path), "%s/%s", dir, QB_SOCKET_NAME);
    return qb_ipc_connect(ipc, path);
}

XrResult qb_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) {
    if (!info || info->type != XR_TYPE_INSTANCE_CREATE_INFO || !instance ||
        (info->enabledExtensionCount && !info->enabledExtensionNames))
        return XR_ERROR_VALIDATION_FAILURE;
    const XrApplicationInfo *app = &info->applicationInfo;
    size_t name_len = strnlen(app->applicationName, XR_MAX_APPLICATION_NAME_SIZE);
    if (name_len == XR_MAX_APPLICATION_NAME_SIZE ||
        strnlen(app->engineName, XR_MAX_ENGINE_NAME_SIZE) == XR_MAX_ENGINE_NAME_SIZE)
        return XR_ERROR_VALIDATION_FAILURE;
    if (!name_len) return XR_ERROR_NAME_INVALID;
    /* an OpenXR 1.0 runtime that also lets 1.1 applications in */
    if (XR_VERSION_MAJOR(app->apiVersion) != 1 || XR_VERSION_MINOR(app->apiVersion) > 1)
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    uint32_t exts = XR_VERSION_MINOR(app->apiVersion) >= 1 ? QB_API_1_1 : 0;
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        const char *want = info->enabledExtensionNames[i];
        uint32_t bit = 0;
        for (size_t e = 0; want && e < QB_COUNT(extensions); e++)
            if (!strcmp(want, extensions[e].name)) bit = extensions[e].bit;
        if (!bit) return XR_ERROR_EXTENSION_NOT_PRESENT;
        exts |= bit;
    }

    struct qb_instance *inst = calloc(1, sizeof(*inst));
    if (!inst) return XR_ERROR_OUT_OF_MEMORY;
    XrResult r = connect_service(&inst->ipc);
    if (XR_FAILED(r)) {
        free(inst);
        return r;
    }
    struct qb_hello hello = {.protocol_version = QB_PROTOCOL_VERSION, .guest_pid = (uint32_t)getpid()};
    memcpy(hello.app_name, app->applicationName, name_len);
    r = qb_ipc_call(&inst->ipc, QB_OP_HELLO, &hello, sizeof(hello), NULL, 0, &inst->sys, sizeof(inst->sys), NULL, 0,
                    NULL);
    if (XR_SUCCEEDED(r) && (inst->sys.protocol_version != QB_PROTOCOL_VERSION || inst->sys.view_count != QB_MAX_VIEWS))
        r = XR_ERROR_RUNTIME_FAILURE;
    /* the service reports rate 0 when Meta's runtime lacks XR_FB_display_refresh_rate */
    if (XR_SUCCEEDED(r) && (exts & QB_EXT_REFRESH_RATE) && !(inst->sys.current_refresh_rate > 0.0f))
        r = XR_ERROR_EXTENSION_NOT_PRESENT;
    if (XR_FAILED(r)) {
        qb_ipc_close(&inst->ipc);
        free(inst);
        return r;
    }
    inst->sys.system_name[QB_NAME_LEN - 1] = inst->sys.runtime_name[QB_NAME_LEN - 1] = '\0';
    if (inst->sys.refresh_rate_count > QB_MAX_REFRESH_RATES) inst->sys.refresh_rate_count = QB_MAX_REFRESH_RATES;
    inst->refresh_rate = inst->sys.current_refresh_rate;
    inst->exts = exts;
    pthread_mutex_init(&inst->lock, NULL);
    inst->magic = QB_MAGIC_INSTANCE;
    *instance = (XrInstance)inst;
    return XR_SUCCESS;
}

XrResult qb_xrDestroyInstance(XrInstance instance) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    pthread_mutex_lock(&inst->lock);
    if (inst->session) qb_session_destroy(inst->session);
    qb_input_destroy_all(inst);
    for (uint32_t i = 0; i < inst->path_count; i++) free(inst->paths[i]);
    free(inst->paths);
    qb_ipc_close(&inst->ipc);
    inst->magic = 0;
    pthread_mutex_unlock(&inst->lock);
    pthread_mutex_destroy(&inst->lock);
    free(inst);
    return XR_SUCCESS;
}

XrResult qb_xrGetInstanceProperties(XrInstance instance, XrInstanceProperties *properties) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!properties || properties->type != XR_TYPE_INSTANCE_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    properties->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    snprintf(properties->runtimeName, XR_MAX_RUNTIME_NAME_SIZE, "%s", "DroidDeck Quest bridge");
    return XR_SUCCESS;
}

#define QB_ENUM_CASE(name, value) \
    case name:                    \
        return #name;

static const char *result_name(XrResult value) {
    switch (value) {
        XR_LIST_ENUM_XrResult(QB_ENUM_CASE)
    default:
        return NULL;
    }
}

static const char *structure_name(XrStructureType value) {
    switch (value) {
        XR_LIST_ENUM_XrStructureType(QB_ENUM_CASE)
    default:
        return NULL;
    }
}

XrResult qb_xrResultToString(XrInstance instance, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char *name = result_name(value);
    if (name)
        snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s", name);
    else
        snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "XR_UNKNOWN_%s_%d", value < 0 ? "FAILURE" : "SUCCESS", (int)value);
    return XR_SUCCESS;
}

XrResult qb_xrStructureTypeToString(XrInstance instance, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!buffer) return XR_ERROR_VALIDATION_FAILURE;
    const char *name = structure_name(value);
    if (name)
        snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "%s", name);
    else
        snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
    return XR_SUCCESS;
}

/* The checks every system query starts with. */
static XrResult check_system(XrInstance instance, XrSystemId systemId) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    return systemId == QB_SYSTEM_ID ? XR_SUCCESS : XR_ERROR_SYSTEM_INVALID;
}

XrResult qb_xrGetSystem(XrInstance instance, const XrSystemGetInfo *info, XrSystemId *systemId) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SYSTEM_GET_INFO || !systemId) return XR_ERROR_VALIDATION_FAILURE;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *systemId = QB_SYSTEM_ID;
    return XR_SUCCESS;
}

XrResult qb_xrGetSystemProperties(XrInstance instance, XrSystemId systemId, XrSystemProperties *properties) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!properties || properties->type != XR_TYPE_SYSTEM_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    const struct qb_instance *inst = qb_instance_get(instance);
    properties->systemId = systemId;
    properties->vendorId = inst->sys.vendor_id;
    snprintf(properties->systemName, XR_MAX_SYSTEM_NAME_SIZE, "%s", inst->sys.system_name);
    properties->graphicsProperties.maxSwapchainImageWidth = inst->sys.max_swapchain_width;
    properties->graphicsProperties.maxSwapchainImageHeight = inst->sys.max_swapchain_height;
    properties->graphicsProperties.maxLayerCount = qb_max_layers(inst);
    properties->trackingProperties.orientationTracking = inst->sys.orientation_tracking ? XR_TRUE : XR_FALSE;
    properties->trackingProperties.positionTracking = inst->sys.position_tracking ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XrResult qb_xrEnumerateEnvironmentBlendModes(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type,
                                             uint32_t cap, uint32_t *count, XrEnvironmentBlendMode *modes) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!count || (cap && !modes)) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    r = qb_count(cap, count, 1);
    if (r == XR_SUCCESS && cap) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return r;
}

XrResult qb_xrEnumerateViewConfigurations(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t *count,
                                          XrViewConfigurationType *types) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!count || (cap && !types)) return XR_ERROR_VALIDATION_FAILURE;
    r = qb_count(cap, count, 1);
    if (r == XR_SUCCESS && cap) types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return r;
}

XrResult qb_xrGetViewConfigurationProperties(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type,
                                             XrViewConfigurationProperties *properties) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!properties || properties->type != XR_TYPE_VIEW_CONFIGURATION_PROPERTIES) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    properties->viewConfigurationType = type;
    properties->fovMutable = XR_TRUE; /* layer fovs go to Meta's compositor as submitted */
    return XR_SUCCESS;
}

XrResult qb_xrEnumerateViewConfigurationViews(XrInstance instance, XrSystemId systemId, XrViewConfigurationType type,
                                              uint32_t cap, uint32_t *count, XrViewConfigurationView *views) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!count || (cap && !views)) return XR_ERROR_VALIDATION_FAILURE;
    if (type != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    r = qb_count(cap, count, QB_MAX_VIEWS);
    if (r != XR_SUCCESS || !cap) return r;
    const struct qb_instance *inst = qb_instance_get(instance);
    for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
        const struct qb_view_config *v = &inst->sys.views[i];
        if (views[i].type != XR_TYPE_VIEW_CONFIGURATION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
        views[i].recommendedImageRectWidth = v->recommended_width;
        views[i].maxImageRectWidth = v->max_width;
        views[i].recommendedImageRectHeight = v->recommended_height;
        views[i].maxImageRectHeight = v->max_height;
        views[i].recommendedSwapchainSampleCount = v->recommended_samples;
        views[i].maxSwapchainSampleCount = v->max_samples;
    }
    return XR_SUCCESS;
}

/* ---- paths */

static int element_valid(const char *s, size_t n) {
    int only_dots = 1;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (c == '.') continue;
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        only_dots = 0;
    }
    return !only_dots;
}

int qb_name_valid(const char *name) {
    return element_valid(name, strlen(name));
}

/* The spec's well-formed path: "/" then elements of [a-z0-9-_.], none empty or only periods. */
static int path_valid(const char *s) {
    size_t n = strnlen(s, XR_MAX_PATH_LENGTH);
    if (n == XR_MAX_PATH_LENGTH || s[0] != '/') return 0;
    for (const char *p = s + 1;;) {
        const char *e = strchr(p, '/');
        if (!element_valid(p, e ? (size_t)(e - p) : strlen(p))) return 0;
        if (!e) return 1;
        p = e + 1;
    }
}

XrPath qb_path_intern(struct qb_instance *inst, const char *str) {
    for (uint32_t i = 0; i < inst->path_count; i++)
        if (!strcmp(inst->paths[i], str)) return i + 1;
    if (inst->path_count == inst->path_cap) {
        uint32_t cap = inst->path_cap ? inst->path_cap * 2 : 64;
        char **paths = realloc(inst->paths, cap * sizeof(*paths));
        if (!paths) return XR_NULL_PATH;
        inst->paths = paths;
        inst->path_cap = cap;
    }
    char *copy = strdup(str);
    if (!copy) return XR_NULL_PATH;
    inst->paths[inst->path_count++] = copy;
    return inst->path_count;
}

const char *qb_path_string(const struct qb_instance *inst, XrPath path) {
    return path != XR_NULL_PATH && path <= inst->path_count ? inst->paths[path - 1] : NULL;
}

XrResult qb_xrStringToPath(XrInstance instance, const char *pathString, XrPath *path) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!pathString || !path) return XR_ERROR_VALIDATION_FAILURE;
    if (!path_valid(pathString)) return XR_ERROR_PATH_FORMAT_INVALID;
    QB_LOCK(inst);
    XrPath p = qb_path_intern(inst, pathString);
    if (p == XR_NULL_PATH) return XR_ERROR_OUT_OF_MEMORY;
    *path = p;
    return XR_SUCCESS;
}

XrResult qb_xrPathToString(XrInstance instance, XrPath path, uint32_t cap, uint32_t *count, char *buffer) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!count || (cap && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(inst);
    const char *str = qb_path_string(inst, path);
    if (!str) return XR_ERROR_PATH_INVALID;
    uint32_t n = (uint32_t)strlen(str) + 1;
    XrResult r = qb_count(cap, count, n);
    if (r == XR_SUCCESS && cap) memcpy(buffer, str, n);
    return r;
}

/* ---- events */

_Static_assert(sizeof(union qb_xr_event) <= sizeof(XrEventDataBuffer), "xrPollEvent copies a whole qb_xr_event");

void qb_event_push(struct qb_instance *inst, const union qb_xr_event *event) {
    if (inst->event_count == QB_EVENT_QUEUE) {
        inst->events_lost++;
        return;
    }
    inst->events[(inst->event_head + inst->event_count++) % QB_EVENT_QUEUE] = *event;
}

static XrSession event_session(const union qb_xr_event *e) {
    switch (e->base.type) {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        return e->state.session;
    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        return e->space_change.session;
    case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        return e->profile.session;
    default:
        return XR_NULL_HANDLE;
    }
}

void qb_event_drop_session(struct qb_instance *inst, XrSession session) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < inst->event_count; i++) {
        const union qb_xr_event *e = &inst->events[(inst->event_head + i) % QB_EVENT_QUEUE];
        if (event_session(e) != session) inst->events[(inst->event_head + kept++) % QB_EVENT_QUEUE] = *e;
    }
    inst->event_count = kept;
}

static void service_event(struct qb_instance *inst, const struct qb_event *e) {
    static const XrReferenceSpaceType ref_types[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                     XR_REFERENCE_SPACE_TYPE_STAGE};
    struct qb_session *s = inst->session;
    union qb_xr_event x;
    memset(&x, 0, sizeof(x));
    switch (e->type) {
    case QB_EVENT_SESSION_STATE:
        if (s && e->a >= XR_SESSION_STATE_IDLE && e->a <= XR_SESSION_STATE_EXITING)
            qb_session_set_state(s, (XrSessionState)e->a, e->time);
        return;
    case QB_EVENT_REFERENCE_SPACE_CHANGE_PENDING:
        if (!s || e->a > QB_SPACE_STAGE) return;
        x.space_change.type = XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING;
        x.space_change.session = (XrSession)s;
        x.space_change.referenceSpaceType = ref_types[e->a];
        x.space_change.changeTime = e->time;
        x.space_change.poseInPreviousSpace = qb_pose_from_wire(&e->pose);
        /* the wire has no poseValid: a non-unit quaternion stands for "unknown" */
        x.space_change.poseValid = qb_pose_valid(&x.space_change.poseInPreviousSpace) ? XR_TRUE : XR_FALSE;
        break;
    case QB_EVENT_INTERACTION_PROFILE_CHANGED:
        if (!s || !s->attached) return;
        x.profile.type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
        x.profile.session = (XrSession)s;
        break;
    case QB_EVENT_REFRESH_RATE_CHANGED: {
        float from = inst->refresh_rate;
        inst->refresh_rate = e->f;
        if (!(inst->exts & QB_EXT_REFRESH_RATE) || from == e->f) return;
        x.refresh.type = XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB;
        x.refresh.fromDisplayRefreshRate = from;
        x.refresh.toDisplayRefreshRate = e->f;
        break;
    }
    case QB_EVENT_INSTANCE_LOSS_PENDING:
        if (inst->loss_queued) return;
        inst->loss_queued = 1;
        x.instance_loss.type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        x.instance_loss.lossTime = e->time;
        break;
    default:
        return;
    }
    qb_event_push(inst, &x);
}

XrResult qb_xrPollEvent(XrInstance instance, XrEventDataBuffer *eventData) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!eventData || eventData->type != XR_TYPE_EVENT_DATA_BUFFER) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(inst);
    /* Drain only while there is room: the service keeps the rest, so only our own events can be lost. */
    while (!inst->ipc.lost && inst->event_count < QB_EVENT_QUEUE) {
        struct qb_event e;
        if (qb_ipc_call(&inst->ipc, QB_OP_POLL_EVENT, NULL, 0, NULL, 0, &e, sizeof(e), NULL, 0, NULL) != XR_SUCCESS ||
            e.type == QB_EVENT_NONE)
            break;
        service_event(inst, &e);
    }
    if (inst->ipc.lost && !inst->loss_queued && inst->event_count < QB_EVENT_QUEUE) {
        union qb_xr_event e;
        memset(&e, 0, sizeof(e));
        e.instance_loss.type = XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING;
        e.instance_loss.lossTime = qb_now();
        inst->loss_queued = 1;
        qb_event_push(inst, &e);
    }
    union qb_xr_event out;
    if (inst->event_count) {
        out = inst->events[inst->event_head];
        inst->event_head = (inst->event_head + 1) % QB_EVENT_QUEUE;
        inst->event_count--;
    } else if (inst->events_lost) {
        memset(&out, 0, sizeof(out));
        out.lost.type = XR_TYPE_EVENT_DATA_EVENTS_LOST;
        out.lost.lostEventCount = inst->events_lost;
        inst->events_lost = 0;
    } else {
        return XR_EVENT_UNAVAILABLE;
    }
    memcpy(eventData, &out, sizeof(out));
    return XR_SUCCESS;
}

/* ---- XR_KHR_convert_timespec_time: XrTime is CLOCK_MONOTONIC ns */

XrResult qb_xrConvertTimespecTimeToTimeKHR(XrInstance instance, const struct timespec *timespecTime, XrTime *time) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!timespecTime || !time) return XR_ERROR_VALIDATION_FAILURE;
    if (timespecTime->tv_sec < 0 || timespecTime->tv_nsec < 0 || timespecTime->tv_nsec >= 1000000000 ||
        timespecTime->tv_sec >= INT64_MAX / 1000000000 || (!timespecTime->tv_sec && !timespecTime->tv_nsec))
        return XR_ERROR_TIME_INVALID;
    *time = (XrTime)timespecTime->tv_sec * 1000000000 + timespecTime->tv_nsec;
    return XR_SUCCESS;
}

XrResult qb_xrConvertTimeToTimespecTimeKHR(XrInstance instance, XrTime time, struct timespec *timespecTime) {
    if (!qb_instance_get(instance)) return XR_ERROR_HANDLE_INVALID;
    if (!timespecTime) return XR_ERROR_VALIDATION_FAILURE;
    if (time <= 0) return XR_ERROR_TIME_INVALID;
    timespecTime->tv_sec = (time_t)(time / 1000000000);
    timespecTime->tv_nsec = (long)(time % 1000000000);
    return XR_SUCCESS;
}

/* ---- XR_KHR_vulkan_enable / XR_KHR_vulkan_enable2: qb_vulkan does the work, under the lock */

XrResult qb_xrGetVulkanInstanceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t *count,
                                             char *buffer) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!count || (cap && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = qb_instance_get(instance);
    QB_LOCK(inst);
    return qb_vk_instance_extensions(cap, count, buffer);
}

XrResult qb_xrGetVulkanDeviceExtensionsKHR(XrInstance instance, XrSystemId systemId, uint32_t cap, uint32_t *count,
                                           char *buffer) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!count || (cap && !buffer)) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = qb_instance_get(instance);
    QB_LOCK(inst);
    return qb_vk_device_extensions(cap, count, buffer);
}

XrResult qb_xrGetVulkanGraphicsDeviceKHR(XrInstance instance, XrSystemId systemId, VkInstance vkInstance,
                                         VkPhysicalDevice *vkPhysicalDevice) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!vkInstance || !vkPhysicalDevice) return XR_ERROR_VALIDATION_FAILURE;
    /* XR_KHR_vulkan_enable apps never hand over their vkGetInstanceProcAddr */
    PFN_vkGetInstanceProcAddr gipa = qb_vk_loader_gipa();
    if (!gipa) return XR_ERROR_RUNTIME_FAILURE;
    struct qb_instance *inst = qb_instance_get(instance);
    QB_LOCK(inst);
    return qb_vk_physical_device(vkInstance, gipa, vkPhysicalDevice);
}

XrResult qb_xrGetVulkanGraphicsRequirementsKHR(XrInstance instance, XrSystemId systemId,
                                               XrGraphicsRequirementsVulkanKHR *graphicsRequirements) {
    XrResult r = check_system(instance, systemId);
    if (r != XR_SUCCESS) return r;
    if (!graphicsRequirements || graphicsRequirements->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)
        return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = qb_instance_get(instance);
    QB_LOCK(inst);
    r = qb_vk_requirements(graphicsRequirements);
    if (XR_SUCCEEDED(r)) inst->gfx_requirements = 1;
    return r;
}

/* XrGraphicsRequirementsVulkan2KHR is the same struct with the same type value. */
XrResult qb_xrGetVulkanGraphicsRequirements2KHR(XrInstance instance, XrSystemId systemId,
                                                XrGraphicsRequirementsVulkanKHR *graphicsRequirements) {
    return qb_xrGetVulkanGraphicsRequirementsKHR(instance, systemId, graphicsRequirements);
}

XrResult qb_xrCreateVulkanInstanceKHR(XrInstance instance, const XrVulkanInstanceCreateInfoKHR *createInfo,
                                      VkInstance *vulkanInstance, VkResult *vulkanResult) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || createInfo->createFlags ||
        !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanCreateInfo || !vulkanInstance || !vulkanResult)
        return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->systemId != QB_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    QB_LOCK(inst);
    return qb_vk_create_instance(createInfo, vulkanInstance, vulkanResult);
}

XrResult qb_xrCreateVulkanDeviceKHR(XrInstance instance, const XrVulkanDeviceCreateInfoKHR *createInfo,
                                    VkDevice *vulkanDevice, VkResult *vulkanResult) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!createInfo || createInfo->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || createInfo->createFlags ||
        !createInfo->pfnGetInstanceProcAddr || !createInfo->vulkanPhysicalDevice || !createInfo->vulkanCreateInfo ||
        !vulkanDevice || !vulkanResult)
        return XR_ERROR_VALIDATION_FAILURE;
    if (createInfo->systemId != QB_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    QB_LOCK(inst);
    return qb_vk_create_device(createInfo, vulkanDevice, vulkanResult);
}

XrResult qb_xrGetVulkanGraphicsDevice2KHR(XrInstance instance, const XrVulkanGraphicsDeviceGetInfoKHR *getInfo,
                                          VkPhysicalDevice *vulkanPhysicalDevice) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!getInfo || getInfo->type != XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR || !getInfo->vulkanInstance ||
        !vulkanPhysicalDevice)
        return XR_ERROR_VALIDATION_FAILURE;
    if (getInfo->systemId != QB_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    QB_LOCK(inst);
    return qb_vk_physical_device2(getInfo, vulkanPhysicalDevice);
}

/* ---- XR_FB_display_refresh_rate: the rates from HELLO, the current one kept by REFRESH_RATE_CHANGED */

XrResult qb_xrEnumerateDisplayRefreshRatesFB(XrSession session, uint32_t cap, uint32_t *count, float *rates) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!count || (cap && !rates)) return XR_ERROR_VALIDATION_FAILURE;
    const struct qb_hello_reply *sys = &s->inst->sys;
    QB_LOCK(s->inst);
    XrResult r = qb_count(cap, count, sys->refresh_rate_count);
    if (r == XR_SUCCESS && cap) memcpy(rates, sys->refresh_rates, sys->refresh_rate_count * sizeof(*rates));
    return qb_session_result(s, r);
}

XrResult qb_xrGetDisplayRefreshRateFB(XrSession session, float *displayRefreshRate) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!displayRefreshRate) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    *displayRefreshRate = s->inst->refresh_rate;
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrRequestDisplayRefreshRateFB(XrSession session, float displayRefreshRate) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    const struct qb_hello_reply *sys = &s->inst->sys;
    int listed = displayRefreshRate == 0.0f; /* 0: no preference, the runtime picks */
    for (uint32_t i = 0; i < sys->refresh_rate_count; i++) listed |= sys->refresh_rates[i] == displayRefreshRate;
    if (!listed) return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    struct qb_refresh_rate req = {.rate = displayRefreshRate};
    struct qb_status st;
    QB_LOCK(s->inst);
    XrResult r = qb_ipc_call(&s->inst->ipc, QB_OP_SET_REFRESH_RATE, &req, sizeof(req), NULL, 0, &st, sizeof(st), NULL,
                             0, NULL);
    return XR_FAILED(r) ? r : qb_session_result(s, XR_SUCCESS);
}

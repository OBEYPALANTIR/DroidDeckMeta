/* qb_vulkan.c - the Vulkan half of libopenxr_questbridge.so (qb_vulkan.h): XR_KHR_vulkan_enable(2), the
 * service's gralloc dma-bufs imported into the app's device, the guest-owned images an app gets where those
 * cannot be handed over directly, the blit/resolve into the imported image on release, and the frame's
 * sync_file. Nothing links libvulkan: every entry point comes through a vkGetInstanceProcAddr. */
#define VK_NO_PROTOTYPES 1
#define XR_NO_PROTOTYPES 1
#define XR_USE_GRAPHICS_API_VULKAN 1
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

/* qb_vk_* are the core's alone; questbridge.json names the library's one exported symbol */
#pragma GCC visibility push(hidden)
#include "qb_vulkan.h"

#define QB_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define QB_ERR(...) fprintf(stderr, "questbridge: " __VA_ARGS__)
/* DRM_FORMAT_MOD_LINEAR; the service's only other modifier is DRM_FORMAT_MOD_QCOM_COMPRESSED (UBWC) */
#define QB_MOD_LINEAR 0ull
/* The usage of an imported image the app renders into directly. */
#define QB_IMPORT_USAGE                                                                                         \
    (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | \
     VK_IMAGE_USAGE_SAMPLED_BIT)

static const char *const instance_exts[] = {
    VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
};

/* All required but the last, which is listed and enabled only where the device offers it. */
static const char *const device_exts[] = {
    VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
    VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
    VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
    VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
    VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
};
#define QB_REQUIRED_DEVICE_EXTS ((uint32_t)QB_COUNT(device_exts) - 1u)
#define QB_FOREIGN_EXT (&device_exts[QB_REQUIRED_DEVICE_EXTS])

#define QB_VK_INSTANCE_FNS(X)                                                                           \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) X(EnumerateDeviceExtensionProperties)     \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) X(GetDeviceProcAddr) \
    X(CreateDevice) X(DestroyInstance)
/* Core since 1.1; the KHR name serves a 1.0 instance that enabled the instance extensions above. */
#define QB_VK_INSTANCE11_FNS(X) X(GetPhysicalDeviceImageFormatProperties2) X(GetPhysicalDeviceExternalFenceProperties)
#define QB_VK_DEVICE_FNS(X)                                                                                \
    X(GetDeviceQueue) X(QueueSubmit) X(QueueWaitIdle) X(CreateCommandPool) X(DestroyCommandPool)           \
    X(AllocateCommandBuffers) X(FreeCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)              \
    X(CmdPipelineBarrier) X(CmdBlitImage) X(CmdResolveImage) X(CreateFence) X(DestroyFence) X(ResetFences) \
    X(WaitForFences) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory)       \
    X(FreeMemory) X(BindImageMemory) X(GetMemoryFdPropertiesKHR)

struct qb_vk_fns {
#define X(name) PFN_vk##name name;
    QB_VK_INSTANCE_FNS(X) QB_VK_INSTANCE11_FNS(X) QB_VK_DEVICE_FNS(X) X(GetFenceFdKHR)
#undef X
};

struct qb_vk {
    struct qb_vk_fns fn;
    VkPhysicalDevice phys;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
    int foreign; /* VK_EXT_queue_family_foreign is enabled: imported images change hands through it */
    int sync_fd; /* the frame fence exports as a sync_file */
    VkPhysicalDeviceMemoryProperties mem;
    VkCommandPool pool;
    VkFence fence;
};

/* shortcut: images reach the app UNDEFINED, not in OpenXR's acquire layout, because xrCreateSwapchain may not
 * touch the queue; Turnip lays images out the same in every layout. A transition needs a first-acquire hook. */
struct qb_vk_swapchain {
    uint32_t count;
    int indirect;
    VkImage app[QB_MAX_SWAPCHAIN_IMAGES]; /* what the app renders into: imp[i] itself when direct */
    VkDeviceMemory app_mem[QB_MAX_SWAPCHAIN_IMAGES];
    VkImage imp[QB_MAX_SWAPCHAIN_IMAGES]; /* over the service's dma-bufs; none in a local swapchain */
    VkDeviceMemory imp_mem[QB_MAX_SWAPCHAIN_IMAGES];
    VkCommandBuffer cmd[QB_MAX_SWAPCHAIN_IMAGES]; /* app[i] into imp[i] */
    VkImage resolve;                              /* multisampled BGRA: a resolve cannot change formats */
    VkDeviceMemory resolve_mem;
};

static pthread_once_t loader_once = PTHREAD_ONCE_INIT, probe_once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr loader_gipa;
/* The instance qb_vk_create_instance made, and the app's loader it came from. */
static VkInstance created_instance;
static PFN_vkGetInstanceProcAddr created_gipa;
/* VK_EXT_queue_family_foreign is used only where it is known to be enabled: on the device
 * qb_vk_create_device enabled it on, or on an app's own device once qb_vk_device_extensions listed it. */
static VkDevice created_device;
static int created_foreign, foreign_listed;

static XrResult xr_from_vk(VkResult r) {
    return r == VK_ERROR_OUT_OF_HOST_MEMORY || r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? XR_ERROR_OUT_OF_MEMORY
                                                                                   : XR_ERROR_RUNTIME_FAILURE;
}

static void load_instance_fns(struct qb_vk_fns *f, PFN_vkGetInstanceProcAddr gipa, VkInstance instance) {
#define X(name) f->name = (PFN_vk##name)gipa(instance, "vk" #name);
    QB_VK_INSTANCE_FNS(X)
#undef X
#define X(name)                                                \
    if (!(f->name = (PFN_vk##name)gipa(instance, "vk" #name))) \
        f->name = (PFN_vk##name)gipa(instance, "vk" #name "KHR");
    QB_VK_INSTANCE11_FNS(X)
#undef X
}

/* 0 when an entry point the import needs is missing, i.e. the app left out a listed extension. */
static int load_device_fns(struct qb_vk_fns *f, VkDevice device) {
    int ok = 1;
#define X(name) ok &= (f->name = (PFN_vk##name)f->GetDeviceProcAddr(device, "vk" #name)) != NULL;
    QB_VK_DEVICE_FNS(X)
#undef X
    f->GetFenceFdKHR = (PFN_vkGetFenceFdKHR)f->GetDeviceProcAddr(device, "vkGetFenceFdKHR");
    return ok;
}

/* 1 when the device offers every one of `names`. */
static int device_has(const struct qb_vk_fns *f, VkPhysicalDevice pd, const char *const *names, uint32_t n) {
    uint32_t count = 0, found = 0;
    if (f->EnumerateDeviceExtensionProperties(pd, NULL, &count, NULL) != VK_SUCCESS) return 0;
    VkExtensionProperties *props = calloc(count + 1, sizeof(*props));
    if (!props) return 0;
    VkResult r = f->EnumerateDeviceExtensionProperties(pd, NULL, &count, props);
    for (uint32_t i = 0; i < n && (r == VK_SUCCESS || r == VK_INCOMPLETE); i++)
        for (uint32_t j = 0; j < count; j++)
            if (!strcmp(names[i], props[j].extensionName)) {
                found++;
                break;
            }
    free(props);
    return found == n;
}

/* `names` space-separated, in the two-call idiom (the count includes the NUL). */
static XrResult ext_string(const char *const *names, uint32_t n, uint32_t cap, uint32_t *count, char *buf) {
    uint32_t len = 0;
    for (uint32_t i = 0; i < n; i++) len += (uint32_t)strlen(names[i]) + 1;
    *count = len;
    if (!cap) return XR_SUCCESS;
    if (cap < len) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < n; i++) {
        size_t l = strlen(names[i]);
        memcpy(buf, names[i], l);
        buf += l;
        *buf++ = i + 1 < n ? ' ' : '\0';
    }
    return XR_SUCCESS;
}

/* The app's extension names plus the ones of `add` it lacks; free() the result, NULL when out of memory. */
static const char **merge_names(const char *const *app, uint32_t app_n, const char *const *add, uint32_t add_n,
                                uint32_t *n) {
    const char **names = malloc((app_n + add_n) * sizeof(*names));
    if (!names) return NULL;
    *n = 0;
    for (uint32_t i = 0; i < app_n; i++) names[(*n)++] = app[i];
    for (uint32_t i = 0; i < add_n; i++) {
        uint32_t j = 0;
        while (j < app_n && strcmp(app[j], add[i])) j++;
        if (j == app_n) names[(*n)++] = add[i];
    }
    return names;
}

/* The loader `instance` came from: the app's for the one qb_vk_create_instance made, else libvulkan's. */
static PFN_vkGetInstanceProcAddr instance_gipa(VkInstance instance) {
    return instance && instance == created_instance ? created_gipa : qb_vk_loader_gipa();
}

/* Whether the device XR_KHR_vulkan_enable apps are told to use offers VK_EXT_queue_family_foreign, asked
 * through a throwaway instance because theirs may not exist yet. */
static void probe_foreign(void) {
    PFN_vkGetInstanceProcAddr gipa = qb_vk_loader_gipa();
    PFN_vkCreateInstance create = gipa ? (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance") : NULL;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pEngineName = "questbridge",
                             .apiVersion = VK_API_VERSION_1_1};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app};
    VkInstance instance;
    if (!create || create(&ci, NULL, &instance) != VK_SUCCESS) return;
    struct qb_vk_fns f;
    VkPhysicalDevice pd;
    load_instance_fns(&f, gipa, instance);
    if (qb_vk_physical_device(instance, gipa, &pd) == XR_SUCCESS)
        foreign_listed = device_has(&f, pd, QB_FOREIGN_EXT, 1);
    if (f.DestroyInstance) f.DestroyInstance(instance, NULL);
}

static void open_loader(void) {
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL); /* kept for the process's lifetime */
    if (lib) loader_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
}

/* ---- XR_KHR_vulkan_enable */

XrResult qb_vk_instance_extensions(uint32_t cap, uint32_t *count, char *buf) {
    return ext_string(instance_exts, (uint32_t)QB_COUNT(instance_exts), cap, count, buf);
}

XrResult qb_vk_device_extensions(uint32_t cap, uint32_t *count, char *buf) {
    pthread_once(&probe_once, probe_foreign);
    return ext_string(device_exts, QB_REQUIRED_DEVICE_EXTS + (uint32_t)foreign_listed, cap, count, buf);
}

XrResult qb_vk_physical_device(VkInstance instance, PFN_vkGetInstanceProcAddr gipa, VkPhysicalDevice *out) {
    struct qb_vk_fns f;
    uint32_t n = 0;
    if (!instance || !gipa) return XR_ERROR_RUNTIME_FAILURE;
    load_instance_fns(&f, gipa, instance);
    if (!f.EnumeratePhysicalDevices || !f.GetPhysicalDeviceProperties || !f.EnumerateDeviceExtensionProperties ||
        f.EnumeratePhysicalDevices(instance, &n, NULL) != VK_SUCCESS || !n)
        return XR_ERROR_RUNTIME_FAILURE;
    VkPhysicalDevice *all = calloc(n, sizeof(*all));
    if (!all) return XR_ERROR_OUT_OF_MEMORY;
    VkResult r = f.EnumeratePhysicalDevices(instance, &n, all);
    VkPhysicalDevice gpu = VK_NULL_HANDLE, cpu = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < n && !gpu && (r == VK_SUCCESS || r == VK_INCOMPLETE); i++) {
        VkPhysicalDeviceProperties p;
        f.GetPhysicalDeviceProperties(all[i], &p);
        if (p.apiVersion < VK_API_VERSION_1_1 || !device_has(&f, all[i], device_exts, QB_REQUIRED_DEVICE_EXTS))
            continue;
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU)
            gpu = all[i];
        else if (!cpu)
            cpu = all[i];
    }
    free(all);
    if (!gpu && !cpu) {
        QB_ERR("no Vulkan 1.1 device offers the dma-buf import extensions\n");
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *out = gpu ? gpu : cpu;
    return XR_SUCCESS;
}

XrResult qb_vk_requirements(XrGraphicsRequirementsVulkanKHR *out) {
    out->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
    out->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}

PFN_vkGetInstanceProcAddr qb_vk_loader_gipa(void) {
    pthread_once(&loader_once, open_loader);
    return loader_gipa;
}

/* ---- XR_KHR_vulkan_enable2: a failing vkCreate* is reported through vk_result, the call itself succeeds */

XrResult qb_vk_create_instance(const XrVulkanInstanceCreateInfoKHR *info, VkInstance *instance, VkResult *vk_result) {
    PFN_vkCreateInstance create =
        (PFN_vkCreateInstance)info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return XR_ERROR_RUNTIME_FAILURE;
    VkInstanceCreateInfo ci = *info->vulkanCreateInfo;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    if (ci.pApplicationInfo) app = *ci.pApplicationInfo;
    if (app.apiVersion < VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1; /* the import path is 1.1 core */
    ci.pApplicationInfo = &app;
    uint32_t n;
    const char **names = merge_names(ci.ppEnabledExtensionNames, ci.enabledExtensionCount, instance_exts,
                                     (uint32_t)QB_COUNT(instance_exts), &n);
    if (!names) return XR_ERROR_OUT_OF_MEMORY;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = names;
    *vk_result = create(&ci, info->vulkanAllocator, instance);
    free(names);
    if (*vk_result == VK_SUCCESS) {
        created_instance = *instance;
        created_gipa = info->pfnGetInstanceProcAddr;
    }
    return XR_SUCCESS;
}

XrResult qb_vk_create_device(const XrVulkanDeviceCreateInfoKHR *info, VkDevice *device, VkResult *vk_result) {
    /* vkCreateDevice is reached through the instance the physical device came from */
    if (!created_instance) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_vk_fns f;
    load_instance_fns(&f, info->pfnGetInstanceProcAddr, created_instance);
    if (!f.CreateDevice || !f.EnumerateDeviceExtensionProperties) return XR_ERROR_RUNTIME_FAILURE;
    int foreign = device_has(&f, info->vulkanPhysicalDevice, QB_FOREIGN_EXT, 1);
    VkDeviceCreateInfo ci = *info->vulkanCreateInfo;
    uint32_t n;
    const char **names = merge_names(ci.ppEnabledExtensionNames, ci.enabledExtensionCount, device_exts,
                                     QB_REQUIRED_DEVICE_EXTS + (uint32_t)foreign, &n);
    if (!names) return XR_ERROR_OUT_OF_MEMORY;
    ci.enabledExtensionCount = n;
    ci.ppEnabledExtensionNames = names;
    *vk_result = f.CreateDevice(info->vulkanPhysicalDevice, &ci, info->vulkanAllocator, device);
    free(names);
    if (*vk_result == VK_SUCCESS) {
        created_device = *device;
        created_foreign = foreign;
    }
    return XR_SUCCESS;
}

XrResult qb_vk_physical_device2(const XrVulkanGraphicsDeviceGetInfoKHR *info, VkPhysicalDevice *out) {
    return qb_vk_physical_device(info->vulkanInstance, instance_gipa(info->vulkanInstance), out);
}

/* ---- session binding */

/* The frame fence, exportable as a sync_file where the driver can do that (else xrEndFrame waits for it). */
static VkResult create_fence(struct qb_vk *vk) {
    VkPhysicalDeviceExternalFenceInfo info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
                                              .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkExternalFenceProperties props = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
    if (vk->fn.GetFenceFdKHR && vk->fn.GetPhysicalDeviceExternalFenceProperties)
        vk->fn.GetPhysicalDeviceExternalFenceProperties(vk->phys, &info, &props);
    vk->sync_fd = (props.externalFenceFeatures & VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT) != 0;
    if (!vk->sync_fd) QB_ERR("the device cannot export a sync_file fence: every xrEndFrame waits for the GPU\n");
    VkExportFenceCreateInfo exp = {.sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO,
                                   .handleTypes = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
    VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = vk->sync_fd ? &exp : NULL};
    return vk->fn.CreateFence(vk->device, &ci, NULL, &vk->fence);
}

/* Everything qb_vk_bind sets up on the app's device; free_vk undoes a partial run. */
static XrResult init_vk(struct qb_vk *vk, const XrGraphicsBindingVulkanKHR *binding, PFN_vkGetInstanceProcAddr gipa) {
    vk->phys = binding->physicalDevice;
    vk->device = binding->device;
    vk->family = binding->queueFamilyIndex;
    vk->foreign = binding->device == created_device ? created_foreign : foreign_listed;
    load_instance_fns(&vk->fn, gipa, binding->instance);
    if (!vk->fn.GetDeviceProcAddr || !vk->fn.GetPhysicalDeviceQueueFamilyProperties ||
        !vk->fn.GetPhysicalDeviceMemoryProperties || !vk->fn.GetPhysicalDeviceImageFormatProperties2 ||
        !load_device_fns(&vk->fn, vk->device)) {
        QB_ERR("the app's VkDevice lacks the extensions xrGetVulkanDeviceExtensionsKHR lists\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    uint32_t n = 0;
    vk->fn.GetPhysicalDeviceQueueFamilyProperties(vk->phys, &n, NULL);
    VkQueueFamilyProperties *families = calloc(n + 1, sizeof(*families));
    if (!families) return XR_ERROR_OUT_OF_MEMORY;
    vk->fn.GetPhysicalDeviceQueueFamilyProperties(vk->phys, &n, families);
    /* the blit and the resolve are graphics work */
    int usable = vk->family < n && (families[vk->family].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                 binding->queueIndex < families[vk->family].queueCount;
    free(families);
    if (!usable) {
        QB_ERR("queue %u of family %u is not a graphics queue of the device\n", binding->queueIndex, vk->family);
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    vk->fn.GetDeviceQueue(vk->device, vk->family, binding->queueIndex, &vk->queue);
    vk->fn.GetPhysicalDeviceMemoryProperties(vk->phys, &vk->mem);
    VkCommandPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .queueFamilyIndex = vk->family};
    VkResult r = vk->fn.CreateCommandPool(vk->device, &pool, NULL, &vk->pool);
    if (r == VK_SUCCESS) r = create_fence(vk);
    return r == VK_SUCCESS ? XR_SUCCESS : xr_from_vk(r);
}

static void free_vk(struct qb_vk *vk) {
    if (vk->fence) vk->fn.DestroyFence(vk->device, vk->fence, NULL);
    if (vk->pool) vk->fn.DestroyCommandPool(vk->device, vk->pool, NULL);
    free(vk);
}

XrResult qb_vk_bind(const XrGraphicsBindingVulkanKHR *binding, struct qb_vk **out) {
    if (!binding->instance || !binding->device) return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    PFN_vkGetInstanceProcAddr gipa = instance_gipa(binding->instance);
    VkPhysicalDevice pick;
    XrResult r = qb_vk_physical_device(binding->instance, gipa, &pick);
    if (XR_FAILED(r)) return r;
    if (pick != binding->physicalDevice) {
        QB_ERR("the session's VkPhysicalDevice is not the one xrGetVulkanGraphicsDevice(2)KHR returns\n");
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    struct qb_vk *vk = calloc(1, sizeof(*vk));
    if (!vk) return XR_ERROR_OUT_OF_MEMORY;
    r = init_vk(vk, binding, gipa);
    if (XR_FAILED(r)) {
        free_vk(vk);
        return r;
    }
    *out = vk;
    return XR_SUCCESS;
}

void qb_vk_unbind(struct qb_vk *vk) {
    if (!vk) return;
    vk->fn.QueueWaitIdle(vk->queue); /* the bound queue is the only one this module ever submitted to */
    free_vk(vk);
}

/* ---- swapchains */

static int memory_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* The service allocates the BGRA formats as their RGBA counterpart; the blit swaps the channels back. */
static VkFormat service_format(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_SRGB    ? VK_FORMAT_R8G8B8A8_SRGB
           : f == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_R8G8B8A8_UNORM
                                           : f;
}

/* The other half of a UNORM/SRGB pair, VK_FORMAT_UNDEFINED when there is none. Images list both as view
 * formats, so the app may render through either and Turnip keeps UBWC on the mutable image. */
static VkFormat srgb_partner(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32:
        return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
        return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default:
        return VK_FORMAT_UNDEFINED;
    }
}

static VkImageUsageFlags vk_usage(XrSwapchainUsageFlags u) {
    VkImageUsageFlags v = 0;
    if (u & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) v |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) v |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (u & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) v |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) v |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (u & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) v |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (u & XR_SWAPCHAIN_USAGE_SAMPLED_BIT) v |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (u & XR_SWAPCHAIN_USAGE_INPUT_ATTACHMENT_BIT_KHR) v |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return v;
}

/* Asked before importing: will the driver take a dma-buf with this modifier as this image? */
static int modifier_ok(struct qb_vk *vk, VkFormat format, uint64_t modifier, VkImageUsageFlags usage,
                       const VkImageFormatListCreateInfo *views, uint32_t w, uint32_t h) {
    VkPhysicalDeviceExternalImageFormatInfo ext = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
        .pNext = views,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkPhysicalDeviceImageDrmFormatModifierInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT,
        .pNext = &ext,
        .drmFormatModifier = modifier,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                             .pNext = &mod,
                                             .format = format,
                                             .type = VK_IMAGE_TYPE_2D,
                                             .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                                             .usage = usage,
                                             .flags = views ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0};
    VkExternalImageFormatProperties extp = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = &extp};
    return vk->fn.GetPhysicalDeviceImageFormatProperties2(vk->phys, &info, &props) == VK_SUCCESS &&
           (extp.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) &&
           w <= props.imageFormatProperties.maxExtent.width && h <= props.imageFormatProperties.maxExtent.height;
}

/* One image over a service dma-buf: explicit single-plane layout, dedicated imported memory. *fd turns -1
 * once the driver owns it (a successful import hands the fd over). */
static XrResult import_image(struct qb_vk *vk, VkFormat format, uint32_t w, uint32_t h, VkImageUsageFlags usage,
                             const VkImageFormatListCreateInfo *views, const struct qb_swapchain_info *info, int *fd,
                             VkImage *image, VkDeviceMemory *mem) {
    VkSubresourceLayout plane = {
        .offset = 0, .size = 0, .rowPitch = info->stride_bytes, .arrayPitch = 0, .depthPitch = 0};
    VkImageDrmFormatModifierExplicitCreateInfoEXT mod = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
        .pNext = views,
        .drmFormatModifier = info->modifier,
        .drmFormatModifierPlaneCount = 1,
        .pPlaneLayouts = &plane};
    VkExternalMemoryImageCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
                                           .pNext = &mod,
                                           .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT};
    VkImageCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .pNext = &ext,
                             .flags = views ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = format,
                             .extent = {w, h, 1},
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                             .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vk->fn.CreateImage(vk->device, &ici, NULL, image);
    if (r != VK_SUCCESS) {
        QB_ERR("vkCreateImage(format %d %ux%u, modifier %#llx, pitch %u) -> %d\n", (int)format, w, h,
               (unsigned long long)info->modifier, info->stride_bytes, (int)r);
        return xr_from_vk(r);
    }
    VkMemoryRequirements req;
    vk->fn.GetImageMemoryRequirements(vk->device, *image, &req);
    VkMemoryFdPropertiesKHR fdp = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    r = vk->fn.GetMemoryFdPropertiesKHR(vk->device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, *fd, &fdp);
    int type = r == VK_SUCCESS ? memory_type(&vk->mem, req.memoryTypeBits & fdp.memoryTypeBits, 0) : -1;
    /* a layout larger than the buffer would read past its end: the two drivers disagree on the layout */
    if (type < 0 || (info->size_bytes && req.size > info->size_bytes)) {
        QB_ERR("cannot import the %llu-byte dma-buf as format %d %ux%u (layout %llu bytes, memory types %#x & %#x)\n",
               (unsigned long long)info->size_bytes, (int)format, w, h, (unsigned long long)req.size,
               req.memoryTypeBits, fdp.memoryTypeBits);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                                   .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
                                   .fd = *fd};
    VkMemoryDedicatedAllocateInfo ded = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                         .pNext = &imp,
                                         .image = *image};
    /* a dedicated allocation must be exactly the image's size, which the check above fits in the buffer */
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .pNext = &ded,
                                .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)type};
    r = vk->fn.AllocateMemory(vk->device, &mai, NULL, mem);
    if (r == VK_SUCCESS) {
        *fd = -1;
        r = vk->fn.BindImageMemory(vk->device, *image, *mem, 0);
    }
    if (r != VK_SUCCESS) {
        QB_ERR("importing the dma-buf as format %d %ux%u -> %d\n", (int)format, w, h, (int)r);
        return xr_from_vk(r);
    }
    return XR_SUCCESS;
}

/* A guest-owned image of exactly what the app asked for, plus TRANSFER_SRC for the copy out. */
static XrResult local_image(struct qb_vk *vk, const XrSwapchainCreateInfo *ci, VkImage *image, VkDeviceMemory *mem) {
    const VkFormat format = (VkFormat)ci->format;
    const uint32_t longest = ci->width > ci->height ? ci->width : ci->height;
    VkFormat pair[2] = {format, srgb_partner(format)};
    const int mutable_format = pair[1] || (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT);
    VkImageFormatListCreateInfo list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                        .viewFormatCount = 2,
                                        .pViewFormats = pair};
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = pair[1] ? &list : NULL,
        .flags = mutable_format ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {ci->width, ci->height, 1},
        .mipLevels = ci->mipCount,
        .arrayLayers = ci->arraySize,
        .samples = (VkSampleCountFlagBits)ci->sampleCount,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = vk_usage(ci->usageFlags) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkPhysicalDeviceImageFormatInfo2 info = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
                                             .pNext = ici.pNext,
                                             .format = format,
                                             .type = VK_IMAGE_TYPE_2D,
                                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                                             .usage = ici.usage,
                                             .flags = ici.flags};
    VkImageFormatProperties2 props = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
    const VkImageFormatProperties *p = &props.imageFormatProperties;
    /* outside these limits vkCreateImage is undefined behaviour rather than an error */
    if (vk->fn.GetPhysicalDeviceImageFormatProperties2(vk->phys, &info, &props) != VK_SUCCESS ||
        (ci->sampleCount & (ci->sampleCount - 1)) || !(p->sampleCounts & ci->sampleCount) ||
        (ci->sampleCount > 1 && ci->mipCount > 1) || ci->mipCount > p->maxMipLevels ||
        ci->mipCount > 32u - (uint32_t)__builtin_clz(longest) || ci->arraySize > p->maxArrayLayers ||
        ci->width > p->maxExtent.width || ci->height > p->maxExtent.height)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    VkResult r = vk->fn.CreateImage(vk->device, &ici, NULL, image);
    if (r != VK_SUCCESS) return xr_from_vk(r);
    VkMemoryRequirements req;
    vk->fn.GetImageMemoryRequirements(vk->device, *image, &req);
    int type = memory_type(&vk->mem, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = memory_type(&vk->mem, req.memoryTypeBits, 0);
    if (type < 0) return XR_ERROR_RUNTIME_FAILURE;
    VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = req.size,
                                .memoryTypeIndex = (uint32_t)type};
    r = vk->fn.AllocateMemory(vk->device, &mai, NULL, mem);
    if (r == VK_SUCCESS) r = vk->fn.BindImageMemory(vk->device, *image, *mem, 0);
    return r == VK_SUCCESS ? XR_SUCCESS : xr_from_vk(r);
}

/* Recorded once per image because the commands never change; SIMULTANEOUS_USE lets a release resubmit them
 * even when the service hands an index back before the GPU has finished the previous copy. */
static VkResult record_copy(struct qb_vk *vk, struct qb_vk_swapchain *sc, const XrSwapchainCreateInfo *ci,
                            VkFormat imported, uint32_t i) {
    const uint32_t w = ci->width, h = ci->height, layers = ci->arraySize;
    const uint32_t outside = vk->foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
    const uint32_t ours = vk->foreign ? vk->family : VK_QUEUE_FAMILY_IGNORED;
    const VkImageSubresourceRange app_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    const VkImageSubresourceRange imp_range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkCommandBuffer cb = sc->cmd[i];
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                      .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT};
    VkResult r = vk->fn.BeginCommandBuffer(cb, &begin);
    if (r != VK_SUCCESS) return r;
    /* The app releases its image COLOR_ATTACHMENT_OPTIMAL (OpenXR's release layout) after whatever wrote it
     * last; the imported image is overwritten whole and comes back from the service every time. */
    VkImageMemoryBarrier pre[3] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = sc->app[i],
         .subresourceRange = app_range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = outside,
         .dstQueueFamilyIndex = ours,
         .image = sc->imp[i],
         .subresourceRange = imp_range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = sc->resolve,
         .subresourceRange = app_range},
    };
    vk->fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                              NULL, sc->resolve ? 3 : 2, pre);
    VkImage src = sc->app[i];
    if (ci->sampleCount > 1) {
        for (uint32_t l = 0; l < layers; l++) {
            VkImageResolve region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1},
                                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, sc->resolve ? l : 0, 1},
                                     .dstOffset = {sc->resolve ? 0 : (int32_t)(l * w), 0, 0},
                                     .extent = {w, h, 1}};
            vk->fn.CmdResolveImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   sc->resolve ? sc->resolve : sc->imp[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                   &region);
        }
        if (sc->resolve) {
            VkImageMemoryBarrier mid = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .image = sc->resolve,
                                        .subresourceRange = app_range};
            vk->fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                      NULL, 1, &mid);
            src = sc->resolve;
        }
    }
    if (ci->sampleCount == 1 || sc->resolve) {
        /* the rects are 1:1, so the filter only matters for the BGRA -> RGBA conversion */
        const VkFilter filter = (VkFormat)ci->format != imported ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        for (uint32_t l = 0; l < layers; l++) {
            VkImageBlit region = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, l, 1},
                                  .srcOffsets = {{0, 0, 0}, {(int32_t)w, (int32_t)h, 1}},
                                  .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .dstOffsets = {{(int32_t)(l * w), 0, 0}, {(int32_t)((l + 1) * w), (int32_t)h, 1}}};
            vk->fn.CmdBlitImage(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc->imp[i],
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
        }
    }
    VkImageMemoryBarrier post[2] = {
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = sc->app[i],
         .subresourceRange = app_range},
        {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = ours,
         .dstQueueFamilyIndex = outside,
         .image = sc->imp[i],
         .subresourceRange = imp_range},
    };
    vk->fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0,
                              NULL, 0, NULL, 2, post);
    return vk->fn.EndCommandBuffer(cb);
}

/* The guest-owned side of an indirect swapchain: images of exactly `ci`, a single-sample one when a resolve
 * has to precede a format change, and the copy into each imported image. */
static XrResult make_indirect(struct qb_vk *vk, struct qb_vk_swapchain *sc, const XrSwapchainCreateInfo *ci,
                              VkFormat imported) {
    XrResult xr = XR_SUCCESS;
    for (uint32_t i = 0; i < sc->count && XR_SUCCEEDED(xr); i++) xr = local_image(vk, ci, &sc->app[i], &sc->app_mem[i]);
    if (XR_SUCCEEDED(xr) && ci->sampleCount > 1 && (VkFormat)ci->format != imported) {
        XrSwapchainCreateInfo one = *ci;
        one.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        one.sampleCount = one.mipCount = 1;
        xr = local_image(vk, &one, &sc->resolve, &sc->resolve_mem);
    }
    if (XR_FAILED(xr)) return xr;
    VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = vk->pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = sc->count};
    VkResult r = vk->fn.AllocateCommandBuffers(vk->device, &ai, sc->cmd);
    for (uint32_t i = 0; i < sc->count && r == VK_SUCCESS; i++) r = record_copy(vk, sc, ci, imported, i);
    return r == VK_SUCCESS ? XR_SUCCESS : xr_from_vk(r);
}

/* Everything a swapchain holds; also the cleanup of a half-built one (null handles are no-ops). */
static void free_swapchain(struct qb_vk *vk, struct qb_vk_swapchain *sc) {
    vk->fn.FreeCommandBuffers(vk->device, vk->pool, QB_MAX_SWAPCHAIN_IMAGES, sc->cmd);
    for (uint32_t i = 0; i < QB_MAX_SWAPCHAIN_IMAGES; i++) {
        if (sc->app[i] != sc->imp[i]) vk->fn.DestroyImage(vk->device, sc->app[i], NULL);
        vk->fn.DestroyImage(vk->device, sc->imp[i], NULL);
        vk->fn.FreeMemory(vk->device, sc->app_mem[i], NULL);
        vk->fn.FreeMemory(vk->device, sc->imp_mem[i], NULL);
    }
    vk->fn.DestroyImage(vk->device, sc->resolve, NULL);
    vk->fn.FreeMemory(vk->device, sc->resolve_mem, NULL);
    free(sc);
}

static XrResult import_swapchain(struct qb_vk *vk, const XrSwapchainCreateInfo *ci,
                                 const struct qb_swapchain_info *info, int *fds, uint32_t count,
                                 struct qb_vk_swapchain **out) {
    const VkFormat format = (VkFormat)ci->format, imported = service_format(format);
    const uint32_t w = ci->width * ci->arraySize, h = ci->height;
    VkFormat pair[2] = {imported, srgb_partner(imported)};
    VkImageFormatListCreateInfo list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
                                        .viewFormatCount = 2,
                                        .pViewFormats = pair};
    const VkImageFormatListCreateInfo *views = pair[1] ? &list : NULL;
    VkImageUsageFlags usage = QB_IMPORT_USAGE;
    /* whatever the imported image cannot be for the app goes through a guest-owned image */
    int indirect = ci->arraySize > 1 || ci->sampleCount > 1 || ci->mipCount > 1 || imported != format ||
                   (vk_usage(ci->usageFlags) & ~QB_IMPORT_USAGE) ||
                   ((ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) && !views);
    if (!modifier_ok(vk, imported, info->modifier, usage, views, w, h)) {
        if (info->modifier != QB_MOD_LINEAR) {
            QB_ERR("the driver refuses the service's format %d %ux%u buffers (modifier %#llx)\n", (int)imported, w, h,
                   (unsigned long long)info->modifier);
            return XR_ERROR_RUNTIME_FAILURE;
        }
        /* a linear buffer the app cannot render into directly can still take the blit */
        usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        views = NULL;
        indirect = 1;
        if (!modifier_ok(vk, imported, info->modifier, usage, NULL, w, h)) {
            QB_ERR("the driver refuses the service's linear format %d %ux%u buffers, even as a blit target\n",
                   (int)imported, w, h);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    struct qb_vk_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return XR_ERROR_OUT_OF_MEMORY;
    sc->count = count;
    sc->indirect = indirect;
    XrResult xr = XR_SUCCESS;
    for (uint32_t i = 0; i < count && XR_SUCCEEDED(xr); i++)
        xr = import_image(vk, imported, w, h, usage, views, info, &fds[i], &sc->imp[i], &sc->imp_mem[i]);
    if (XR_SUCCEEDED(xr)) {
        if (indirect)
            xr = make_indirect(vk, sc, ci, imported);
        else
            memcpy(sc->app, sc->imp, sizeof(sc->app));
    }
    if (XR_FAILED(xr)) {
        free_swapchain(vk, sc);
        return xr;
    }
    *out = sc;
    return XR_SUCCESS;
}

XrResult qb_vk_swapchain_import(struct qb_vk *vk, const XrSwapchainCreateInfo *ci,
                                const struct qb_swapchain_info *info, const int *fds, uint32_t fd_count,
                                struct qb_vk_swapchain **out) {
    int own[QB_MAX_SWAPCHAIN_IMAGES]; /* the fds not handed to the driver yet */
    if (!fd_count || fd_count > QB_MAX_SWAPCHAIN_IMAGES || fd_count != info->image_count) {
        for (uint32_t i = 0; i < fd_count; i++) close(fds[i]);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    memcpy(own, fds, fd_count * sizeof(*fds));
    XrResult xr = import_swapchain(vk, ci, info, own, fd_count, out);
    for (uint32_t i = 0; i < fd_count; i++)
        if (own[i] >= 0) close(own[i]);
    return xr;
}

XrResult qb_vk_swapchain_create_local(struct qb_vk *vk, const XrSwapchainCreateInfo *ci, uint32_t image_count,
                                      struct qb_vk_swapchain **out) {
    if (!image_count || image_count > QB_MAX_SWAPCHAIN_IMAGES) return XR_ERROR_RUNTIME_FAILURE;
    struct qb_vk_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return XR_ERROR_OUT_OF_MEMORY;
    sc->count = image_count;
    XrResult xr = XR_SUCCESS;
    for (uint32_t i = 0; i < image_count && XR_SUCCEEDED(xr); i++)
        xr = local_image(vk, ci, &sc->app[i], &sc->app_mem[i]);
    if (XR_FAILED(xr)) {
        free_swapchain(vk, sc);
        return xr;
    }
    *out = sc;
    return XR_SUCCESS;
}

void qb_vk_swapchain_destroy(struct qb_vk *vk, struct qb_vk_swapchain *sc) {
    if (!sc) return;
    vk->fn.QueueWaitIdle(vk->queue); /* a copy, or the app's own rendering, may still use the images */
    free_swapchain(vk, sc);
}

uint32_t qb_vk_swapchain_image_count(const struct qb_vk_swapchain *sc) {
    return sc->count;
}

VkImage qb_vk_swapchain_app_image(const struct qb_vk_swapchain *sc, uint32_t index) {
    return index < sc->count ? sc->app[index] : VK_NULL_HANDLE;
}

int qb_vk_swapchain_is_indirect(const struct qb_vk_swapchain *sc) {
    return sc->indirect;
}

XrResult qb_vk_swapchain_release(struct qb_vk *vk, struct qb_vk_swapchain *sc, uint32_t index) {
    if (!sc->indirect) return XR_SUCCESS;
    if (index >= sc->count) return XR_ERROR_RUNTIME_FAILURE;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                       .commandBufferCount = 1,
                       .pCommandBuffers = &sc->cmd[index]};
    VkResult r = vk->fn.QueueSubmit(vk->queue, 1, &si, VK_NULL_HANDLE);
    return r == VK_SUCCESS ? XR_SUCCESS : xr_from_vk(r);
}

uint32_t qb_vk_extra_formats(uint32_t cap, int64_t *out) {
    static const int64_t extra[] = {VK_FORMAT_D32_SFLOAT,    VK_FORMAT_D24_UNORM_S8_UINT,
                                    VK_FORMAT_D16_UNORM,     VK_FORMAT_D32_SFLOAT_S8_UINT,
                                    VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM};
    for (uint32_t i = 0; i < cap && i < QB_COUNT(extra); i++) out[i] = extra[i];
    return (uint32_t)QB_COUNT(extra);
}

/* Depth and stencil (VkFormat 124..130) stay with the app: the service composites colour only. */
int qb_vk_format_is_local(int64_t format) {
    return format >= VK_FORMAT_D16_UNORM && format <= VK_FORMAT_D32_SFLOAT_S8_UINT;
}

/* ---- frame completion */

int qb_vk_end_frame_fence(struct qb_vk *vk) {
    VkResult r = vk->fn.ResetFences(vk->device, 1, &vk->fence);
    /* no batches: the fence signals once everything already on the queue has finished */
    if (r == VK_SUCCESS) r = vk->fn.QueueSubmit(vk->queue, 0, NULL, vk->fence);
    if (r != VK_SUCCESS) {
        QB_ERR("submitting the frame fence -> %d\n", (int)r);
        vk->fn.QueueWaitIdle(vk->queue);
        return -1;
    }
    if (vk->sync_fd) {
        VkFenceGetFdInfoKHR get = {.sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR,
                                   .fence = vk->fence,
                                   .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT};
        int fd = -1;
        /* success with -1 means already signalled: nothing for the service to wait for */
        if (vk->fn.GetFenceFdKHR(vk->device, &get, &fd) == VK_SUCCESS) return fd;
        QB_ERR("vkGetFenceFdKHR(SYNC_FD) failed: every xrEndFrame waits for the GPU from now on\n");
        vk->sync_fd = 0;
    }
    vk->fn.WaitForFences(vk->device, 1, &vk->fence, VK_TRUE, UINT64_MAX);
    return -1;
}

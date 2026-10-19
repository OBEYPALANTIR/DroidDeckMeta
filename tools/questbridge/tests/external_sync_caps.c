/* Diagnostic CLI: inventory each Vulkan physical device's external fence, semaphore and buffer
 * memory handle capabilities as the real driver reports them. Not a unit test and not a pass/fail
 * hardware gate: exit 0 means the inventory completed, even when every capability is zero.
 * Exit 1 means loader/instance/device-enumeration setup failed or a device was below Vulkan 1.1.
 * Creates only a VkInstance (no extensions, no surface, no VkDevice, no queue submission).
 * Buffer results cover VkBuffer TRANSFER_SRC|TRANSFER_DST only; they are not image-format proof.
 *
 * Build (guest-native image): gcc -std=gnu11 -O2 -Wall -Wextra -o external_sync_caps
 *     tools/questbridge/tests/external_sync_caps.c -ldl
 * Run in the guest: ./external_sync_caps   (no arguments; honours VK_ICD_FILENAMES etc.) */
#define VK_NO_PROTOTYPES 1
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

static const char *vk_result_name(VkResult r)
{
    switch (r) {
#define R(x) case x: return #x;
    R(VK_SUCCESS) R(VK_NOT_READY) R(VK_TIMEOUT) R(VK_EVENT_SET) R(VK_EVENT_RESET) R(VK_INCOMPLETE)
    R(VK_ERROR_OUT_OF_HOST_MEMORY) R(VK_ERROR_OUT_OF_DEVICE_MEMORY) R(VK_ERROR_INITIALIZATION_FAILED)
    R(VK_ERROR_DEVICE_LOST) R(VK_ERROR_MEMORY_MAP_FAILED) R(VK_ERROR_LAYER_NOT_PRESENT)
    R(VK_ERROR_EXTENSION_NOT_PRESENT) R(VK_ERROR_FEATURE_NOT_PRESENT) R(VK_ERROR_INCOMPATIBLE_DRIVER)
    R(VK_ERROR_TOO_MANY_OBJECTS) R(VK_ERROR_FORMAT_NOT_SUPPORTED) R(VK_ERROR_FRAGMENTED_POOL)
    R(VK_ERROR_UNKNOWN) R(VK_ERROR_OUT_OF_POOL_MEMORY) R(VK_ERROR_INVALID_EXTERNAL_HANDLE)
#undef R
    default: return "VK_RESULT_UNRECOGNISED";
    }
}

struct flag_name { uint32_t bit; const char *name; };

static void print_flags(const char *label, uint32_t value, const struct flag_name *names)
{
    printf(" %s=0x%" PRIx32 "[", label, value);
    const char *sep = "";
    uint32_t known = 0;
    for (; names->name; names++) {
        known |= names->bit;
        if (value & names->bit) {
            printf("%s%s", sep, names->name);
            sep = "|";
        }
    }
    if (value & ~known)
        printf("%sUNNAMED:0x%" PRIx32, sep, value & ~known);
    printf("]");
}

static const struct flag_name fence_handles[] = {
    { VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
    { VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32" },
    { VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
    { VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" },
    { 0, NULL },
};
static const struct flag_name fence_features[] = {
    { VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT, "EXPORTABLE" },
    { VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT, "IMPORTABLE" },
    { 0, NULL },
};
static const struct flag_name semaphore_handles[] = {
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT, "D3D12_FENCE" },
    { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" },
    { 0, NULL },
};
static const struct flag_name semaphore_features[] = {
    { VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT, "EXPORTABLE" },
    { VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT, "IMPORTABLE" },
    { 0, NULL },
};
static const struct flag_name memory_handles[] = {
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT, "OPAQUE_WIN32" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_KMT_BIT, "OPAQUE_WIN32_KMT" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, "D3D11_TEXTURE" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_KMT_BIT, "D3D11_TEXTURE_KMT" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_HEAP_BIT, "D3D12_HEAP" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D12_RESOURCE_BIT, "D3D12_RESOURCE" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "DMA_BUF" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, "ANDROID_HARDWARE_BUFFER" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, "HOST_ALLOCATION" },
    { VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_MAPPED_FOREIGN_MEMORY_BIT_EXT, "HOST_MAPPED_FOREIGN_MEMORY" },
    { 0, NULL },
};
static const struct flag_name memory_features[] = {
    { VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT, "DEDICATED_ONLY" },
    { VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT, "EXPORTABLE" },
    { VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT, "IMPORTABLE" },
    { 0, NULL },
};

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkEnumerateDeviceExtensionProperties enumerate_device_extensions;
static PFN_vkGetPhysicalDeviceProperties get_properties;
static PFN_vkGetPhysicalDeviceProperties2 get_properties2;
static PFN_vkGetPhysicalDeviceExternalFenceProperties get_fence_properties;
static PFN_vkGetPhysicalDeviceExternalSemaphoreProperties get_semaphore_properties;
static PFN_vkGetPhysicalDeviceExternalBufferProperties get_buffer_properties;

static void print_version(const char *label, uint32_t v)
{
    printf(" %s=%u.%u.%u", label, VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v),
           VK_API_VERSION_PATCH(v));
}

static void print_uuid(const char *label, const uint8_t *bytes, size_t n)
{
    printf(" %s=", label);
    for (size_t i = 0; i < n; i++)
        printf("%02x", bytes[i]);
}

/* Returns the listed spec version, or 0 when the extension is not listed. */
static uint32_t extension_version(const VkExtensionProperties *exts, uint32_t count, const char *name)
{
    for (uint32_t i = 0; i < count; i++)
        if (!strcmp(exts[i].extensionName, name))
            return exts[i].specVersion;
    return 0;
}

/* Returns 0 when the device inventory completed, 1 when it could not be queried. */
static int inventory_device(uint32_t index, VkPhysicalDevice gpu, uint32_t instance_api)
{
    VkPhysicalDeviceProperties props;
    get_properties(gpu, &props);
    printf("gpu[%u] name=\"%s\" vendorID=0x%04" PRIx32 " deviceID=0x%08" PRIx32
           " deviceType=%d driverVersion=0x%08" PRIx32, index, props.deviceName, props.vendorID,
           props.deviceID, (int)props.deviceType, props.driverVersion);
    print_version("apiVersion", props.apiVersion);
    printf("\n");
    if (props.apiVersion < VK_API_VERSION_1_1) {
        printf("gpu[%u] ERROR device apiVersion below 1.1; core external-handle queries are not valid\n", index);
        return 1;
    }

    uint32_t ext_count = 0;
    VkResult r = enumerate_device_extensions(gpu, NULL, &ext_count, NULL);
    if (r != VK_SUCCESS) {
        printf("gpu[%u] ERROR vkEnumerateDeviceExtensionProperties(count)=%s (%d)\n", index,
               vk_result_name(r), r);
        return 1;
    }
    VkExtensionProperties *exts = calloc(ext_count ? ext_count : 1, sizeof(*exts));
    if (!exts) {
        printf("gpu[%u] ERROR out of host memory for %u extensions\n", index, ext_count);
        return 1;
    }
    r = enumerate_device_extensions(gpu, NULL, &ext_count, exts);
    if (r != VK_SUCCESS) {
        printf("gpu[%u] ERROR vkEnumerateDeviceExtensionProperties(list)=%s (%d)\n", index,
               vk_result_name(r), r);
        free(exts);
        return 1;
    }
    /* Listing status is reported beside, never instead of, the driver's query answers. */
    static const char *const watched[] = {
        "VK_KHR_external_fence_fd", "VK_KHR_external_semaphore_fd", "VK_KHR_external_memory_fd",
        "VK_EXT_external_memory_dma_buf", "VK_KHR_timeline_semaphore", "VK_KHR_driver_properties",
        "VK_EXT_image_drm_format_modifier", "VK_ANDROID_external_memory_android_hardware_buffer",
    };
    for (size_t i = 0; i < sizeof(watched) / sizeof(watched[0]); i++) {
        uint32_t spec = extension_version(exts, ext_count, watched[i]);
        if (spec)
            printf("gpu[%u] ext %s=listed specVersion=%u\n", index, watched[i], spec);
        else
            printf("gpu[%u] ext %s=not-listed\n", index, watched[i]);
    }

    /* Physical-device-level 1.2 core use needs both the instance and the device at 1.2. */
    uint32_t effective_api = props.apiVersion < instance_api ? props.apiVersion : instance_api;
    int core12 = effective_api >= VK_API_VERSION_1_2;
    int timeline = core12 || extension_version(exts, ext_count, "VK_KHR_timeline_semaphore");
    int driver_props = core12 || extension_version(exts, ext_count, "VK_KHR_driver_properties");
    /* DMA_BUF is not a core handle type; querying it without the extension is invalid usage. */
    int dma_buf = extension_version(exts, ext_count, "VK_EXT_external_memory_dma_buf") != 0;
    free(exts);

    VkPhysicalDeviceDriverProperties driver = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceIDProperties id = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES,
        .pNext = driver_props ? &driver : NULL,
    };
    VkPhysicalDeviceProperties2 props2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id };
    get_properties2(gpu, &props2);
    printf("gpu[%u]", index);
    print_uuid("deviceUUID", id.deviceUUID, VK_UUID_SIZE);
    print_uuid("driverUUID", id.driverUUID, VK_UUID_SIZE);
    printf(" deviceLUIDValid=%u", id.deviceLUIDValid);
    if (id.deviceLUIDValid)
        print_uuid("deviceLUID", id.deviceLUID, VK_LUID_SIZE);
    printf(" deviceNodeMask=0x%" PRIx32 "\n", id.deviceNodeMask);
    if (driver_props)
        printf("gpu[%u] driverID=%d driverName=\"%s\" driverInfo=\"%s\" conformance=%u.%u.%u.%u\n",
               index, (int)driver.driverID, driver.driverName, driver.driverInfo,
               driver.conformanceVersion.major, driver.conformanceVersion.minor,
               driver.conformanceVersion.subminor, driver.conformanceVersion.patch);
    else
        printf("gpu[%u] driverProperties=skipped reason=neither-effective-1.2-nor-VK_KHR_driver_properties\n", index);

    static const struct { VkExternalFenceHandleTypeFlagBits bit; const char *name; } fence_types[] = {
        { VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
        { VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" },
    };
    for (size_t i = 0; i < 2; i++) {
        VkPhysicalDeviceExternalFenceInfo info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
            .handleType = fence_types[i].bit,
        };
        VkExternalFenceProperties out = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES };
        get_fence_properties(gpu, &info, &out);
        printf("gpu[%u] fence handle=%s", index, fence_types[i].name);
        print_flags("features", out.externalFenceFeatures, fence_features);
        print_flags("exportFromImported", out.exportFromImportedHandleTypes, fence_handles);
        print_flags("compatible", out.compatibleHandleTypes, fence_handles);
        printf("\n");
    }

    static const struct { VkExternalSemaphoreHandleTypeFlagBits bit; const char *name; } sem_types[] = {
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
        { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD" },
    };
    for (int timeline_pass = 0; timeline_pass < 2; timeline_pass++) {
        const char *type_name = timeline_pass ? "TIMELINE" : "BINARY";
        if (timeline_pass && !timeline) {
            printf("gpu[%u] semaphore type=TIMELINE skipped reason=neither-effective-1.2-nor-VK_KHR_timeline_semaphore\n",
                   index);
            continue;
        }
        for (size_t i = 0; i < 2; i++) {
            VkSemaphoreTypeCreateInfo type_info = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            };
            /* Binary is queried with no chain: the spec default, and valid without 1.2. */
            VkPhysicalDeviceExternalSemaphoreInfo info = {
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                .pNext = timeline_pass ? &type_info : NULL,
                .handleType = sem_types[i].bit,
            };
            VkExternalSemaphoreProperties out = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
            get_semaphore_properties(gpu, &info, &out);
            printf("gpu[%u] semaphore type=%s handle=%s", index, type_name, sem_types[i].name);
            print_flags("features", out.externalSemaphoreFeatures, semaphore_features);
            print_flags("exportFromImported", out.exportFromImportedHandleTypes, semaphore_handles);
            print_flags("compatible", out.compatibleHandleTypes, semaphore_handles);
            printf("\n");
        }
    }

    static const struct { VkExternalMemoryHandleTypeFlagBits bit; const char *name; } mem_types[] = {
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD" },
        { VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, "DMA_BUF" },
    };
    for (size_t i = 0; i < 2; i++) {
        if (mem_types[i].bit == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT && !dma_buf) {
            printf("gpu[%u] buffer handle=%s SKIPPED reason=VK_EXT_external_memory_dma_buf-not-listed"
                   " (query would be invalid usage; no capability value reported)\n",
                   index, mem_types[i].name);
            continue;
        }
        VkPhysicalDeviceExternalBufferInfo info = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .handleType = mem_types[i].bit,
        };
        VkExternalBufferProperties out = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
        get_buffer_properties(gpu, &info, &out);
        const VkExternalMemoryProperties *m = &out.externalMemoryProperties;
        printf("gpu[%u] buffer handle=%s usage=TRANSFER_SRC|TRANSFER_DST", index, mem_types[i].name);
        print_flags("features", m->externalMemoryFeatures, memory_features);
        print_flags("exportFromImported", m->exportFromImportedHandleTypes, memory_handles);
        print_flags("compatible", m->compatibleHandleTypes, memory_handles);
        printf(" scope=buffer-only-not-image-format-proof\n");
    }
    return 0;
}

#define LOAD(dst, instance, name) do { \
    dst = (void *)gipa(instance, name); \
    if (!dst) { \
        printf("ERROR vkGetInstanceProcAddr(%s) returned NULL\n", name); \
        status = 1; \
        goto out; \
    } \
} while (0)

int main(int argc, char **argv)
{
    (void)argv;
    if (argc != 1) {
        fprintf(stderr, "usage: external_sync_caps   (no arguments; inventory only, not a hardware pass)\n");
        return 2;
    }
    setvbuf(stdout, NULL, _IOLBF, 0);

    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("ERROR dlopen(libvulkan.so.1): %s\n", dlerror());
        return 1;
    }
    int status = 0;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkDestroyInstance destroy_instance = NULL;
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        printf("ERROR dlsym(vkGetInstanceProcAddr): %s\n", dlerror());
        status = 1;
        goto out;
    }

    PFN_vkEnumerateInstanceVersion enumerate_instance_version;
    LOAD(enumerate_instance_version, VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    uint32_t loader_api = 0;
    VkResult r = enumerate_instance_version(&loader_api);
    if (r != VK_SUCCESS) {
        printf("ERROR vkEnumerateInstanceVersion=%s (%d)\n", vk_result_name(r), r);
        status = 1;
        goto out;
    }
    if (loader_api < VK_API_VERSION_1_1) {
        printf("ERROR loader instance version below 1.1\n");
        status = 1;
        goto out;
    }
    /* 1.1 is the floor; ask for 1.2 when the loader has it so core timeline queries are legal. */
    uint32_t instance_api = loader_api >= VK_API_VERSION_1_2 ? VK_API_VERSION_1_2 : VK_API_VERSION_1_1;
    printf("loader");
    print_version("instanceVersion", loader_api);
    print_version("requestedApiVersion", instance_api);
    printf("\n");

    PFN_vkCreateInstance create_instance;
    LOAD(create_instance, VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "external_sync_caps",
        .apiVersion = instance_api,
    };
    VkInstanceCreateInfo create = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    r = create_instance(&create, NULL, &instance);
    if (r != VK_SUCCESS) {
        printf("ERROR vkCreateInstance=%s (%d)\n", vk_result_name(r), r);
        instance = VK_NULL_HANDLE;
        status = 1;
        goto out;
    }
    LOAD(destroy_instance, instance, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices enumerate_devices;
    LOAD(enumerate_devices, instance, "vkEnumeratePhysicalDevices");
    LOAD(enumerate_device_extensions, instance, "vkEnumerateDeviceExtensionProperties");
    LOAD(get_properties, instance, "vkGetPhysicalDeviceProperties");
    LOAD(get_properties2, instance, "vkGetPhysicalDeviceProperties2");
    LOAD(get_fence_properties, instance, "vkGetPhysicalDeviceExternalFenceProperties");
    LOAD(get_semaphore_properties, instance, "vkGetPhysicalDeviceExternalSemaphoreProperties");
    LOAD(get_buffer_properties, instance, "vkGetPhysicalDeviceExternalBufferProperties");

    uint32_t count = 0;
    r = enumerate_devices(instance, &count, NULL);
    if (r != VK_SUCCESS) {
        printf("ERROR vkEnumeratePhysicalDevices(count)=%s (%d)\n", vk_result_name(r), r);
        status = 1;
        goto out;
    }
    if (!count) {
        printf("ERROR vkEnumeratePhysicalDevices reported 0 devices\n");
        status = 1;
        goto out;
    }
    VkPhysicalDevice *gpus = calloc(count, sizeof(*gpus));
    if (!gpus) {
        printf("ERROR out of host memory for %u devices\n", count);
        status = 1;
        goto out;
    }
    r = enumerate_devices(instance, &count, gpus);
    if (r != VK_SUCCESS) {
        printf("ERROR vkEnumeratePhysicalDevices(list)=%s (%d)\n", vk_result_name(r), r);
        free(gpus);
        status = 1;
        goto out;
    }
    printf("physicalDevices=%u\n", count);
    for (uint32_t i = 0; i < count; i++)
        status |= inventory_device(i, gpus[i], instance_api);
    free(gpus);
    printf(status ? "inventory=incomplete\n"
                  : "inventory=complete (capability report only; not a hardware interop pass)\n");

out:
    if (instance && destroy_instance)
        destroy_instance(instance, NULL);
    dlclose(lib);
    return status;
}

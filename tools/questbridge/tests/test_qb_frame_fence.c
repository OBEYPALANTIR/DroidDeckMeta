/* Execute the production frame-completion path with Vulkan SYNC_FD copy-transference semantics.
 * Built by tools/tests/test_questbridge_frame_fence.py; no Vulkan loader or GPU is used. */
#include <fcntl.h>
#include <stdio.h>

#include "../runtime/qb_vulkan.c"
#pragma GCC visibility pop

enum payload { RESET, PENDING, SIGNALED };
struct fake_fence {
    enum payload payload;
    unsigned submissions, exports;
};
static struct {
    struct qb_vk vk;
    struct fake_fence fences[QB_MAX_SWAPCHAIN_IMAGES];
    enum payload submitted_payload, exported_payload;
    VkResult reset_result, submit_result, export_result, wait_result;
    unsigned resets, submits, exports, waits, reset_waits, completed_waits;
    int export_minus_one;
} fake;
static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        printf("FAIL: line %d: %s\n", __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static struct fake_fence *find_fence(VkFence handle) {
    for (uint32_t i = 0; i < QB_MAX_SWAPCHAIN_IMAGES; i++)
        if (fake.vk.frame[i] == handle) return &fake.fences[i];
    CHECK(0 && "unknown fence handle");
    return NULL;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_reset(VkDevice device, uint32_t count, const VkFence *handles) {
    CHECK(device == fake.vk.device && count == 1 && handles != NULL);
    fake.resets++;
    if (fake.reset_result != VK_SUCCESS) return fake.reset_result;
    struct fake_fence *fence = find_fence(*handles);
    if (!fence) return VK_ERROR_UNKNOWN;
    CHECK(fence->payload != PENDING);
    fence->payload = RESET;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_submit(VkQueue queue, uint32_t count,
                                                 const VkSubmitInfo *submits, VkFence handle) {
    CHECK(queue == fake.vk.queue && count == 0 && submits == NULL);
    fake.submits++;
    if (fake.submit_result != VK_SUCCESS) return fake.submit_result;
    struct fake_fence *fence = find_fence(handle);
    if (!fence) return VK_ERROR_UNKNOWN;
    CHECK(fence->payload == RESET);
    fence->payload = fake.submitted_payload;
    fence->submissions++;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_export(VkDevice device, const VkFenceGetFdInfoKHR *info, int *fd) {
    CHECK(device == fake.vk.device);
    CHECK(info->sType == VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR && info->pNext == NULL);
    CHECK(info->handleType == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT);
    fake.exports++;
    if (fake.export_result != VK_SUCCESS) return fake.export_result;
    struct fake_fence *fence = find_fence(info->fence);
    if (!fence) return VK_ERROR_UNKNOWN;
    CHECK(fence->payload == PENDING || fence->payload == SIGNALED);
    if (fake.export_minus_one) {
        CHECK(fence->payload == SIGNALED);
        *fd = -1;
    } else {
        /* A real disposable descriptor models ownership, not the kernel sync_file's readiness. */
        *fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        CHECK(*fd >= 0);
        if (*fd < 0) return VK_ERROR_TOO_MANY_OBJECTS;
    }
    fake.exported_payload = fence->payload;
    fence->payload = RESET; /* SYNC_FD export copies the payload and resets its source, including -1. */
    fence->exports++;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL fake_wait(VkDevice device, uint32_t count, const VkFence *handles,
                                               VkBool32 wait_all, uint64_t timeout) {
    CHECK(device == fake.vk.device && count == 1 && handles != NULL);
    CHECK(wait_all == VK_TRUE && timeout == UINT64_MAX);
    fake.waits++;
    struct fake_fence *fence = find_fence(*handles);
    if (!fence) return VK_ERROR_UNKNOWN;
    if (fence->payload == RESET) {
        fake.reset_waits++;
        return VK_TIMEOUT; /* Never hang the regression gate on an exported/reset source. */
    }
    if (fake.wait_result != VK_SUCCESS) return fake.wait_result;
    fence->payload = SIGNALED; /* Deterministically finish a queued payload on a successful CPU wait. */
    fake.completed_waits++;
    return VK_SUCCESS;
}

static void setup(void) {
    memset(&fake, 0, sizeof(fake));
    fake.submitted_payload = PENDING;
    fake.vk.device = (VkDevice)(uintptr_t)1;
    fake.vk.queue = (VkQueue)(uintptr_t)2;
    fake.vk.sync_fd = 1;
    fake.vk.fn.ResetFences = fake_reset;
    fake.vk.fn.QueueSubmit = fake_submit;
    fake.vk.fn.GetFenceFdKHR = fake_export;
    fake.vk.fn.WaitForFences = fake_wait;
    for (uint32_t i = 0; i < QB_MAX_SWAPCHAIN_IMAGES; i++)
        fake.vk.frame[i] = (VkFence)(uintptr_t)(i + 1);
}

static void positive_exports(enum payload payload) {
    setup();
    fake.submitted_payload = payload;
    const uint32_t frames = 2 * QB_MAX_SWAPCHAIN_IMAGES + 1;
    for (uint32_t i = 0; i < frames; i++) {
        int fd = -99;
        XrResult result = qb_vk_end_frame_fence(&fake.vk, &fd);
        CHECK(result == XR_SUCCESS);
        CHECK(fd >= 0 && fcntl(fd, F_GETFD) != -1);
        if (fd >= 0) CHECK(close(fd) == 0);
        CHECK(fake.vk.frame_next == (i + 1) % QB_MAX_SWAPCHAIN_IMAGES);
        CHECK(fake.exported_payload == payload);
        CHECK(fake.fences[i % QB_MAX_SWAPCHAIN_IMAGES].payload == RESET);
        CHECK(fake.waits == 0 && fake.reset_waits == 0);
    }
    CHECK(fake.resets == frames && fake.submits == frames && fake.exports == frames);
    for (uint32_t i = 0; i < QB_MAX_SWAPCHAIN_IMAGES; i++) {
        unsigned expected = 2 + (i == 0);
        CHECK(fake.fences[i].submissions == expected && fake.fences[i].exports == expected);
    }
}

static void already_signaled_exports(void) {
    setup();
    fake.submitted_payload = SIGNALED;
    fake.export_minus_one = 1;
    const uint32_t frames = 2 * QB_MAX_SWAPCHAIN_IMAGES + 1;
    for (uint32_t i = 0; i < frames; i++) {
        int fd = -99;
        CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == XR_SUCCESS);
        CHECK(fd == -1);
        CHECK(fake.fences[i % QB_MAX_SWAPCHAIN_IMAGES].payload == RESET);
        CHECK(fake.exported_payload == SIGNALED);
        CHECK(fake.waits == 0 && fake.reset_waits == 0);
    }
    CHECK(fake.exports == frames && fake.submits == frames);
    CHECK(fake.vk.frame_next == 1);
}

static void cpu_completion(int failed_export) {
    setup();
    fake.vk.sync_fd = failed_export;
    fake.export_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
    const uint32_t frames = 2 * QB_MAX_SWAPCHAIN_IMAGES + 1;
    for (uint32_t i = 0; i < frames; i++) {
        int fd = -99;
        CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == XR_SUCCESS);
        CHECK(fd == -1);
        CHECK(fake.vk.sync_fd == 0);
        CHECK(fake.fences[i % QB_MAX_SWAPCHAIN_IMAGES].payload == SIGNALED);
        CHECK(fake.completed_waits == i + 1);
        CHECK(fake.reset_waits == 0);
    }
    CHECK(fake.exports == (unsigned)failed_export);
    CHECK(fake.waits == frames && fake.submits == frames && fake.resets == frames);
    CHECK(fake.vk.frame_next == 1);
}

static void reset_and_submit_errors(void) {
    const VkResult errors[] = {VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY, VK_ERROR_OUT_OF_DEVICE_MEMORY};
    const XrResult expected[] = {XR_ERROR_RUNTIME_FAILURE, XR_ERROR_OUT_OF_MEMORY, XR_ERROR_OUT_OF_MEMORY};
    for (uint32_t i = 0; i < QB_COUNT(errors); i++) {
        for (int submit_error = 0; submit_error < 2; submit_error++) {
            setup();
            if (submit_error) fake.submit_result = errors[i];
            else fake.reset_result = errors[i];
            int fd = -99;
            CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == expected[i]);
            CHECK(fd == -1);
            CHECK(fake.vk.frame_next == 0 && fake.vk.frame_pending[0] == 0);
            CHECK(fake.fences[0].payload == RESET && fake.fences[0].submissions == 0);
            CHECK(fake.resets == 1 && fake.submits == (unsigned)submit_error);
            CHECK(fake.exports == 0 && fake.waits == 0);
        }
    }
}

static void wait_errors(void) {
    const VkResult errors[] = {VK_TIMEOUT, VK_ERROR_DEVICE_LOST, VK_ERROR_OUT_OF_HOST_MEMORY,
                               VK_ERROR_OUT_OF_DEVICE_MEMORY};
    const XrResult expected[] = {XR_ERROR_RUNTIME_FAILURE, XR_ERROR_RUNTIME_FAILURE,
                                 XR_ERROR_OUT_OF_MEMORY, XR_ERROR_OUT_OF_MEMORY};
    for (uint32_t i = 0; i < QB_COUNT(errors); i++) {
        for (int failed_export = 0; failed_export < 2; failed_export++) {
            setup();
            fake.vk.sync_fd = failed_export;
            fake.export_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
            fake.wait_result = errors[i];
            int fd = -99;
            CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == expected[i]);
            CHECK(fd == -1 && fake.vk.frame_pending[0] == 1);
            CHECK(fake.fences[0].payload == PENDING);
            CHECK(fake.waits == 1 && fake.completed_waits == 0 && fake.reset_waits == 0);
            CHECK(fake.exports == (unsigned)failed_export && fake.vk.sync_fd == 0);

            /* Fill the remaining slots, then encounter the earlier genuinely pending submission. */
            fake.wait_result = VK_SUCCESS;
            for (uint32_t slot = 1; slot < QB_MAX_SWAPCHAIN_IMAGES; slot++) {
                fd = -99;
                CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == XR_SUCCESS && fd == -1);
            }
            CHECK(fake.vk.frame_next == 0);
            unsigned resets = fake.resets, submits = fake.submits, waits = fake.waits;
            fake.wait_result = errors[i];
            fd = -99;
            CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == expected[i]);
            CHECK(fd == -1 && fake.vk.frame_next == 0 && fake.vk.frame_pending[0] == 1);
            CHECK(fake.resets == resets && fake.submits == submits && fake.waits == waits + 1);
            CHECK(fake.fences[0].payload == PENDING);

            fake.wait_result = VK_SUCCESS;
            fd = -99;
            CHECK(qb_vk_end_frame_fence(&fake.vk, &fd) == XR_SUCCESS && fd == -1);
            CHECK(fake.resets == resets + 1 && fake.submits == submits + 1 && fake.waits == waits + 3);
            CHECK(fake.fences[0].payload == SIGNALED && fake.vk.frame_pending[0] == 0);
            CHECK(fake.vk.frame_next == 1 && fake.reset_waits == 0);
        }
    }
}

int main(void) {
    positive_exports(PENDING);
    positive_exports(SIGNALED);
    already_signaled_exports();
    cpu_completion(1);
    cpu_completion(0);
    reset_and_submit_errors();
    wait_errors();
    if (failures) return 1;
    puts("PASS: qb_vk_end_frame_fence payload ownership and error propagation");
    return 0;
}

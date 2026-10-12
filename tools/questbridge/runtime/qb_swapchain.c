/* qb_swapchain.c - XrSwapchain: the service's formats plus qb_vulkan's extras, service-allocated
 * swapchains imported into the app's device (guest-only ones for depth), and the acquire/wait/release
 * order the spec requires. Release remembers the index END_FRAME hands to the service. */
#define _GNU_SOURCE
#include "qb_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* qb_vulkan reaches the BGRA formats by blitting into the RGBA buffer the service allocates. */
static int64_t service_format(int64_t format) {
    switch (format) {
    case VK_FORMAT_B8G8R8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return VK_FORMAT_R8G8B8A8_UNORM;
    default:
        return format;
    }
}

static int has_format(const int64_t *list, uint32_t n, int64_t format) {
    for (uint32_t i = 0; i < n; i++)
        if (list[i] == format) return 1;
    return 0;
}

/* The service's formats (asked once per session, in its order of preference), then qb_vulkan's extras
 * that stay local or have a service format behind them. `out` holds 2 * QB_MAX_FORMATS. */
static XrResult session_formats(struct qb_session *s, int64_t *out, uint32_t *count) {
    if (!s->formats_cached) {
        struct qb_formats f;
        XrResult r = qb_ipc_call(&s->inst->ipc, QB_OP_ENUM_FORMATS, NULL, 0, NULL, 0, &f, sizeof(f), NULL, 0, NULL);
        if (XR_FAILED(r)) return r;
        s->format_count = f.count < QB_MAX_FORMATS ? f.count : QB_MAX_FORMATS;
        memcpy(s->formats, f.formats, s->format_count * sizeof(*f.formats));
        s->formats_cached = 1;
    }
    uint32_t n = 0;
    for (uint32_t i = 0; i < s->format_count; i++)
        if (!has_format(out, n, s->formats[i])) out[n++] = s->formats[i];
    int64_t extra[QB_MAX_FORMATS];
    uint32_t extras = qb_vk_extra_formats(QB_MAX_FORMATS, extra);
    if (extras > QB_MAX_FORMATS) extras = QB_MAX_FORMATS;
    for (uint32_t i = 0; i < extras; i++)
        if (!has_format(out, n, extra[i]) &&
            (qb_vk_format_is_local(extra[i]) || has_format(s->formats, s->format_count, service_format(extra[i]))))
            out[n++] = extra[i];
    *count = n;
    return XR_SUCCESS;
}

XrResult qb_xrEnumerateSwapchainFormats(XrSession session, uint32_t cap, uint32_t *count, int64_t *formats) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!count || (cap && !formats)) return XR_ERROR_VALIDATION_FAILURE;
    int64_t all[2 * QB_MAX_FORMATS];
    uint32_t n;
    QB_LOCK(s->inst);
    XrResult r = session_formats(s, all, &n);
    if (XR_FAILED(r)) return r;
    r = qb_count(cap, count, n);
    if (r == XR_SUCCESS && cap) memcpy(formats, all, n * sizeof(*all));
    return qb_session_result(s, r);
}

static void destroy_remote(struct qb_session *s, uint32_t id) {
    struct qb_swapchain_ref ref = {.swapchain_id = id};
    struct qb_status st;
    qb_ipc_call(&s->inst->ipc, QB_OP_DESTROY_SWAPCHAIN, &ref, sizeof(ref), NULL, 0, &st, sizeof(st), NULL, 0, NULL);
}

/* CREATE_SWAPCHAIN (array layers side by side, BGRA as RGBA), then qb_vulkan imports the dma-bufs and
 * owns them from then on, success or not. */
static XrResult create_remote(struct qb_session *s, const XrSwapchainCreateInfo *ci, struct qb_swapchain *sc) {
    struct qb_swapchain_create req = {.usage_flags = ci->usageFlags, .format = service_format(ci->format),
                                      .width = ci->width * ci->arraySize, .height = ci->height};
    struct qb_swapchain_info info;
    int fds[QB_MAX_SWAPCHAIN_IMAGES];
    uint32_t nfds = 0;
    XrResult r = qb_ipc_call(&s->inst->ipc, QB_OP_CREATE_SWAPCHAIN, &req, sizeof(req), NULL, 0, &info, sizeof(info),
                             fds, QB_MAX_SWAPCHAIN_IMAGES, &nfds);
    if (XR_FAILED(r)) return r;
    if (info.swapchain_id && nfds && info.image_count == nfds) {
        r = qb_vk_swapchain_import(s->vk, ci, &info, fds, nfds, &sc->vk);
        if (XR_SUCCEEDED(r)) {
            sc->id = info.swapchain_id;
            return XR_SUCCESS;
        }
    } else {
        for (uint32_t i = 0; i < nfds; i++) close(fds[i]);
        r = XR_ERROR_RUNTIME_FAILURE;
    }
    if (info.swapchain_id) destroy_remote(s, info.swapchain_id);
    return r;
}

XrResult qb_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *swapchain) {
    const XrSwapchainCreateFlags known = XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT | XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT;
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!ci || ci->type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !swapchain || (ci->createFlags & ~known) || !ci->width ||
        !ci->height || !ci->sampleCount || !ci->arraySize || !ci->mipCount || (ci->faceCount != 1 && ci->faceCount != 6) ||
        (uint64_t)ci->width * ci->arraySize > UINT32_MAX)
        return XR_ERROR_VALIDATION_FAILURE;
    /* nothing protected crosses the dma-buf import, and there is no cube layer to show a cubemap with */
    if ((ci->createFlags & XR_SWAPCHAIN_CREATE_PROTECTED_CONTENT_BIT) || ci->faceCount == 6)
        return XR_ERROR_FEATURE_UNSUPPORTED;
    int64_t all[2 * QB_MAX_FORMATS];
    uint32_t n;
    QB_LOCK(s->inst);
    XrResult r = session_formats(s, all, &n);
    if (XR_FAILED(r)) return r;
    if (!has_format(all, n, ci->format)) return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
    struct qb_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return XR_ERROR_OUT_OF_MEMORY;
    r = qb_vk_format_is_local(ci->format) ? qb_vk_swapchain_create_local(s->vk, ci, QB_LOCAL_IMAGES, &sc->vk)
                                          : create_remote(s, ci, sc);
    if (XR_FAILED(r)) {
        free(sc);
        return r;
    }
    sc->magic = QB_MAGIC_SWAPCHAIN;
    sc->sess = s;
    sc->next = s->swapchains;
    s->swapchains = sc;
    sc->image_count = qb_vk_swapchain_image_count(sc->vk);
    sc->width = ci->width;
    sc->height = ci->height;
    sc->array_size = ci->arraySize;
    sc->flags = ci->createFlags;
    sc->released = -1;
    if (!sc->image_count || sc->image_count > QB_MAX_SWAPCHAIN_IMAGES) {
        qb_swapchain_destroy(sc);
        return XR_ERROR_RUNTIME_FAILURE;
    }
    *swapchain = (XrSwapchain)sc;
    return qb_session_result(s, XR_SUCCESS);
}

void qb_swapchain_destroy(struct qb_swapchain *sc) {
    struct qb_session *s = sc->sess;
    for (struct qb_swapchain **p = &s->swapchains; *p; p = &(*p)->next)
        if (*p == sc) {
            *p = sc->next;
            break;
        }
    qb_vk_swapchain_destroy(s->vk, sc->vk); /* idles the queue before the service frees the buffers */
    if (sc->id) destroy_remote(s, sc->id);
    sc->magic = 0;
    free(sc);
}

XrResult qb_xrDestroySwapchain(XrSwapchain swapchain) {
    struct qb_swapchain *sc = qb_swapchain_get(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(sc->sess->inst);
    qb_swapchain_destroy(sc);
    return XR_SUCCESS;
}

XrResult qb_xrEnumerateSwapchainImages(XrSwapchain swapchain, uint32_t cap, uint32_t *count,
                                       XrSwapchainImageBaseHeader *images) {
    struct qb_swapchain *sc = qb_swapchain_get(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!count || (cap && !images)) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(sc->sess->inst);
    XrResult r = qb_count(cap, count, sc->image_count);
    if (r != XR_SUCCESS || !cap) return r;
    /* XrSwapchainImageVulkan2KHR is the same struct with the same type value */
    XrSwapchainImageVulkanKHR *out = (XrSwapchainImageVulkanKHR *)images;
    for (uint32_t i = 0; i < sc->image_count; i++) {
        if (out[i].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR) return XR_ERROR_VALIDATION_FAILURE;
        out[i].image = qb_vk_swapchain_app_image(sc->vk, i);
    }
    return XR_SUCCESS;
}

XrResult qb_xrAcquireSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageAcquireInfo *info, uint32_t *index) {
    struct qb_swapchain *sc = qb_swapchain_get(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if ((info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO) || !index) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_session *s = sc->sess;
    struct qb_instance *inst = s->inst;
    QB_LOCK(inst);
    if (sc->acquired_count + sc->acquiring >= sc->image_count ||
        ((sc->flags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) && sc->ever_acquired))
        return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t i;
    if (!sc->id) {
        /* guest-only images are released in acquire order, so round-robin never hands out a held one */
        i = sc->next_local++ % sc->image_count;
    } else {
        struct qb_swapchain_ref ref = {.swapchain_id = sc->id};
        struct qb_image_index reply;
        /* ACQUIRE_IMAGE blocks until the service has copied that image out; keep the instance usable */
        sc->acquiring++;
        pthread_mutex_unlock(&inst->lock);
        XrResult r = qb_ipc_call(&inst->ipc, QB_OP_ACQUIRE_IMAGE, &ref, sizeof(ref), NULL, 0, &reply, sizeof(reply),
                                 NULL, 0, NULL);
        pthread_mutex_lock(&inst->lock);
        sc->acquiring--;
        if (XR_FAILED(r)) return r;
        if (reply.index >= sc->image_count) return XR_ERROR_RUNTIME_FAILURE;
        i = reply.index;
    }
    sc->acquired[(sc->acquired_head + sc->acquired_count++) % QB_MAX_SWAPCHAIN_IMAGES] = i;
    sc->ever_acquired = 1;
    *index = i;
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrWaitSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageWaitInfo *info) {
    struct qb_swapchain *sc = qb_swapchain_get(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(sc->sess->inst);
    if (!sc->acquired_count || sc->waited) return XR_ERROR_CALL_ORDER_INVALID;
    sc->waited = 1; /* ACQUIRE_IMAGE only answers once the image is free, so there is nothing to wait for */
    return qb_session_result(sc->sess, XR_SUCCESS);
}

XrResult qb_xrReleaseSwapchainImage(XrSwapchain swapchain, const XrSwapchainImageReleaseInfo *info) {
    struct qb_swapchain *sc = qb_swapchain_get(swapchain);
    if (!sc) return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_session *s = sc->sess;
    QB_LOCK(s->inst);
    if (!sc->waited) return XR_ERROR_CALL_ORDER_INVALID;
    uint32_t i = sc->acquired[sc->acquired_head];
    sc->acquired_head = (sc->acquired_head + 1) % QB_MAX_SWAPCHAIN_IMAGES;
    sc->acquired_count--;
    sc->waited = 0;
    XrResult r = qb_vk_swapchain_release(s->vk, sc->vk, i);
    if (XR_FAILED(r)) return r;
    sc->released = i;
    return qb_session_result(s, XR_SUCCESS);
}

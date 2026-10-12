/*
 * questbridge_protocol.h - the wire between libopenxr_questbridge.so (the OpenXR runtime inside the
 * Linux guest, glibc) and the xr-bridge-service inside the DroidDeck app (bionic). Both sides include
 * this one header; nothing else about the wire is shared.
 *
 * Transport: one AF_UNIX SOCK_SEQPACKET connection per XrInstance, path in $DROIDDECK_XR_SOCKET. Every
 * message is one datagram: struct qb_hdr, then the opcode's body struct, then nothing. File
 * descriptors ride in one SCM_RIGHTS control message on the same datagram, in the order the body
 * documents. The guest sends requests; the service answers every request with exactly one reply
 * carrying the same seq and opcode. Replies that can fail start with a struct qb_status (an XrResult
 * mirror); a failed reply carries only the status. Both sides are aarch64 little-endian, so the
 * structs go on the wire as they are: fixed-width fields, explicit padding, no pointers.
 *
 * Who owns what. The service owns the one real OpenXR session with Meta's runtime, its frame loop,
 * its action set (the full Oculus Touch profile) and its reference spaces. The guest runtime presents
 * those to the app as its own. Frames are pipelined Monado-style: the guest's WAIT_FRAME returns when
 * the service's next real xrWaitFrame returns, END_FRAME hands over a layer list plus a release fence,
 * and the service submits the newest complete guest frame on its next real frame (one compositor
 * frame of added latency; measured, see docs/vr.md). While no guest frame is in flight the service
 * shows the Steam panel quad on its own.
 *
 * Swapchain images are gralloc AHardwareBuffers the service allocates (GPU_COLOR_OUTPUT |
 * GPU_SAMPLED_IMAGE). Each image's dma-buf fd (native_handle->data[0]) crosses the socket once, with
 * the pitch and DRM modifier sniffed from the gralloc handle the way sc_layer.c and Mesa's
 * u_gralloc_fallback.c do; the guest's Turnip imports it with VK_EXT_external_memory_dma_buf +
 * VK_EXT_image_drm_format_modifier (explicit layout). Rendering completion crosses as a sync_file fd on
 * END_FRAME; the service copies the referenced images into the runtime's own swapchains and an image
 * index is handed back by ACQUIRE_IMAGE only once that copy has finished.
 */
#ifndef QUESTBRIDGE_PROTOCOL_H
#define QUESTBRIDGE_PROTOCOL_H

#include <stdint.h>

#define QB_PROTOCOL_VERSION 1u
#define QB_SOCKET_ENV "DROIDDECK_XR_SOCKET"
/* The socket lives under the app's files dir, which the guest sees at the same path through proot. */
#define QB_SOCKET_NAME "xr-bridge.sock"

#define QB_MAX_VIEWS 2u
#define QB_MAX_SWAPCHAIN_IMAGES 4u
#define QB_MAX_FORMATS 16u
#define QB_MAX_REFRESH_RATES 8u
#define QB_MAX_LAYERS 8u
#define QB_NAME_LEN 128u

enum qb_op {
    QB_OP_HELLO = 1,          /* qb_hello            -> qb_hello_reply */
    QB_OP_CREATE_SESSION,     /* (none)              -> qb_status; the guest app's xrCreateSession */
    QB_OP_BEGIN_SESSION,      /* (none)              -> qb_status; guest frames start being submitted */
    QB_OP_END_SESSION,        /* (none)              -> qb_status; the panel comes back */
    QB_OP_DESTROY_SESSION,    /* (none)              -> qb_status */
    QB_OP_REQUEST_EXIT,       /* (none)              -> qb_status; service answers with EXITING states */
    QB_OP_POLL_EVENT,         /* (none)              -> qb_event (QB_EVENT_NONE when the queue is empty) */
    QB_OP_ENUM_FORMATS,       /* (none)              -> qb_formats */
    QB_OP_CREATE_SWAPCHAIN,   /* qb_swapchain_create -> qb_swapchain_info + image_count dma-buf fds */
    QB_OP_DESTROY_SWAPCHAIN,  /* qb_swapchain_ref    -> qb_status */
    QB_OP_ACQUIRE_IMAGE,      /* qb_swapchain_ref    -> qb_image_index; blocks until an image is free */
    QB_OP_WAIT_FRAME,         /* (none)              -> qb_frame_state; blocks until the next real frame */
    QB_OP_BEGIN_FRAME,        /* (none)              -> qb_status */
    QB_OP_END_FRAME,          /* qb_end_frame + 0 or 1 sync_file fd -> qb_status */
    QB_OP_LOCATE_VIEWS,       /* qb_locate_views     -> qb_views */
    QB_OP_LOCATE_SPACE,       /* qb_locate_space     -> qb_space_location */
    QB_OP_SYNC_ACTIONS,       /* (none)              -> qb_input_state; the newest snapshot */
    QB_OP_APPLY_HAPTIC,       /* qb_haptic           -> qb_status */
    QB_OP_SET_REFRESH_RATE,   /* qb_refresh_rate     -> qb_status */
    QB_OP_STAGE_BOUNDS,       /* (none)              -> qb_stage_bounds */
};

struct qb_hdr {
    uint32_t op;        /* enum qb_op */
    uint32_t seq;       /* request counter; the reply repeats it */
    uint32_t body_len;  /* bytes after this header */
    uint32_t fd_count;  /* fds in SCM_RIGHTS on this datagram */
};

/* XrResult mirror: 0 = XR_SUCCESS, negative = the XrResult error the guest returns to the app. */
struct qb_status {
    int32_t xr_result;
    uint32_t pad;
};

struct qb_hello {
    uint32_t protocol_version;
    uint32_t guest_pid;
    char app_name[QB_NAME_LEN];     /* XrApplicationInfo.applicationName, for the service log */
};

struct qb_view_config {
    uint32_t recommended_width, recommended_height;
    uint32_t max_width, max_height;
    uint32_t recommended_samples, max_samples;
};

/* The reference and action spaces the service locates. A guest XrSpace is one of these plus a pose
 * offset the guest composes itself (xrCreateReferenceSpace / xrCreateActionSpace poseInSpace). */
enum qb_space {
    QB_SPACE_VIEW = 0,
    QB_SPACE_LOCAL,
    QB_SPACE_STAGE,
    QB_SPACE_LEFT_GRIP,
    QB_SPACE_LEFT_AIM,
    QB_SPACE_RIGHT_GRIP,
    QB_SPACE_RIGHT_AIM,
    QB_SPACE_COUNT
};

struct qb_hello_reply {
    struct qb_status status;
    uint32_t protocol_version;
    uint32_t vendor_id;
    char system_name[QB_NAME_LEN];
    char runtime_name[QB_NAME_LEN];     /* Meta's XrInstanceProperties.runtimeName */
    uint64_t runtime_version;           /* XrVersion */
    uint32_t max_swapchain_width, max_swapchain_height, max_layer_count;
    uint32_t orientation_tracking, position_tracking;   /* XrSystemTrackingProperties */
    uint32_t view_count;                                /* 2 */
    struct qb_view_config views[QB_MAX_VIEWS];
    uint32_t space_mask;                                /* bit (1 << qb_space) per supported space */
    uint32_t refresh_rate_count;
    float refresh_rates[QB_MAX_REFRESH_RATES];
    float current_refresh_rate;                         /* 0 when XR_FB_display_refresh_rate is absent */
    uint32_t pad;
};

struct qb_formats {
    struct qb_status status;
    uint32_t count;
    uint32_t pad;
    int64_t formats[QB_MAX_FORMATS];    /* VkFormat values the service can back with gralloc buffers */
};

/* Mirrors XrSwapchainCreateInfo. array_size, mip_count and sample_count other than 1 are the guest's
 * business: it gives the app a guest-owned image and blits/resolves into the imported single-layer
 * images on release. The service only ever allocates 2D single-sample single-mip buffers. */
struct qb_swapchain_create {
    uint64_t usage_flags;               /* XrSwapchainUsageFlags, informational */
    int64_t format;                     /* VkFormat, one of qb_formats */
    uint32_t width, height;
};

struct qb_swapchain_info {
    struct qb_status status;
    uint32_t swapchain_id;              /* service-chosen, non-zero */
    uint32_t image_count;               /* fds on this datagram, image i = fd i; plane 0 of each buffer */
    uint32_t stride_bytes;              /* row pitch of plane 0 */
    uint32_t pad;
    uint64_t modifier;                  /* DRM_FORMAT_MOD_LINEAR or DRM_FORMAT_MOD_QCOM_COMPRESSED */
    uint64_t size_bytes;                /* dma-buf size (lseek SEEK_END), for the import allocation */
};

struct qb_swapchain_ref {
    uint32_t swapchain_id;
    uint32_t pad;
};

struct qb_image_index {
    struct qb_status status;
    uint32_t index;
    uint32_t pad;
};

struct qb_frame_state {
    struct qb_status status;
    int64_t predicted_display_time;     /* XrTime, Meta's clock (CLOCK_MONOTONIC ns on Android) */
    int64_t predicted_display_period;
    uint32_t should_render;
    uint32_t pad;
};

struct qb_posef {
    float qx, qy, qz, qw;
    float x, y, z;
    float pad;
};

struct qb_fovf {
    float angle_left, angle_right, angle_up, angle_down;
};

struct qb_locate_views {
    uint32_t base_space;                /* enum qb_space */
    uint32_t pad;
    int64_t time;
};

struct qb_views {
    struct qb_status status;
    uint64_t view_state_flags;          /* XrViewStateFlags */
    uint32_t count;
    uint32_t pad;
    struct qb_posef poses[QB_MAX_VIEWS];
    struct qb_fovf fovs[QB_MAX_VIEWS];
};

struct qb_locate_space {
    uint32_t space;                     /* enum qb_space */
    uint32_t base_space;                /* enum qb_space */
    int64_t time;
};

struct qb_space_location {
    struct qb_status status;
    uint64_t location_flags;            /* XrSpaceLocationFlags */
    struct qb_posef pose;
};

enum qb_layer_type {
    QB_LAYER_PROJECTION = 1,
    QB_LAYER_QUAD = 2,
};

struct qb_sub_image {
    uint32_t swapchain_id;
    uint32_t image_index;               /* the index the guest last released for this swapchain */
    int32_t x, y;
    int32_t width, height;
    uint32_t array_index;               /* always 0 on the wire; arrays are flattened guest-side */
    uint32_t pad;
};

struct qb_projection_view {
    struct qb_posef pose;
    struct qb_fovf fov;
    struct qb_sub_image sub_image;
};

struct qb_layer {
    uint32_t type;                      /* enum qb_layer_type */
    uint32_t layer_flags;               /* XrCompositionLayerFlags */
    uint32_t space;                     /* enum qb_space */
    uint32_t view_count;                /* projection: 2; quad: 0 */
    struct qb_posef space_offset;       /* the guest XrSpace's pose offset within `space` */
    struct qb_projection_view views[QB_MAX_VIEWS];
    /* quad only */
    struct qb_sub_image quad_image;
    struct qb_posef quad_pose;          /* in the layer's space (offset already applied) */
    float quad_width, quad_height;
    uint32_t eye_visibility;            /* XrEyeVisibility */
    uint32_t pad;
};

/* fd 0 (when fd_count == 1): a sync_file that signals when every referenced image is rendered. Without
 * it the guest has already waited for its own fence and the service may read the images at once. */
struct qb_end_frame {
    int64_t display_time;
    uint32_t environment_blend_mode;    /* XrEnvironmentBlendMode */
    uint32_t layer_count;
    struct qb_layer layers[QB_MAX_LAYERS];
};

enum qb_button {
    QB_BTN_PRIMARY_CLICK = 1u << 0,     /* A (right) / X (left) */
    QB_BTN_PRIMARY_TOUCH = 1u << 1,
    QB_BTN_SECONDARY_CLICK = 1u << 2,   /* B (right) / Y (left) */
    QB_BTN_SECONDARY_TOUCH = 1u << 3,
    QB_BTN_MENU_CLICK = 1u << 4,        /* left hand only on Touch */
    QB_BTN_THUMBSTICK_CLICK = 1u << 5,
    QB_BTN_THUMBSTICK_TOUCH = 1u << 6,
    QB_BTN_TRIGGER_TOUCH = 1u << 7,
    QB_BTN_THUMBREST_TOUCH = 1u << 8,
};

struct qb_hand_state {
    uint32_t active;                    /* the controller is connected and tracked this frame */
    uint32_t buttons;                   /* enum qb_button bits */
    float trigger, squeeze;             /* 0..1 */
    float thumbstick_x, thumbstick_y;   /* -1..1 */
};

struct qb_input_state {
    struct qb_status status;
    int64_t time;                       /* when the snapshot was taken */
    struct qb_hand_state hands[2];      /* 0 = left, 1 = right */
};

struct qb_haptic {
    uint32_t hand;                      /* 0 = left, 1 = right */
    uint32_t pad;
    int64_t duration_ns;                /* XR_MIN_HAPTIC_DURATION (-1) = runtime default */
    float frequency;                    /* 0 = XR_FREQUENCY_UNSPECIFIED */
    float amplitude;                    /* 0..1 */
};

struct qb_refresh_rate {
    float rate;
    uint32_t pad;
};

struct qb_stage_bounds {
    struct qb_status status;
    float width, depth;                 /* XrExtent2Df of the STAGE space; 0,0 when unknown */
};

enum qb_event_type {
    QB_EVENT_NONE = 0,
    QB_EVENT_SESSION_STATE,             /* a = XrSessionState */
    QB_EVENT_REFERENCE_SPACE_CHANGE_PENDING, /* a = enum qb_space, pose = poseInPreviousSpace */
    QB_EVENT_INTERACTION_PROFILE_CHANGED,
    QB_EVENT_REFRESH_RATE_CHANGED,      /* f = the new rate */
    QB_EVENT_INSTANCE_LOSS_PENDING,     /* time = lossTime */
};

struct qb_event {
    struct qb_status status;
    uint32_t type;                      /* enum qb_event_type */
    uint32_t a;
    int64_t time;
    float f;
    uint32_t pad;
    struct qb_posef pose;
};

#endif

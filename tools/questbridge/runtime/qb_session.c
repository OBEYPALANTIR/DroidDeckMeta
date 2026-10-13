/* qb_session.c - XrSession, its spaces and its frame loop. The guest queues IDLE itself (creation and
 * xrEndSession); every other state comes from the service's SESSION_STATE events. Spaces are a service
 * space plus a pose offset composed here. Frames follow the spec's pacing: a waited frame must begin
 * before the next xrWaitFrame returns, and END_FRAME carries the layer list plus the render fence. */
#define _GNU_SOURCE
#include "qb_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const XrPosef identity = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};

void qb_session_set_state(struct qb_session *s, XrSessionState state, XrTime time) {
    if (state == s->state) return;
    s->state = state;
    union qb_xr_event e;
    memset(&e, 0, sizeof(e));
    e.state.type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e.state.session = (XrSession)s;
    e.state.state = state;
    e.state.time = time > 0 ? time : qb_now();
    qb_event_push(s->inst, &e);
}

/* A request without a body, answered by a bare qb_status. */
static XrResult call(struct qb_session *s, uint32_t op) {
    struct qb_status st;
    return qb_ipc_call(&s->inst->ipc, op, NULL, 0, NULL, 0, &st, sizeof(st), NULL, 0, NULL);
}

XrResult qb_xrCreateSession(XrInstance instance, const XrSessionCreateInfo *info, XrSession *session) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SESSION_CREATE_INFO || info->createFlags || !session)
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->systemId != QB_SYSTEM_ID) return XR_ERROR_SYSTEM_INVALID;
    const XrGraphicsBindingVulkanKHR *binding = NULL;
    for (const XrBaseInStructure *n = info->next; n; n = n->next)
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) binding = (const XrGraphicsBindingVulkanKHR *)n;
    if (!binding || !(inst->exts & (QB_EXT_VULKAN | QB_EXT_VULKAN2))) return XR_ERROR_GRAPHICS_DEVICE_INVALID;

    QB_LOCK(inst);
    if (!inst->gfx_requirements) return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
    if (inst->session) return XR_ERROR_LIMIT_REACHED; /* the service runs one session per connection */
    struct qb_session *s = calloc(1, sizeof(*s));
    if (!s) return XR_ERROR_OUT_OF_MEMORY;
    s->inst = inst;
    s->profile = -1;
    XrResult r = qb_vk_bind(binding, &s->vk);
    if (XR_SUCCEEDED(r)) {
        r = call(s, QB_OP_CREATE_SESSION);
        if (XR_FAILED(r)) qb_vk_unbind(s->vk);
    }
    if (XR_FAILED(r)) {
        free(s);
        return r;
    }
    pthread_cond_init(&s->frame_cond, NULL);
    s->magic = QB_MAGIC_SESSION;
    inst->session = s;
    qb_session_set_state(s, XR_SESSION_STATE_IDLE, 0);
    *session = (XrSession)s;
    return XR_SUCCESS;
}

void qb_session_destroy(struct qb_session *s) {
    struct qb_instance *inst = s->inst;
    while (s->swapchains) qb_swapchain_destroy(s->swapchains);
    while (s->spaces) {
        struct qb_space_handle *sp = s->spaces;
        s->spaces = sp->next;
        sp->magic = 0;
        free(sp);
    }
    qb_input_detach(s);
    call(s, QB_OP_DESTROY_SESSION);
    qb_vk_unbind(s->vk);
    qb_event_drop_session(inst, (XrSession)s);
    inst->session = NULL;
    s->magic = 0;
    pthread_cond_destroy(&s->frame_cond);
    free(s);
}

XrResult qb_xrDestroySession(XrSession session) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    qb_session_destroy(s);
    return XR_SUCCESS;
}

XrResult qb_xrBeginSession(XrSession session, const XrSessionBeginInfo *info) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SESSION_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    if (info->primaryViewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    QB_LOCK(s->inst);
    if (s->running) return XR_ERROR_SESSION_RUNNING;
    if (s->state != XR_SESSION_STATE_READY) return XR_ERROR_SESSION_NOT_READY;
    XrResult r = call(s, QB_OP_BEGIN_SESSION);
    if (XR_FAILED(r)) return r;
    s->running = 1;
    s->frame_waited = s->frame_begun = 0;
    return XR_SUCCESS;
}

XrResult qb_xrEndSession(XrSession session) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (s->state != XR_SESSION_STATE_STOPPING && s->state != XR_SESSION_STATE_LOSS_PENDING)
        return XR_ERROR_SESSION_NOT_STOPPING;
    XrResult r = call(s, QB_OP_END_SESSION);
    if (XR_FAILED(r)) return r;
    s->running = 0;
    s->frame_waited = s->frame_begun = 0;
    pthread_cond_broadcast(&s->frame_cond); /* a parked xrWaitFrame now fails with SESSION_NOT_RUNNING */
    if (s->state == XR_SESSION_STATE_STOPPING) qb_session_set_state(s, XR_SESSION_STATE_IDLE, 0);
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrRequestExitSession(XrSession session) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    XrResult r = call(s, QB_OP_REQUEST_EXIT); /* STOPPING and later EXITING arrive as events */
    return XR_FAILED(r) ? r : qb_session_result(s, XR_SUCCESS);
}

/* ---- spaces */

static int ref_space(XrReferenceSpaceType type) {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
        return QB_SPACE_VIEW;
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return QB_SPACE_LOCAL;
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return QB_SPACE_STAGE;
    default:
        return -1;
    }
}

static int ref_supported(const struct qb_session *s, XrReferenceSpaceType type) {
    int q = ref_space(type);
    return q >= 0 && (s->inst->sys.space_mask & (1u << q));
}

/* The service space `sp` is located through; 0 while it cannot be (an inactive or unbound action). */
static int space_base(const struct qb_space_handle *sp, uint32_t *space) {
    if (!sp->action) {
        *space = sp->ref;
        return 1;
    }
    return qb_input_pose_space(sp->sess, sp->action, sp->user, space);
}

static XrResult new_space(struct qb_session *s, uint32_t ref, struct qb_action *action, int user, const XrPosef *offset,
                          XrSpace *out) {
    struct qb_space_handle *sp = malloc(sizeof(*sp));
    if (!sp) return XR_ERROR_OUT_OF_MEMORY;
    *sp = (struct qb_space_handle){.magic = QB_MAGIC_SPACE, .sess = s, .next = s->spaces, .ref = ref,
                                   .action = action, .user = user, .offset = *offset};
    s->spaces = sp;
    *out = (XrSpace)sp;
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrEnumerateReferenceSpaces(XrSession session, uint32_t cap, uint32_t *count, XrReferenceSpaceType *spaces) {
    static const XrReferenceSpaceType types[] = {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
                                                 XR_REFERENCE_SPACE_TYPE_STAGE};
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!count || (cap && !spaces)) return XR_ERROR_VALIDATION_FAILURE;
    XrReferenceSpaceType list[3];
    uint32_t n = 0;
    for (uint32_t i = 0; i < 3; i++)
        if (ref_supported(s, types[i])) list[n++] = types[i];
    QB_LOCK(s->inst);
    XrResult r = qb_count(cap, count, n);
    if (r == XR_SUCCESS && cap) memcpy(spaces, list, n * sizeof(*list));
    return qb_session_result(s, r);
}

XrResult qb_xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *info, XrSpace *space) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_REFERENCE_SPACE_CREATE_INFO || !space) return XR_ERROR_VALIDATION_FAILURE;
    if (!ref_supported(s, info->referenceSpaceType)) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    if (!qb_pose_valid(&info->poseInReferenceSpace)) return XR_ERROR_POSE_INVALID;
    QB_LOCK(s->inst);
    return new_space(s, (uint32_t)ref_space(info->referenceSpaceType), NULL, -1, &info->poseInReferenceSpace, space);
}

XrResult qb_xrGetReferenceSpaceBoundsRect(XrSession session, XrReferenceSpaceType type, XrExtent2Df *bounds) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!bounds) return XR_ERROR_VALIDATION_FAILURE;
    if (!ref_supported(s, type)) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    bounds->width = bounds->height = 0.0f;
    QB_LOCK(s->inst);
    if (ref_space(type) != QB_SPACE_STAGE) return XR_SPACE_BOUNDS_UNAVAILABLE;
    struct qb_stage_bounds sb;
    XrResult r = qb_ipc_call(&s->inst->ipc, QB_OP_STAGE_BOUNDS, NULL, 0, NULL, 0, &sb, sizeof(sb), NULL, 0, NULL);
    if (XR_FAILED(r)) return r;
    if (!(sb.width > 0.0f && sb.depth > 0.0f)) return XR_SPACE_BOUNDS_UNAVAILABLE;
    bounds->width = sb.width;
    bounds->height = sb.depth;
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *info, XrSpace *space) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_ACTION_SPACE_CREATE_INFO || !space) return XR_ERROR_VALIDATION_FAILURE;
    struct qb_action *a = qb_action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    if (a->set->inst != s->inst) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!qb_pose_valid(&info->poseInActionSpace)) return XR_ERROR_POSE_INVALID;
    QB_LOCK(s->inst);
    int user;
    XrResult r = qb_input_subaction(a, info->subactionPath, &user);
    if (r != XR_SUCCESS) return r;
    /* resolved at every locate: the bindings are only known once the action set is attached */
    return new_space(s, 0, a, user, &info->poseInActionSpace, space);
}

XrResult qb_xrDestroySpace(XrSpace space) {
    struct qb_space_handle *sp = qb_space_handle_get(space);
    if (!sp) return XR_ERROR_HANDLE_INVALID;
    struct qb_session *s = sp->sess;
    QB_LOCK(s->inst);
    for (struct qb_space_handle **p = &s->spaces; *p; p = &(*p)->next)
        if (*p == sp) {
            *p = sp->next;
            break;
        }
    sp->magic = 0;
    free(sp);
    return XR_SUCCESS;
}

/* Lock held: `sp` in `base`, as inverse(base offset) * LOCATE_SPACE * (sp offset); flags 0 while either
 * cannot be located. Velocities go through the same composition: the offsets are rigid transforms, so
 * each wire vector is rotated by the inverse of the base offset's orientation (magnitudes unchanged);
 * vf/linear/angular are untouched when the app chained no velocity (NULL). */
static XrResult locate(const struct qb_space_handle *sp, const struct qb_space_handle *base, XrTime time,
                       XrSpaceLocationFlags *flags, XrPosef *pose, XrSpaceVelocityFlags *vf, XrVector3f *linear,
                       XrVector3f *angular) {
    *flags = 0;
    *pose = identity;
    if (vf) *vf = 0;
    if (linear) *linear = (XrVector3f){0.0f, 0.0f, 0.0f};
    if (angular) *angular = (XrVector3f){0.0f, 0.0f, 0.0f};
    struct qb_locate_space req = {.time = time};
    if (!space_base(sp, &req.space) || !space_base(base, &req.base_space)) return XR_SUCCESS;
    struct qb_space_location loc;
    XrResult r = qb_ipc_call(&sp->sess->inst->ipc, QB_OP_LOCATE_SPACE, &req, sizeof(req), NULL, 0, &loc, sizeof(loc),
                             NULL, 0, NULL);
    if (XR_FAILED(r)) return r;
    *flags = loc.location_flags;
    *pose = qb_pose_mul(qb_pose_mul(qb_pose_inv(base->offset), qb_pose_from_wire(&loc.pose)), sp->offset);
    if (vf) {
        *vf = loc.velocity_flags;
        if (loc.velocity_flags && linear && angular) {
            /* inverse(base offset) as a rotation: conjugate of a unit quaternion */
            XrQuaternionf inv = {-base->offset.orientation.x, -base->offset.orientation.y,
                                 -base->offset.orientation.z, base->offset.orientation.w};
            *linear = qb_quat_rotate(inv, (XrVector3f){loc.linear[0], loc.linear[1], loc.linear[2]});
            *angular = qb_quat_rotate(inv, (XrVector3f){loc.angular[0], loc.angular[1], loc.angular[2]});
        }
    }
    return XR_SUCCESS;
}

XrResult qb_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation *location) {
    struct qb_space_handle *sp = qb_space_handle_get(space), *base = qb_space_handle_get(baseSpace);
    if (!sp || !base) return XR_ERROR_HANDLE_INVALID;
    if (!location || location->type != XR_TYPE_SPACE_LOCATION || sp->sess != base->sess)
        return XR_ERROR_VALIDATION_FAILURE;
    if (time <= 0) return XR_ERROR_TIME_INVALID;
    XrSpaceVelocity *velocity = NULL;
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)location->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITY) velocity = (XrSpaceVelocity *)n;
    struct qb_session *s = sp->sess;
    QB_LOCK(s->inst);
    XrResult r;
    if (velocity) {
        velocity->velocityFlags = 0;
        velocity->linearVelocity = (XrVector3f){0.0f, 0.0f, 0.0f};
        velocity->angularVelocity = (XrVector3f){0.0f, 0.0f, 0.0f};
        r = locate(sp, base, time, &location->locationFlags, &location->pose, &velocity->velocityFlags,
                   &velocity->linearVelocity, &velocity->angularVelocity);
    } else {
        r = locate(sp, base, time, &location->locationFlags, &location->pose, NULL, NULL, NULL);
    }
    return XR_FAILED(r) ? r : qb_session_result(s, XR_SUCCESS);
}

/* OpenXR 1.1 core: xrLocateSpace for a batch (the loader calls through a NULL slot without it). */
XrResult qb_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo *info, XrSpaceLocations *locations) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SPACES_LOCATE_INFO || !info->spaceCount || !info->spaces || !locations ||
        locations->type != XR_TYPE_SPACE_LOCATIONS || locations->locationCount != info->spaceCount ||
        !locations->locations)
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->time <= 0) return XR_ERROR_TIME_INVALID;
    const struct qb_space_handle *base = qb_space_handle_get(info->baseSpace);
    if (!base) return XR_ERROR_HANDLE_INVALID;
    if (base->sess != s) return XR_ERROR_VALIDATION_FAILURE;
    XrSpaceVelocities *velocities = NULL;
    for (XrBaseOutStructure *n = locations->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITIES) velocities = (XrSpaceVelocities *)n;
    if (velocities && (velocities->velocityCount != info->spaceCount || !velocities->velocities))
        return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    for (uint32_t i = 0; i < info->spaceCount; i++) {
        const struct qb_space_handle *sp = qb_space_handle_get(info->spaces[i]);
        if (!sp) return XR_ERROR_HANDLE_INVALID;
        if (sp->sess != s) return XR_ERROR_VALIDATION_FAILURE;
        XrSpaceVelocityData *v = velocities ? &velocities->velocities[i] : NULL;
        XrResult r;
        if (v) {
            v->velocityFlags = 0;
            v->linearVelocity = (XrVector3f){0.0f, 0.0f, 0.0f};
            v->angularVelocity = (XrVector3f){0.0f, 0.0f, 0.0f};
            r = locate(sp, base, info->time, &locations->locations[i].locationFlags,
                       &locations->locations[i].pose, &v->velocityFlags, &v->linearVelocity,
                       &v->angularVelocity);
        } else {
            r = locate(sp, base, info->time, &locations->locations[i].locationFlags,
                       &locations->locations[i].pose, NULL, NULL, NULL);
        }
        if (XR_FAILED(r)) return r;
    }
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrLocateViews(XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t cap,
                          uint32_t *count, XrView *views) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_VIEW_LOCATE_INFO || !state || state->type != XR_TYPE_VIEW_STATE || !count ||
        (cap && !views))
        return XR_ERROR_VALIDATION_FAILURE;
    if (info->viewConfigurationType != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO)
        return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    if (info->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    struct qb_space_handle *sp = qb_space_handle_get(info->space);
    if (!sp) return XR_ERROR_HANDLE_INVALID;
    if (sp->sess != s) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    XrResult r = qb_count(cap, count, QB_MAX_VIEWS);
    if (r != XR_SUCCESS || !cap) return r;
    for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
        if (views[i].type != XR_TYPE_VIEW) return XR_ERROR_VALIDATION_FAILURE;
        views[i].pose = identity;
        views[i].fov = (XrFovf){0.0f, 0.0f, 0.0f, 0.0f};
    }
    state->viewStateFlags = 0;
    struct qb_locate_views req = {.time = info->displayTime};
    if (!space_base(sp, &req.base_space)) return qb_session_result(s, XR_SUCCESS);
    struct qb_views v;
    r = qb_ipc_call(&s->inst->ipc, QB_OP_LOCATE_VIEWS, &req, sizeof(req), NULL, 0, &v, sizeof(v), NULL, 0, NULL);
    if (XR_FAILED(r)) return r;
    if (v.count != QB_MAX_VIEWS) return XR_ERROR_RUNTIME_FAILURE;
    XrPosef to_app = qb_pose_inv(sp->offset); /* the service answers in `base_space`, the app asked in `space` */
    for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
        views[i].pose = qb_pose_mul(to_app, qb_pose_from_wire(&v.poses[i]));
        views[i].fov = (XrFovf){v.fovs[i].angle_left, v.fovs[i].angle_right, v.fovs[i].angle_up, v.fovs[i].angle_down};
    }
    state->viewStateFlags = v.view_state_flags;
    return qb_session_result(s, XR_SUCCESS);
}

/* ---- frames */

XrResult qb_xrWaitFrame(XrSession session, const XrFrameWaitInfo *info, XrFrameState *state) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if ((info && info->type != XR_TYPE_FRAME_WAIT_INFO) || !state || state->type != XR_TYPE_FRAME_STATE)
        return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = s->inst;
    struct qb_frame_state fs;
    QB_LOCK(inst);
    while (s->running && (s->frame_waiting || s->frame_waited)) pthread_cond_wait(&s->frame_cond, &inst->lock);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    s->frame_waiting = 1;
    /* WAIT_FRAME blocks until the service's next real frame; the render thread keeps the instance meanwhile
     * (but queues behind this request for the socket). */
    pthread_mutex_unlock(&inst->lock);
    XrResult r = qb_ipc_call(&inst->ipc, QB_OP_WAIT_FRAME, NULL, 0, NULL, 0, &fs, sizeof(fs), NULL, 0, NULL);
    pthread_mutex_lock(&inst->lock);
    s->frame_waiting = 0;
    pthread_cond_broadcast(&s->frame_cond);
    if (XR_FAILED(r)) return r;
    s->frame_waited = 1;
    state->predictedDisplayTime = fs.predicted_display_time;
    state->predictedDisplayPeriod = fs.predicted_display_period;
    state->shouldRender = fs.should_render ? XR_TRUE : XR_FALSE;
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrBeginFrame(XrSession session, const XrFrameBeginInfo *info) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (info && info->type != XR_TYPE_FRAME_BEGIN_INFO) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (!s->frame_waited) return XR_ERROR_CALL_ORDER_INVALID;
    XrResult r = call(s, QB_OP_BEGIN_FRAME);
    /* the waited frame is spent either way; a failed one must not park the next xrWaitFrame forever */
    s->frame_waited = 0;
    pthread_cond_broadcast(&s->frame_cond);
    if (XR_FAILED(r)) return r;
    r = s->frame_begun ? XR_FRAME_DISCARDED : XR_SUCCESS; /* the previous frame never reached xrEndFrame */
    s->frame_begun = 1;
    return qb_session_result(s, r);
}

static XrResult pack_sub_image(const struct qb_session *s, const XrSwapchainSubImage *si, struct qb_sub_image *out) {
    const struct qb_swapchain *sc = qb_swapchain_get(si->swapchain);
    if (!sc || sc->sess != s) return XR_ERROR_HANDLE_INVALID;
    /* guest-only swapchains (depth) never reach the service, and a layer needs a released image */
    if (!sc->id || sc->released < 0) return XR_ERROR_LAYER_INVALID;
    if (si->imageArrayIndex >= sc->array_size) return XR_ERROR_VALIDATION_FAILURE;
    const XrRect2Di *rc = &si->imageRect;
    if (rc->offset.x < 0 || rc->offset.y < 0 || rc->extent.width <= 0 || rc->extent.height <= 0 ||
        (int64_t)rc->offset.x + rc->extent.width > (int64_t)sc->width ||
        (int64_t)rc->offset.y + rc->extent.height > (int64_t)sc->height)
        return XR_ERROR_SWAPCHAIN_RECT_INVALID;
    /* array layers sit side by side in the service's buffer, which is width * arraySize wide */
    *out = (struct qb_sub_image){.swapchain_id = sc->id, .image_index = (uint32_t)sc->released,
                                 .x = rc->offset.x + (int32_t)(si->imageArrayIndex * sc->width), .y = rc->offset.y,
                                 .width = rc->extent.width, .height = rc->extent.height};
    return XR_SUCCESS;
}

/* One app layer as the wire's qb_layer. *keep = 0 for a layer in an action space that cannot be located
 * this frame: Meta's compositor could only show it in the wrong place. */
static XrResult pack_layer(const struct qb_session *s, const XrCompositionLayerBaseHeader *l, struct qb_layer *out,
                           int *keep) {
    *keep = 0;
    if (!l || (l->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION && l->type != XR_TYPE_COMPOSITION_LAYER_QUAD))
        return XR_ERROR_LAYER_INVALID;
    const struct qb_space_handle *sp = qb_space_handle_get(l->space);
    if (!sp || sp->sess != s) return XR_ERROR_HANDLE_INVALID;
    memset(out, 0, sizeof(*out));
    out->layer_flags = (uint32_t)l->layerFlags;
    out->space_offset = qb_pose_to_wire(sp->offset);
    XrResult r;
    if (l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        const XrCompositionLayerProjection *p = (const XrCompositionLayerProjection *)l;
        if (p->viewCount != QB_MAX_VIEWS || !p->views) return XR_ERROR_VALIDATION_FAILURE;
        out->type = QB_LAYER_PROJECTION;
        out->view_count = QB_MAX_VIEWS;
        for (uint32_t i = 0; i < QB_MAX_VIEWS; i++) {
            const XrCompositionLayerProjectionView *v = &p->views[i];
            if (v->type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW) return XR_ERROR_VALIDATION_FAILURE;
            if (!qb_pose_valid(&v->pose)) return XR_ERROR_POSE_INVALID;
            if ((r = pack_sub_image(s, &v->subImage, &out->views[i].sub_image)) != XR_SUCCESS) return r;
            out->views[i].pose = qb_pose_to_wire(v->pose);
            out->views[i].fov = (struct qb_fovf){v->fov.angleLeft, v->fov.angleRight, v->fov.angleUp, v->fov.angleDown};
        }
    } else {
        const XrCompositionLayerQuad *q = (const XrCompositionLayerQuad *)l;
        if ((uint32_t)q->eyeVisibility > XR_EYE_VISIBILITY_RIGHT) return XR_ERROR_VALIDATION_FAILURE;
        if (!qb_pose_valid(&q->pose)) return XR_ERROR_POSE_INVALID;
        if ((r = pack_sub_image(s, &q->subImage, &out->quad_image)) != XR_SUCCESS) return r;
        out->type = QB_LAYER_QUAD;
        out->quad_pose = qb_pose_to_wire(qb_pose_mul(sp->offset, q->pose));
        out->quad_width = q->size.width;
        out->quad_height = q->size.height;
        out->eye_visibility = (uint32_t)q->eyeVisibility;
    }
    *keep = space_base(sp, &out->space);
    return XR_SUCCESS;
}

XrResult qb_xrEndFrame(XrSession session, const XrFrameEndInfo *info) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_FRAME_END_INFO || (info->layerCount && !info->layers))
        return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = s->inst;
    struct qb_end_frame ef;
    memset(&ef, 0, sizeof(ef));
    QB_LOCK(inst);
    if (!s->running) return XR_ERROR_SESSION_NOT_RUNNING;
    if (!s->frame_begun) return XR_ERROR_CALL_ORDER_INVALID;
    if (info->displayTime <= 0) return XR_ERROR_TIME_INVALID;
    if (info->environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE) return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
    if (info->layerCount > qb_max_layers(inst)) return XR_ERROR_LAYER_LIMIT_EXCEEDED;
    /* a rejected layer list leaves the frame begun, so the app may still end it */
    for (uint32_t i = 0; i < info->layerCount; i++) {
        int keep;
        XrResult r = pack_layer(s, info->layers[i], &ef.layers[ef.layer_count], &keep);
        if (r != XR_SUCCESS) return r;
        ef.layer_count += (uint32_t)keep;
    }
    ef.display_time = info->displayTime;
    ef.environment_blend_mode = (uint32_t)info->environmentBlendMode;
    /* the service reads the images once this signals; -1: qb_vulkan already waited for the GPU */
    int fence = ef.layer_count ? qb_vk_end_frame_fence(s->vk) : -1;
    struct qb_status st;
    XrResult r = qb_ipc_call(&inst->ipc, QB_OP_END_FRAME, &ef, sizeof(ef), &fence, fence >= 0 ? 1 : 0, &st, sizeof(st),
                             NULL, 0, NULL);
    if (fence >= 0) close(fence);
    s->frame_begun = 0;
    return XR_FAILED(r) ? r : qb_session_result(s, XR_SUCCESS);
}

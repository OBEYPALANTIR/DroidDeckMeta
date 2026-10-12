/* qb_input.c - OpenXR actions over the service's Oculus Touch snapshots. Suggested bindings for any
 * well-formed interaction profile are accepted (xrizer gives up when one is refused), but only those for
 * oculus/touch_controller, else khr/simple_controller (the profiles Quest controllers provide), resolve
 * at xrAttachSessionActionSets; every xrSyncActions turns one SYNC_ACTIONS snapshot into action states. */
#define _GNU_SOURCE
#include "qb_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum qb_kind {
    K_NONE, /* in the profile, but nothing on Quest drives it */
    K_BUTTON,
    K_TRIGGER,
    K_SQUEEZE,
    K_STICK_X,
    K_STICK_Y,
    K_STICK,  /* the thumbstick as a vector2f */
    K_SELECT, /* simple_controller select: the trigger past half way */
    K_GRIP,
    K_AIM,
    K_HAPTIC,
};

#define HAND_L (1u << QB_USER_LEFT)
#define HAND_R (1u << QB_USER_RIGHT)
#define HANDS (HAND_L | HAND_R)

struct qb_component {
    const char *path; /* after the top-level user path */
    uint8_t users;
    uint8_t kind;
    uint16_t button; /* enum qb_button, K_BUTTON only */
};

static const struct qb_component touch_components[] = {
    {"input/x/click", HAND_L, K_BUTTON, QB_BTN_PRIMARY_CLICK},
    {"input/x/touch", HAND_L, K_BUTTON, QB_BTN_PRIMARY_TOUCH},
    {"input/y/click", HAND_L, K_BUTTON, QB_BTN_SECONDARY_CLICK},
    {"input/y/touch", HAND_L, K_BUTTON, QB_BTN_SECONDARY_TOUCH},
    {"input/menu/click", HAND_L, K_BUTTON, QB_BTN_MENU_CLICK},
    {"input/a/click", HAND_R, K_BUTTON, QB_BTN_PRIMARY_CLICK},
    {"input/a/touch", HAND_R, K_BUTTON, QB_BTN_PRIMARY_TOUCH},
    {"input/b/click", HAND_R, K_BUTTON, QB_BTN_SECONDARY_CLICK},
    {"input/b/touch", HAND_R, K_BUTTON, QB_BTN_SECONDARY_TOUCH},
    {"input/system/click", HAND_R, K_NONE, 0},
    {"input/squeeze/value", HANDS, K_SQUEEZE, 0},
    {"input/trigger/value", HANDS, K_TRIGGER, 0},
    {"input/trigger/touch", HANDS, K_BUTTON, QB_BTN_TRIGGER_TOUCH},
    {"input/thumbstick", HANDS, K_STICK, 0},
    {"input/thumbstick/x", HANDS, K_STICK_X, 0},
    {"input/thumbstick/y", HANDS, K_STICK_Y, 0},
    {"input/thumbstick/click", HANDS, K_BUTTON, QB_BTN_THUMBSTICK_CLICK},
    {"input/thumbstick/touch", HANDS, K_BUTTON, QB_BTN_THUMBSTICK_TOUCH},
    {"input/thumbrest/touch", HANDS, K_BUTTON, QB_BTN_THUMBREST_TOUCH},
    {"input/grip/pose", HANDS, K_GRIP, 0},
    {"input/aim/pose", HANDS, K_AIM, 0},
    {"output/haptic", HANDS, K_HAPTIC, 0},
    {NULL, 0, 0, 0},
};

static const struct qb_component simple_components[] = {
    {"input/select/click", HANDS, K_SELECT, 0},
    {"input/menu/click", HANDS, K_BUTTON, QB_BTN_MENU_CLICK}, /* Touch has a menu button on the left only */
    {"input/grip/pose", HANDS, K_GRIP, 0},
    {"input/aim/pose", HANDS, K_AIM, 0},
    {"output/haptic", HANDS, K_HAPTIC, 0},
    {NULL, 0, 0, 0},
};

struct qb_profile {
    const char *path, *name;
    const struct qb_component *components;
};

/* In QB_DRIVEN_PROFILES order. */
static const struct qb_profile profiles[QB_DRIVEN_PROFILES] = {
    {"/interaction_profiles/oculus/touch_controller", "Oculus Touch Controller", touch_components},
    {"/interaction_profiles/khr/simple_controller", "Khronos Simple Controller", simple_components},
};

static const char *const user_paths[QB_USER_COUNT] = {"/user/hand/left", "/user/hand/right", "/user/head",
                                                      "/user/gamepad", "/user/treadmill"};
static const char *const user_names[QB_USER_COUNT] = {"Left Hand", "Right Hand", "Head", "Gamepad", "Treadmill"};

static int user_index(const char *path) {
    for (int u = 0; u < QB_USER_COUNT; u++)
        if (!strcmp(path, user_paths[u])) return u;
    return -1;
}

/* The top-level user path `path` starts with (-1: none) and what follows it. */
static int split_user(const char *path, const char **rest) {
    for (int u = 0; u < QB_USER_COUNT; u++) {
        size_t n = strlen(user_paths[u]);
        if (!strncmp(path, user_paths[u], n) && path[n] == '/') {
            *rest = path + n + 1;
            return u;
        }
    }
    return -1;
}

/* "input/<id>" without a component: the spec lets it bind to one of the identifier's components. */
static int is_identifier(const char *rest) {
    const char *slash = strchr(rest, '/');
    return slash && !strchr(slash + 1, '/');
}

/* "/interaction_profiles/<vendor>/<type>"; XrPath atoms are already well formed. */
static int profile_path_valid(const char *path) {
    static const char prefix[] = "/interaction_profiles/";
    const char *slash;
    return !strncmp(path, prefix, sizeof(prefix) - 1) && (slash = strchr(path + sizeof(prefix) - 1, '/')) &&
           !strchr(slash + 1, '/');
}

/* "/user/.../input/..." or "/user/.../output/..."; sources this runtime cannot drive are ignored later. */
static int binding_path_valid(const char *path) {
    return !strncmp(path, "/user/", 6) && (strstr(path, "/input/") || strstr(path, "/output/"));
}

static const struct qb_component *find(const struct qb_profile *p, int u, const char *rest) {
    for (const struct qb_component *c = p->components; c->path; c++)
        if ((c->users & (1u << u)) && !strcmp(c->path, rest)) return c;
    return NULL;
}

static int compatible(int kind, XrActionType type) {
    switch (type) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT:
        return kind == K_BUTTON || kind == K_TRIGGER || kind == K_SQUEEZE || kind == K_STICK_X || kind == K_STICK_Y ||
               kind == K_SELECT;
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
        return kind == K_STICK;
    case XR_ACTION_TYPE_POSE_INPUT:
        return kind == K_GRIP || kind == K_AIM;
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
        return kind == K_HAPTIC;
    default:
        return 0;
    }
}

/* The component a suggested binding drives for an action of `type`. A bare identifier binds to click
 * then value (boolean), value then click (float), or pose; incompatible bindings are ignored. */
static const struct qb_component *resolve(const struct qb_profile *p, int u, const char *rest, XrActionType type) {
    static const char *const bool_order[] = {"click", "value", NULL}, *const float_order[] = {"value", "click", NULL},
                             *const pose_order[] = {"pose", NULL}, *const no_order[] = {NULL};
    const struct qb_component *c = find(p, u, rest);
    if (c && compatible(c->kind, type)) return c;
    if (!is_identifier(rest)) return NULL;
    const char *const *order = type == XR_ACTION_TYPE_BOOLEAN_INPUT ? bool_order
                               : type == XR_ACTION_TYPE_FLOAT_INPUT ? float_order
                               : type == XR_ACTION_TYPE_POSE_INPUT  ? pose_order
                                                                    : no_order;
    for (; *order; order++) {
        char path[XR_MAX_PATH_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", rest, *order);
        if ((c = find(p, u, path)) && compatible(c->kind, type)) return c;
    }
    return NULL;
}

static void source_value(const struct qb_component *c, const struct qb_hand_state *h, float v[2]) {
    v[0] = v[1] = 0.0f;
    switch (c->kind) {
    case K_BUTTON:
        v[0] = (h->buttons & c->button) ? 1.0f : 0.0f;
        break;
    case K_TRIGGER:
        v[0] = h->trigger;
        break;
    case K_SQUEEZE:
        v[0] = h->squeeze;
        break;
    case K_STICK_X:
        v[0] = h->thumbstick_x;
        break;
    case K_STICK_Y:
        v[0] = h->thumbstick_y;
        break;
    case K_STICK:
        v[0] = h->thumbstick_x;
        v[1] = h->thumbstick_y;
        break;
    case K_SELECT:
        v[0] = h->trigger > 0.5f ? 1.0f : 0.0f;
        break;
    default:
        break;
    }
}

/* Combining several sources (spec): OR for booleans, the largest magnitude for floats, the longest vector. */
static int dominates(XrActionType type, const float v[2], const float cur[2]) {
    switch (type) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
        return v[0] > cur[0];
    case XR_ACTION_TYPE_FLOAT_INPUT:
        return v[0] * v[0] > cur[0] * cur[0];
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
        return v[0] * v[0] + v[1] * v[1] > cur[0] * cur[0] + cur[1] * cur[1];
    default:
        return 0;
    }
}

/* One xrSyncActions for one action: `users` are the hands its set was synced for (0 = inactive). */
static void update_action(struct qb_action *a, const struct qb_input_state *in, uint32_t users, XrTime now) {
    for (int k = 0; k < 3; k++) { /* left, right, either */
        struct qb_action_value nv = {0};
        for (uint32_t i = 0; i < a->binding_count; i++) {
            const struct qb_binding *b = &a->bindings[i];
            if ((k < 2 && b->hand != k) || b->suppressed || !(users & (1u << b->hand)) || !in->hands[b->hand].active)
                continue;
            float v[2];
            source_value(b->c, &in->hands[b->hand], v);
            if (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT) v[0] = v[0] > 0.5f ? 1.0f : 0.0f;
            if (!nv.active || dominates(a->type, v, nv.v)) {
                nv.v[0] = v[0];
                nv.v[1] = v[1];
            }
            nv.active = 1;
        }
        struct qb_action_value *ov = &a->value[k];
        if (nv.active) {
            /* an inactive action reads as zero, so becoming active with a value is a change */
            nv.changed = nv.v[0] != ov->v[0] || nv.v[1] != ov->v[1];
            nv.changed_at = nv.changed || !ov->active ? now : ov->changed_at;
        }
        *ov = nv;
    }
}

static int set_attached(const struct qb_session *s, const struct qb_action_set *set) {
    for (uint32_t i = 0; i < s->attached_count; i++)
        if (s->attached_sets[i] == set) return 1;
    return 0;
}

static int set_uses(const struct qb_action_set *set, const struct qb_binding *b) {
    for (const struct qb_action *a = set->actions; a; a = a->next)
        for (uint32_t i = 0; !a->destroyed && i < a->binding_count; i++)
            if (a->bindings[i].c == b->c && a->bindings[i].hand == b->hand) return 1;
    return 0;
}

/* Spec: a source bound in several active action sets only drives the highest-priority set's actions. */
static void suppress(struct qb_session *s) {
    for (uint32_t i = 0; i < s->attached_count; i++) {
        const struct qb_action_set *set = s->attached_sets[i];
        for (struct qb_action *a = set->actions; a; a = a->next)
            for (uint32_t k = 0; k < a->binding_count; k++) {
                struct qb_binding *b = &a->bindings[k];
                b->suppressed = 0;
                for (uint32_t j = 0; j < s->attached_count && !b->suppressed; j++) {
                    const struct qb_action_set *other = s->attached_sets[j];
                    b->suppressed = other->priority > set->priority && (other->sync_users & (1u << b->hand)) &&
                                    set_uses(other, b);
                }
            }
    }
}

/* The current profile per hand: the resolved one while that controller is active. */
static void update_profiles(struct qb_session *s) {
    int changed = 0;
    for (int h = 0; h < 2; h++) {
        XrPath p = s->profile >= 0 && s->input.hands[h].active ? s->inst->profile_path[s->profile] : XR_NULL_PATH;
        changed |= p != s->current_profile[h];
        s->current_profile[h] = p;
    }
    if (!changed) return;
    union qb_xr_event e;
    memset(&e, 0, sizeof(e));
    e.profile.type = XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED;
    e.profile.session = (XrSession)s;
    qb_event_push(s->inst, &e);
}

/* The action's sources under the session's profile, from that profile's suggestions. */
static XrResult resolve_bindings(struct qb_session *s, struct qb_action *a) {
    struct qb_instance *inst = s->inst;
    free(a->bindings);
    a->bindings = NULL;
    a->binding_count = 0;
    memset(a->value, 0, sizeof(a->value));
    if (s->profile < 0 || a->destroyed) return XR_SUCCESS;
    const struct qb_profile *p = &profiles[s->profile];
    for (uint32_t i = 0; i < inst->suggestion_count[s->profile]; i++) {
        const struct qb_suggestion *sg = &inst->suggestions[s->profile][i];
        const char *rest = "";
        int u = sg->action == a ? split_user(qb_path_string(inst, sg->binding), &rest) : -1;
        if (u < 0 || u > QB_USER_RIGHT || (a->users && !(a->users & (1u << u)))) continue;
        const struct qb_component *c = resolve(p, u, rest, a->type);
        if (!c || c->kind == K_NONE) continue;
        int dup = 0;
        for (uint32_t k = 0; k < a->binding_count; k++) dup |= a->bindings[k].c == c && a->bindings[k].hand == u;
        if (dup) continue;
        char full[XR_MAX_PATH_LENGTH];
        snprintf(full, sizeof(full), "%s/%s", user_paths[u], c->path);
        XrPath source = qb_path_intern(inst, full);
        if (source == XR_NULL_PATH) return XR_ERROR_OUT_OF_MEMORY;
        struct qb_binding *grown = realloc(a->bindings, (a->binding_count + 1) * sizeof(*grown));
        if (!grown) return XR_ERROR_OUT_OF_MEMORY;
        a->bindings = grown;
        a->bindings[a->binding_count++] = (struct qb_binding){.c = c, .path = source, .hand = (uint8_t)u};
    }
    return XR_SUCCESS;
}

XrResult qb_input_subaction(const struct qb_action *a, XrPath path, int *user) {
    *user = -1;
    if (path == XR_NULL_PATH) return XR_SUCCESS;
    const char *str = qb_path_string(a->set->inst, path);
    if (!str) return XR_ERROR_PATH_INVALID;
    int u = user_index(str);
    if (u < 0 || !(a->users & (1u << u))) return XR_ERROR_PATH_UNSUPPORTED;
    *user = u;
    return XR_SUCCESS;
}

int qb_input_pose_space(const struct qb_session *s, const struct qb_action *a, int user, uint32_t *space) {
    if (!set_attached(s, a->set)) return 0;
    for (uint32_t i = 0; i < a->binding_count; i++) {
        const struct qb_binding *b = &a->bindings[i];
        if ((user >= 0 && b->hand != user) || !a->value[b->hand].active) continue;
        int grip = b->c->kind == K_GRIP;
        *space = b->hand == QB_USER_LEFT ? (grip ? QB_SPACE_LEFT_GRIP : QB_SPACE_LEFT_AIM)
                                         : (grip ? QB_SPACE_RIGHT_GRIP : QB_SPACE_RIGHT_AIM);
        return 1;
    }
    return 0;
}

void qb_input_detach(struct qb_session *s) {
    for (uint32_t i = 0; i < s->attached_count; i++)
        for (struct qb_action *a = s->attached_sets[i]->actions; a; a = a->next) {
            free(a->bindings);
            a->bindings = NULL;
            a->binding_count = 0;
            memset(a->value, 0, sizeof(a->value));
        }
    free(s->attached_sets);
    s->attached_sets = NULL;
    s->attached_count = 0;
    s->attached = 0;
    s->profile = -1;
    s->current_profile[0] = s->current_profile[1] = XR_NULL_PATH;
}

void qb_input_destroy_all(struct qb_instance *inst) {
    while (inst->action_sets) {
        struct qb_action_set *set = inst->action_sets;
        inst->action_sets = set->next;
        while (set->actions) {
            struct qb_action *a = set->actions;
            set->actions = a->next;
            a->magic = 0;
            free(a->bindings);
            free(a);
        }
        set->magic = 0;
        free(set);
    }
    for (int p = 0; p < QB_DRIVEN_PROFILES; p++) {
        free(inst->suggestions[p]);
        inst->suggestions[p] = NULL;
        inst->suggestion_count[p] = 0;
    }
}

/* ---- action sets and actions */

/* Names must be NUL-terminated in their arrays, non-empty, and well-formed path elements. */
static XrResult check_names(const char *name, size_t name_size, const char *localized, size_t localized_size) {
    size_t n = strnlen(name, name_size), ln = strnlen(localized, localized_size);
    if (n == name_size || ln == localized_size) return XR_ERROR_VALIDATION_FAILURE;
    if (!n) return XR_ERROR_NAME_INVALID;
    if (!ln) return XR_ERROR_LOCALIZED_NAME_INVALID;
    return qb_name_valid(name) ? XR_SUCCESS : XR_ERROR_PATH_FORMAT_INVALID;
}

XrResult qb_xrCreateActionSet(XrInstance instance, const XrActionSetCreateInfo *info, XrActionSet *actionSet) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_ACTION_SET_CREATE_INFO || !actionSet) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = check_names(info->actionSetName, sizeof(info->actionSetName), info->localizedActionSetName,
                             sizeof(info->localizedActionSetName));
    if (r != XR_SUCCESS) return r;
    QB_LOCK(inst);
    for (const struct qb_action_set *o = inst->action_sets; o; o = o->next) {
        if (o->destroyed) continue;
        if (!strcmp(o->name, info->actionSetName)) return XR_ERROR_NAME_DUPLICATED;
        if (!strcmp(o->localized, info->localizedActionSetName)) return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
    }
    struct qb_action_set *set = calloc(1, sizeof(*set));
    if (!set) return XR_ERROR_OUT_OF_MEMORY;
    set->magic = QB_MAGIC_ACTION_SET;
    set->inst = inst;
    set->priority = info->priority;
    memcpy(set->name, info->actionSetName, sizeof(set->name));
    memcpy(set->localized, info->localizedActionSetName, sizeof(set->localized));
    set->next = inst->action_sets;
    inst->action_sets = set;
    *actionSet = (XrActionSet)set;
    return XR_SUCCESS;
}

XrResult qb_xrDestroyActionSet(XrActionSet actionSet) {
    struct qb_action_set *set = qb_action_set_get(actionSet);
    if (!set) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(set->inst);
    /* shortcut: sets and actions stay allocated until xrDestroyInstance, so action spaces and an attached
     * session keep working without refcounts; add refcounts if an app churns action sets */
    for (struct qb_action *a = set->actions; a; a = a->next) {
        a->magic = 0;
        a->destroyed = 1;
    }
    set->magic = 0;
    set->destroyed = 1;
    return XR_SUCCESS;
}

XrResult qb_xrCreateAction(XrActionSet actionSet, const XrActionCreateInfo *info, XrAction *action) {
    struct qb_action_set *set = qb_action_set_get(actionSet);
    if (!set) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_ACTION_CREATE_INFO || !action ||
        (info->countSubactionPaths && !info->subactionPaths))
        return XR_ERROR_VALIDATION_FAILURE;
    switch (info->actionType) {
    case XR_ACTION_TYPE_BOOLEAN_INPUT:
    case XR_ACTION_TYPE_FLOAT_INPUT:
    case XR_ACTION_TYPE_VECTOR2F_INPUT:
    case XR_ACTION_TYPE_POSE_INPUT:
    case XR_ACTION_TYPE_VIBRATION_OUTPUT:
        break;
    default:
        return XR_ERROR_VALIDATION_FAILURE;
    }
    XrResult r = check_names(info->actionName, sizeof(info->actionName), info->localizedActionName,
                             sizeof(info->localizedActionName));
    if (r != XR_SUCCESS) return r;
    QB_LOCK(set->inst);
    if (set->locked) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (const struct qb_action *o = set->actions; o; o = o->next) {
        if (o->destroyed) continue;
        if (!strcmp(o->name, info->actionName)) return XR_ERROR_NAME_DUPLICATED;
        if (!strcmp(o->localized, info->localizedActionName)) return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
    }
    uint32_t users = 0;
    for (uint32_t i = 0; i < info->countSubactionPaths; i++) {
        const char *str = qb_path_string(set->inst, info->subactionPaths[i]);
        if (!str) return XR_ERROR_PATH_INVALID;
        int u = user_index(str);
        if (u < 0 || (users & (1u << u))) return XR_ERROR_PATH_UNSUPPORTED;
        users |= 1u << u;
    }
    struct qb_action *a = calloc(1, sizeof(*a));
    if (!a) return XR_ERROR_OUT_OF_MEMORY;
    a->magic = QB_MAGIC_ACTION;
    a->set = set;
    a->type = info->actionType;
    a->users = users;
    memcpy(a->name, info->actionName, sizeof(a->name));
    memcpy(a->localized, info->localizedActionName, sizeof(a->localized));
    a->next = set->actions;
    set->actions = a;
    *action = (XrAction)a;
    return XR_SUCCESS;
}

XrResult qb_xrDestroyAction(XrAction action) {
    struct qb_action *a = qb_action_get(action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(a->set->inst);
    a->magic = 0;
    a->destroyed = 1; /* stays in its set until xrDestroyInstance: action spaces outlive the handle */
    return XR_SUCCESS;
}

/* ---- bindings */

XrResult qb_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding *sb) {
    struct qb_instance *inst = qb_instance_get(instance);
    if (!inst) return XR_ERROR_HANDLE_INVALID;
    if (!sb || sb->type != XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING || !sb->countSuggestedBindings ||
        !sb->suggestedBindings)
        return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(inst);
    const char *profile = qb_path_string(inst, sb->interactionProfile);
    if (!profile) return XR_ERROR_PATH_INVALID;
    if (!profile_path_valid(profile)) return XR_ERROR_PATH_UNSUPPORTED;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++) {
        const XrActionSuggestedBinding *b = &sb->suggestedBindings[i];
        const struct qb_action *a = qb_action_get(b->action);
        if (!a) return XR_ERROR_HANDLE_INVALID;
        if (a->set->inst != inst) return XR_ERROR_VALIDATION_FAILURE;
        if (a->set->locked) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
        const char *path = qb_path_string(inst, b->binding);
        if (!path) return XR_ERROR_PATH_INVALID;
        if (!binding_path_valid(path)) return XR_ERROR_PATH_UNSUPPORTED;
    }
    size_t p = 0;
    while (p < QB_DRIVEN_PROFILES && strcmp(profiles[p].path, profile)) p++;
    if (p == QB_DRIVEN_PROFILES) return XR_SUCCESS; /* accepted, but no Quest controller presents it */
    struct qb_suggestion *list = malloc(sb->countSuggestedBindings * sizeof(*list));
    if (!list) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < sb->countSuggestedBindings; i++)
        list[i] = (struct qb_suggestion){qb_action_get(sb->suggestedBindings[i].action),
                                         sb->suggestedBindings[i].binding};
    free(inst->suggestions[p]); /* a new suggestion for a profile replaces the previous one */
    inst->suggestions[p] = list;
    inst->suggestion_count[p] = sb->countSuggestedBindings;
    inst->profile_path[p] = sb->interactionProfile;
    return XR_SUCCESS;
}

XrResult qb_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *info) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO || !info->countActionSets || !info->actionSets)
        return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = s->inst;
    QB_LOCK(inst);
    if (s->attached) return XR_ERROR_ACTIONSETS_ALREADY_ATTACHED;
    for (uint32_t i = 0; i < info->countActionSets; i++) {
        const struct qb_action_set *set = qb_action_set_get(info->actionSets[i]);
        if (!set) return XR_ERROR_HANDLE_INVALID;
        if (set->inst != inst) return XR_ERROR_VALIDATION_FAILURE;
    }
    s->attached_sets = calloc(info->countActionSets, sizeof(*s->attached_sets));
    if (!s->attached_sets) return XR_ERROR_OUT_OF_MEMORY;
    s->attached = 1;
    /* Quest controllers are Touch; simple_controller is emulated on them when that is all the app knows */
    s->profile = inst->suggestion_count[0] ? 0 : inst->suggestion_count[1] ? 1 : -1;
    for (uint32_t i = 0; i < info->countActionSets; i++) {
        struct qb_action_set *set = qb_action_set_get(info->actionSets[i]);
        if (set_attached(s, set)) continue;
        set->locked = 1;
        s->attached_sets[s->attached_count++] = set;
        for (struct qb_action *a = set->actions; a; a = a->next) {
            XrResult r = resolve_bindings(s, a);
            if (r != XR_SUCCESS) {
                qb_input_detach(s);
                return r;
            }
        }
    }
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevelUserPath, XrInteractionProfileState *state) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!state || state->type != XR_TYPE_INTERACTION_PROFILE_STATE) return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    if (!s->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    const char *str = qb_path_string(s->inst, topLevelUserPath);
    if (!str) return XR_ERROR_PATH_INVALID;
    int u = user_index(str);
    if (u < 0) return XR_ERROR_PATH_UNSUPPORTED;
    state->interactionProfile = u <= QB_USER_RIGHT ? s->current_profile[u] : XR_NULL_PATH;
    return qb_session_result(s, XR_SUCCESS);
}

/* ---- sync and state */

XrResult qb_xrSyncActions(XrSession session, const XrActionsSyncInfo *info) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_ACTIONS_SYNC_INFO || (info->countActiveActionSets && !info->activeActionSets))
        return XR_ERROR_VALIDATION_FAILURE;
    struct qb_instance *inst = s->inst;
    QB_LOCK(inst);
    for (uint32_t i = 0; i < info->countActiveActionSets; i++) {
        const XrActiveActionSet *e = &info->activeActionSets[i];
        const struct qb_action_set *set = qb_action_set_get(e->actionSet);
        if (!set) return XR_ERROR_HANDLE_INVALID;
        if (!set_attached(s, set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
        if (e->subactionPath == XR_NULL_PATH) continue;
        const char *str = qb_path_string(inst, e->subactionPath);
        if (!str) return XR_ERROR_PATH_INVALID;
        if (user_index(str) < 0) return XR_ERROR_PATH_UNSUPPORTED;
    }
    /* Not focused: every action reads inactive and nothing is asked of the service. */
    int live = s->state == XR_SESSION_STATE_FOCUSED && info->countActiveActionSets;
    if (live) {
        struct qb_input_state in;
        XrResult r = qb_ipc_call(&inst->ipc, QB_OP_SYNC_ACTIONS, NULL, 0, NULL, 0, &in, sizeof(in), NULL, 0, NULL);
        if (XR_FAILED(r)) return r;
        s->input = in;
        update_profiles(s);
    }
    for (uint32_t i = 0; i < s->attached_count; i++) s->attached_sets[i]->sync_users = 0;
    for (uint32_t i = 0; live && i < info->countActiveActionSets; i++) {
        const XrActiveActionSet *e = &info->activeActionSets[i];
        qb_action_set_get(e->actionSet)->sync_users |=
            e->subactionPath == XR_NULL_PATH ? (1u << QB_USER_COUNT) - 1
                                             : 1u << user_index(qb_path_string(inst, e->subactionPath));
    }
    suppress(s);
    XrTime now = live && s->input.time > 0 ? s->input.time : qb_now();
    for (uint32_t i = 0; i < s->attached_count; i++)
        for (struct qb_action *a = s->attached_sets[i]->actions; a; a = a->next)
            update_action(a, &s->input, s->attached_sets[i]->sync_users, now);
    return s->state == XR_SESSION_STATE_FOCUSED ? qb_session_result(s, XR_SUCCESS) : XR_SESSION_NOT_FOCUSED;
}

/* Shared by xrGetActionState* and the haptics calls. */
static XrResult check_action(const struct qb_session *s, const struct qb_action *a, XrActionType type, XrPath subaction,
                             int *user) {
    if (a->set->inst != s->inst) return XR_ERROR_VALIDATION_FAILURE;
    if (a->type != type) return XR_ERROR_ACTION_TYPE_MISMATCH;
    if (!set_attached(s, a->set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    return qb_input_subaction(a, subaction, user);
}

static XrResult get_value(XrSession session, const XrActionStateGetInfo *info, XrActionType type,
                          struct qb_action_value *out) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_ACTION_STATE_GET_INFO) return XR_ERROR_VALIDATION_FAILURE;
    const struct qb_action *a = qb_action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    int user;
    XrResult r = check_action(s, a, type, info->subactionPath, &user);
    if (r != XR_SUCCESS) return r;
    if (user > QB_USER_RIGHT)
        memset(out, 0, sizeof(*out)); /* head, gamepad, treadmill: nothing on Quest behind them */
    else
        *out = a->value[user < 0 ? 2 : user];
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateBoolean *state) {
    struct qb_action_value v;
    if (!state || state->type != XR_TYPE_ACTION_STATE_BOOLEAN) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = get_value(session, getInfo, XR_ACTION_TYPE_BOOLEAN_INPUT, &v);
    if (XR_FAILED(r)) return r;
    state->currentState = v.v[0] != 0.0f ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = v.changed;
    state->lastChangeTime = v.changed_at;
    state->isActive = v.active ? XR_TRUE : XR_FALSE;
    return r;
}

XrResult qb_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStateFloat *state) {
    struct qb_action_value v;
    if (!state || state->type != XR_TYPE_ACTION_STATE_FLOAT) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = get_value(session, getInfo, XR_ACTION_TYPE_FLOAT_INPUT, &v);
    if (XR_FAILED(r)) return r;
    state->currentState = v.v[0];
    state->changedSinceLastSync = v.changed;
    state->lastChangeTime = v.changed_at;
    state->isActive = v.active ? XR_TRUE : XR_FALSE;
    return r;
}

XrResult qb_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo *getInfo,
                                     XrActionStateVector2f *state) {
    struct qb_action_value v;
    if (!state || state->type != XR_TYPE_ACTION_STATE_VECTOR2F) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = get_value(session, getInfo, XR_ACTION_TYPE_VECTOR2F_INPUT, &v);
    if (XR_FAILED(r)) return r;
    state->currentState.x = v.v[0];
    state->currentState.y = v.v[1];
    state->changedSinceLastSync = v.changed;
    state->lastChangeTime = v.changed_at;
    state->isActive = v.active ? XR_TRUE : XR_FALSE;
    return r;
}

XrResult qb_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *getInfo, XrActionStatePose *state) {
    struct qb_action_value v;
    if (!state || state->type != XR_TYPE_ACTION_STATE_POSE) return XR_ERROR_VALIDATION_FAILURE;
    XrResult r = get_value(session, getInfo, XR_ACTION_TYPE_POSE_INPUT, &v);
    if (XR_FAILED(r)) return r;
    state->isActive = v.active ? XR_TRUE : XR_FALSE;
    return r;
}

XrResult qb_xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo *info,
                                             uint32_t cap, uint32_t *count, XrPath *sources) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO || !count || (cap && !sources))
        return XR_ERROR_VALIDATION_FAILURE;
    const struct qb_action *a = qb_action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    if (a->set->inst != s->inst) return XR_ERROR_VALIDATION_FAILURE;
    if (!set_attached(s, a->set)) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    XrResult r = qb_count(cap, count, a->binding_count);
    if (r == XR_SUCCESS && cap)
        for (uint32_t i = 0; i < a->binding_count; i++) sources[i] = a->bindings[i].path;
    return qb_session_result(s, r);
}

/* Appends `text` after a space; `words`: path components become capitalised words ("trigger/value" ->
 * "Trigger Value"). */
static void append(char *out, size_t cap, const char *text, int words) {
    size_t n = strlen(out);
    if (n && n + 1 < cap) out[n++] = ' ';
    for (int start = 1; *text && n + 1 < cap; text++) {
        char c = *text;
        if (words && (c == '/' || c == '_')) {
            c = ' ';
            start = 1;
        } else if (words && start) {
            if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
            start = 0;
        }
        out[n++] = c;
    }
    out[n] = '\0';
}

XrResult qb_xrGetInputSourceLocalizedName(XrSession session, const XrInputSourceLocalizedNameGetInfo *info,
                                          uint32_t cap, uint32_t *count, char *buffer) {
    const XrInputSourceLocalizedNameFlags all = XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT |
                                                XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT |
                                                XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT;
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_INPUT_SOURCE_LOCALIZED_NAME_GET_INFO || !count || (cap && !buffer) ||
        !info->whichComponents || (info->whichComponents & ~all))
        return XR_ERROR_VALIDATION_FAILURE;
    QB_LOCK(s->inst);
    if (!s->attached) return XR_ERROR_ACTIONSET_NOT_ATTACHED;
    const char *path = qb_path_string(s->inst, info->sourcePath), *rest = "";
    if (!path) return XR_ERROR_PATH_INVALID;
    int u = split_user(path, &rest);
    if (u < 0 && (u = user_index(path)) < 0) return XR_ERROR_PATH_UNSUPPORTED;
    char name[256] = "";
    if (info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT) append(name, sizeof(name), user_names[u], 0);
    if ((info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_INTERACTION_PROFILE_BIT) && s->profile >= 0)
        append(name, sizeof(name), profiles[s->profile].name, 0);
    const char *component = strchr(rest, '/'); /* past "input/" or "output/" */
    if ((info->whichComponents & XR_INPUT_SOURCE_LOCALIZED_NAME_COMPONENT_BIT) && component)
        append(name, sizeof(name), component + 1, 1);
    uint32_t n = (uint32_t)strlen(name) + 1;
    XrResult r = qb_count(cap, count, n);
    if (r == XR_SUCCESS && cap) memcpy(buffer, name, n);
    return qb_session_result(s, r);
}

/* ---- haptics */

static XrResult haptic(XrSession session, const XrHapticActionInfo *info, XrDuration duration, float frequency,
                       float amplitude) {
    struct qb_session *s = qb_session_get(session);
    if (!s) return XR_ERROR_HANDLE_INVALID;
    if (!info || info->type != XR_TYPE_HAPTIC_ACTION_INFO) return XR_ERROR_VALIDATION_FAILURE;
    const struct qb_action *a = qb_action_get(info->action);
    if (!a) return XR_ERROR_HANDLE_INVALID;
    QB_LOCK(s->inst);
    int user;
    XrResult r = check_action(s, a, XR_ACTION_TYPE_VIBRATION_OUTPUT, info->subactionPath, &user);
    if (r != XR_SUCCESS) return r;
    if (s->state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    uint32_t done = 0;
    for (uint32_t i = 0; i < a->binding_count; i++) {
        const struct qb_binding *b = &a->bindings[i];
        /* only hands the action was active for in the last xrSyncActions */
        if ((user >= 0 && b->hand != user) || !a->value[b->hand].active || (done & (1u << b->hand))) continue;
        struct qb_haptic h = {.hand = b->hand, .duration_ns = duration, .frequency = frequency,
                              .amplitude = amplitude < 0.0f ? 0.0f : amplitude > 1.0f ? 1.0f : amplitude};
        struct qb_status st;
        done |= 1u << b->hand;
        r = qb_ipc_call(&s->inst->ipc, QB_OP_APPLY_HAPTIC, &h, sizeof(h), NULL, 0, &st, sizeof(st), NULL, 0, NULL);
        if (XR_FAILED(r)) return r;
    }
    return qb_session_result(s, XR_SUCCESS);
}

XrResult qb_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo,
                                  const XrHapticBaseHeader *hapticFeedback) {
    if (!hapticFeedback || hapticFeedback->type != XR_TYPE_HAPTIC_VIBRATION) return XR_ERROR_VALIDATION_FAILURE;
    const XrHapticVibration *v = (const XrHapticVibration *)hapticFeedback;
    return haptic(session, hapticActionInfo, v->duration, v->frequency, v->amplitude);
}

XrResult qb_xrStopHapticFeedback(XrSession session, const XrHapticActionInfo *hapticActionInfo) {
    return haptic(session, hapticActionInfo, 0, 0.0f, 0.0f);
}

/*
 * qb_math.h - the pose math libopenxr_questbridge.so does itself: a guest XrSpace is a service space
 * plus a rigid pose offset (questbridge_protocol.h), so every located pose and velocity is composed
 * here from what came over the wire. Header-only and free of the runtime's other dependencies so
 * tools/questbridge/tests can compile it on its own.
 *
 * a * b is b (given in a's frame) expressed in a's parent frame.
 */
#ifndef QB_MATH_H
#define QB_MATH_H

#include <openxr/openxr.h>

#include "questbridge_protocol.h"

static inline XrQuaternionf qb_quat_mul(XrQuaternionf a, XrQuaternionf b) {
    return (XrQuaternionf){
        a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
    };
}

static inline XrQuaternionf qb_quat_conj(XrQuaternionf q) {
    return (XrQuaternionf){-q.x, -q.y, -q.z, q.w};
}

static inline XrVector3f qb_quat_rotate(XrQuaternionf q, XrVector3f v) {
    XrVector3f t = {2.0f * (q.y * v.z - q.z * v.y), 2.0f * (q.z * v.x - q.x * v.z), 2.0f * (q.x * v.y - q.y * v.x)};
    return (XrVector3f){
        v.x + q.w * t.x + (q.y * t.z - q.z * t.y),
        v.y + q.w * t.y + (q.z * t.x - q.x * t.z),
        v.z + q.w * t.z + (q.x * t.y - q.y * t.x),
    };
}

static inline XrVector3f qb_vec3_cross(XrVector3f a, XrVector3f b) {
    return (XrVector3f){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

static inline XrPosef qb_pose_mul(XrPosef a, XrPosef b) {
    XrVector3f p = qb_quat_rotate(a.orientation, b.position);
    return (XrPosef){qb_quat_mul(a.orientation, b.orientation),
                     {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}

static inline XrPosef qb_pose_inv(XrPosef a) {
    XrQuaternionf q = qb_quat_conj(a.orientation);
    XrVector3f p = qb_quat_rotate(q, a.position);
    return (XrPosef){q, {-p.x, -p.y, -p.z}};
}

/* A unit orientation within 1% (NaN fails). */
static inline int qb_pose_valid(const XrPosef *p) {
    const XrQuaternionf *q = &p->orientation;
    float n = q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w;
    return n > 0.98f && n < 1.02f;
}

static inline XrPosef qb_pose_from_wire(const struct qb_posef *p) {
    return (XrPosef){{p->qx, p->qy, p->qz, p->qw}, {p->x, p->y, p->z}};
}

static inline struct qb_posef qb_pose_to_wire(XrPosef p) {
    return (struct qb_posef){p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w,
                             p.position.x, p.position.y, p.position.z, 0.0f};
}

/* Velocity of guest space (wire * sp_offset) in guest space (wire base * base_offset), from the wire
 * velocity of the wire space's origin. The offsets are rigid and fixed, so: the angular velocity is the
 * wire one; the linear velocity is the wire origin's plus the arc the offset point sweeps,
 * w x (R_wire * sp_offset.position), which needs a valid angular velocity when that offset is not
 * zero (without it the linear velocity is unknown and its flag is cleared). Both are then expressed in
 * the base guest space by the inverse of the base offset's rotation (a static offset adds no velocity). */
static inline void qb_velocity_compose(XrPosef base_offset, XrPosef sp_offset, XrQuaternionf wire_orientation,
                                       XrSpaceVelocityFlags wire_flags, XrVector3f wire_linear,
                                       XrVector3f wire_angular, XrSpaceVelocityFlags *flags, XrVector3f *linear,
                                       XrVector3f *angular) {
    *flags = wire_flags;
    *linear = (XrVector3f){0.0f, 0.0f, 0.0f};
    *angular = (XrVector3f){0.0f, 0.0f, 0.0f};
    XrVector3f lin = wire_linear;
    int offset = sp_offset.position.x != 0.0f || sp_offset.position.y != 0.0f || sp_offset.position.z != 0.0f;
    if ((wire_flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) && offset) {
        if (wire_flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) {
            XrVector3f arc = qb_vec3_cross(wire_angular, qb_quat_rotate(wire_orientation, sp_offset.position));
            lin = (XrVector3f){lin.x + arc.x, lin.y + arc.y, lin.z + arc.z};
        } else {
            *flags &= ~(XrSpaceVelocityFlags)XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        }
    }
    XrQuaternionf inv = qb_quat_conj(base_offset.orientation);
    if (*flags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) *linear = qb_quat_rotate(inv, lin);
    if (*flags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) *angular = qb_quat_rotate(inv, wire_angular);
}

#endif

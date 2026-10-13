/* qb_math.h regression: an action space with a pose offset sweeps an arc when its controller rotates,
 * so its linear velocity carries w x r on top of the origin's. Built and run by
 * tools/tests/test_questbridge_math.py; exit 0 means every case held. */
#include <math.h>
#include <stdio.h>

#include "qb_math.h"

static int failures;

static void expect(const char *what, XrVector3f got, float x, float y, float z) {
    if (fabsf(got.x - x) > 1e-5f || fabsf(got.y - y) > 1e-5f || fabsf(got.z - z) > 1e-5f) {
        printf("FAIL: %s = (%g, %g, %g), want (%g, %g, %g)\n", what, got.x, got.y, got.z, x, y, z);
        failures++;
    }
}

static void expect_flags(const char *what, XrSpaceVelocityFlags got, XrSpaceVelocityFlags want) {
    if (got != want) {
        printf("FAIL: %s flags 0x%llx, want 0x%llx\n", what, (unsigned long long)got, (unsigned long long)want);
        failures++;
    }
}

int main(void) {
    const XrPosef identity = {{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}};
    const XrSpaceVelocityFlags both = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
    const XrVector3f zero = {0.0f, 0.0f, 0.0f};
    XrSpaceVelocityFlags flags;
    XrVector3f linear, angular;

    /* Stationary origin spinning about +Z at 1 rad/s, offset point 1 m along +X: v = w x r = (0, 1, 0). */
    XrPosef offset_x = {{0.0f, 0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}};
    qb_velocity_compose(identity, offset_x, identity.orientation, both, zero, (XrVector3f){0.0f, 0.0f, 1.0f},
                        &flags, &linear, &angular);
    expect_flags("arc", flags, both);
    expect("arc linear", linear, 0.0f, 1.0f, 0.0f);
    expect("arc angular", angular, 0.0f, 0.0f, 1.0f);

    /* The offset is given in the wire space's frame: yawed 90 degrees about +Y, the +X offset points
     * along world -Z, and spinning about +X gives w x r = (1,0,0) x (0,0,-1) = (0, 1, 0). */
    float s = sinf((float)M_PI / 4.0f), c = cosf((float)M_PI / 4.0f);
    XrQuaternionf yaw90 = {0.0f, s, 0.0f, c};
    qb_velocity_compose(identity, offset_x, yaw90, both, zero, (XrVector3f){1.0f, 0.0f, 0.0f}, &flags, &linear,
                        &angular);
    expect("rotated-offset linear", linear, 0.0f, 1.0f, 0.0f);

    /* No offset: the wire velocity passes through unchanged. */
    qb_velocity_compose(identity, identity, yaw90, both, (XrVector3f){1.0f, 2.0f, 3.0f},
                        (XrVector3f){0.0f, 0.0f, 1.0f}, &flags, &linear, &angular);
    expect("no-offset linear", linear, 1.0f, 2.0f, 3.0f);
    expect("no-offset angular", angular, 0.0f, 0.0f, 1.0f);

    /* An offset with no angular velocity available: the linear velocity is unknown, its flag clears. */
    qb_velocity_compose(identity, offset_x, identity.orientation, XR_SPACE_VELOCITY_LINEAR_VALID_BIT,
                        (XrVector3f){1.0f, 0.0f, 0.0f}, zero, &flags, &linear, &angular);
    expect_flags("offset without angular", flags, 0);
    expect("offset without angular linear", linear, 0.0f, 0.0f, 0.0f);

    /* A base space rotated 90 degrees about +Y: a world +X velocity reads as base-frame +Z. */
    XrPosef base_yaw = {yaw90, {5.0f, 0.0f, 0.0f}};
    qb_velocity_compose(base_yaw, identity, identity.orientation, both, (XrVector3f){1.0f, 0.0f, 0.0f}, zero,
                        &flags, &linear, &angular);
    expect("base-rotated linear", linear, 0.0f, 0.0f, 1.0f);

    /* Pose composition, for the record: inverse(base) * wire * offset. The point is at world X=3, 2 m on
     * the -X side of the base at X=5; the base's inverse yaw maps world -X to its -Z. */
    XrPosef wire = {identity.orientation, {2.0f, 0.0f, 0.0f}};
    XrPosef pose = qb_pose_mul(qb_pose_mul(qb_pose_inv(base_yaw), wire), offset_x);
    expect("composed position", pose.position, 0.0f, 0.0f, -2.0f);

    if (failures) return 1;
    puts("PASS: qb_math velocity composition");
    return 0;
}

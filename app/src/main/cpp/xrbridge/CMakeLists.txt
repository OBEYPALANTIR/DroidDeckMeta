# ── XR bridge, app side (PLAN.md section 3) ──────────────────────────────────
# xr_session.cpp drives the headset's OpenXR runtime from this process through the Khronos loader
# (prefab, from the AAR in app/build.gradle); bridge_service.cpp is the xr-bridge-service a Linux
# guest's OpenXR runtime talks to, bridge_jni.cpp its JNI face (XrBridge.kt); sample_main.cpp is the
# hidden XrSampleActivity's native half. Vulkan is the system loader, opened with dlopen like
# deviceinfo, so the library still loads on a phone without it - and never Turnip: the runtime
# composites from the system driver's images. libnativewindow's AHardwareBuffer_getNativeHandle is
# dlsym'd as in waylandcomp; the public AHardwareBuffer calls come through libandroid.
find_package(OpenXR REQUIRED CONFIG)

add_library(xrbridge SHARED
        xr_session.cpp
        bridge_service.cpp
        bridge_jni.cpp
        sample_main.cpp
)
target_compile_options(xrbridge PRIVATE
        -std=c++17 -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
target_include_directories(xrbridge PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/openxr/include
        # The guest <-> service wire format; the bridge service is built into this library.
        ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../tools/questbridge/protocol
)
target_link_libraries(xrbridge
        OpenXR::openxr_loader
        android
        dl
        log
)

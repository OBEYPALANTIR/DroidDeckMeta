// XrBridge.kt's native half: one xrbridge::BridgeService at a time, behind the four calls of the
// contract (nativeStart, nativePanelSurface, nativeState, nativeStop).
#include "bridge_service.h"

#include <android/log.h>

#include <memory>
#include <mutex>
#include <string>
#include <utility>

#define TAG "xrbridge"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

namespace {

using xrbridge::BridgeService;

std::mutex g_lock;  // every native method
std::unique_ptr<BridgeService> g_service;
jobject g_activity = nullptr;  // global reference, as long as g_service lives

void stopService(JNIEnv *env) {
    if (!g_service) return;
    g_service->stop();
    g_service.reset();
    env->DeleteGlobalRef(std::exchange(g_activity, nullptr));
}

}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_com_droiddeck_launcher_xr_XrBridge_nativeStart(JNIEnv *env, jobject, jobject activity, jstring socketName, jint panelWidth,
                                                    jint panelHeight) {
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_service) {
        const int state = g_service->state();
        if (state == BridgeService::kRunning || state == BridgeService::kGuestConnected) return JNI_TRUE;  // a resume without the pause
        stopService(env);  // ended or failed: start over
    }
    JavaVM *vm = nullptr;
    if (!activity || !socketName || panelWidth <= 0 || panelHeight <= 0 || env->GetJavaVM(&vm) != JNI_OK) {
        LOGE("bridge: nativeStart: no activity, socket name, panel size or JavaVM");
        return JNI_FALSE;
    }
    const char *chars = env->GetStringUTFChars(socketName, nullptr);
    if (!chars) return JNI_FALSE;  // OutOfMemoryError pending
    const std::string name(chars);
    env->ReleaseStringUTFChars(socketName, chars);
    g_activity = env->NewGlobalRef(activity);
    g_service = std::make_unique<BridgeService>();
    // A failed service stays until nativeStop or the next nativeStart, so nativeState() reads -1.
    return g_service->start(vm, g_activity, name, static_cast<uint32_t>(panelWidth), static_cast<uint32_t>(panelHeight)) ? JNI_TRUE
                                                                                                                         : JNI_FALSE;
}

extern "C" JNIEXPORT jobject JNICALL Java_com_droiddeck_launcher_xr_XrBridge_nativePanelSurface(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_service ? g_service->panelSurface(env) : nullptr;
}

extern "C" JNIEXPORT jint JNICALL Java_com_droiddeck_launcher_xr_XrBridge_nativeState(JNIEnv *, jobject) {
    std::lock_guard<std::mutex> lock(g_lock);
    return g_service ? g_service->state() : BridgeService::kIdle;
}

extern "C" JNIEXPORT void JNICALL Java_com_droiddeck_launcher_xr_XrBridge_nativeStop(JNIEnv *env, jobject) {
    std::lock_guard<std::mutex> lock(g_lock);
    stopService(env);
}

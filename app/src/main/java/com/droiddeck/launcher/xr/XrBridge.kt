package com.droiddeck.launcher.xr

import android.app.Activity
import android.view.Surface

/**
 * The xr-bridge-service (PLAN.md section 3 item 2, cpp/xrbridge/bridge_jni.cpp): one OpenXR session
 * on the headset's runtime that shows the Steam panel as a quad and composites the frames of a Linux
 * guest's OpenXR runtime connecting to an abstract socket. Log tag `xrbridge`.
 */
object XrBridge {
    init {
        System.loadLibrary("xrbridge")
    }

    /** Starts the session and listens on [socketName] (`@name`: abstract); false when the session cannot be created. */
    external fun nativeStart(activity: Activity, socketName: String, panelWidth: Int, panelHeight: Int): Boolean

    /** The Surface the Steam panel is drawn into; null before the session runs or without XR_KHR_android_surface_swapchain. */
    external fun nativePanelSurface(): Surface?

    /** 0 idle, 1 starting, 2 running (panel only), 3 guest connected, 4 exiting, -1 failed. */
    external fun nativeState(): Int

    /** Ends the session and joins its threads; safe to call twice. */
    external fun nativeStop()
}

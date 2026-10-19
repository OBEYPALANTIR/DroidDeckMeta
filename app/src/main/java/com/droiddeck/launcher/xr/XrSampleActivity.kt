package com.droiddeck.launcher.xr

import android.app.Activity
import android.os.Bundle
import android.util.Log
import android.view.WindowManager

/**
 * Hidden OpenXR sample (PLAN.md Loop 2 item 1): proves the headset's OpenXR runtime can be driven
 * from this process. Nothing in the app starts it; on a Quest,
 * `adb shell am start -n <package>/com.droiddeck.launcher.xr.XrSampleActivity`, then
 * `adb logcat -s xrbridge`. The native half is cpp/xrbridge/sample_main.cpp.
 */
class XrSampleActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        try {
            System.loadLibrary("xrbridge")
        } catch (e: UnsatisfiedLinkError) {
            Log.e("xrbridge", "libxrbridge.so did not load", e)
            finish()
        }
    }

    override fun onResume() {
        super.onResume()
        nativeStart(this)
    }

    override fun onPause() {
        nativeStop()
        super.onPause()
    }

    private external fun nativeStart(activity: Activity)

    private external fun nativeStop()
}

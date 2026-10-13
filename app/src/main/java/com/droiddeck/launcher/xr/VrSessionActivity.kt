package com.droiddeck.launcher.xr

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import android.view.WindowManager
import android.widget.Toast
import com.droiddeck.launcher.R
import com.droiddeck.launcher.core.FileUtils
import com.droiddeck.launcher.gpu.TurnipDriver
import com.droiddeck.launcher.runtime.LinuxRuntimeInstaller
import com.droiddeck.launcher.session.SessionEvents
import com.droiddeck.launcher.session.SessionPaths
import com.droiddeck.launcher.session.SessionPhase
import com.droiddeck.launcher.session.SessionPrefs
import com.droiddeck.launcher.session.SessionService
import com.droiddeck.launcher.session.SessionState
import com.droiddeck.launcher.session.SteamRepair
import com.droiddeck.launcher.wayland.CompositorHost
import com.droiddeck.launcher.wayland.WaylandCompositor
import java.io.File
import java.util.concurrent.Executors

/**
 * The Steam session in the headset (PLAN.md Loop 2 item 8, SessionService.MODE_VR): the XR bridge's
 * OpenXR session on Meta's runtime, and the session's compositor drawing Steam into the Surface
 * behind the bridge's panel quad layer. Everything below the compositor is SessionService's, as for
 * SessionActivity. Leaving this activity ends the session: there is nowhere else to show it.
 * Opened by `droiddeckctl start vr` (AgentStartActivity) and by the session's notification.
 */
class VrSessionActivity : Activity() {
    private val handler = Handler(Looper.getMainLooper())
    private var panel: Surface? = null
    private var resumed = false
    private var presenting = false
    private var bridgeStarted = false
    /** This activity started the session or joined it, so leaving it stops the session. */
    private var inSession = false

    private val endListener: (Int) -> Unit = { status ->
        runOnUiThread {
            inSession = false
            Log.i(TAG, "the session ended with status $status")
            // Big Picture's Switch to Desktop: the flat session it asked for starts in this one's place.
            if (status == 0) SessionState.relaunch?.let { next ->
                SessionState.relaunch = null
                startActivity(next)
            }
            finish()
        }
    }
    private val firstFrame = Runnable {
        SessionEvents.firstFrame()
        SteamRepair.clientShown(this)
    }

    private val poll = object : Runnable {
        override fun run() {
            // Read before the Surface: the bridge makes the panel before it first reports running.
            val state = XrBridge.nativeState()
            if (state == STATE_EXITING || state == STATE_FAILED) {
                Log.i(TAG, "the XR session ended (bridge state $state)")
                finish()
                return
            }
            if (panel == null) {
                panel = XrBridge.nativePanelSurface()
                if (panel == null && state >= STATE_RUNNING) {
                    unavailable("no panel Surface: the runtime lacks XR_KHR_android_surface_swapchain")
                    return
                }
            }
            present()
            handler.postDelayed(this, POLL_MS)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        if (LinuxRuntimeInstaller.isRemoving() || LinuxRuntimeInstaller.isMaintaining()) {
            Toast.makeText(this, R.string.session_wait_removal, Toast.LENGTH_SHORT).show()
            finish()
            return
        }
        WaylandCompositor.setFirstFrameListener(firstFrame)
        SessionState.endListener = endListener
        bridgeStarted = true
        // Off the main thread: nativeStart waits out the whole OpenXR and Vulkan start, which may need
        // this looper. One thread for the bridge's calls keeps a stop behind its start.
        BRIDGE.execute {
            val started = runCatching {
                XrBridge.nativeStart(this, "@$packageName.xr-bridge", PANEL_WIDTH, PANEL_HEIGHT)
            }.onFailure { Log.e(TAG, "libxrbridge", it) }.getOrDefault(false)
            handler.post {
                if (isFinishing || isDestroyed) return@post
                if (started) poll.run() else unavailable("the XR bridge could not create its OpenXR session")
            }
        }
    }

    override fun onResume() {
        super.onResume()
        resumed = true
        // A session paused by the screen going off (the headset taken off) has no Resume to press in here.
        if (SessionState.suspended) SessionService.resume(this)
        present()
    }

    override fun onPause() {
        resumed = false
        present()
        super.onPause()
    }

    override fun onDestroy() {
        handler.removeCallbacks(poll)
        // The panel Surface goes with the bridge's session; onPause has already taken the compositor off it.
        if (bridgeStarted) BRIDGE.execute {
            runCatching { XrBridge.nativeStop() }.onFailure { Log.w(TAG, "the XR bridge did not stop", it) }
        }
        // As SessionActivity's Stop does.
        if (inSession) SessionService.stop(this)
        if (SessionState.endListener === endListener) SessionState.endListener = null
        WaylandCompositor.clearFirstFrameListener(firstFrame)
        super.onDestroy()
    }

    /**
     * The compositor draws into the panel only while this activity is in front: the runtime takes no
     * panel frames otherwise, and the bridge's state stays running (2) or guest connected (3) through
     * Meta's own STOPPING and IDLE, so the activity's pause is the signal.
     */
    private fun present() {
        val surface = panel ?: return
        val visible = resumed && !isFinishing && XrBridge.nativeState() in STATE_RUNNING..STATE_GUEST
        if (visible == presenting) return
        presenting = visible
        if (visible) attach(surface) else CompositorHost.detach(surface)
    }

    /** SessionActivity.surfaceCreated for the panel: the compositor on its Surface, then the session. */
    private fun attach(surface: Surface) {
        val starting = !inSession && !SessionState.running
        if (starting) {
            if (SessionState.phase in setOf(SessionPhase.IDLE, SessionPhase.FAILED)) {
                // A Steam session to everything but the guest's command line; its folder says vr.
                SessionEvents.begin(this, SessionService.MODE_STEAM, SessionPaths.label(SessionService.MODE_VR, null))
            }
            SessionEvents.transition(SessionPhase.STARTING_COMPOSITOR, "compositor.starting")
            // The panel is the whole output, so no Follow screen. The rate is Meta's default display
            // rate, 72 Hz unless the bridge asks for another; the Android display reports something else.
            SessionState.outputSize = PANEL_WIDTH to PANEL_HEIGHT
            SessionState.followScreen = false
            SessionState.refreshHz = PANEL_HZ
            SessionState.fpsLimit = SessionPrefs.fpsLimit(this, SessionService.MODE_STEAM)
        }
        val runtimeDir = File(filesDir, ".wayland-rt").apply { mkdirs() }
        // The compositor hands this keymap to wl_keyboard clients, which is how the guest reads the
        // evdev codes injected into it (droiddeckctl input key/text in here).
        FileUtils.copyAsset(this, "wayland/keymap.xkb", File(runtimeDir, "keymap.xkb"))
        // Turnip, not the system driver: importing gamescope's dma-bufs needs VK_EXT_image_drm_format_modifier.
        val turnip = TurnipDriver(this)
        val driverId = if (CompositorHost.isStarted) null else turnip.install()
        if (starting) {
            // The compositor opens its log once, when it starts, and the session's folder is claimed now.
            val waylandLog = File(SessionPaths.beginOrCurrent(this), "wayland.log")
            WaylandCompositor.setSessionLogFile(waylandLog)
            runCatching { android.system.Os.setenv("BL_WAYLAND_LOG", waylandLog.path, true) }
                .onFailure { Log.w(TAG, "could not point the compositor's log at $waylandLog", it) }
        }
        // The first-frame notice is one-shot in native code and the listener is process-wide: an
        // earlier session may have spent the notice, and a pause/resume re-attach below never
        // re-arms it (SessionService.start, the only other re-arm, runs only when starting). While
        // this session still waits for its first frame, re-register and re-arm so its frames fire.
        WaylandCompositor.setFirstFrameListener(firstFrame)
        if (!SessionState.firstFrameSeen) WaylandCompositor.nativeResetFirstFrame()
        SessionEvents.markReadyIfPossible()
        // A zero-copy game goes onto an Android layer of its own, which the headset never shows.
        WaylandCompositor.nativeSetZeroCopy(false)
        CompositorHost.startOrAttach(
            surface, runtimeDir.path,
            driverId?.let { turnip.driverPath(it) }, driverId?.let { turnip.libraryName(it) },
            applicationInfo.nativeLibraryDir, PANEL_WIDTH, PANEL_HEIGHT, SessionState.refreshHz, SessionState.fpsLimit,
        )
        if (starting) {
            SessionEvents.record("compositor.started")
            SessionEvents.transition(SessionPhase.STARTING_GUEST, "guest.starting")
            SessionService.start(
                this, SessionService.MODE_VR,
                steamUi = intent.getStringExtra(SessionService.EXTRA_STEAM_UI),
                steamUrl = intent.getStringExtra(SessionService.EXTRA_STEAM_URL),
            )
        }
        inSession = true
    }

    private fun unavailable(reason: String) {
        Log.e(TAG, reason)
        Toast.makeText(this, R.string.xr_bridge_unavailable, Toast.LENGTH_LONG).show()
        finish()
    }

    companion object {
        private const val TAG = "VrSessionActivity"
        /** The Steam panel's quad layer, and with it the session's output size. */
        private const val PANEL_WIDTH = 1920
        private const val PANEL_HEIGHT = 1080
        private const val PANEL_HZ = 72f
        private const val POLL_MS = 50L
        // XrBridge.nativeState(): 0 idle, 1 starting, 2 running, 3 guest connected, 4 exiting, -1 failed.
        private const val STATE_RUNNING = 2
        private const val STATE_GUEST = 3
        private const val STATE_EXITING = 4
        private const val STATE_FAILED = -1
        private val BRIDGE = Executors.newSingleThreadExecutor { Thread(it, "xrbridge-control") }
    }
}

# Experimental Quest 3/3S support

Quest 3/3S support is experimental. Tier A runs DroidDeck as a 2D panel;
this fork also carries an immersive XR bridge (Tier B, below). Quest 2 is
outside the bundled Adreno 7xx driver path.

Install the standard DroidDeck APK on Quest 3/3S. Device detection on the
first launch selects manual drivers, the bundled
`turnip-quest` Android driver and the default Linux driver. It enables
`hdr.steam` to use the existing SurfaceControl presentation workaround from
[the Quest report](https://github.com/Droid-Deck/DroidDeck/issues/245).
These defaults apply once; subsequent user changes are retained. Colour
accuracy and headset performance still need testing.

Both the Android driver wrapper and the Linux session preload use
`tools/quest/kgsl_compat.c`. Successful legacy allocation/free calls are
unchanged. Only `EACCES`/`EPERM` failures retry through `GPUOBJ_ALLOC`,
`GPUOBJ_INFO` and `GPUOBJ_FREE`. Unhandled controller ioctls forward to the next
preload hook rather than skipping it with a raw syscall.

## The Android driver (`turnip-quest`)

The app-side compositor renders through this bionic Turnip; it is what puts every
frame on the panel, so a layout mistake here faults the GPU. The first Quest
adapter (`turnip25.1.0-quest1`) wrapped upstream's phone driver, really
`Mesa 25.0.0-devel (git-365cd04375)`, whose `u_gralloc` fallback identifies a QTI
gralloc buffer only when the first int after the fds is the `'gmsm'` magic
(`src/util/u_gralloc/u_gralloc_fallback.c`). Quest's gralloc (`libgralloccore.so`,
vros 209) builds handles as `{fd, fd_metadata, 1, 'gmsm', flags, ...}` (2 fds /
27 ints), so the magic sits one int later and the modifier stays
`DRM_FORMAT_MOD_INVALID`, for which Turnip's default layout is tiled + UBWC. The
app's own sniff (`sc_layer.c`) logged the same miss: `gralloc handle layout
unknown (2 fds, 27 ints)`. The rest is inferred from the fault geometry: the XR
panel swapchain's 1920x1080 buffers map as exactly 0x7E9000 bytes (linear), the
UBWC layout needs 0x801000, and every fault sat `+0x280..+0x15680` past a buffer
end; a normal 2D window presumably gets UBWC buffers, which is why the wrong guess
held there. No fault address was bound to a specific buffer. What is established
is attribution: the same Mesa build with and without the sniff fix, on the same
headset, faults within 2 s of the first frame versus running 120 s clean (dossier
`features/loop5-driver-fix.md`).

`turnip-quest` is Mesa built from upstream DroidDeck's driver recipe
([Droid-Deck/Drivers-CI](https://github.com/Droid-Deck/Drivers-CI), the
`DD-Turnip-vX.Y.Z-source.tar.xz` release asset: Mesa plus DroidDeck's patch
series, including the FD740 UBWC hint and the KGSL wait fixes) with
`tools/quest/mesa-patches/gralloc_ubwc_detect.py` overlaid: the fallback sniff
accepts the magic at either slot and reads the UBWC flag from the int after it,
keeping DD-Turnip's gmsm-less read for newer phone grallocs. The base driver is
checked in as `tools/quest/turnip-quest-base.tzst` (adrenotools `meta.json`,
`libvulkan_freedreno.so`, the recipe's `NOTICE`, Mesa's licence overview as
`LICENSE.mesa`, and `PROVENANCE`: source tarball SHA-256, Mesa version, NDK and
overlay patch hashes; the binary is not bit-reproducible between builds).
`tools/build_local.sh` and CI wrap it with the KGSL adapter using the pinned NDK,
carrying those files along, and runtime input verification attests the result.
The Corresponding Source for the driver is the DD-Turnip source tarball named in
`PROVENANCE`; the patch overlay is in this repository.
A bundled driver is re-unpacked whenever the APK carrying it changes
(`TurnipDriver.install` stamps the directory with the package update time).

```sh
# Rebuild the base driver from a DD-Turnip source tarball into a new work directory
# (macOS: brew install meson ninja bison glslang pkgconf; the script never deletes anything)
tools/quest/build-mesa-android.sh DD-Turnip-v0.2.0-source.tar.xz "$ANDROID_HOME/ndk/27.0.12077973" /tmp/quest-mesa
cp /tmp/quest-mesa/out/turnip-quest-base.tzst tools/quest/
# Rebuild only the wrapped Android driver archive
python3 tools/quest/build-driver.py "$ANDROID_HOME/ndk/27.0.12077973"
```

Installs that stored `androidDriver=turnip25.1.0-quest1` are moved to `turnip-quest`
on the next start. The guest's Linux Turnip is a separate copy and is unchanged.

Quest compatibility is included in the standard APK; no separate Quest
package is needed. Other devices retain their existing defaults. The remaining
performance-name APK variants continue to use separate app data and bwrap sockets.

## Observed headset status

Last updated: 2026-10-13. On the tested Quest 3 (Horizon vros 209), Steam is
signed in and Skyrim VR is installed. Sign-in/install are no longer blockers.
Skyrim reached OpenXR through Proton ARM64/xrizer, but DXVK device creation
failed before game rendering; no Skyrim FPS measurement exists.
The native samples below do not establish real-game or Quest 3S compatibility.

Restoring `turnip25.1.0-quest1` and `hdr.steam=true`, then cold-starting the app,
reached READY/firstFrame in about 8 s in session `2026-10-13-11-vr`.
READY recovery did **not** establish GPU reliability: the same session recorded
an app-side compositor fault storm, then a sensor/camera HAL abort and app
termination. This is correlation, not proven causality. The archive's Mesa 25.1
label was subsequently corrected by binary inspection (above). Neither shim
ownership nor native WSI layout/sizing is proven as the cause. That historical
setting combination is not proof the immersive panel needs HDR.

Local evidence root `D`: `~/.agents/artifacts/2026-10-12/droiddeckmeta`.
See `D/features/loop3-skyrimvr.md`, `D/features/loop3-dxvk-bisect.md`,
`D/ledger.jsonl` (session 11) and
`D/evidence/device/20261013-030811-skyrimvr-3/`.
Use scrubbed Share logs when reporting failures; keep raw session logs private.
Host allocation/preload tests do not establish headset operation.

Synchronization/import source repairs and VR startup guards are present, but
their runtime reliability is unproved. The attempted two-minute smoke at
`D/evidence/device/20261013-045928-sync-panel-smoke/` timed out launching:
185.7 s elapsed, 0 s panel time, no firstFrame, `clean_120s=false`.
Null fault fields and empty logs are not a clean run. The headset was asleep;
the session was stopped. No further blind fault run is authorized.

The APK also carries a hidden OpenXR probe for the XR bridge work,
`com.droiddeck.launcher.xr.XrSampleActivity` (`app/src/main/cpp/xrbridge`). The
app never starts it. Over ADB,
`adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.xr.XrSampleActivity`
opens an immersive session on Meta's runtime through the Khronos loader. It
clears the left eye red and the right eye blue, pulses a controller when its
trigger is pulled, and logs the runtime's extensions, swapchain formats, view
poses and controller state to `adb logcat -s xrbridge OpenXR-Loader`. Quit it
from the system menu. It is a probe, not proof of real-game compatibility.

## Immersive VR (fork)

`droiddeckctl start vr` opens an immersive session with the Steam panel as a
quad layer. Native `hello_xr -g Vulkan2` rendered at ~72 guest frames/s
(0 dropped over 20 s); the OpenVR Vulkan smoke through xrizer rendered at
67–73 guest frames/s with 0 submit errors over 60 s. These are sample results,
not Skyrim performance. Evidence: `D/features.md` L2-3 and
`D/features/loop3-openvr-smoke.md`.
See [Immersive VR](vr.md) for the game matrix, architecture and limitations.

VR pins `turnip-quest` without overwriting the user's 2D driver preference.
Its panel is SDR, with VR HDR state explicitly disabled; saved `hdr.steam`
remains a 2D preference. If an older/incompatible process-wide compositor is
still present (including an open HDR gate or a completed VR session), VR refuses
startup and logs a cold-restart reason. It neither silently exits the app
process nor stops an unrelated guest. End only the intended session, fully
close/reopen the app, and start VR with the headset awake; activity reattachment
alone cannot replace the compositor. These guards do not establish GPU safety.

## Proton games on Quest 3

These rendering/presentation experiments are not validated fixes for Skyrim's
device-creation failure (PLAN.md section 12.A.5):

```sh
tools/droiddeckctl env set DXVK_DISABLE_TIMELINE_SEMAPHORES=1
tools/droiddeckctl env set TU_DEBUG=noconform
tools/droiddeckctl env set MESA_VK_WSI_PRESENT_MODE=mailbox
```


DXVK requested a 37-extension list containing the three win32 external-memory,
external-semaphore and keyed-mutex extensions; safe mode repeated the failure.
Native guest Turnip 26.2.2 created a VkDevice with the 34 non-win32 names but
rejected all 37. This does not prove which Windows/Proton layer is responsible.
`DXVK_NO_VR=1` is an **explicit opt-in diagnostic only**, not a shipping default
with xrizer. Unset remains unset; user `1`/`0` overrides are preserved. It disables
DXVK's VR-extension provider, not the bridge. Session 11 was interrupted before
DXVK; game device creation and shared-texture negotiation remain **unvalidated**.
See `D/features/loop3-dxvk-options.md` and `D/features/loop3-dxvk-bisect.md`.

## OS boundary

Replacing Horizon with Steam Frame's OS depends on a bootloader route and Quest
display/tracking/controller drivers and firmware. Linux userspace under Horizon
remains the approach; Meta's runtime continues to own display and tracking.
See `D/features/steam-frame-os-port.md`.

# Experimental Quest 3/3S support

Quest 3/3S runs DroidDeck as a 2D panel. This does not add immersive VR or
OpenXR support. Quest 2 is outside the bundled Adreno 7xx driver path.

Install the standard DroidDeck APK on Quest 3/3S. Device detection on the
first launch selects manual drivers, the bundled
`turnip25.1.0-quest1` Android driver and the default Linux driver. It enables
`hdr.steam` to use the existing SurfaceControl presentation workaround from
[the Quest report](https://github.com/Droid-Deck/DroidDeck/issues/245).
These defaults apply once; subsequent user changes are retained. Colour
accuracy and headset performance still need testing.

Both the Android driver wrapper and the Linux session preload use
`tools/quest/kgsl_compat.c`. Successful legacy allocation/free calls are
unchanged. Only `EACCES`/`EPERM` failures retry through `GPUOBJ_ALLOC`,
`GPUOBJ_INFO` and `GPUOBJ_FREE`. The Android wrapper depends on the original
bundled Turnip binary. Unhandled controller ioctls forward to the next
preload hook rather than skipping it with a raw syscall.

`tools/build_local.sh` and CI build the wrapper from source using the pinned
Android NDK; the normal preload builds include the shared Linux adapter.
Runtime input verification checks the resulting driver archive and invalidates
stale prepared assets after compatibility sources change. To rebuild only the
Android driver archive:

```sh
python3 tools/quest/build-driver.py "$ANDROID_HOME/ndk/27.3.13750724"
```

Quest compatibility is included in the standard APK; no separate Quest
package is needed. Other devices retain their existing defaults. The remaining
performance-name APK variants continue to use separate app data and bwrap sockets.

Quest 3 testers have successfully launched games.

On a headset, check the Steam login panel, sign-in, game installation and
then an owned game's rendering/input. Capture the device/OS, session logs,
`files/quest-startup.txt` and any `files/quest-last-crash.txt` failure.
Host allocation and preload-chain tests do not establish headset operation.

The APK also carries a hidden OpenXR probe for the XR bridge work,
`com.droiddeck.launcher.xr.XrSampleActivity` (`app/src/main/cpp/xrbridge`). The
app never starts it. Over ADB,
`adb shell am start -n com.droiddeck.launcher/com.droiddeck.launcher.xr.XrSampleActivity`
opens an immersive session on Meta's runtime through the Khronos loader. It
clears the left eye red and the right eye blue, pulses a controller when its
trigger is pulled, and logs the runtime's extensions, swapchain formats, view
poses and controller state to `adb logcat -s xrbridge OpenXR-Loader`. Quit it
from the system menu. It is a probe, not VR support.

## Immersive VR (fork)

Tier A above is the 2D panel. On Quest 3 (vros 209) the Steam login panel
reached READY twice with stock settings; sign-in, game install and game
launch are untested (user credentials). The immersive side, Tier B, is the
fork's XR bridge: `droiddeckctl start vr` opens an immersive session with
the Steam panel as a quad layer, and guest OpenXR titles render through it
(hello_xr at 72 fps, 0 dropped). See `docs/vr.md` for architecture,
commands, build, status and limits.

## Proton games on Quest 3

Untested on DroidDeck's DXVK/Turnip builds (PLAN.md section 12.A.5):

```sh
tools/droiddeckctl env set DXVK_DISABLE_TIMELINE_SEMAPHORES=1
tools/droiddeckctl env set TU_DEBUG=noconform
tools/droiddeckctl env set MESA_VK_WSI_PRESENT_MODE=mailbox
```


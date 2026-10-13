# Immersive VR on Quest 3 (fork)

Tier B of the fork: SteamVR titles running standalone on a Meta Quest 3,
without a PC or streaming. Native arm64 OpenXR games run against a bridge
runtime in the Linux guest; OpenVR games reach the same runtime through an
arm64 xrizer build. Windows VR titles under Proton ARM64 are investigation
only (see "Limitations").

The bridge is GPL-3.0 like the rest of the fork. Valve's Steam client,
Proton and SteamVR, and Meta's OpenXR runtime stay proprietary inputs: used,
not redistributed. The Steam Frame SteamVR package is not copied into the
APK or the runtime (PLAN.md section 9).

## Architecture

The app owns the one real OpenXR session with Meta's runtime. The guest
runs `libopenxr_questbridge.so`, an OpenXR runtime that marshals every call
to the app over an abstract `SOCK_SEQPACKET` socket. The wire is
`tools/questbridge/protocol/questbridge_protocol.h`, the one header both
sides include.

```
 Horizon OS (Android 14, SELinux enforcing)
 ┌───────────────────────────────────────────────────────────────────┐
 │ DroidDeck app (bionic)                                            │
 │  VrSessionActivity (immersive, Meta XR)                           │
 │  xr-bridge-service (app/src/main/cpp/xrbridge)                    │
 │  ├ one OpenXR session on Meta's runtime (Khronos loader 1.1.63)   │
 │  ├ Steam panel as a Surface swapchain → 1920x1080 quad layer      │
 │  │  (1.6 m wide, 1.5 m ahead of where the session started)        │
 │  └ swapchain images = gralloc AHB-backed VkImages                 │
 └──────────────┬──────────────────┬─────────────────────────────────┘
                │ proot            │ abstract socket + fd passing
 ┌──────────────▼──────────────────▼─────────────────────────────────┐
 │ Linux guest (glibc, Arch ARM)                                      │
 │  gamescope ─ Xwayland ─ Steam client (arm64)                       │
 │  libopenxr_questbridge.so (+ questbridge.json, XR_RUNTIME_JSON)    │
 │  xrizer bin/linuxarm64/vrclient.so (OpenVR → OpenXR)               │
 │  native arm64 OpenXR games ── hello_xr -g Vulkan2                  │
 └───────────────────────────────────────────────────────────────────┘
```

Frames are pipelined one compositor frame by design (protocol header):
the guest's `WAIT_FRAME` returns when the service's next real `xrWaitFrame`
returns, `END_FRAME` hands over a layer list plus a release fence, and the
service submits the newest complete guest frame on its next real frame.
While no guest frame is in flight the service shows the Steam panel quad.

Swapchain images are gralloc buffers the service allocates
(`GPU_COLOR_OUTPUT | GPU_SAMPLED_IMAGE`). Each image's dma-buf fd crosses
the socket once; the guest's Turnip imports it with
`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`.
Rendering completion crosses as a sync_file fd on `END_FRAME`; the service
copies the referenced images into the runtime's own swapchains, and an
image index is handed back by `ACQUIRE_IMAGE` only once that copy has
finished. Array swapchains are flattened side by side guest-side
(`sub_image.x = layer * width`); depth swapchains stay guest-only and never
reach the service (`tools/questbridge/runtime/qb_session.c`).

## How to run

Start the VR session from an authorized ADB host:

```sh
tools/droiddeckctl start vr --wait
```

This opens `VrSessionActivity` (immersive, Steam panel as the quad layer)
and starts the bridge service listening on the abstract socket
`@<package>.xr-bridge`. Inside the guest the `vr` session mode
(`vr_environment` in `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`)
exports `XR_RUNTIME_JSON=/usr/local/lib/droiddeck-xr/questbridge.json`,
`DROIDDECK_XR_SOCKET` for that socket, prepends the bridge dir to
`LD_LIBRARY_PATH`, and writes `~/.config/openvr/openvrpaths.vrpath`
pointing OpenVR titles at `/usr/local/lib/xrizer`.

Check the bridge with the Khronos sample. `hello_xr`'s POSIX build quits
on stdin EOF, so keep stdin open:

```sh
tools/droiddeckctl guest -- sh -c '(sleep 20 | droiddeck-hello-xr)'
```

Logs: `adb logcat -s xrbridge` (bridge service and sample; the Khronos
loader logs as `OpenXR-Loader`), and the session log via
`tools/droiddeckctl logs latest ./session-artifacts`.

Gralloc buffers are linear (CPU usage bits) by default. To let gralloc
compress (UBWC):

```sh
tools/droiddeckctl env set DROIDDECK_XR_UBWC=1
```

or `setprop debug.droiddeck.xr.ubwc 1`. Off by default because the guest
Turnip reading QTI's UBWC is unproven on Quest
(`app/src/main/cpp/xrbridge/bridge_service.cpp`).

## Building

Two scripts build the guest pieces into `app/build/questbridge/linuxfs`:

```sh
bash tools/questbridge/build-guest.sh
bash tools/questbridge/xrizer/build-xrizer.sh
```

`build-guest.sh` builds the Khronos loader, `hello_xr` and
`libopenxr_questbridge.so` plus the `droiddeck-hello-xr` wrapper.
`build-xrizer.sh` builds xrizer's `bin/linuxarm64/vrclient.so` unpatched.
Run xrizer second: `build-guest.sh` recreates the output tree from scratch.
Both build natively on arm64 hosts and cross on x86_64
(`DROIDDECK_GUEST_CROSS=1` forces cross).

`tools/build_local.sh` runs both scripts and lays the result over
`app/src/main/assets/linuxfs/`; CI does the same
(`.github/workflows/build.yml`, "Build the guest OpenXR pieces").
`SessionFiles.kt` stages the files into installed runtimes:
`/usr/local/bin/droiddeck-hello-xr` and
`/usr/local/lib/droiddeck-xr/{hello_xr, libopenxr_loader.so.1,
libopenxr_questbridge.so, questbridge.json}` plus xrizer's
`bin/linuxarm64/vrclient.so`.

On Apple silicon, Docker must run arm64 natively: start colima with
Rosetta before building:

```sh
colima start --vz-rosetta --memory 8 --cpu 8
```

Without it the cross toolchain runs under QEMU, where the amd64 g++
segfaults at random and cargo is starved or deadlocks (see the headers of
both build scripts). JDK 17 via Homebrew; pass `-PndkVersion` for the NDK
actually installed.

## Status

From the dossier (`features.md`), Quest 3 vros 209, app 0.4.0:

| id | status | result |
|---|---|---|
| L2-1 | answered | Sample XR app driven from the DroidDeck process (targetSdk 28): instance, FOCUSED session, 2x1680x1760 RGBA8 sRGB swapchains, 72/80/90/120 Hz, 72 Hz frame loop |
| L2-2 | answered | Service allocates linear gralloc AHBs (stride 6912, 12165120 B); dma-buf fds cross the socket; guest Turnip imports them; `xrCreateSwapchain` succeeds for both eyes |
| L2-3 | answered | 72 guest frames/s, 144 copies/s, 0 dropped over 20 s (dips to 66-69 in some seconds = guest pacing); Phase 2 go |
| L2-4 | partial | Views/spaces/poses flow (hand cubes at controller poses); button/axis/haptic state needs a person holding the controllers |
| L2-5 | answered | `libopenxr_questbridge.so` (69 entry points) + loader + hello_xr built natively, staged into the installed runtime by SessionFiles |
| L2-6 | answered | xrizer builds unpatched for aarch64; bundled as `/usr/local/lib/xrizer`; runtime smoke pending |
| L2-7 | partial | Proton ARM64 VR path is investigation only, no device run |
| L2-8 | answered | `droiddeckctl start vr` → READY + firstFrame; panel quad at 72 Hz; guest layers replace the panel while a guest session runs |

Measured numbers: 72 guest fps, 144 copies/s, 0 dropped over 20 s; one
compositor frame of pipelining latency by design.
The KGSL policy denial is confirmed on device: `GPUMEM_ALLOC_ID`/`FREE_ID`
return EACCES and the compat path fires (PLAN.md section 12.A.1).

## Windows VR titles (Proton)

Now that xrizer ships as an aarch64 `vrclient.so`, two candidate paths exist,
both untested: needs a signed-in Steam with a VR title.

(a) OpenVR titles: `openvr_api.dll` -> Proton's `vrclient` thunk ->
`/usr/local/lib/xrizer/bin/linuxarm64/vrclient.so` (set by
`openvrpaths.vrpath`, which the `vr` session writes) -> bridge.
(b) OpenXR titles: `openxr_loader.dll` -> `wineopenxr` -> `XR_RUNTIME_JSON`
-> bridge.

Valve's Proton takes path (a) only when the prefix registry carries
`SteamGameId` and a working `vrclient.so`; DroidDeck's Proton builds may
differ, so check both before assuming the thunk loads.
Fallback: OpenComposite plus a Windows OpenXR runtime inside the prefix,
which avoids the thunks entirely (WinlatorXR's proven path). Also untested:
needs a signed-in Steam with a VR title.

## Quest 3 switches for Proton games

From PLAN.md section 12.A.5; each targets a WinlatorXR-observed symptom.
All three are untested on DroidDeck's DXVK/Turnip builds.

```sh
tools/droiddeckctl env set DXVK_DISABLE_TIMELINE_SEMAPHORES=1
tools/droiddeckctl env set TU_DEBUG=noconform
tools/droiddeckctl env set MESA_VK_WSI_PRESENT_MODE=mailbox
```

`DXVK_DISABLE_TIMELINE_SEMAPHORES=1` targets timeline-semaphore trouble on
Adreno 740 under Horizon; `TU_DEBUG=noconform` drops the sysmem path;
`MESA_VK_WSI_PRESENT_MODE=mailbox` matches WinlatorXR's present mode.

## Limitations and unknowns


- No virtual pad or Bluetooth gamepad in VR: `input pad`, `stick` and
  `trigger` answer `NO_PAD` in a vr session. Keys, text, taps and swipes
  reach the Steam panel. The panel is not operable from the headset yet;
  Touch reaches only VR titles through the bridge.
- `LOCATE_SPACE` and `LOCATE_VIEWS` report no velocities
  (`velocityFlags = 0`); xrizer throws are zero.
- At most 8 layers per frame (`QB_MAX_LAYERS`); array swapchains are
  flattened side by side; depth layers are not forwarded.
- Proton ARM64 Windows VR titles are investigation only; sign-in and game
  launch are untested (user credentials).
- Open unknowns that matter, in `unknowns.json`: L2-2-01 through L2-2-08
  (KGSL timestamp/sync interop, GPUOBJ import of foreign dma-bufs, UBWC
  modifiers, arraySize > 1, Turnip version match, service-side driver
  choice, dma_heap access), L2-6-01 through L2-6-08 (xrizer aarch64 tests,
  vtable dispatch, vulkan_enable v1, session choreography, binding sets,
  glibc floor, x86-64 titles under FEX, format/colour edge cases),
  L0-3-09 (immersive session from a targetSdk-28 APK on current Horizon
  OS), L0-4-01/L0-4-02 (2D activity state under the immersive activity).

## Re-sync cadence

Rebase `quest-experimental` on upstream `main` weekly. Upstream carries
the Quest 2D fixes (KGSL adapter, ioctl forwarding, Quest defaults); the
fork carries only what upstream declines: `tools/questbridge/`,
`app/src/main/cpp/xrbridge/`, the `xr/` Kotlin activities and the `vr`
mode in `droiddeck-session`. Feed every Tier A fix upstream as a PR.

## Reporting a headset run

Use the app's Share logs output for anything attached to an upstream
issue; keep raw `files/logs/` pulls private. Pull the session bundle:

```sh
tools/droiddeckctl logs latest ./session-artifacts
```

Every run is bound to the target record (PLAN.md section 4): device
(Quest 3 | 3S), Horizon build, kernel, GPU chip id, app commit and
variant, runtime versions (Turnip Android/Linux, gamescope, Proton, FEX),
bridge and xrizer commits, Meta runtime version, and the evidence ids. A
claim without a session zip and the app commit does not enter the ledger.

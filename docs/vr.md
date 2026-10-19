# Immersive VR on Quest 3 (fork)

Tier B of the fork targets SteamVR titles running standalone on a Meta Quest 3,
without a PC or streaming. Native arm64 OpenXR samples run against a bridge
runtime in the Linux guest; an OpenVR smoke sample reaches it through arm64
xrizer. These samples rendered at approximately 72 Hz. Windows Skyrim VR under
Proton reached OpenXR but failed DXVK device creation before rendering.

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

On Quest, VR pins `turnip-quest` for this session without changing the
user's saved 2D driver preference. The XR panel is SDR: VR disables the HDR
request and clears inherited HDR state, independently of `hdr.steam`.
The compositor is process-wide. If an older/incompatible compositor remains
(including an open HDR gate or a completed VR session), startup refuses with a
logged cold-restart reason; it does not silently exit the app process or stop an
unrelated guest. Stop the session you intend to end, fully close/reopen the app,
then start VR with the headset awake. Reopening only the activity is insufficient.

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

Last updated: 2026-10-13. Evidence below is from Quest 3, Horizon vros 209,
app 0.4.0; it does not establish Quest 3S or real-game compatibility.
`D` denotes the local dossier `~/.agents/artifacts/2026-10-12/droiddeckmeta`.

| Application / path | Observed result | Measured sample rate | Evidence in `D` |
|---|---|---|---|
| `hello_xr -g Vulkan2`, native arm64 OpenXR | Guest stereo frames rendered through the bridge | ~72 guest frames/s; 144 copies/s, 0 dropped over 20 s | `features.md` L2-3; `evidence/device/hello_xr-bridge.png` |
| OpenVR Vulkan smoke → xrizer → bridge | Stereo frames composited; 0 submit errors over 60 s | 67–73 guest frames/s; some seconds dropped 1–2 | `features/loop3-openvr-smoke.md`; `evidence/device/20261012-204838-openvr/` |
| Skyrim VR, Windows OpenVR / Proton ARM64 | Reached OpenXR; DXVK failed before swapchains/rendering, 0 guest frames | No game FPS measurement | `features/loop3-skyrimvr.md`; `features/loop3-dxvk-bisect.md` |

Sample throughput is not a Skyrim performance result. Views/controller poses
flowed; buttons, axes and haptics still need an awake-controller run.
The KGSL allocation compatibility path logged success, but the faulting buffer's
identity and allocation route were not traced; shim causality is unproved.

## Windows VR titles (Proton)

Steam is signed in and Skyrim VR is installed; credentials/install are no
longer blockers. The launch mapped Proton's `vrclient_x64.so`, arm64 xrizer
and the bridge; the game opened an OpenXR instance and briefly began a session.
No game swapchain or rendered guest frame was observed.

The subsequent DXVK log requested 37 device extensions, including
`VK_KHR_external_memory_win32`, `VK_KHR_external_semaphore_win32` and
`VK_KHR_win32_keyed_mutex`; the safe-mode retry repeated the same list and
`VK_ERROR_EXTENSION_NOT_PRESENT`. Native guest Turnip 26.2.2 probes created a
VkDevice with the 34 non-win32 extensions (`VK_SUCCESS`), while the full 37
failed. That isolates an extension-list difference, not the failing layer in
the installed Windows/Proton chain. The probes do not identify who inserted
the win32 names or prove any installed-layer ownership theory.
Evidence: `D/evidence/device/20261013T0656Z-dxvk-bisect/`.

A later isolated Windows DXVK probe created **both** FL11.1 devices with the
same 37 logged names, including that Win32 trio. Thus the native 37-versus-34
comparison is not proof that the trio causes the installed Proton failure.
With per-call native DLL overrides, shared NT-handle creation/open succeeded
but pixel comparison failed on cycle 0; without them, the control used builtin
WineD3D and returned `E_NOTIMPL`. The probe and Skyrim prefixes contain identical
`d3d11.dll`, `dxgi.dll` and `winevulkan.dll` bytes. This narrows a file-identity
difference, not launch-state or VR-provider differences.

The source contract distinguishes the paths: Proton obtains the submitting
D3D11 texture's actual Vulkan image/device/graphics queue through DXVK interop,
unwraps Wine handles, and passes them to xrizer. xrizer copies/blits/resolves
on that **same game device and queue** into the OpenXR swapchain. QuestBridge
imports service DMA_BUF images there and completes frames with a SYNC_FD fence
or a real CPU fence wait. The default probe's two-device NT-handle/keyed-mutex
relay is **not mandatory native interop**; its failure and absent timeline
OPAQUE_FD semaphore support do not by themselves block the native route.
Source: `local://proton-texture-contract.md` (pinned Proton
`experimental-11.0-20261008`, xrizer `5ae098be…`, current `qb_vulkan.c`).
This establishes a source contract, not a Skyrim rendering pass.

**[INFERENCE] VR-provider extras are a likely discriminator, not a proved
cause.** In the pinned source, DXVK appends OpenVR device-extension extras
after logging its normal enabled list. Proton maps the FD memory/semaphore/
fence names to Win32; runtime Linux-only DMA_BUF/DRM-modifier/foreign-queue
extras could instead reach Wine's client validator and be rejected before
host device creation. The no-Steam `runinprefix` probe explicitly logged
`OpenVR: could not open registry key, status 2`; it did not exercise this
game-launch contract. Proton creates `HKCU\Software\Wine\VR` with
`REG_OPTION_VOLATILE` (`vrclient_main.c:348-349`), so absence from disk
`user.reg` proves nothing about a running game. No live per-adapter
`PCIID:5143:0b00` value or exact failing client/host extension trace was captured.
Capture those during the actual game launch before attributing Skyrim's failure.
See “Gated diagnostics” below and the 2026-10-13 dossier's
`evidence/20261013-123855-sharing-resume/`.

`DXVK_NO_VR=1` is an **explicit opt-in diagnostic only**, not a shipping default,
including with xrizer installed. `vr_environment` leaves an unset value unset
and preserves user `1`/`0` overrides. It disables upstream DXVK's VR-extension
provider, not the OpenXR bridge. Session `2026-10-13-11-vr` was interrupted
before DXVK; real-game device creation and native texture submission remain
unvalidated (`D/features/loop3-dxvk-options.md`).

The Windows OpenXR path through `wineopenxr`, and the alternative
OpenComposite + Windows OpenXR runtime inside the prefix, remain unvalidated
as complete rendering paths on this fork.

## READY recovery is not GPU recovery

In session `2026-10-13-11-vr`, restoring the Quest defaults
(`turnip25.1.0-quest1`, `hdr.steam=true`) and cold-starting the app reached
READY/firstFrame in about 8 s. This historical run does not establish an HDR
requirement for the immersive panel or a reliable driver configuration.

The same session recorded an app-side compositor GPU page-fault storm, followed
by a sensor/camera HAL abort and app termination. Timing is correlation, not
proven causality. The panel-only bisect reproduced faults with a 1920x1080
screen swapchain; small 2D controls were clean, but size and surface type remain
confounded. Native WSI metadata/layout sizing is a hypothesis, not a proven
shim failure (`D/features/loop5-gpu-fault.md`).

The archive label is not the binary version: the shipped driver is
`Mesa 25.0.0-devel (git-365cd04375)`, SHA-256
`af0d6bb38756eace17b43311c5c61a7ef3dc6e83d5cb8a63577d6a1d10c0aed0`.
`D/evidence/bundled-driver-backend.txt` records QCOM/fallback backend strings
and no IMapper backend; mapper4 was compiled out despite the headset's mapper4
stack. A replacement driver needs a matching, app-accessible metadata backend
for modifier/plane layout and import-size checks, not arbitrary allocation
padding. Neither the active guessing backend nor the faulting BO is identified.

Source repairs now cover transfer-stage acquire waits, FOREIGN ownership,
GENERAL source preservation/release, per-image present semaphores, import-size
bounds and quarantine on failed release-fence waits. Guest bridge handoffs and
fence status/fd handling were also repaired. These are source repairs, not a
runtime reliability pass. The attempted two-minute panel smoke at
`D/evidence/device/20261013-045928-sync-panel-smoke/` timed out in `am start`
after 90 s: elapsed 185.7 s, panel time 0, no firstFrame,
`clean_120s=false`; empty logs and null fault fields are not a clean run.
The headset was asleep and the session was stopped. GPU reliability remains
unresolved; no further blind fault run is authorized.

## Horizon remains the host OS

Replacing Horizon with Steam Frame's OS would require a bootloader route plus
Quest display, tracking/controller and firmware support; an ARM64 userspace
or recovery image does not supply those. Linux userspace under Horizon, with
Meta's runtime owning display/tracking, remains the approach.
See `D/features/steam-frame-os-port.md`.

## Quest 3 switches for Proton games

From PLAN.md section 12.A.5; these target WinlatorXR-observed symptoms and
remain untested on DroidDeck's DXVK/Turnip builds:

```sh
tools/droiddeckctl env set TU_DEBUG=noconform
tools/droiddeckctl env set MESA_VK_WSI_PRESENT_MODE=mailbox
```

`TU_DEBUG=noconform` drops the sysmem path;
`MESA_VK_WSI_PRESENT_MODE=mailbox` matches WinlatorXR's present mode.
`DXVK_DISABLE_TIMELINE_SEMAPHORES` is **not a stock/current-fork fix**.
Consider it only as an optional fork-specific experiment after proving the
installed DXVK carries a patch that implements it; do not set it generically
or treat it as restoring NT sharing or native VR rendering.

## Limitations and unknowns


- No virtual pad or Bluetooth gamepad in VR: `input pad`, `stick` and
  `trigger` answer `NO_PAD` in a vr session. Keys, text, taps and swipes
  reach the Steam panel. The panel is not operable from the headset yet;
  Touch reaches only VR titles through the bridge.
- `LOCATE_SPACE` and `LOCATE_VIEWS` report no velocities
  (`velocityFlags = 0`); xrizer throws are zero.
- At most 8 layers per frame (`QB_MAX_LAYERS`); array swapchains are
  flattened side by side; depth layers are not forwarded.
- Proton ARM64 Windows VR rendering remains unproven: Skyrim launches but
  fails before rendering; DXVK extension handling and GPU reliability are open.
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

## Gated diagnostics (2026-10-13)

These experiments are not a GPU-reliability or Skyrim VR compatibility pass.
The parent captured bounded probes in 2D session `2026-10-13-18-steam`, which
reached READY/firstFrame with no GPU fault reported during that interval.
An externally started `2026-10-13-19-vr` subsequently became IDLE; the follow-up
capability inventory returned `NO_ACTIVE_SESSION`, not a capability result.
Do not start another GPU/VR run without the parent's awake, stable-session gate.
User-authorized keep-awake remains enabled; restoration is the parent's decision.

### Windows D3D11 sharing probe

Build separately; do not package the diagnostic into the APK or a game folder:

```sh
bash tools/questbridge/d3d11-probe/build-probe.sh
# Output: app/build/questbridge/d3d11-probe/d3d11-probe.exe
```

The recipe also copies the pinned OpenVR source's actual `LICENSE` beside the
executable as `LICENSE.openvr` and fails if it is absent; retain it when staging.

The corrected x86-64 Windows PE rebuilt successfully (2,912,716 bytes), with
`LICENSE.openvr` (1,488 bytes). A disposable Wine container exercised the
latest corrected executable: `--help` exited 0, `--cycles 8` exited 2 and
`--seconds nan` exited 2. These paths return before GPU API calls; they do not
validate texture sharing or VR submission. The compiled help advertises:

| Flag | Meaning / default |
|---|---|
| `--help` | Print usage |
| `--adapter N` | Both hardware devices use adapter N; default 0, software refused |
| `--cycles N` | Shared-texture round trips; default 16, must exceed 8 |
| `--timeout-ms MS` | Wait budget; default 2000 ms (keyed mutex; corrected source also bounds Map polling) |
| `--vr` | Also initialize OpenVR, wait for poses and submit both eyes |
| `--seconds S` | VR-loop duration; default 20 s |

For an authorized guest run, stage the executable and `LICENSE.openvr` only in
`/tmp/droiddeck-diagnostics/`, outside games/existing prefixes, and ensure no
registered shortcut targets the executable. The captured launch selected
`own` prefix `/root/.local/share/Steam/steamapps/compatdata/2495824316` for
`/tmp/droiddeck-diagnostics/d3d11-probe.exe`. Its high-bit ID is
`CRC32(quoted path + basename without extension) | 0x80000000`, not Skyrim's
611670. The launcher overwrites outer `STEAM_COMPAT_DATA_PATH`; that variable
does not provide isolation. Do not use `--window` (it requests WineD3D).

Set **both variables on every call**: `DROIDDECK_NO_STEAM=1` selects Valve
Proton's `runinprefix` here, and `WINEDLLOVERRIDES=d3d11,dxgi=n` selects the
installed native DXVK DLLs. Omitting the second variable used builtin
WineD3D despite the installed DXVK files. Do not save either as a persistent
preference or change the Skyrim prefix. Only the diagnostic's own prefix may
be prepared by its wrapper.

After staging, run help first, then shared-only if authorized:

```sh
/Users/tylermanring/orca/DroidDeckMeta-quest/tools/droiddeckctl guest --timeout 105 \
  --env DROIDDECK_NO_STEAM=1 --env WINEDLLOVERRIDES=d3d11,dxgi=n -- sh -c \
  'timeout -k 5s 75s droiddeck-proton-run /tmp/droiddeck-diagnostics/d3d11-probe.exe --help'

/Users/tylermanring/orca/DroidDeckMeta-quest/tools/droiddeckctl guest --timeout 105 \
  --env DROIDDECK_NO_STEAM=1 --env WINEDLLOVERRIDES=d3d11,dxgi=n -- sh -c \
  'timeout -k 5s 75s droiddeck-proton-run /tmp/droiddeck-diagnostics/d3d11-probe.exe'
```

The 75-second guest process timeout, with forced termination after 5 seconds,
is separate from the 105-second control request timeout. An outer process
timeout is mandatory for **every GPU run**, including shared-only:
device creation and driver calls can still hang.

Default mode opens no VR session: two hardware D3D11 devices on one adapter
perform 16 real NT-handle/keyed-mutex round trips, clearing known RGBA values
and comparing every consumer pixel with RowPitch respected. Corrected source
uses `Map(DO_NOT_WAIT)` with `WAS_STILL_DRAWING` polling bounded by the
`--timeout-ms` budget; this does not replace the outer timeout. Require exit 0
and `PASS shared-cycles=16`; retain any `FAIL stage=... hr=...` as diagnosis.
Only after shared-only passes and XR is stable may the parent use `--vr`.
Keep the outer timeout: `WaitGetPoses` can block beyond `--seconds`. Require
nonzero `PASS vr-submits`, no submit errors, and captured visible output in
both eyes; a submit count alone is not stereo-rendering evidence.
This is the probe's own sequencing gate, not a requirement that every native
Proton/xrizer game first pass a separate-device NT-sharing test.

### Captured sharing and capability results

Evidence root: `/Users/tylermanring/.agents/artifacts/2026-10-13/droiddeckmeta/evidence/20261013-123855-sharing-resume/`.
`probe-help-direct.json` exited 0 and confirmed the own-prefix/runinprefix
route. `sharing-result.json` is the no-overrides WineD3D control: reported
GTX 470, then `CreateSharedHandle` returned `0x80004001` (`E_NOTIMPL`).
It is not a DXVK sharing result.

`sharing-dxvk-result.json` used DXVK-LOW-LATENCY v3.1.1-2 and guest Turnip
26.2.2. Both devices reached FL11.1 with the Win32 trio enabled; NT-handle
creation and consumer open returned success (`S_OK`). It exited 1 in 4.063 s,
not by timeout: `FAIL stage=compare-pixels cycle=0`, first bad pixel `(0,0)`,
`got=00000000 want=0B4783FF rowpitch=1024`. Logs also report
`Exporting semaphores of type 2 not supported by device`, failed shared
NT-handle opens in `DxvkFence` and `DxvkMemoryAllocator`, and
`AcquireSync: Not supported`. API creation/open success does not make this a
sharing pass or identify a single root cause.

`installed-renderer-identities.jsonl` and `skyrim-prefix-identities.jsonl`
record identical probe/Skyrim system32 DLL hashes:

| DLL | Bytes | PE machine | SHA-256 |
|---|---:|---|---|
| `d3d11.dll` | 6266880 | `0x8664` | `62bf43affe4e2f0625322a6f94ef639756207c80071dba89b4a803cd42038558` |
| `dxgi.dll` | 4329472 | `0x8664` | `0f511c96bc61cc460cc7054e6afc4067dcea919e826e668832672fbfd35906ae` |
| `winevulkan.dll` | 1114112 | `0xaa64` | `74f9b4370529f46662b3f1adbd651a7df789bd1a7cffd8be825d77c8d402acf9` |

Installed Proton reports `1791480286 experimental-11.0-20261008-arm64`.
The native DLL identities match its `dxvk/aarch64-windows` files, despite
their x86-64 PE machine; its `dxvk/x86_64-windows` files have different hashes.
Directory names/version labels alone are not component-source provenance.

The native **capability query** in `external-sync-caps.json` returned:

| Primitive | `OPAQUE_FD` | `SYNC_FD` |
|---|---|---|
| Fence | features `0x0` | `0x3`, EXPORTABLE + IMPORTABLE |
| Binary semaphore | features `0x0` | `0x3`, EXPORTABLE + IMPORTABLE |
| Timeline semaphore | features `0x0` | features `0x0` |

Fence SYNC_FD compatible/export-from-imported mask is `0x8`; binary semaphore
SYNC_FD mask is `0x10`. The zero-feature rows also have zero compatibility
and export-from-imported masks. External buffers with TRANSFER_SRC|TRANSFER_DST
usage reported `0x6` (EXPORTABLE + IMPORTABLE) for OPAQUE_FD and DMA_BUF, with
compatible/export-from-imported `0x201` (OPAQUE_FD|DMA_BUF). These are
**buffer-only properties**, not image-format/import or hardware-sharing proof.

The corrected capability enum guard compiled (parent-reported), but its
follow-up inventory did not run after the external `19-vr` session became
IDLE (`external-sync-caps-final.json`: `NO_ACTIVE_SESSION`). The initial
capture remains valid for its listed `VK_EXT_external_memory_dma_buf`
extension; do not present it as a post-correction rerun. No `--vr` submission,
Skyrim rendering or authoritative native Android WSI output census is proven.

### Fence and native WSI evidence boundaries

The production `qb_vk_end_frame_fence` regression was RED before the fix and
GREEN after it (10 targeted host tests); the parent's normal-Linux fence gate
also passed. Every successful SYNC_FD export now clears the slot's pending
state, including positive fds and -1, because copy transference resets the
source fence. The stateful fake returns `VK_TIMEOUT` rather than hanging on
an exported/reset source; the harness subprocess has a 10-second timeout and
makes no GPU calls. This is host evidence only, not kernel sync_file, real GPU
or headset validation. Final parent gates on the current quest tree reported
**598 tools tests OK, 28 capability skips**, and Gradle
`externalNativeBuildRelease` + `testReleaseUnitTest` **BUILD SUCCESSFUL**.
The earlier 574-tools/709-JVM counts are historical, not the current run's
counts. These host/build gates do not establish hardware compatibility.

**Genuine native prerequisites are staged and a diagnostic driver compiled,
but it is not deployable as a working driver.** The parent reports 659 staged
files, 20 targeted host tests PASS, and a completed 676-target non-stub mapper4
build. `app/build/questbridge/native-driver/provenance.json` records Mesa
`9fd4c56e2cbcff25d50a63a5420a4f31d67dbebf`, `android-stub=false`, the real
HIDL mapper4 backend, and output SHA-256
`dae5771c2e97b50b6d05b213ad280131eacffb7fb2ce634be4dceab0d3c4bc19`.
Parent-only build recipe (not an install command):

```sh
bash /Users/tylermanring/orca/DroidDeckMeta-quest/tools/questbridge/native-driver/build-driver.sh \
  /Users/tylermanring/.cache/droiddeck-build/quest-native-mapper/sysroot-aosp14-fixed \
  /Users/tylermanring/Library/Android/sdk/ndk/27.0.12077973
```

**Do not replace the working driver.** This candidate fails closed on
non-mapper4/unknown metadata, unsupported layouts and unknown/insufficient
capacity; its one-plane, one-layer, exactly-one-fd limits are likely to refuse
Quest's observed two-fd handles. Compilation does not prove actual app
AdrenoTools namespace loading or mapper access; neither is established.
The staged platform libraries are link inputs, not libraries to bundle.
The provenance identifies that built snapshot, not subsequent source edits.

A complete no-application-submit WSI destination census remains unproven:
public `VkImage` handles do not expose the destination AHB/native handle, and
guest source-AHB logs are not output-image evidence. The later gate must join
real destination images to native imports, authoritative planes/modifier,
data-fd capacity and BO identity, then stop before application drawing.
No fake getter, mirrored ABI, padding or blind fault rerun substitutes for it.
Fault ownership/cause remain unknown; this is diagnostic-only, not a playable
driver, root-cause fix or guarantee of zero system-runtime GPU work.

Diagnostic evidence and gates:
`~/.agents/artifacts/2026-10-13/droiddeckmeta/orchestration-results.md`.

### Handoff: headset testing belongs to the other session

The user chose **“Leave headset testing to the other session.”** This lane
must not run further hardware tests. Intervening `19/21/22/23-vr` sessions
were not started by this lane; latest parent-observed `23-vr` was IDLE.
Do not attribute their state or any faults to this diagnostic.

- Compiled artifact:
  `/Users/tylermanring/orca/DroidDeckMeta-quest/app/build/questbridge/native-driver/libvulkan_freedreno.so`.
  Its adjacent `provenance.json` records the SHA above.
- Rebuild dependency: the exact `sysroot-aosp14-fixed` path and NDK in the
  preceding recipe; deployment requirements are in the current dossier's
  `native-metadata-probe-plan.md`.
- The candidate was **not installed or bundled by this lane**. The receiver
  must not blindly replace the working driver: the candidate may reject all
  Quest two-fd handles, and app-namespace resolution remains unknown.
- An **in-app no-application-submit WSI census still needs implementation**
  before deployment testing: correlate destination/import identities and stop
  before drawing/submission. The compiled driver alone is not that gate.


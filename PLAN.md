# DroidDeckMeta: Steam and SteamVR titles standalone on Meta Quest 3

Date: 2026-10-12. Target repo: https://github.com/OBEYPALANTIR/DroidDeckMeta (fork of Droid-Deck/DroidDeck, GPL-3.0). Source material: the fork at `10ef8c4` (0.4.0, identical to upstream `main`, 0 ahead / 0 behind), upstream issue [#245](https://github.com/Droid-Deck/DroidDeck/issues/245) (Quest 3/3S tracking), issue [#367](https://github.com/Droid-Deck/DroidDeck/issues/367) (SteamVR request), PR [#592](https://github.com/Droid-Deck/DroidDeck/pull/592) (experimental Quest 3/3S), PR [#598](https://github.com/Droid-Deck/DroidDeck/pull/598) (zip dump, changes requested), the repo's `docs/development/*` and `tools/linuxfs/overlay/usr/local/bin/droiddeck-session`, and the web sources listed in section 11.

Assumption: "Devii gaming" is the product name this fork ships under. Nothing in the repo, upstream, or the web mentions it, so the plan treats it as a brand/distribution decision (section 10, item 1) and not as a technical dependency.

## 1. Goal

One APK on a Meta Quest 3 that:

1. **Tier A, flat:** runs Steam Big Picture, Proton (Windows) and native Linux games on the headset as a 2D panel in Horizon Home, exactly what DroidDeck does on an Adreno 7xx phone today. This is the "works on Quest" milestone and is already in flight upstream (PR #592).
2. **Tier B, VR:** launches SteamVR titles (OpenVR and OpenXR games from the Steam library) immersively on the headset's own display, using the Quest's native OpenXR runtime for display, tracking and Touch controllers. No PC, no streaming.

"SteamVR" here means SteamVR *titles*, not Valve's `vrserver`/`vrcompositor` stack. The plan's recommendation (section 3) is to replace that stack with an OpenXR bridge, the way the OpenVR-on-OpenXR layer xrizer already does on Linux desktops; Valve's own arm64 SteamVR exists only as a proprietary Steam Frame system package and is a reference artifact, not a shipping component.

## 2. What exists today (observed 2026-10-12)

**The fork.** `OBEYPALANTIR/DroidDeckMeta` is byte-identical to upstream `main` at 0.4.0 (13). No Quest, VR, OpenXR or "Devii" code or docs exist in it. Every observation below is therefore about upstream code the fork inherits.

**How a DroidDeck session works** (the pieces VR has to plug into):

- Android app `com.droiddeck.launcher`, Kotlin, `minSdk 26`, `targetSdk 28` (deliberate: a 29+ domain may not exec files from app data), `arm64-v8a` only, requires Vulkan 1.1 (`app/src/main/AndroidManifest.xml:37`). Packages: `agent`, `audio`, `core`, `gpu`, `input`, `runtime`, `session`, `wayland`, `ui`, `stores`.
- The Linux guest is an Arch Linux ARM rootfs run under **proot** (`docs/development/proot-performance.md`). `droiddeck-session` execs **gamescope** (`--backend wayland --expose-wayland`, two Xwayland servers, `droiddeck-session:259-313`) and then the Steam client from `steamrtarm64/steam` (`droiddeck-session:1030`). Windows games run through Valve's ARM64 Proton; x86 Linux binaries through `droiddeck-fex` (FEX).
- The app hosts its **own Wayland compositor** (`app/src/main/cpp/waylandcomp`, `framegen/VulkanRendererContext.h`) that gamescope renders into. Guest buffers reach it as dma-bufs or, via the custom `banner-ahb-v1` protocol, as **AHardwareBuffers passed over a Unix socket** and placed straight on an Android `SurfaceControl` (`waylandcomp/generated/banner-ahb-v1-server-protocol.h:56-66`). This is the exact primitive a VR swapchain bridge needs.
- Vulkan is Mesa **Turnip** on KGSL in two places: a glibc build in the guest and a bionic build in the app (`gpu/DriverBundle.kt`, `gpu/TurnipDriver.java`, `cpp/adrenotools`). `BL_VK_DRIVER` points the guest at an imported ICD (`droiddeck-session:116-137`).
- GPU gating is by Adreno family and a tested-list, not a hard refusal (`gpu/GpuInfo.kt:31-48`: `A8XX`, `A7XX`, `A7XX_LOW`, `A6XX`, `ADRENO_UNKNOWN`, `NOT_ADRENO`; support `TESTED | UNTESTED | UNSUPPORTED`). Quest 3 and 3S are Adreno 740 (XR2 Gen 2, chip id `0x43050b00`) and fall into `A7XX`.
- Input: the guest sees controllers through the `libfakeinput.so` preload (`app/src/main/cpp/fakeinput_steam.cpp`), which emulates Steam Deck / Steam Controller / Xbox / DualSense devices from Android input, and the app's virtual pad. The agent bridge (`docs/agent-control.md`, `tools/droiddeckctl`) can drive sessions, inject input, dump UI and pull logs over ADB, which is the QA harness for everything below.
- Build: `tools/build_local.sh` (Docker, JDK 17, NDK 27.3, Rust, `gh`, `zstd`), components fetched by pinned tag from `Droid-Deck/DroidDeck-Components` (gamescope 3.16.29-p9 with nine patches under `tools/gamescope/patches`, KDE desktop, Debian 11 image for x86 preloads).

**Quest status upstream** (the part Tier A inherits):

- Issue #245 (open, 2026-10-05): on Quest 3S and Quest 3 every Steam session dies at `vkCreateDevice`. Root cause confirmed on device: Horizon OS SELinux denies `IOCTL_KGSL_GPUMEM_ALLOC_ID` (0x934) and `GPUMEM_FREE_ID` to app domains while the `GPUOBJ_ALLOC/INFO/FREE` family is allowed. Turnip uses the denied call for every non-shareable BO. Both Turnips are affected; the bionic one fails silently ("0 windows open"). A second finding: `fakeinput_steam.cpp` ends unhandled `ioctl`s with `syscall(SYS_ioctl, …)` instead of `dlsym(RTLD_NEXT, …)`, so any preload after it (a KGSL shim) never sees Proton processes. The maintainer position: the ioctl forwarding fix is welcome, the `GPUOBJ` fallback "belongs in Mesa", and Quest/Horizon OS "is not a supported target today".
- PR #592 (open, 2026-10-11, by maintainer xXJSONDeruloXx, CI green, no headset test by author): adds `tools/quest/kgsl_compat.c` (retry `GPUMEM_ALLOC_ID`/`FREE_ID` as `GPUOBJ_*` only after `EACCES`/`EPERM`), an Android driver wrapper around the bundled Turnip (`turnip25.1.0-quest1`) and the same adapter in `libblsession`, the `ioctl` → `RTLD_NEXT` fix, `session/QuestExperimental.kt` (Quest 3/3S detection by `Build.MODEL`, one-time defaults: manual driver mode, the wrapped driver, `hdr.steam=true` to route Steam frames through the SurfaceControl path that avoids panel corruption), startup/crash reports in `files/quest-startup.txt` and `files/quest-last-crash.txt`, a `quest` release variant (`com.droiddeck.quest`, own bwrap socket), and `docs/quest.md`. Its doc states plainly: "This does not add immersive VR or OpenXR support."
- First headset report on PR #592 (AshMorrocco, 2026-10-12, Quest 3): the `vkCreateDevice` error is gone, but the guest exits after ~20 ms with no `session.log` and an empty crash buffer. That is the current frontier for Tier A.
- PR #598 is a zip of a decompiled (smali) rebuild of the same idea; changes requested. Nothing in it beyond #592.

**VR landscape** (what Tier B can build on, see section 11):

- Valve's Steam Frame is an arm64 SteamOS headset with a native arm64 SteamVR (`/opt/steamvr/bin/linuxarm64/vrclient.so`, OpenXR manifest `/opt/steamvr/steamxr_linuxarm64.json`). Native arm64 OpenXR games (OpenMW-VR, OpenCE, Chromium WebXR) run there against that runtime; x86-64 Windows OpenXR games under Proton ARM64 + FEX currently crash in `xrCreateInstance` (Proton issue #10211), with at least one case fixed by shipping the x86-64 VC++ runtime next to the game.
- xrizer reimplements OpenVR on top of any OpenXR runtime so OpenVR games run without SteamVR. It is immature, Rust, and packaged for x86_64 only; no arm64 build is published.
- Monado is the open-source OpenXR runtime with Linux and Android support and a client/service split: `libopenxr_monado.so` in the app talks to `monado-service` over a locating Unix socket, a per-client RPC fd and a shared-memory fd for device data; on Android it uses Binder/AIDL instead. A Monado-on-Quest-2 demo exists; nothing shows Horizon OS letting a third-party runtime own the display.
- On Quest, the OpenXR runtime that owns the display, tracking and controllers is Meta's, reached from a NativeActivity through the Khronos loader shipped in Meta's OpenXR Mobile SDK. That is bionic code; the glibc guest cannot dlopen it.
- WinlatorXR (section 12) is the closest working relative: a GPL-3.0 Winlator fork that runs Windows games on Quest 2/3/3S/Pro and Pico, same `minSdk 26 / targetSdk 28` arm64 app shape, bionic Wine (no proot), Turnip on KGSL, and since dawn-42 (2026-10-09) a PCVR path: OpenComposite (OpenVR→OpenXR) inside Wine → its own Windows OpenXR runtime DLL (`OpenXR-WXR`) → UDP poses + `AHardwareBuffer`-over-Unix-socket frames → the app's Meta OpenXR session. A code comment reports its Quest 3 Turnip issuing the legacy `GPUMEM_ALLOC_ID` ioctl successfully on one unspecified August 2026 build, which #245 reports as denied; that is an A/B hypothesis, not a contradiction, until both process contexts are captured.

## 3. Architecture

```
 Horizon OS (Android 14, SELinux enforcing)
 ┌──────────────────────────────────────────────────────────────────────────┐
 │ DroidDeck app (bionic)                                                   │
 │  LauncherActivity (2D panel)     VrSessionActivity (immersive, Meta XR)  │
 │  waylandcomp  ◄── dma-buf/AHB ──┐   xr-bridge-service (new)              │
 │  SurfaceControl scanout         │   ├ Meta OpenXR runtime (loader)       │
 │  Turnip (bionic, wrapped)       │   ├ swapchain = AHB-backed VkImages    │
 └──────────────┬──────────────────┼───┴──────────┬──────────────────────────┘
                │ proot            │ unix socket + fd passing (new protocol)
 ┌──────────────▼──────────────────▼──────────────▼──────────────────────────┐
 │ Linux guest (glibc, Arch ARM)                                             │
 │  gamescope ─ Xwayland ─ Steam client (arm64)                              │
 │  libopenxr_questbridge.so (new OpenXR runtime, Monado-IPC style client)   │
 │  xrizer (arm64 build) = OpenVR → OpenXR for OpenVR titles                 │
 │  native arm64 OpenXR games ──┐                                            │
 │  Proton ARM64 ─ wineopenxr/vrclient thunk ─ x86-64 Windows VR games (FEX) │
 └───────────────────────────────────────────────────────────────────────────┘
```

Components, smallest first:

1. **Tier A baseline** = PR #592 tracked as a branch in the fork plus whatever fixes the 20 ms guest exit. No new design; the work is diagnosis on hardware.
2. **`xr-bridge-service`** (app side, C++/NDK, ~2 to 4k lines): owns one Meta OpenXR instance/session, creates Vulkan swapchains whose images are `AHardwareBuffer`-backed, runs the `xrWaitFrame/xrBeginFrame/xrEndFrame` loop at the headset's rate, exports poses, views, input action state and swapchain buffers to the guest, imports the guest's rendered layers. Lives next to `waylandcomp` and reuses its AHB-over-socket code path.
3. **`libopenxr_questbridge.so`** (guest side, C, glibc): an OpenXR runtime with an ICD manifest (`questbridge.json`) that implements the core OpenXR 1.0 API plus `XR_KHR_vulkan_enable2`, `XR_KHR_composition_layer_*` as needed, and the Oculus Touch interaction profile, by marshalling to the service. Modelled on Monado's IPC client (`ipc-design`), not a fork of it: Monado's service assumes it owns the compositor, which Meta's runtime will not allow.
4. **`droiddeck-session vr`** mode: a session that starts the Steam client under gamescope as today (Steam's own UI stays a 2D panel layer in the headset) and sets `XR_RUNTIME_JSON` to the bridge for everything Steam launches. For OpenVR titles, `openvrpaths.vrpath` points at an arm64 xrizer build.
5. **VrSessionActivity**: the immersive Android activity (`com.oculus.intent.category.VR`, hybrid-app switch from the 2D launcher). Hosts the bridge service's OpenXR session, shows the Steam panel as a quad layer when no immersive layer is submitted.
6. **Agent driver**: Claude Code with the REA MCP server (installed and healthy on Tyler as of 2026-10-11, Ghidra-only provider) for the binary targets in section 5, and `tools/droiddeckctl` over ADB for on-device loops.

## 4. Target record (what every loop writes about)

```json
{
  "device": "Quest 3 | Quest 3S",
  "horizon_build": "UP1A.231005.007.A1 / v<xx>",
  "kernel": "5.10.246",
  "gpu": {"chip_id": "0x43050b00", "kgsl_name": "Adreno740v3"},
  "droiddeck": {"commit": "<sha>", "variant": "standard | quest", "package": "com.droiddeck.quest"},
  "runtime": {"turnip_android": "turnip-quest", "turnip_linux": "<tag>", "gamescope": "3.16.29-p9", "proton": "<tag>", "fex": "<tag>"},
  "vr": {"meta_runtime_version": "", "bridge_commit": "", "xrizer_commit": ""},
  "evidence": ["ids from the ledger"]
}
```

Device facts come from `files/quest-startup.txt` (PR #592), `droiddeckctl state`, `getprop`, `/sys/class/kgsl/kgsl-3d0/`, never from the model string alone.

## 5. The loops

Each loop is a worklist with a fixed-point stop condition, a budget, and a ledger write per iteration (section 6). A loop never repeats a probe whose evidence ID already covers the same device build and app commit.

### Loop 0: Device truth and Horizon OS policy (reverse engineering, read-only)

Why: everything in #245 was found by probing the kernel from an app context. Tier B needs the same map for binder, the XR runtime and the display, before any code is written.

1. **KGSL ioctl map**: re-run the #245 probe on Quest 3 (not just 3S) for every `IOCTL_KGSL_*` Turnip and gamescope use (`GPUOBJ_*`, `GPUMEM_*`, `SYNCSOURCE_*`, `TIMESTAMP_EVENT`, `DRAWCTXT_*`, `SUBMIT_COMMANDS`, large allocations, `GPUOBJ_IMPORT` of dma-bufs). Record `EACCES` vs `OK` per call. Evidence: probe source + output. **A/B control (section 12, A1):** on the same headset and Horizon build, run WinlatorXR dawn-42 (Turnip container, `KGSL_SHIM_DEBUG=1`) and the DroidDeck PR #592 build, and for each capture (a) the SELinux context of the process that opens `/dev/kgsl-3d0` (`/proc/<pid>/attr/current`, `ps -AZ`), (b) proof that ioctl `0x934` is actually issued in that process (shim counters on every call, not only failures; or a `strace`-equivalent), (c) every `avc: denied … ioctlcmd=0x934` line with its `scontext`, (d) `/proc/<pid>/maps` to show which Vulkan driver `.so` is mapped, because WinlatorXR silently runs the system driver when a Turnip fails to `dlopen` (`turnip_compat_shim.c:29-30`) and "Vulkan works" then says nothing about Turnip's KGSL path. Only with all four can the cause be split between policy (same context, different build → xperm rule changed), driver (T26 or Mesa never issues `0x934`, or Turnip was not the driver running → the shim is unexercised), and interposition (the preload or DT_NEEDED shim is not in the adrenotools-loaded driver's call path → "no fallback fired" was unobservable; AOSP bionic does propagate `LD_PRELOAD` into `SHARED` child namespaces, which is what libadrenotools creates, so this arm is plausible rather than likely, and Horizon's bionic is unread). Equal `targetSdk` does not establish equal domains; #245's first avc line is from `u:r:shell:s0` and @shukumei7's app-context table has no avc line, so neither side has a clean app-domain observation yet.
2. **SELinux surface for the app domain**: `/sys/fs/selinux/policy` is unreadable to apps, so derive the policy from `avc: denied` logcat lines under each experiment (`getenforce`, `dmesg` may be denied; `logcat` is not). Record every denial class touched by the session (kgsl, binder services, `/dev/dri`, sockets, `/proc` reads).
3. **Meta OpenXR runtime inventory** (REA: `inspect_android_package`, `inspect_binary_layout`, Ghidra for exported symbols only): which system packages host the runtime (`com.oculus.vrruntimeservice` and friends on this build), which `.so` the Khronos loader resolves to, which Vulkan extensions and image formats its swapchains require, whether `XR_KHR_vulkan_enable2` and `XR_FB_*` layers are present, which interaction profiles and `XR_EXT_hand_tracking` are offered. Static facts only; the behaviour is confirmed in Loop 2 by a 200-line sample app.
4. **Display path**: does an immersive session coexist with a `SurfaceControl` panel layer; what the panel app's `SurfaceControl` becomes when the app goes immersive (hybrid-app rules); refresh rate options (72/90/120 Hz) and per-eye swapchain sizes.
5. **Developer settings**: Horizon OS has no "Restrict child processes" toggle UI; verify the app's "Fix it for me" ADB path (`settings put global settings_enable_monitor_phantom_procs false`) works and survives reboot and OS updates.
6. **Steam Frame arm64 SteamVR as a reference** (own device or published issue logs only): the OpenXR manifest layout, `vrclient.so` ABI version, how Proton ARM64's `wineopenxr` locates it, the `xrCreateInstance` crash in Proton issue #10211. Interface facts for the bridge; no code or binaries are copied.

Stop when the worklist is empty. Output `identity.json` per device build and `unknowns.json` for anything undetermined (for example a denied probe).

### Loop 1: Tier A, flat Steam on Quest 3 (fix until the PR #592 path is stable)

Worklist seeded from the #592 headset report (guest exits after ~20 ms):

1. Install the signed PR #592 test build (`DroidDeck-CI` release `pr-592`) or a fork build from the same commit. Capture `files/quest-startup.txt`, `logcat`, `droiddeckctl logs latest`.
2. Bisect the exit: run `droiddeckctl guest -- true`, then `gamescope --version`, then `vkcube` under the wrapped Linux Turnip, then `droiddeck-session steam`. The first failing step names the layer (proot exec, preload chain, ICD, gamescope backend, app compositor).
3. Known candidates to check in order: (a) the Linux preload adapter not in `ld.so.preload` order the maintainer described; (b) `libblaudit.so` / fastpath under Horizon's seccomp; (c) `BL_VK_DRIVER` manifest unreadable so the session "IGNORING" it and falling back to an unwrapped Turnip (`droiddeck-session:137`); (d) the `quest` variant's bwrap socket name; (e) `hdr.steam` SurfaceControl path on Quest 3's panel compositor; (f) the panel corruption that `hdr.steam` works around is a Turnip 26.3.0 regression per WinlatorXR (section 12, A2): retest with `turnip25.1.0-quest1` and `hdr.steam=false` before keeping the workaround.
4. Then the `docs/quest.md` checklist: Steam login panel, sign-in, install an owned game, run it (rendering and input), Proton game, native game, desktop mode, sleep/wake, session save/restore. Mono audio, PulseAudio sink, Touch controllers as gamepad through `libfakeinput`.
5. Feed every fix upstream as a PR against #592's branch or `main` (the maintainers asked for exactly this). The fork carries only what upstream declines.

Stop when the checklist passes on two consecutive Horizon builds, or budget hits. Output: `tierA-matrix.md` (device build × app commit × scenario × result).

### Loop 2: XR bridge (the new engineering, with per-feature trace)

Seeded by the Meta runtime inventory from Loop 0. Each item is a feature with an evidence-backed "answered / partial / unresolved" status:

1. **Sample app**: a 200-line NDK OpenXR app in the DroidDeck APK (hidden activity) proving Meta's runtime can be driven from this process: instance, session, Vulkan swapchain, clear-colour frame loop, Touch poses. Static facts from Loop 0 become observed facts here.
2. **Swapchain transport**: runtime-created swapchain images → `AHardwareBuffer` → send over the bridge socket (`AHardwareBuffer_sendHandleToUnixSocket`, as `banner-ahb-v1` already does) → guest imports as `VkImage` through `VK_ANDROID_external_memory_android_hardware_buffer` is bionic-only, so the guest imports the dma-buf fd (`VK_EXT_external_memory_dma_buf`) on its Turnip. Verify both Turnips agree on format/modifier/tiling for the formats Meta's runtime offers; record which formats work. Start from WinlatorXR's `direct.h` wire format and `direct.c` consumer (section 12, B1) as a reference, not a finished bridge: it has no consumer→producer release fence (only HELLO/BUFFER/FRAME), so two things are added, a RELEASE/ack message carrying the consumer's completion fence per slot, and the dma-buf import path with glibc↔bionic format/modifier verification.
3. **Frame loop and timing**: `xrWaitFrame` predicted display time crosses the socket with sub-millisecond latency budget; the guest's `xrEndFrame` returns the layer list; service submits. Measure added latency and dropped frames with the runtime's own stats; target 72 Hz first, 90 Hz as stretch.
4. **Space and input**: reference spaces, view poses, Touch action bindings for the `/interaction_profiles/oculus/touch_controller` profile, haptics. Hand tracking, passthrough, guardian events: out of scope for v1, recorded as unknowns. WinlatorXR's XrAPI 0.7 packet (section 12, B2) is a field-proven schema for what crosses the socket.
5. **Guest runtime**: `libopenxr_questbridge.so` + `questbridge.json`; conformance smoke with the Khronos `hello_xr` (Vulkan) built for arm64 Linux inside the guest. Skeleton: port the action/path/profile/event-queue layer of `OpenXR-WXR/src/runtime.cpp` (section 12, B3), drop its D3D/GL/Win32 code.
6. **xrizer arm64**: build on Arch ARM in the guest's toolchain, fix what does not compile, run an OpenVR sample against the bridge. Report its status to the xrizer project. Scope: native Linux OpenVR titles only; Windows OpenVR titles go through OpenComposite x86-64 DLLs inside Proton instead (section 12, B4).
7. **Proton ARM64 VR path**: trace how DroidDeck's Proton resolves `openvr_api` / `wineopenxr` to a Linux runtime under WoW64 + FEX (REA on the Proton tree and `lsteamclient`/`vrclient` thunks; the Steam Frame crash in Proton issue #10211 and its VC++ runtime workaround are the first hypotheses). Pair findings with a minimal Windows OpenXR test app. Second hypothesis, from WinlatorXR (section 12, B4): bypass `wineopenxr` entirely with a Windows runtime DLL registered in the prefix plus a Wine unixlib relay; #10211 is then never reached.
8. **Session mode**: `droiddeck-session vr`, the immersive activity, the Steam panel as a quad layer, return-to-2D when the game exits.

Each feature: static path first (source and REA evidence), runtime corroboration on the headset, classification, and expansion of newly found entry points back into the worklist. Parallel workers may take disjoint features (transport vs input vs Proton path) against the same evidence IDs.

### Loop 3: Game matrix and verification

Runs once Loop 2 has an end-to-end frame.

1. Title tiers: (a) native arm64 Linux OpenXR (OpenMW-VR, OpenCE VR mode) as the bridge's conformance set; (b) Linux x86-64 OpenVR via xrizer under FEX; (c) Windows x86-64 OpenVR/OpenXR under Proton ARM64 (Beat Saber-class first, nothing GPU-heavy). Expect Snapdragon XR2 Gen 2 under proot and FEX to run only light titles; record frame times, not impressions.
2. Cross-checks: the same title through Steam Link streaming on the same headset as the "it is the game, not us" control; `compare` the bridge's frame stats with the sample app's.
3. Coverage: every Loop 0 unknown either resolved, worked around, or listed with its remedy; zero-coverage components (audio spatialisation, haptics, sleep/wake in VR) re-enter Loop 2.

Stop when no unresolved item has an available remedy, or budget hits.

### Loop 4: Release and host matrix

`quest` variant APK signed and sideloaded (Horizon developer mode; no Meta store submission is planned), `docs/quest.md` and a `docs/vr.md` kept current, update channel wired to the fork's own `DroidDeck-CI`-style releases if the fork diverges from upstream. Re-run Loop 1's checklist and Loop 3's matrix on every Horizon OS update (Meta ships monthly; SELinux and runtime changes are the main regression source).

### Re-sync loop (upstream tracking)

The fork is identical to upstream today. Rebase the fork's branches on upstream `main` weekly; upstream merges PR #592 or its successor first. Only diverge on files upstream declines (the VR bridge is the expected divergence; the KGSL and ioctl fixes should land upstream).

## 6. Ledger and dossier layout

```
~/.agents/artifacts/<date>/droiddeckmeta/
  identity.json          Loop 0: device, OS, policy map, runtime inventory
  unknowns.json          REA record_unknown export (denied probes, untested paths)
  tierA-matrix.md        Loop 1 results per device build × commit × scenario
  features.md            Loop 2 table: id, feature, status, evidence ids, one-line explanation
  features/<id>.md       per-feature write-up with cited evidence and measurements
  games.md               Loop 3 matrix: title, tier, runtime path, fps/frametime, result
  ledger.jsonl           every probe/tool call: time, tool, inputs digest, evidence_id, outcome
  evidence/              rea evidence-export bundles, droiddeckctl logs, session zips (scrubbed)
  report.md              plain-language status, what works, what is blocked and by what
```

The ledger is the resume point. Session zips from **Share logs** are already scrubbed by the app; keep the raw `files/logs/` pulls private.

## 7. Phased delivery

**Phase 0: prerequisites (approval per install or purchase)**
- A Quest 3 (not only a 3S) in developer mode with ADB over USB and Wi-Fi; a second headset is optional for the Steam Link control.
- Fork hygiene: branch `quest-experimental` tracking upstream `fix/ioctl-preload-forwarding` (PR #592); CI enabled on the fork so signed test builds exist.
- Build host: Docker, JDK 17, Android SDK + NDK 27.3.13750724, Rust, `gh`, `zstd` on an arm64 Linux box or Docker on the Mac (`tools/build_local.sh`; `tools/test_local.sh` runs the preload suite in Linux). Defer heavy builds while Logic Pro is open (machine rule).
- REA MCP registered (done 2026-10-11, Ghidra provider); Meta OpenXR Mobile SDK downloaded for headers and the loader; Khronos `hello_xr` sources.
- Decide Devii naming/package id before any APK is distributed (section 10).

**Phase 1: Tier A on hardware (Loop 0 steps 1, 2, 5 and Loop 1).** Target: Steam signs in, an owned flat game runs with Touch controllers as a gamepad, on Quest 3. Deliver fixes upstream. Est. 1 to 3 weeks of on-device iteration; the 20 ms guest exit is the unknown.

**Phase 2: Loop 0 steps 3, 4, 6 and Loop 2 items 1 to 3.** Sample XR app in the APK, swapchain transport, frame loop. Exit criterion: a guest-side `hello_xr` renders a stereo cube in the headset through the bridge at 72 Hz. This is the go/no-go for Tier B.

**Phase 3: Loop 2 items 4 to 8.** Input, guest runtime packaging, xrizer arm64, Proton ARM64 path, `vr` session mode. Exit criterion: one native arm64 OpenXR title and one OpenVR title launched from Steam Big Picture in the headset.

**Phase 4: Loop 3 and Loop 4.** Game matrix, docs, signed `quest` APK, re-sync cadence.

**Phase 5 (optional): submit the bridge upstream** as an opt-in component, mirroring how `tools/quest/` landed, if the maintainers accept Quest as a target by then.

## 8. Budgets and stop conditions

| Loop | Default budget | Stop |
|---|---|---|
| L0 device truth | 5 device-days, 1 Horizon build | worklist empty; denials recorded |
| L1 Tier A | 15 device-days | checklist green twice |
| L2 XR bridge | 30 engineering days, go/no-go at item 3 | `hello_xr` at 72 Hz, then one game per path |
| L3 game matrix | 10 device-days, 12 titles | no remedy for remaining failures |
| L4 release | per Horizon update | matrix re-run green |

Every headset run is logged through `droiddeckctl logs latest` and bound to the target record; a claim without a session zip and the app commit does not enter the ledger.

## 9. Boundaries

- GPL-3.0: the fork and the bridge stay GPL-3.0; Valve's Steam client, Proton and SteamVR, and Meta's runtime remain proprietary inputs that are used, not redistributed. The Steam Frame SteamVR package is not copied into the APK or the runtime.
- No SELinux, signature or Horizon OS policy bypass; the KGSL work routes allocations through calls the policy already permits (the #245/#592 approach), and the bridge uses the public OpenXR loader path.
- No Meta store submission, no sideload guidance beyond developer mode; the user owns the games they run.
- Logs, session zips and REA evidence carry no Steam credentials; the app's scrubbed **Share logs** output is the only thing attached to upstream issues.
- Per-eye render at Quest 3 native resolution under proot and FEX is not a performance promise; the plan reports frame times per title.

## 10. Open decisions for you

1. **Devii gaming**: product name only, or a separate package id (`com.devii.droiddeck`?) and update channel distinct from upstream's `com.droiddeck.quest` variant? Separate ids keep app data apart and avoid being overwritten by upstream test builds.
2. **Hardware**: Quest 3 only, or Quest 3 and 3S (same SoC; the 3S has a smaller per-eye panel and is where #245 was diagnosed)?
3. **Tier B scope for v1**: native arm64 + OpenVR via xrizer only, or also Windows VR titles under Proton ARM64 (the Proton issue #10211 crash makes that the riskiest path)? WinlatorXR's evidence (section 12, B4) inverts this: Windows OpenVR titles through OpenComposite-in-Wine are the path that already works on a Quest 3, and the Linux-side runtime is the unproven one.
4. **Upstream posture**: contribute Tier A fixes to Droid-Deck/DroidDeck (recommended; the maintainers asked), or keep everything in the fork?
5. **Where the dossier lives**: `~/.agents/artifacts/<date>/droiddeckmeta/` as proposed, or `docs/` inside the fork.

## 11. Sources

- Fork: https://github.com/OBEYPALANTIR/DroidDeckMeta (compare vs upstream: 0 ahead, 0 behind at 2026-10-12)
- Upstream issue #245: https://github.com/Droid-Deck/DroidDeck/issues/245
- Upstream issue #367: https://github.com/Droid-Deck/DroidDeck/issues/367
- Upstream PR #592: https://github.com/Droid-Deck/DroidDeck/pull/592 (branch `fix/ioctl-preload-forwarding`, `docs/quest.md`, `tools/quest/kgsl_compat.c`, `session/QuestExperimental.kt`)
- Upstream PR #598: https://github.com/Droid-Deck/DroidDeck/pull/598
- Proton ARM64 OpenXR crash on Steam Frame: https://github.com/ValveSoftware/Proton/issues/10211
- Steam Frame arm64 SteamVR paths and native OpenXR ports: https://github.com/OpenCommunityEdition/OpenCE/pull/85, https://github.com/stephen-moskal/openmw-vr-steam-frame, https://github.com/kotorvr/EchoFramePCVR, https://github.com/CircuitLord/CircuitLordVRModInstaller/issues/45
- xrizer: https://github.com/Supreeeme/xrizer, https://vronlinux.org/docs/fossvr/xrizer/
- Monado IPC design: https://monado.pages.freedesktop.org/monado/ipc-design.html, https://monado.freedesktop.org/getting-started.html
- Monado on Quest 2 demo: https://www.youtube.com/watch?v=ICBHr8ll84o
- WinlatorXR: https://github.com/WinlatorXR/WinlatorXR (dawn-42, `39b270c`, 2026-10-09), https://github.com/WinlatorXR/OpenXR-WXR (`5f7a5c4`), https://github.com/WinlatorXR/OpenComposite (`5990925`); driver trail commits `9adf47e224` (kgsl shim), `749af4145f` (shim confirmed no-op), `063ca4e5e5` (T26 Turnip tested on Quest 3), `2fd6ebedb6` (T26 default for Quest 3, 2026-09-09); https://vr.org/articles/winlatorxr-half-life-2-quest-3-standalone-2026

## 12. WinlatorXR findings (2026-10-12)

Source: WinlatorXR dawn-42, OpenXR-WXR and the OpenComposite fork, read from clones at `/tmp/wxr`; four cited reports are in `~/.agents/artifacts/2026-10-12/droiddeckmeta/evidence/winlatorxr-{gpu,runtime,apphost,openvr-input}.md`. Paths below are relative to those clones; nothing was built or run on a headset. All three repos are GPL-3.0, so code can move into the fork.

**What it is.** The same Android app shape as DroidDeck (`app/build.gradle:28-34`: compileSdk 34, minSdk 26, targetSdk 28, arm64 only, `useLegacyPackaging`), hybrid 2D launcher + immersive `MetaQuest extends XrActivity extends XServerDisplayActivity` in its own `:vr_process` with `com.oculus.intent.category.VR` (`AndroidManifest.xml:80-94`). The guest is bionic Wine (their Proton-11.0-2-arm64ec build, Box64/FEX) started with `ProcessBuilder` from the files dir (`ProcessHelper.java:89-98`, `BionicProgramLauncherComponent.java:399-416`); the glibc/proot launcher is dead code (`XServerDisplayActivity.java:1705-1711`). X11 is Winlator's in-app Java X server; the app composites guest `AHardwareBuffer`s as `EGLImage`s with the vendor GLES driver and never runs Turnip itself (`cpp/winlator/gpu_image.c:20-70,91-108`). No Steam client: the store tab is a JavaSteam downloader with Goldberg's ColdClientLoader as the Steam stand-in.

### A. Tier A (flat Steam, #245, PR #592)

1. **A/B hypothesis on the KGSL denial, not a refutation of #245.** `cpp/winlator/kgsl_turnip_shim.c` is the same design as PR #592's `kgsl_compat.c` (try `GPUMEM_ALLOC_ID`/`FREE_ID`, on any errno translate to `GPUOBJ_ALLOC`+`GPUOBJ_INFO` / `GPUOBJ_FREE`, lines 137-216), preloaded only on Quest 3/3S (`BionicProgramLauncherComponent.java:357-363`). Its header (lines 39-46) and the launcher comment record one on-device test (Quest 3, 2026-08-30, `KGSL_SHIM_DEBUG=1`, Horizon build unrecorded) where the legacy ioctls succeeded and the fallback never fired; the T26 Turnip (Mesa 26.1.0-devel) became the Quest 3 default on 2026-09-09 after commit `063ca4e5e5` "tested working turnip driver for quest 3". Against that: #245 (`UP1A.231005.007.A1`, patch 2026-06-04) reports `EACCES` from a shell probe and, per @shukumei7, from inside the app context, and PR #592's shim made `vkCreateDevice` succeed on a Quest 3. Four readings fit "no fallback fired" and none is excluded: policy allowed `0x934` for that build or domain (same `targetSdk 28` does not prove the same domain; seapp_contexts and Horizon additions can key on more); Turnip never issued `0x934`, or Turnip was not the driver running (WinlatorXR falls back to the system driver silently); the `LD_PRELOAD` shim never reached the adrenotools-namespace driver (plausible, not tested); or the log was never visible: the shim prints only on legacy failure, `KGSL_SHIM_DEBUG` is set by no app code (only via a shortcut's `envVars`, `XServerDisplayActivity.java:1740`), child stdout/stderr go to `/dev/null` without a debug callback (`ProcessHelper.java:92-96`), and `LaunchReport.java:102` drops non-matching lines. Stock dawn-42 can therefore supply process context and avc lines for the control but not ioctl-issuance proof; that needs a shim rebuilt with success counters or an `LD_PRELOAD` override through `envVars` (re-list `libandroid-sysvshm.so:libevshim.so` first, since `putAll` replaces the launcher's value, `BionicProgramLauncherComponent.java:381-383`). Loop 0 item 1 captures all of this on both apps before any reading is named. Each answer has a different consequence: build-dependent policy is a Loop 4 regression source; a driver difference means the Turnip build choice matters more than the shim; interposition or invisibility means WinlatorXR's observation says nothing about policy.
2. **The panel corruption is probably a Turnip regression, not a compositor-path bug.** The shim header (lines 43-45) attributes "static/noise" on Quest 3 to `adrenotools-Turnip_v26.3.0_r9` and says `turnip25.1.0` fixed it. #245's reporter ran `Mesa_Turnip_v26.3.0-20261004-r2` and worked around garbling with `hdr.steam`; PR #592 already bundles `turnip25.1.0-quest1`. Loop 1 item 3(f).
3. **Keep Turnip out of the app process.** WinlatorXR's compositor is vendor GLES + `EGLImage`; DroidDeck's `hdr.steam` SurfaceControl path is the equivalent. Making that the only Quest path removes the second (bionic) Turnip and its silent `dev_init` failure from #245.
4. **Preload chain hygiene matches PR #592, with one difference.** `cpp/winlator/evshim.c:256-272` forwards `ioctl` through `dlsym(RTLD_NEXT)` (the `libfakeinput` fix). WinlatorXR's KGSL shim is `LD_PRELOAD`-only (`BionicProgramLauncherComponent.java:349-363`); its only `patchelf --add-needed` (`AdrenotoolsManager.java:281-311`) attaches `libturnip_compat_shim.so` (C11 `threads.h` and libcutils ATrace stubs for Turnip builds that fail to `dlopen` on older devices) and returns early on `SDK_INT >= 30` (`:282`), so it never runs on Quest. DT_NEEDED injection into the driver is PR #592's bionic-wrapper technique, not WinlatorXR's; the control therefore does not share #592's guarantee that the shim sits in the driver's call path.
5. **Quest 3 runtime knobs worth copying into the gamescope/DXVK session:** `DXVK_DISABLE_TIMELINE_SEMAPHORES=1` in every preset, a "binsem" DXVK build, `TU_DEBUG=noconform` (no `sysmem` on the Proton 11 preset), `MESA_VK_WSI_PRESENT_MODE=mailbox` (`core/DefaultVersion.java:42-43,59-62,73-75`). Timeline-semaphore trouble on Adreno 740 under Horizon is a candidate for whatever follows the 20 ms exit.
6. **Not solved by WinlatorXR:** the 20 ms guest exit (no proot, no glibc, no gamescope, no Wayland/dma-buf path anywhere); Steam client in the guest; the Qualcomm driver fallback (`adrenotools-v819.2_Quest3_Pico4Ultra`, extracted from Quest 3 firmware) is bionic-only via libadrenotools and cannot enter the glibc guest.

### B. Tier B (XR bridge)

1. **Frame transport is a working reference for the plan's design, with a missing fence.** Abstract `AF_UNIX` `SOCK_SEQPACKET` `@winlatorxr.direct`, fixed-size `HELLO{version,pid}` / `BUFFER{slot,layer,w,h}` / `FRAME{frame,display_time,space,views[2]{slot,layer,rect,quat,pos,fov},quads[4]}` messages, `AHardwareBuffer_sendHandleToUnixSocket` once per buffer, one `SYNC_FD` per layer per frame (`cpp/xr/direct.h:38-98`, `direct.c:159-258`). Producer: the Wine process allocates RGBA8 AHBs, a relay `VkDevice` on the same Turnip imports DXVK's D3D11 shared NT handle as an opaque fd (wineserver `d3dkmt_object_open`, pinned to their Proton build) and `vkCmdBlitImage`s into the AHB, exporting the semaphore as a sync fd (`tools/wxr_bridge/wxr_bridge_unix.c:141-174,358-441,579-604,731-803`). Consumer: the app imports AHB → `EGLImage` → `glBlitFramebuffer` into its per-eye `GL_SRGB8_ALPHA8` Meta swapchain after `eglWaitSyncKHR`, submits a projection layer with the game's own pose and FOV in a mirrored space, re-submits the last frame every host tick, and falls back to its own layer after 5 s without frames (`direct.c:470-515,549-574,665-696`, `renderer.c:564-575`). **The hazard:** the sync fd only covers producer completion; the consumer's GL read is asynchronous and nothing tells the producer when it has finished (no release/ack message in `direct.h:44-48`; reuse relies on 3-image rotation plus the producer's own fences, `wxr_bridge_unix.c:617,761`). Rotation is not a synchronisation guarantee; a producer can overwrite a slot mid-read. DroidDeck's bridge adds a per-slot RELEASE message carrying the consumer's completion fence (an `EGL_SYNC_NATIVE_FENCE_ANDROID` fd exported after the blit, or Vulkan equivalent) that the producer waits on before reusing the slot. Also added: a dma-buf import branch (`EGL_EXT_image_dma_buf_import` or `VK_EXT_external_memory_dma_buf`) for a glibc producer, which WinlatorXR never exercises, with glibc↔bionic format/modifier/UBWC verification (WinlatorXR notes Turnip keeps UBWC on a mutable image only when told the view formats, `wxr_bridge_unix.c:360-362`). Known costs to beat: two GPU copies per eye per frame plus a CPU fence wait; D3D11-only (D3D12/GL/Vulkan games fall back to a 2D preview window).
2. **Control channel.** Poses and input go over loopback UDP ("XrAPI", host→guest 7872, guest→host 7278): a <1 KB binary packet per host frame with 12 floats (sticks, triggers, grips, IPD, FOV, Hz), 19 button bits, a recenter id, and N (space, base) located pose pairs with velocity that the guest registered (`java/com/winlator/xr/api/XrVersion07.java:38-83`, `OpenXR-WXR/src/WinXrApiUDP.cpp:94-214`); no `XrSpace` semantics cross the wire. Touch bindings: `cpp/xr/input.c:52-122`. Pacing is the weak point: guest `xrWaitFrame` free-runs on a QPC timer and Meta's predicted display time never reaches the game (`runtime.cpp:2681-2705`); DroidDeck's bridge should carry `{predictedDisplayTime, period, frameId}` and block the guest on it.
3. **Guest runtime skeleton.** `OpenXR-WXR/src/runtime.cpp` implements 70 entry points (`kFnTable`, lines 6562-6641) and advertises `XR_KHR_D3D11_enable`, `D3D12_enable`, `opengl_enable`, `vulkan_enable`/`enable2`, `composition_layer_cylinder`, `composition_layer_depth` (accepted, unused), `visibility_mask`, `EXT_palm_pose`, `win32_convert_performance_counter_time` (lines 1101-1112). The action/path/interaction-profile/bound-source/event-queue layer (roughly lines 2600-2680, 5655-5788, 6316-6428, 6506-6522) carries OpenComposite-specific quirks and ports to `libopenxr_questbridge.so`; the D3D/GL/Win32/preview-window code does not.
4. **Windows VR titles: OpenComposite in the prefix, no Proton thunks.** Per shortcut, `PcvrRuntime.apply()` writes `openvrpaths.vrpath` → `C:\OpenComposite\Runtime` (`vrclient_x64.dll`/`vrclient.dll`, `opencomposite.ini` = `initUsingVulkan=true`) and `HKLM\Software\Khronos\OpenXR\1\ActiveRuntime` → `C:\oxrwxr\openxr_simulator.json` → `openxr_wxr.dll` (plus the Wow6432Node/32-bit twins) into the prefix, backing up what was there (`java/com/winlator/xr/utils/PcvrRuntime.java:66-70,192-244,313-328`). A game's own `openvr_api.dll` or `openxr_loader.dll` then finds a Windows runtime in the same process; `wineopenxr`, `vrclient` and `lsteamclient` are never involved, so Proton issue #10211 is not on the path. Transferable OpenComposite patches (fork commits #1-#10): BC/unknown-texture skip, soft stubs, session reuse on double init, eye-mismatch survival, GL blit fallback, stick-as-touchpad. For DroidDeck this needs either a PE runtime plus a unixlib relay like `wxr_bridge` against Valve's Proton ARM64 (the `d3dkmt_object_open` handle→fd step is Proton-version-specific), or the planned Linux `.so` behind `wineopenxr`; xrizer is then only for native Linux OpenVR titles.
5. **Meta session boilerplate to reuse:** `cpp/xr/engine.c:38-241` (loader init, extension probing, GLES binding), `renderer.c:927-989` (72 Hz first then configured rate, performance levels, thread hints), `renderer.c:353-782` (projection/quad/cylinder/equirect/passthrough layer assembly), `cpp/xr/oneeuro.c` (controller filter), manifest `supportedDevices quest2|questpro|quest3|quest3s` and permissions (`AndroidManifest.xml:38-39,91,181-182`). Their 2D UI inside VR is drawn to bitmaps and shown as quad layers, not an Android panel (`XrContentDialog.java:114-167`); a `SurfaceControl` panel coexisting with an immersive session (Loop 0 item 4) stays unanswered.
6. **Not solved by WinlatorXR:** zero-copy, host-driven pacing, Vulkan/GL/D3D12 games on the direct path, depth/foveation/spacewarp, hand tracking, more than one client, a glibc producer, SteamVR's `vrserver`/`vrcompositor` or Steam's own VR launch flow.

### C. Decisions this changes

- Section 10 item 3: Windows OpenVR titles via OpenComposite-in-Wine are the proven path on Quest 3; the native Linux runtime is the unproven one. Scope v1 Tier B around OpenComposite + a Windows runtime unless the Proton ARM64 relay turns out harder than the Linux `.so`.
- Loop 0 gains the WinlatorXR A/B control as its first item; it is a three-way discrimination (policy / driver / interposition), and PR #592's shim stays in place until the policy branch is confirmed or excluded on an app-domain observation.
- Loop 2 items 2 to 5 start from WinlatorXR files instead of blank designs; the budget for item 3 (go/no-go) does not change, the pacing design does.

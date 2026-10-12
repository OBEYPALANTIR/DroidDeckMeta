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
  "runtime": {"turnip_android": "turnip25.1.0-quest1", "turnip_linux": "<tag>", "gamescope": "3.16.29-p9", "proton": "<tag>", "fex": "<tag>"},
  "vr": {"meta_runtime_version": "", "bridge_commit": "", "xrizer_commit": ""},
  "evidence": ["ids from the ledger"]
}
```

Device facts come from `files/quest-startup.txt` (PR #592), `droiddeckctl state`, `getprop`, `/sys/class/kgsl/kgsl-3d0/`, never from the model string alone.

## 5. The loops

Each loop is a worklist with a fixed-point stop condition, a budget, and a ledger write per iteration (section 6). A loop never repeats a probe whose evidence ID already covers the same device build and app commit.

### Loop 0: Device truth and Horizon OS policy (reverse engineering, read-only)

Why: everything in #245 was found by probing the kernel from an app context. Tier B needs the same map for binder, the XR runtime and the display, before any code is written.

1. **KGSL ioctl map**: re-run the #245 probe on Quest 3 (not just 3S) for every `IOCTL_KGSL_*` Turnip and gamescope use (`GPUOBJ_*`, `GPUMEM_*`, `SYNCSOURCE_*`, `TIMESTAMP_EVENT`, `DRAWCTXT_*`, `SUBMIT_COMMANDS`, large allocations, `GPUOBJ_IMPORT` of dma-bufs). Record `EACCES` vs `OK` per call. Evidence: probe source + output.
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
3. Known candidates to check in order: (a) the Linux preload adapter not in `ld.so.preload` order the maintainer described; (b) `libblaudit.so` / fastpath under Horizon's seccomp; (c) `BL_VK_DRIVER` manifest unreadable so the session "IGNORING" it and falling back to an unwrapped Turnip (`droiddeck-session:137`); (d) the `quest` variant's bwrap socket name; (e) `hdr.steam` SurfaceControl path on Quest 3's panel compositor.
4. Then the `docs/quest.md` checklist: Steam login panel, sign-in, install an owned game, run it (rendering and input), Proton game, native game, desktop mode, sleep/wake, session save/restore. Mono audio, PulseAudio sink, Touch controllers as gamepad through `libfakeinput`.
5. Feed every fix upstream as a PR against #592's branch or `main` (the maintainers asked for exactly this). The fork carries only what upstream declines.

Stop when the checklist passes on two consecutive Horizon builds, or budget hits. Output: `tierA-matrix.md` (device build × app commit × scenario × result).

### Loop 2: XR bridge (the new engineering, with per-feature trace)

Seeded by the Meta runtime inventory from Loop 0. Each item is a feature with an evidence-backed "answered / partial / unresolved" status:

1. **Sample app**: a 200-line NDK OpenXR app in the DroidDeck APK (hidden activity) proving Meta's runtime can be driven from this process: instance, session, Vulkan swapchain, clear-colour frame loop, Touch poses. Static facts from Loop 0 become observed facts here.
2. **Swapchain transport**: runtime-created swapchain images → `AHardwareBuffer` → send over the bridge socket (`AHardwareBuffer_sendHandleToUnixSocket`, as `banner-ahb-v1` already does) → guest imports as `VkImage` through `VK_ANDROID_external_memory_android_hardware_buffer` is bionic-only, so the guest imports the dma-buf fd (`VK_EXT_external_memory_dma_buf`) on its Turnip. Verify both Turnips agree on format/modifier/tiling for the formats Meta's runtime offers; record which formats work.
3. **Frame loop and timing**: `xrWaitFrame` predicted display time crosses the socket with sub-millisecond latency budget; the guest's `xrEndFrame` returns the layer list; service submits. Measure added latency and dropped frames with the runtime's own stats; target 72 Hz first, 90 Hz as stretch.
4. **Space and input**: reference spaces, view poses, Touch action bindings for the `/interaction_profiles/oculus/touch_controller` profile, haptics. Hand tracking, passthrough, guardian events: out of scope for v1, recorded as unknowns.
5. **Guest runtime**: `libopenxr_questbridge.so` + `questbridge.json`; conformance smoke with the Khronos `hello_xr` (Vulkan) built for arm64 Linux inside the guest.
6. **xrizer arm64**: build on Arch ARM in the guest's toolchain, fix what does not compile, run an OpenVR sample against the bridge. Report its status to the xrizer project.
7. **Proton ARM64 VR path**: trace how DroidDeck's Proton resolves `openvr_api` / `wineopenxr` to a Linux runtime under WoW64 + FEX (REA on the Proton tree and `lsteamclient`/`vrclient` thunks; the Steam Frame crash in Proton issue #10211 and its VC++ runtime workaround are the first hypotheses). Pair findings with a minimal Windows OpenXR test app.
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
3. **Tier B scope for v1**: native arm64 + OpenVR via xrizer only, or also Windows VR titles under Proton ARM64 (the Proton issue #10211 crash makes that the riskiest path)?
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

# openvr-d3d11-demo: build, stage, launch

The smallest x64 Windows OpenVR application with one D3D11 device, submitting
per-eye textures through `IVRCompositor::Submit(TextureType_DirectX)`. It is
the Windows-side proof the Quest path still lacks:

```
openvr-d3d11-demo.exe (D3D11) -> Proton/FEX + DXVK -> Proton vrclient (native
Vulkan image/device/queue of the D3D11 texture) -> arm64 xrizer (OpenVR->OpenXR)
-> guest QuestBridge runtime -> DMA_BUF images + completion fences
-> Android XR service -> Meta OpenXR compositor
```

No headset run of this demo has happened yet; every step below the build is
**NOT RUN** until a Quest 3 session records it (see `docs/vr.md`).

## Build (host, Docker)

Same toolchain as `tools/questbridge/d3d11-probe/build-probe.sh`: the
`droiddeck-probe-mingw:24.04-v1` arm64 container (mingw-w64 x86_64 cross
compiler), the pinned OpenVR source from `openvr-sample/openvr.env`
(`v2.15.6`, commit `0924064316de3effbcd1acf1e309182a2deb1c05`) built as a
static `libopenvr_api64.a`, and a `-static` link so the PE imports only system
DLLs.

```sh
bash tools/questbridge/openvr-d3d11-demo/build-demo.sh
# Output: app/build/questbridge/openvr-d3d11-demo/
#   openvr-d3d11-demo.exe  actions.json  bindings/demo_oculus_touch.json  LICENSE.openvr
```

`actions.json` must stay beside the executable: the demo resolves the manifest
from its own directory (`GetModuleFileNameA`).

Without a headset or VR runtime, `openvr-d3d11-demo.exe --selftest` (Wine or
Windows) creates the device and both 1024x1024 eye textures, renders one frame
of each mode, verifies the eye corner pixels and exits 0 with
`PASS selftest eyes+scene rendered`.

## Stage into the guest

Deliver like the sharing probe: outside games and existing prefixes, into
`/tmp/droiddeck-diagnostics/`, with no registered shortcut pointing at it.
Internal storage is visible in the guest as `/root/Storage`
(`app/src/main/java/com/droiddeck/launcher/frontend/AddedGames.kt`).

```sh
adb push app/build/questbridge/openvr-d3d11-demo /sdcard/Download/DroidDeck/diagnostics/openvr-d3d11-demo
tools/droiddeckctl guest -- sh -c \
  'mkdir -p /tmp/droiddeck-diagnostics && cp -R /root/Storage/Download/DroidDeck/diagnostics/openvr-d3d11-demo /tmp/droiddeck-diagnostics/'
```

## Launch through the existing VR path

The VR session is the normal Steam session plus `vr_environment`
(`tools/linuxfs/overlay/usr/local/bin/droiddeck-session`): it exports
`XR_RUNTIME_JSON`, `DROIDDECK_XR_SOCKET`, `LD_LIBRARY_PATH` and writes
`~/.config/openvr/openvrpaths.vrpath` so OpenVR titles load xrizer. The demo
adds no launch path of its own; it runs with `droiddeck-proton-run` inside that
session exactly like the probe's `--vr` mode. Set both variables on every call
(`DROIDDECK_NO_STEAM=1` selects Proton `runinprefix`,
`WINEDLLOVERRIDES=d3d11,dxgi=n` selects the installed native DXVK DLLs) and
keep an outer process timeout: `WaitGetPoses` may block.

```sh
tools/droiddeckctl start vr --wait

# Phase A: red "L" / blue "R" eyes with the frame counter as a 16-bar pattern
tools/droiddeckctl guest --timeout 105 \
  --env DROIDDECK_NO_STEAM=1 --env WINEDLLOVERRIDES=d3d11,dxgi=n -- sh -c \
  'timeout -k 5s 75s droiddeck-proton-run /tmp/droiddeck-diagnostics/openvr-d3d11-demo/openvr-d3d11-demo.exe --mode eyes --seconds 30'

# Phase B: checkerboard floor, cubes, one tracked cube per controller
tools/droiddeckctl guest --timeout 105 \
  --env DROIDDECK_NO_STEAM=1 --env WINEDLLOVERRIDES=d3d11,dxgi=n -- sh -c \
  'timeout -k 5s 75s droiddeck-proton-run /tmp/droiddeck-diagnostics/openvr-d3d11-demo/openvr-d3d11-demo.exe --mode scene --seconds 60'

tools/droiddeckctl logs latest ./session-artifacts
```

Exit codes: 0 after `--seconds` or `VREvent_Quit` (`PASS frames=N`); 1 on
`VR_Init` failure or any `Submit` error, printing the enum symbol
(`FAIL stage=vr-init err=VRInitError_...`, `FAIL stage=submit eye=L err=VRCompositorError_...`);
2 on bad arguments. Once per second the demo logs
`frame= fmt= size= texL= texR= submitL= submitR=`; in scene mode it also logs
the action values when they changed (`input: trigger L= R= stick ... grip= a= b=`).

What a headset run has to record: the 1 Hz log lines, a captured frame in
each eye showing the letter and the bar pattern (so the frame number is
readable), `adb logcat -s xrbridge`, and the session bundle, bound to the
target record in `docs/vr.md` "Reporting a headset run".

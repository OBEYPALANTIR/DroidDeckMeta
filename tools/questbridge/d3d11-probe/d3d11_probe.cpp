// d3d11_probe.cpp — D3D11 NT-handle + keyed-mutex sharing probe for DroidDeck.
//
// Default (shared-only) mode spins NO VR session: it creates two hardware
// D3D11 devices on the SAME DXGI adapter/LUID, shares one RGBA8 render-target
// texture from producer to consumer via IDXGIResource1::CreateSharedHandle /
// ID3D11Device1::OpenSharedResource1, and cycles IDXGIKeyedMutex keys with FINITE waits while
// the producer clears a known colour and the consumer copies it to a staging
// texture and compares every pixel (RowPitch respected). >8 cycles.
//
// Optional --vr mode additionally VR_Inits the pinned OpenVR API and Submits
// the same D3D11 texture for both eyes for a bounded duration. Diagnostic
// only, not a playable app.
//
// Every failure prints "FAIL stage=<stage> hr=0x........ <hint>" and exits
// nonzero. No WARP fallback: software adapters fail unless --allow-software.
// Shared-handle values are never printed (ok/redacted only).
//
// Build: tools/questbridge/d3d11-probe/build-probe.sh (mingw x86_64 cross).

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>

#ifdef HAVE_OPENVR
#include <openvr.h>
#endif

static const int kTexSize = 256;
static const int kMinCycles = 9; // spec: repeat >8 cycles

static void usage(const char *argv0) {
	printf("usage: %s [--adapter N] [--cycles N] [--timeout-ms MS]\n"
	       "       %s --vr [--adapter N] [--seconds S] [--timeout-ms MS]\n"
	       "  --adapter N       DXGI adapter index (default 0); both devices use it\n"
	       "  --cycles N        shared-texture round trips, must be >8 (default 16)\n"
	       "  --seconds S       --vr submit loop duration in seconds (default 20)\n"
	       "  --timeout-ms MS   IDXGIKeyedMutex finite wait (default 2000)\n"
	       "  --vr              also VR_Init + WaitGetPoses + Submit both eyes\n"
	       "  --help            this text\n"
	       "NOTE: the parent process MUST enforce its own OUTER timeout around ALL\n"
	       "      GPU runs (CreateDevice/driver calls may hang regardless); this\n"
	       "      covers the default mode as well as --vr (WaitGetPoses may block).\n",
	    argv0, argv0);
}

// Short, secret-free hint for sharing/D3DKMT-adjacent failures.
static const char *hr_hint(HRESULT hr) {
	switch ((unsigned)hr) {
	case 0x887A0005u: // DXGI_ERROR_DEVICE_REMOVED
		return "device removed; see removed-reason (D3DKMT escape failure under Proton/DXVK surfaces here)";
	case 0x887A0006u: // DXGI_ERROR_DEVICE_HUNG
		return "device hung";
	case 0x887A0007u: // DXGI_ERROR_DEVICE_RESET
		return "device reset";
	case 0x887A002Bu: // DXGI_ERROR_NOT_CURRENTLY_AVAILABLE
		return "resource not currently available (keyed-mutex abandoned?)";
	case 0x887A0001u: // DXGI_ERROR_INVALID_CALL
		return "invalid call (sharing flags / bind flags rejected?)";
	case 0x80070005u: // E_ACCESSDENIED
		return "access denied (NT-handle ACL / sandbox restriction?)";
	case 0x80070057u: // E_INVALIDARG
		return "invalid argument (handle not shareable on this adapter?)";
	case 102u: // WAIT_TIMEOUT as HRESULT
		return "finite keyed-mutex wait expired (producer/consumer desync or GPU stall)";
	case 128u: // WAIT_ABANDONED
		return "keyed-mutex abandoned (peer died holding the key)";
	default:
		return "";
	}
}

static void fail(const char *stage, HRESULT hr, ID3D11Device *dev) {
	const char *hint = hr_hint(hr);
	printf("FAIL stage=%s hr=0x%08X%s%s\n", stage, (unsigned)hr,
	    hint[0] ? " " : "", hint);
	if (dev && (unsigned)hr == 0x887A0005u) {
		HRESULT rr = dev->GetDeviceRemovedReason();
		printf("FAIL stage=%s removed-reason=0x%08X %s\n", stage,
		    (unsigned)rr, hr_hint(rr));
	}
	fflush(stdout);
	exit(1);
}

static const char *fl_name(D3D_FEATURE_LEVEL fl) {
	switch (fl) {
	case D3D_FEATURE_LEVEL_11_1: return "11.1";
	case D3D_FEATURE_LEVEL_11_0: return "11.0";
	case D3D_FEATURE_LEVEL_10_1: return "10.1";
	case D3D_FEATURE_LEVEL_10_0: return "10.0";
	default: return "other";
	}
}

static void fl_bytes(int cycle, uint8_t out[4]) {
	// Known per-cycle colour; v/255.0f clear round-trips exactly through UNORM8.
	out[0] = (uint8_t)((cycle * 53 + 11) & 0xFF);
	out[1] = (uint8_t)((cycle * 97 + 71) & 0xFF);
	out[2] = (uint8_t)((cycle * 31 + 131) & 0xFF);
	out[3] = 0xFF;
}

// Strict unsigned decimal parse: garbage, signs, and overflow all fail.
static bool strict_int(const char *s, int lo, int hi, int *out) {
	if (!s || !*s || strlen(s) > 10) return false;
	unsigned v = 0;
	for (const char *p = s; *p; p++) {
		if (*p < '0' || *p > '9') return false;
		unsigned d = (unsigned)(*p - '0');
		if (v > ((unsigned)hi - d) / 10) return false; // would exceed hi
		v = v * 10 + d;
	}
	if ((int)v < lo) return false;
	*out = (int)v;
	return true;
}

// Strict double parse: trailing junk fails; inf/nan/overflow fail range check.
static bool strict_seconds(const char *s, double *out) {
	if (!s || !*s) return false;
	double v = 0;
	char extra = 0;
	if (sscanf(s, "%lf%c", &v, &extra) != 1) return false;
	if (!(v > 0.0) || v > 3600.0) return false;
	*out = v;
	return true;
}

static const char *need_val(int *i, int argc, char **argv) {
	if (*i + 1 >= argc) {
		printf("FAIL stage=args hr=0x00000000 missing value for %s\n", argv[*i]);
		fflush(stdout);
		exit(2);
	}
	return argv[++(*i)];
}

int main(int argc, char **argv) {
	int adapter_index = 0, cycles = 16, timeout_ms = 2000;
	double seconds = 20.0;
	bool vr = false;
	for (int i = 1; i < argc; i++) {
		const char *val = NULL;
		if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
		else if (!strcmp(argv[i], "--vr")) vr = true;
		else if (!strcmp(argv[i], "--adapter")) {
			val = need_val(&i, argc, argv);
			if (!strict_int(val, 0, 64, &adapter_index)) { printf("FAIL stage=args hr=0x00000000 bad --adapter '%s'\n", val); return 2; }
		} else if (!strcmp(argv[i], "--cycles")) {
			val = need_val(&i, argc, argv);
			if (!strict_int(val, kMinCycles, 1000000, &cycles)) { printf("FAIL stage=args hr=0x00000000 bad --cycles '%s' (must be >8)\n", val); return 2; }
		} else if (!strcmp(argv[i], "--timeout-ms")) {
			val = need_val(&i, argc, argv);
			if (!strict_int(val, 1, 60000, &timeout_ms)) { printf("FAIL stage=args hr=0x00000000 bad --timeout-ms '%s'\n", val); return 2; }
		} else if (!strcmp(argv[i], "--seconds")) {
			val = need_val(&i, argc, argv);
			if (!strict_seconds(val, &seconds)) { printf("FAIL stage=args hr=0x00000000 bad --seconds '%s'\n", val); return 2; }
		}
		else { printf("FAIL stage=args hr=0x00000000 unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
	}
	setvbuf(stdout, NULL, _IONBF, 0);

	// --- Adapter: both devices MUST share it (same LUID). ---
	IDXGIFactory1 *factory = NULL;
	HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory);
	if (FAILED(hr) || !factory) fail("create-factory", hr, NULL);

	IDXGIAdapter1 *adapter = NULL;
	hr = factory->EnumAdapters1((UINT)adapter_index, &adapter);
	if (FAILED(hr) || !adapter) fail("enum-adapter", hr, NULL);
	DXGI_ADAPTER_DESC1 desc;
	hr = adapter->GetDesc1(&desc);
	if (FAILED(hr)) fail("adapter-desc", hr, NULL);
	char name[128];
	wcstombs(name, desc.Description, sizeof(name));
	name[sizeof(name) - 1] = '\0';
	printf("adapter: index=%d desc=\"%s\" vendor=0x%04X device=0x%04X "
	       "luid=%08X:%08X flags=0x%X\n",
	    adapter_index, name, desc.VendorId, desc.DeviceId,
	    (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart,
	    desc.Flags);
	if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
		printf("FAIL stage=adapter hr=0x00000000 software adapter refused: WARP must never mask hardware failure; pick a hardware adapter with --adapter N\n");
		return 1;
	}

	// --- Two hardware devices on that adapter. No WARP fallback. ---
	static const D3D_FEATURE_LEVEL kLevels[] = {
		D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
	};
	ID3D11Device *devA = NULL, *devB = NULL;
	ID3D11DeviceContext *ctxA = NULL, *ctxB = NULL;
	D3D_FEATURE_LEVEL flA, flB;
	hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels, 4,
	    D3D11_SDK_VERSION, &devA, &flA, &ctxA);
	if (FAILED(hr) || !devA) fail("create-device-producer", hr, NULL);
	hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, NULL,
	    D3D11_CREATE_DEVICE_BGRA_SUPPORT, kLevels, 4,
	    D3D11_SDK_VERSION, &devB, &flB, &ctxB);
	if (FAILED(hr) || !devB) fail("create-device-consumer", hr, devA);
	printf("devices: producer-fl=%s consumer-fl=%s\n", fl_name(flA), fl_name(flB));

	ID3D11Device1 *devB1 = NULL;
	hr = devB->QueryInterface(__uuidof(ID3D11Device1), (void **)&devB1);
	if (FAILED(hr)) fail("qi-device1-consumer", hr, devA);

	// --- Shared RGBA8 render target (NT handle + keyed mutex). ---
	D3D11_TEXTURE2D_DESC td;
	memset(&td, 0, sizeof(td));
	td.Width = kTexSize;
	td.Height = kTexSize;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
	td.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
	ID3D11Texture2D *texA = NULL;
	hr = devA->CreateTexture2D(&td, NULL, &texA);
	if (FAILED(hr) || !texA) fail("create-shared-texture", hr, devA);
	ID3D11RenderTargetView *rtvA = NULL;
	hr = devA->CreateRenderTargetView((ID3D11Resource *)texA, NULL, &rtvA);
	if (FAILED(hr) || !rtvA) fail("create-rtv", hr, devA);

	IDXGIResource1 *resourceA = NULL;
	hr = texA->QueryInterface(__uuidof(IDXGIResource1), (void **)&resourceA);
	if (FAILED(hr) || !resourceA) fail("qi-resource1-producer", hr, devA);
	HANDLE shared = NULL;
	hr = resourceA->CreateSharedHandle(NULL,
	    DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, NULL, &shared);
	resourceA->Release();
	if (FAILED(hr) || !shared) fail("create-shared-handle", hr, devA);
	printf("shared: handle=ok size=%dx%d format=RGBA8\n", kTexSize, kTexSize);

	ID3D11Texture2D *texB = NULL;
	hr = devB1->OpenSharedResource1(shared, __uuidof(ID3D11Texture2D), (void **)&texB);
	if (FAILED(hr) || !texB) fail("open-shared-resource", hr, devB);
	printf("shared: open-on-consumer=ok\n");

	IDXGIKeyedMutex *mtxA = NULL, *mtxB = NULL;
	hr = texA->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mtxA);
	if (FAILED(hr)) fail("qi-mutex-producer", hr, devA);
	hr = texB->QueryInterface(__uuidof(IDXGIKeyedMutex), (void **)&mtxB);
	if (FAILED(hr)) fail("qi-mutex-consumer", hr, devA);

	D3D11_TEXTURE2D_DESC sd = td;
	sd.BindFlags = 0;
	sd.MiscFlags = 0;
	sd.Usage = D3D11_USAGE_STAGING;
	sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	ID3D11Texture2D *staging = NULL;
	hr = devB->CreateTexture2D(&sd, NULL, &staging);
	if (FAILED(hr) || !staging) fail("create-staging", hr, devB);

	// --- Cycle keys: producer owns 2i, hands 2i+1 to consumer, gets 2i+2 back. ---
	for (int i = 0; i < cycles; i++) {
		UINT64 kp = (UINT64)(2 * i), kh = kp + 1, kn = kp + 2;
		hr = mtxA->AcquireSync(kp, (DWORD)timeout_ms);
		if (hr != S_OK) fail("acquire-producer", hr, devA);
		uint8_t want[4];
		fl_bytes(i, want);
		float clear[4] = { want[0] / 255.0f, want[1] / 255.0f,
			want[2] / 255.0f, want[3] / 255.0f };
		ctxA->ClearRenderTargetView(rtvA, clear);
		ctxA->Flush();
		hr = mtxA->ReleaseSync(kh);
		if (FAILED(hr)) fail("release-producer", hr, devA);

		hr = mtxB->AcquireSync(kh, (DWORD)timeout_ms);
		if (hr != S_OK) fail("acquire-consumer", hr, devB);
		ctxB->CopyResource((ID3D11Resource *)staging, (ID3D11Resource *)texB);
		ctxB->Flush();
		hr = mtxB->ReleaseSync(kn);
		if (FAILED(hr)) fail("release-consumer", hr, devB);

		D3D11_MAPPED_SUBRESOURCE map;
		// Never block in Map: DO_NOT_WAIT + bounded STILL_DRAWING poll.
		ULONGLONG map_t0 = GetTickCount64();
		for (;;) {
			hr = ctxB->Map((ID3D11Resource *)staging, 0, D3D11_MAP_READ,
			    D3D11_MAP_FLAG_DO_NOT_WAIT, &map);
			if (hr == S_OK) break;
			if ((unsigned)hr != 0x887A0027u) // DXGI_ERROR_WAS_STILL_DRAWING
				fail("map-staging", hr, devB);
			if (GetTickCount64() - map_t0 > (ULONGLONG)timeout_ms) {
				printf("FAIL stage=map-staging cycle=%d hr=0x887A0027 GPU still drawing after %d ms (bounded wait expired)\n",
				    i, timeout_ms);
				fflush(stdout);
				return 1;
			}
			Sleep(1);
		}
		bool ok = true;
		int bx = -1, by = -1;
		uint8_t got[4] = { 0, 0, 0, 0 };
		for (int y = 0; y < kTexSize && ok; y++) {
			const uint8_t *row = (const uint8_t *)map.pData + (size_t)y * map.RowPitch;
			for (int x = 0; x < kTexSize; x++) {
				if (memcmp(row + (size_t)x * 4, want, 4)) {
					ok = false; bx = x; by = y;
					memcpy(got, row + (size_t)x * 4, 4);
					break;
				}
			}
		}
		ctxB->Unmap((ID3D11Resource *)staging, 0);
		if (!ok) {
			printf("FAIL stage=compare-pixels cycle=%d hr=0x00000000 "
			       "first-bad=(%d,%d) got=%02X%02X%02X%02X want=%02X%02X%02X%02X rowpitch=%u\n",
			    i, bx, by, got[0], got[1], got[2], got[3],
			    want[0], want[1], want[2], want[3], map.RowPitch);
			fflush(stdout);
			return 1;
		}
		printf("cycle %d keys %llu->%llu ok rgba=%02X%02X%02X%02X rowpitch=%u\n",
		    i, (unsigned long long)kp, (unsigned long long)kh,
		    want[0], want[1], want[2], want[3], map.RowPitch);
	}
	printf("PASS shared-cycles=%d adapter=%d luid=%08X:%08X\n", cycles,
	    adapter_index, (unsigned)desc.AdapterLuid.HighPart,
	    (unsigned)desc.AdapterLuid.LowPart);

#ifdef HAVE_OPENVR
	if (vr) {
		vr::EVRInitError verr = vr::VRInitError_None;
		vr::IVRSystem *sys = vr::VR_Init(&verr, vr::VRApplication_Scene);
		if (!sys) {
			printf("FAIL stage=vr-init hr=0x00000000 vrerr=%d %s\n", verr,
			    vr::VR_GetVRInitErrorAsEnglishDescription(verr));
			return 1;
		}
		vr::IVRCompositor *comp = vr::VRCompositor();
		if (!comp) {
			printf("FAIL stage=vr-compositor hr=0x00000000 no compositor\n");
			vr::VR_Shutdown();
			return 1;
		}
		uint32_t rw = 0, rh = 0;
		sys->GetRecommendedRenderTargetSize(&rw, &rh);
		printf("vr: runtime=%s recommended=%ux%u\n",
		    sys->GetRuntimeVersion(), rw, rh);
		vr::Texture_t eye;
		eye.handle = (void *)texA; // same D3D11 texture, both eyes
		eye.eType = vr::TextureType_DirectX;
		eye.eColorSpace = vr::ColorSpace_Auto;
		vr::VRTextureBounds_t bounds = { 0, 0, 1, 1 };
		ULONGLONG t0 = GetTickCount64();
		long submits = 0;
		vr::EVRCompositorError first_err = vr::VRCompositorError_None;
		while ((GetTickCount64() - t0) / 1000.0 < seconds) {
			vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
			// May block without a live runtime; parent enforces a process timeout.
			comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
			vr::EVRCompositorError e0 = comp->Submit(vr::Eye_Left, &eye, &bounds, vr::Submit_Default);
			vr::EVRCompositorError e1 = comp->Submit(vr::Eye_Right, &eye, &bounds, vr::Submit_Default);
			if (e0 == vr::VRCompositorError_None && e1 == vr::VRCompositorError_None) {
				submits += 2;
			} else if (first_err == vr::VRCompositorError_None) {
				first_err = e0 != vr::VRCompositorError_None ? e0 : e1;
			}
		}
		printf("vr: submits=%ld first-submit-error=%d seconds=%.1f\n",
		    submits, (int)first_err, seconds);
		vr::VR_Shutdown();
		if (submits == 0) {
			printf("FAIL stage=vr-submit hr=0x00000000 no successful submits\n");
			return 1;
		}
		printf("PASS vr-submits=%ld\n", submits);
	}
#else
	if (vr) {
		printf("FAIL stage=vr hr=0x00000000 built without OpenVR (HAVE_OPENVR unset)\n");
		return 1;
	}
#endif

	if (staging) staging->Release();
	if (mtxB) mtxB->Release();
	if (mtxA) mtxA->Release();
	if (texB) texB->Release();
	if (shared) CloseHandle(shared);
	if (rtvA) rtvA->Release();
	if (texA) texA->Release();
	if (devB1) devB1->Release();
	if (ctxB) ctxB->Release();
	if (ctxA) ctxA->Release();
	if (devB) devB->Release();
	if (devA) devA->Release();
	if (adapter) adapter->Release();
	if (factory) factory->Release();
	return 0;
}

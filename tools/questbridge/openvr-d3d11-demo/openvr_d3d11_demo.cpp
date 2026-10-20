// openvr_d3d11_demo.cpp — the smallest x64 Windows OpenVR application that renders
// with ONE D3D11 device and submits per-eye textures via IVRCompositor::Submit
// (TextureType_DirectX). It is the missing Windows proof for the Quest path:
// D3D11 -> Proton/DXVK -> Proton vrclient -> xrizer -> QuestBridge -> Meta compositor.
//
//   --mode eyes   (default) left eye red + white "L", right eye blue + white "R",
//                 plus the frame counter as a 16-bit bar pattern along the bottom.
//   --mode scene  checkerboard floor, a few cubes, one tracked cube per controller
//                 (actions.json: trigger scales it, A/B fire haptics).
//   --seconds N   stop after N seconds (default 20); VREvent_Quit stops earlier.
//   --selftest    no VR runtime: build the device and both eye textures, render one
//                 frame of each mode, verify the corner pixels, exit 0.
//
// Exit 1 on VR_Init failure or a Submit error (enum name printed), 2 on bad args.
// Diagnostic only: COM objects are not released on the way out (process exit).
//
// Build: tools/questbridge/openvr-d3d11-demo/build-demo.sh (mingw x86_64 cross).

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>

#include <openvr.h>

static const char kShaders[] = R"(
cbuffer C : register(b0) {
	row_major float4x4 mvp;
	row_major float4x4 model;
	float4 color;
	float4 color2;
	float4 params; // x: 1 = checkerboard in world XZ (1 m squares)
};
struct VSOut { float4 pos : SV_Position; float3 wpos : TEXCOORD0; };
VSOut vs(float3 pos : POSITION) {
	VSOut o;
	o.pos = mul(mvp, float4(pos, 1));
	o.wpos = mul(model, float4(pos, 1)).xyz;
	return o;
}
float4 ps(VSOut i) : SV_Target {
	if (params.x > 0.5) {
		float c = fmod(floor(i.wpos.x) + floor(i.wpos.z), 2.0);
		return abs(c) < 0.5 ? color : color2;
	}
	return color;
}
)";

// Row-major 4x4, column-vector convention (v' = M v), the same layout OpenVR uses.
struct Mat4 { float m[4][4]; };

static Mat4 identity() {
	Mat4 r = {};
	for (int i = 0; i < 4; i++) r.m[i][i] = 1;
	return r;
}
static Mat4 mul(const Mat4 &a, const Mat4 &b) {
	Mat4 r = {};
	for (int i = 0; i < 4; i++)
		for (int j = 0; j < 4; j++)
			for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
	return r;
}
static Mat4 translate(float x, float y, float z) {
	Mat4 r = identity();
	r.m[0][3] = x; r.m[1][3] = y; r.m[2][3] = z;
	return r;
}
static Mat4 scale(float x, float y, float z) {
	Mat4 r = identity();
	r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z;
	return r;
}
static Mat4 rotateX(float deg) {
	float c = cosf(deg * 3.14159265f / 180), s = sinf(deg * 3.14159265f / 180);
	Mat4 r = identity();
	r.m[1][1] = c; r.m[1][2] = -s; r.m[2][1] = s; r.m[2][2] = c;
	return r;
}
static Mat4 from34(const vr::HmdMatrix34_t &h) {
	Mat4 r = identity();
	memcpy(r.m, h.m, sizeof(h.m));
	return r;
}
static Mat4 from44(const vr::HmdMatrix44_t &h) {
	Mat4 r;
	memcpy(r.m, h.m, sizeof(h.m));
	return r;
}
// Inverse of a rigid transform (rotation + translation only).
static Mat4 invRigid(const Mat4 &a) {
	Mat4 r = identity();
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
	for (int i = 0; i < 3; i++) {
		r.m[i][3] = 0;
		for (int j = 0; j < 3; j++) r.m[i][3] -= r.m[i][j] * a.m[j][3];
	}
	return r;
}
// Selftest stand-in for GetProjectionMatrix: symmetric 90 degree, D3D clip z in [0,1].
static Mat4 perspective(float nearZ, float farZ) {
	Mat4 r = {};
	r.m[0][0] = 1; r.m[1][1] = 1;
	r.m[2][2] = farZ / (nearZ - farZ);
	r.m[2][3] = nearZ * farZ / (nearZ - farZ);
	r.m[3][2] = -1;
	return r;
}

struct Constants {
	Mat4 mvp, model;
	float color[4], color2[4], params[4];
};

static const char *compositorErrorName(vr::EVRCompositorError e) {
	switch (e) {
	case vr::VRCompositorError_None: return "VRCompositorError_None";
	case vr::VRCompositorError_RequestFailed: return "VRCompositorError_RequestFailed";
	case vr::VRCompositorError_IncompatibleVersion: return "VRCompositorError_IncompatibleVersion";
	case vr::VRCompositorError_DoNotHaveFocus: return "VRCompositorError_DoNotHaveFocus";
	case vr::VRCompositorError_InvalidTexture: return "VRCompositorError_InvalidTexture";
	case vr::VRCompositorError_IsNotSceneApplication: return "VRCompositorError_IsNotSceneApplication";
	case vr::VRCompositorError_TextureIsOnWrongDevice: return "VRCompositorError_TextureIsOnWrongDevice";
	case vr::VRCompositorError_TextureUsesUnsupportedFormat: return "VRCompositorError_TextureUsesUnsupportedFormat";
	case vr::VRCompositorError_SharedTexturesNotSupported: return "VRCompositorError_SharedTexturesNotSupported";
	case vr::VRCompositorError_IndexOutOfRange: return "VRCompositorError_IndexOutOfRange";
	case vr::VRCompositorError_AlreadySubmitted: return "VRCompositorError_AlreadySubmitted";
	case vr::VRCompositorError_InvalidBounds: return "VRCompositorError_InvalidBounds";
	case vr::VRCompositorError_AlreadySet: return "VRCompositorError_AlreadySet";
	default: return "VRCompositorError_<unknown>";
	}
}

static void fail(const char *stage, HRESULT hr) {
	printf("FAIL stage=%s hr=0x%08X\n", stage, (unsigned)hr);
	fflush(stdout);
	exit(1);
}

static const uint32_t kMaxEyeDim = 1024;
static const DXGI_FORMAT kEyeFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

struct Renderer {
	ID3D11Device *dev = nullptr;
	ID3D11DeviceContext *ctx = nullptr;
	ID3D11Texture2D *eyeTex[2] = {};
	ID3D11RenderTargetView *eyeRtv[2] = {};
	ID3D11DepthStencilView *eyeDsv[2] = {};
	ID3D11Buffer *vb = nullptr, *cb = nullptr;
	ID3D11VertexShader *vs = nullptr;
	ID3D11PixelShader *ps = nullptr;
	ID3D11InputLayout *layout = nullptr;
	ID3D11RasterizerState *rs = nullptr;
	ID3D11DepthStencilState *dss = nullptr;
	uint32_t w = 0, h = 0;

	void init(uint32_t width, uint32_t height) {
		w = width; h = height;
		static const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
		D3D_FEATURE_LEVEL got;
		HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
		    D3D11_SDK_VERSION, &dev, &got, &ctx);
		if (FAILED(hr)) fail("create-device", hr);
		printf("d3d11: device=%p feature-level=0x%X\n", (void *)dev, got);

		for (int e = 0; e < 2; e++) {
			D3D11_TEXTURE2D_DESC td = {};
			td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
			td.Format = kEyeFormat; td.SampleDesc.Count = 1;
			td.Usage = D3D11_USAGE_DEFAULT;
			td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
			hr = dev->CreateTexture2D(&td, nullptr, &eyeTex[e]);
			if (FAILED(hr)) fail("create-eye-texture", hr);
			hr = dev->CreateRenderTargetView(eyeTex[e], nullptr, &eyeRtv[e]);
			if (FAILED(hr)) fail("create-rtv", hr);
			td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
			td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
			ID3D11Texture2D *depth = nullptr;
			hr = dev->CreateTexture2D(&td, nullptr, &depth);
			if (FAILED(hr)) fail("create-depth", hr);
			hr = dev->CreateDepthStencilView(depth, nullptr, &eyeDsv[e]);
			if (FAILED(hr)) fail("create-dsv", hr);
		}

		// Vertices 0..5: unit quad in XY (z = 0); 6..41: unit cube. All centred, side 1.
		float v[42][3];
		static const float q[6][2] = { {-.5f,-.5f}, {-.5f,.5f}, {.5f,.5f}, {-.5f,-.5f}, {.5f,.5f}, {.5f,-.5f} };
		for (int i = 0; i < 6; i++) { v[i][0] = q[i][0]; v[i][1] = q[i][1]; v[i][2] = 0; }
		int n = 6;
		for (int axis = 0; axis < 3; axis++)
			for (int side = -1; side <= 1; side += 2)
				for (int i = 0; i < 6; i++) {
					float *p = v[n++];
					p[axis] = 0.5f * side;
					p[(axis + 1) % 3] = q[i][0];
					p[(axis + 2) % 3] = q[i][1];
				}
		D3D11_BUFFER_DESC bd = {};
		bd.ByteWidth = sizeof(v); bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
		D3D11_SUBRESOURCE_DATA sd = { v, 0, 0 };
		hr = dev->CreateBuffer(&bd, &sd, &vb);
		if (FAILED(hr)) fail("create-vb", hr);
		bd = {};
		bd.ByteWidth = sizeof(Constants); bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		hr = dev->CreateBuffer(&bd, nullptr, &cb);
		if (FAILED(hr)) fail("create-cb", hr);

		ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
		hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "demo", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err);
		if (FAILED(hr)) { if (err) printf("%s\n", (char *)err->GetBufferPointer()); fail("compile-vs", hr); }
		hr = D3DCompile(kShaders, sizeof(kShaders) - 1, "demo", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err);
		if (FAILED(hr)) { if (err) printf("%s\n", (char *)err->GetBufferPointer()); fail("compile-ps", hr); }
		hr = dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs);
		if (FAILED(hr)) fail("create-vs", hr);
		hr = dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps);
		if (FAILED(hr)) fail("create-ps", hr);
		D3D11_INPUT_ELEMENT_DESC ie = { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 };
		hr = dev->CreateInputLayout(&ie, 1, vsb->GetBufferPointer(), vsb->GetBufferSize(), &layout);
		if (FAILED(hr)) fail("create-layout", hr);

		D3D11_RASTERIZER_DESC rd = {};
		rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
		hr = dev->CreateRasterizerState(&rd, &rs);
		if (FAILED(hr)) fail("create-rs", hr);
		D3D11_DEPTH_STENCIL_DESC dd = {};
		dd.DepthEnable = TRUE; dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL; dd.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
		hr = dev->CreateDepthStencilState(&dd, &dss);
		if (FAILED(hr)) fail("create-dss", hr);
	}

	void begin(int eye, const float clear[4]) {
		ctx->OMSetRenderTargets(1, &eyeRtv[eye], eyeDsv[eye]);
		ctx->ClearRenderTargetView(eyeRtv[eye], clear);
		ctx->ClearDepthStencilView(eyeDsv[eye], D3D11_CLEAR_DEPTH, 1.0f, 0);
		D3D11_VIEWPORT vp = { 0, 0, (float)w, (float)h, 0, 1 };
		ctx->RSSetViewports(1, &vp);
		UINT stride = 12, offset = 0;
		ctx->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
		ctx->IASetInputLayout(layout);
		ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		ctx->VSSetShader(vs, nullptr, 0);
		ctx->PSSetShader(ps, nullptr, 0);
		ctx->VSSetConstantBuffers(0, 1, &cb);
		ctx->PSSetConstantBuffers(0, 1, &cb);
		ctx->RSSetState(rs);
		ctx->OMSetDepthStencilState(dss, 0);
	}

	// cube=false draws the unit quad, true the unit cube; viewProj * model.
	void draw(const Mat4 &viewProj, const Mat4 &model, const float *color, bool cube,
	          const float *color2 = nullptr) {
		Constants c = {};
		c.mvp = mul(viewProj, model);
		c.model = model;
		memcpy(c.color, color, 16);
		if (color2) { memcpy(c.color2, color2, 16); c.params[0] = 1; }
		ctx->UpdateSubresource(cb, 0, nullptr, &c, 0, 0);
		ctx->Draw(cube ? 36 : 6, cube ? 6 : 0);
	}

	// NDC rectangle (identity view/projection): centre (cx, cy), size (sw, sh).
	void rect(float cx, float cy, float sw, float sh, const float *color) {
		draw(identity(), mul(translate(cx, cy, 0), scale(sw, sh, 1)), color, false);
	}

	// Reads back pixel (x, y) of an eye texture as RGBA8.
	void readPixel(int eye, uint32_t x, uint32_t y, uint8_t out[4]) {
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = 1; td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
		td.Format = kEyeFormat; td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		ID3D11Texture2D *staging = nullptr;
		HRESULT hr = dev->CreateTexture2D(&td, nullptr, &staging);
		if (FAILED(hr)) fail("create-staging", hr);
		D3D11_BOX box = { x, y, 0, x + 1, y + 1, 1 };
		ctx->CopySubresourceRegion(staging, 0, 0, 0, 0, eyeTex[eye], 0, &box);
		D3D11_MAPPED_SUBRESOURCE map;
		hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
		if (FAILED(hr)) fail("map-staging", hr);
		memcpy(out, map.pData, 4);
		ctx->Unmap(staging, 0);
		staging->Release();
	}
};

static const float kWhite[4] = { 1, 1, 1, 1 }, kDark[4] = { 0.2f, 0.2f, 0.2f, 1 };
static const float kRed[4] = { 1, 0, 0, 1 }, kBlue[4] = { 0, 0, 1, 1 };

// Phase A: solid eye colour, white letter, frame counter as 16 bars along the bottom.
static void renderEyes(Renderer &r, int eye, uint32_t frame) {
	r.begin(eye, eye == 0 ? kRed : kBlue);
	if (eye == 0) { // L
		r.rect(-0.15f, 0.0f, 0.1f, 0.6f, kWhite);
		r.rect(0.05f, -0.25f, 0.5f, 0.1f, kWhite);
	} else { // R
		r.rect(-0.2f, 0.0f, 0.1f, 0.6f, kWhite);
		r.rect(0.0f, 0.25f, 0.5f, 0.1f, kWhite);
		r.rect(0.0f, 0.0f, 0.5f, 0.1f, kWhite);
		r.rect(0.2f, 0.125f, 0.1f, 0.35f, kWhite);
		r.rect(0.15f, -0.175f, 0.1f, 0.35f, kWhite);
	}
	for (int bit = 0; bit < 16; bit++) // MSB on the left
		r.rect(-0.75f + bit * 0.1f, -0.8f, 0.06f, 0.1f, (frame >> (15 - bit)) & 1 ? kWhite : kDark);
}

struct Hand { Mat4 pose; bool valid; float trigger; };

// Phase B: checkerboard floor, three fixed cubes, one cube per tracked controller.
static void renderScene(Renderer &r, int eye, const Mat4 &viewProj, const Hand hands[2]) {
	static const float sky[4] = { 0.05f, 0.05f, 0.1f, 1 };
	static const float c0[4] = { 0.8f, 0.8f, 0.8f, 1 }, c1[4] = { 0.15f, 0.15f, 0.15f, 1 };
	static const float cubeColors[3][4] = { { 1, .5f, 0, 1 }, { 0, 1, .5f, 1 }, { .6f, 0, 1, 1 } };
	static const float cubePos[3][4] = { { 0, 1.2f, -2, .3f }, { -1.5f, .5f, -2.5f, .5f }, { 1.5f, .8f, -1.5f, .4f } };
	static const float handColors[2][4] = { { 1, 0.3f, 0.3f, 1 }, { 0.3f, 0.3f, 1, 1 } };
	r.begin(eye, sky);
	r.draw(viewProj, mul(rotateX(-90), scale(20, 20, 1)), c0, false, c1);
	for (int i = 0; i < 3; i++)
		r.draw(viewProj, mul(translate(cubePos[i][0], cubePos[i][1], cubePos[i][2]),
		    scale(cubePos[i][3], cubePos[i][3], cubePos[i][3])), cubeColors[i], true);
	for (int i = 0; i < 2; i++) {
		if (!hands[i].valid) continue;
		float s = 0.05f * (1 + 2 * hands[i].trigger);
		r.draw(viewProj, mul(hands[i].pose, scale(s, s, s)), handColors[i], true);
	}
}

struct Actions {
	vr::IVRInput *input = nullptr;
	vr::VRActionSetHandle_t set = vr::k_ulInvalidActionSetHandle;
	vr::VRActionHandle_t trigger, grip, thumbstick, a, b, haptic, hand[2];
	vr::VRInputValueHandle_t handSource[2];
	// Last logged snapshot, quantised so 1 Hz logging only fires on real change.
	int lastTrigger[2] = { -1, -1 }, lastStick[2][2] = { { -9, -9 }, { -9, -9 } };
	int lastGrip = -1, lastA = -1, lastB = -1;

	bool init() {
		input = vr::VRInput();
		char exe[MAX_PATH];
		GetModuleFileNameA(nullptr, exe, MAX_PATH);
		char *slash = strrchr(exe, '\\');
		if (slash) slash[1] = 0;
		char manifest[MAX_PATH];
		snprintf(manifest, sizeof(manifest), "%sactions.json", exe);
		vr::EVRInputError e = input->SetActionManifestPath(manifest);
		if (e != vr::VRInputError_None) { printf("input: SetActionManifestPath(%s) err=%d; no controller input\n", manifest, e); return false; }
		input->GetActionSetHandle("/actions/demo", &set);
		input->GetActionHandle("/actions/demo/in/trigger", &trigger);
		input->GetActionHandle("/actions/demo/in/grip", &grip);
		input->GetActionHandle("/actions/demo/in/thumbstick", &thumbstick);
		input->GetActionHandle("/actions/demo/in/a_button", &a);
		input->GetActionHandle("/actions/demo/in/b_button", &b);
		input->GetActionHandle("/actions/demo/out/haptic", &haptic);
		input->GetActionHandle("/actions/demo/in/hand_left", &hand[0]);
		input->GetActionHandle("/actions/demo/in/hand_right", &hand[1]);
		input->GetInputSourceHandle("/user/hand/left", &handSource[0]);
		input->GetInputSourceHandle("/user/hand/right", &handSource[1]);
		printf("input: manifest %s\n", manifest);
		return true;
	}

	// Per frame: update, read poses/trigger into hands, fire haptics on A/B press.
	void update(Hand hands[2]) {
		vr::VRActiveActionSet_t active = {};
		active.ulActionSet = set;
		input->UpdateActionState(&active, sizeof(active), 1);
		for (int i = 0; i < 2; i++) {
			vr::InputPoseActionData_t pose = {};
			input->GetPoseActionDataRelativeToNow(hand[i], vr::TrackingUniverseStanding, 0, &pose, sizeof(pose), vr::k_ulInvalidInputValueHandle);
			hands[i].valid = pose.bActive && pose.pose.bPoseIsValid;
			if (hands[i].valid) hands[i].pose = from34(pose.pose.mDeviceToAbsoluteTracking);
			vr::InputAnalogActionData_t an = {};
			input->GetAnalogActionData(trigger, &an, sizeof(an), handSource[i]);
			hands[i].trigger = an.bActive ? an.x : 0;
		}
		// Per hand: activeOrigin is an origin handle, not the input-source handle
		// ulRestrictToDevice wants (xrizer would buzz both controllers).
		vr::VRActionHandle_t buttons[2] = { a, b };
		for (int h = 0; h < 2; h++)
			for (int i = 0; i < 2; i++) {
				vr::InputDigitalActionData_t d = {};
				input->GetDigitalActionData(buttons[i], &d, sizeof(d), handSource[h]);
				if (d.bActive && d.bChanged && d.bState)
					input->TriggerHapticVibrationAction(haptic, 0, 0.1f, 100, 0.8f, handSource[h]);
			}
	}

	// Once per second: print the action values if any changed since the last print.
	void log(const Hand hands[2]) {
		vr::InputDigitalActionData_t dg = {}, da = {}, db = {};
		input->GetDigitalActionData(grip, &dg, sizeof(dg), vr::k_ulInvalidInputValueHandle);
		input->GetDigitalActionData(a, &da, sizeof(da), vr::k_ulInvalidInputValueHandle);
		input->GetDigitalActionData(b, &db, sizeof(db), vr::k_ulInvalidInputValueHandle);
		int stick[2][2], trig[2];
		bool changed = dg.bState != lastGrip || da.bState != lastA || db.bState != lastB;
		for (int i = 0; i < 2; i++) {
			vr::InputAnalogActionData_t an = {};
			input->GetAnalogActionData(thumbstick, &an, sizeof(an), handSource[i]);
			stick[i][0] = (int)lroundf(an.x * 10); stick[i][1] = (int)lroundf(an.y * 10);
			trig[i] = (int)lroundf(hands[i].trigger * 10);
			changed |= stick[i][0] != lastStick[i][0] || stick[i][1] != lastStick[i][1] || trig[i] != lastTrigger[i];
		}
		if (!changed) return;
		printf("input: trigger L=%.1f R=%.1f stick L=(%.1f,%.1f) R=(%.1f,%.1f) grip=%d a=%d b=%d poseL=%d poseR=%d\n",
		    trig[0] / 10.f, trig[1] / 10.f, stick[0][0] / 10.f, stick[0][1] / 10.f, stick[1][0] / 10.f, stick[1][1] / 10.f,
		    dg.bState, da.bState, db.bState, hands[0].valid, hands[1].valid);
		lastGrip = dg.bState; lastA = da.bState; lastB = db.bState;
		for (int i = 0; i < 2; i++) { lastTrigger[i] = trig[i]; lastStick[i][0] = stick[i][0]; lastStick[i][1] = stick[i][1]; }
	}
};

static void usage(const char *argv0) {
	printf("usage: %s [--mode eyes|scene] [--seconds N] [--selftest]\n", argv0);
}

static int selftest() {
	Renderer r;
	r.init(kMaxEyeDim, kMaxEyeDim);
	printf("selftest: fmt=DXGI_FORMAT_R8G8B8A8_UNORM size=%ux%u texL=%p texR=%p\n", r.w, r.h, (void *)r.eyeTex[0], (void *)r.eyeTex[1]);
	Hand hands[2] = {};
	hands[0] = { mul(translate(-0.3f, 1.2f, -0.5f), identity()), true, 0.5f };
	Mat4 view = invRigid(translate(0, 1.6f, 0));
	for (int eye = 0; eye < 2; eye++) {
		renderEyes(r, eye, 0xA5A5);
		uint8_t px[4];
		r.readPixel(eye, 8, 8, px);
		const uint8_t want[2][4] = { { 255, 0, 0, 255 }, { 0, 0, 255, 255 } };
		printf("selftest: eyes %c corner=(%u,%u,%u,%u)\n", eye ? 'R' : 'L', px[0], px[1], px[2], px[3]);
		if (memcmp(px, want[eye], 4)) { printf("FAIL stage=selftest-eyes corner pixel mismatch\n"); return 1; }
		renderScene(r, eye, mul(perspective(0.1f, 100), view), hands);
		// Sky in the corner; a floor checker square (grey c0 or c1, never sky) below
		// centre-right, which only holds if the MVP/inverse/floor rotation chain is right.
		uint8_t sky[4], floor[4];
		r.readPixel(eye, 8, 8, sky);
		r.readPixel(eye, r.w * 3 / 4, r.h * 3 / 4, floor);
		printf("selftest: scene %c sky=(%u,%u,%u) floor=(%u,%u,%u)\n", eye ? 'R' : 'L', sky[0], sky[1], sky[2], floor[0], floor[1], floor[2]);
		bool skyOk = sky[0] == 13 && sky[1] == 13 && (sky[2] == 25 || sky[2] == 26);
		bool floorOk = floor[0] == floor[1] && floor[1] == floor[2] && (floor[0] == 204 || floor[0] == 38);
		if (!skyOk || !floorOk) { printf("FAIL stage=selftest-scene pixel mismatch\n"); return 1; }
	}
	r.ctx->Flush();
	printf("PASS selftest eyes+scene rendered\n");
	return 0;
}

int main(int argc, char **argv) {
	bool scene = false, self = false;
	double seconds = 20;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--help")) { usage(argv[0]); return 0; }
		else if (!strcmp(argv[i], "--selftest")) self = true;
		else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
			i++;
			if (!strcmp(argv[i], "scene")) scene = true;
			else if (strcmp(argv[i], "eyes")) { printf("FAIL stage=args bad --mode '%s'\n", argv[i]); return 2; }
		} else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) {
			char extra = 0;
			if (sscanf(argv[++i], "%lf%c", &seconds, &extra) != 1 || !(seconds > 0) || seconds > 3600) {
				printf("FAIL stage=args bad --seconds '%s'\n", argv[i]); return 2;
			}
		} else { printf("FAIL stage=args unknown arg: %s\n", argv[i]); usage(argv[0]); return 2; }
	}
	setvbuf(stdout, nullptr, _IONBF, 0);
	if (self) return selftest();

	vr::EVRInitError ie = vr::VRInitError_None;
	vr::IVRSystem *sys = vr::VR_Init(&ie, vr::VRApplication_Scene);
	if (!sys) {
		printf("FAIL stage=vr-init err=%s (%s)\n", vr::VR_GetVRInitErrorAsSymbol(ie), vr::VR_GetVRInitErrorAsEnglishDescription(ie));
		return 1;
	}
	vr::IVRCompositor *comp = vr::VRCompositor();
	if (!comp) { printf("FAIL stage=vr-compositor no compositor\n"); vr::VR_Shutdown(); return 1; }
	comp->SetTrackingSpace(vr::TrackingUniverseStanding);
	uint32_t rw = 0, rh = 0;
	sys->GetRecommendedRenderTargetSize(&rw, &rh);
	uint32_t w = rw < kMaxEyeDim ? rw : kMaxEyeDim, h = rh < kMaxEyeDim ? rh : kMaxEyeDim;
	printf("vr: runtime=%s recommended=%ux%u using=%ux%u mode=%s\n", sys->GetRuntimeVersion(), rw, rh, w, h, scene ? "scene" : "eyes");

	Renderer r;
	r.init(w, h);
	Actions actions;
	bool haveInput = scene && actions.init();
	Mat4 eyeToHead[2], proj[2];
	for (int e = 0; e < 2; e++) {
		eyeToHead[e] = from34(sys->GetEyeToHeadTransform((vr::EVREye)e));
		proj[e] = from44(sys->GetProjectionMatrix((vr::EVREye)e, 0.1f, 100.f));
	}

	vr::Texture_t tex[2];
	for (int e = 0; e < 2; e++) tex[e] = { r.eyeTex[e], vr::TextureType_DirectX, vr::ColorSpace_Gamma };
	vr::VRTextureBounds_t bounds = { 0, 0, 1, 1 };
	Hand hands[2] = {};
	ULONGLONG t0 = GetTickCount64(), nextLog = t0 + 1000;
	uint32_t frame = 0;
	vr::EVRCompositorError lastErr[2] = {};
	while ((GetTickCount64() - t0) / 1000.0 < seconds) {
		vr::VREvent_t ev;
		bool quit = false;
		while (sys->PollNextEvent(&ev, sizeof(ev)))
			if (ev.eventType == vr::VREvent_Quit) { sys->AcknowledgeQuit_Exiting(); quit = true; }
		if (quit) { printf("vr: VREvent_Quit after %u frames\n", frame); break; }

		vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
		comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, nullptr, 0); // may block; parent keeps an outer timeout
		Mat4 hmd = poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid
		    ? from34(poses[vr::k_unTrackedDeviceIndex_Hmd].mDeviceToAbsoluteTracking) : identity();
		if (haveInput) actions.update(hands);

		for (int e = 0; e < 2; e++) {
			if (scene) renderScene(r, e, mul(proj[e], invRigid(mul(hmd, eyeToHead[e]))), hands);
			else renderEyes(r, e, frame);
			lastErr[e] = comp->Submit((vr::EVREye)e, &tex[e], &bounds, vr::Submit_Default);
			if (lastErr[e] != vr::VRCompositorError_None) {
				printf("FAIL stage=submit eye=%c err=%s frame=%u\n", e ? 'R' : 'L', compositorErrorName(lastErr[e]), frame);
				vr::VR_Shutdown();
				return 1;
			}
		}
		// Logged frame= is the number encoded in this frame's bar pattern.
		if (GetTickCount64() >= nextLog) {
			nextLog += 1000;
			printf("frame=%u fmt=DXGI_FORMAT_R8G8B8A8_UNORM size=%ux%u texL=%p texR=%p submitL=%s submitR=%s\n",
			    frame, w, h, (void *)r.eyeTex[0], (void *)r.eyeTex[1], compositorErrorName(lastErr[0]), compositorErrorName(lastErr[1]));
			if (haveInput) actions.log(hands);
		}
		frame++;
	}
	printf("PASS frames=%u\n", frame);
	vr::VR_Shutdown();
	return 0;
}

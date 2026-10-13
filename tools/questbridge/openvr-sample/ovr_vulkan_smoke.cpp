// ovr_vulkan_smoke.cpp — minimal OpenVR Vulkan submit smoke for the DroidDeck
// Quest bridge. No SDL, no window, no validation layers.
//
// VR_Init(VRApplication_Scene) -> print HMD props + recommended render target
// size -> create a Vulkan instance/device with the compositor's required
// extensions -> loop WaitGetPoses + per-eye Submit of a VkImage cleared to a
// solid colour (left magenta-ish, right cyan-ish, both pulsed by frame).
// xrizer adopts the app's VkDevice and copies each eye into its array
// swapchain, so the images stay in TRANSFER_SRC_OPTIMAL after our clear.
//
// Usage: ovr-vulkan-smoke [seconds]   (default 25)
// Exit 0 after the loop, 1 on any failure. Every stage prints; flush always
// so a crash still leaves the last stage in the log.

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openvr.h>
#include <vulkan/vulkan.h>

#define CHECK_VK(call) do { \
		VkResult _r = (call); \
		if (_r != VK_SUCCESS) { \
			printf("FAIL vk %s:%d: %s -> %d\n", __FILE__, __LINE__, #call, _r); \
			fflush(stdout); \
			exit(1); \
		} \
	} while (0)

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

// Split a space-separated extension list into a malloc'd argv-style array.
static const char **split_exts(const char *list, uint32_t *out_n) {
	uint32_t n = 0;
	for (const char *p = list; *p;) {
		while (*p == ' ') p++;
		if (!*p) break;
		n++;
		while (*p && *p != ' ') p++;
	}
	const char **exts = (const char **)malloc((n + 1) * sizeof(*exts));
	char *copy = strdup(list), *save = NULL, *tok = strtok_r(copy, " ", &save);
	for (uint32_t i = 0; i < n && tok; i++, tok = strtok_r(NULL, " ", &save))
		exts[i] = tok;
	exts[n] = NULL;
	*out_n = n;
	return exts; // NOTE: keeps `copy` alive via exts[0..n); never freed, fine for a smoke
}

static uint32_t find_mem_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want) {
	VkPhysicalDeviceMemoryProperties props;
	vkGetPhysicalDeviceMemoryProperties(pd, &props);
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
		if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & want) == want)
			return i;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++)
		if (bits & (1u << i))
			return i;
	printf("FAIL no Vulkan memory type for bits 0x%x\n", bits);
	fflush(stdout);
	exit(1);
}

static const char *eye_name(vr::EVREye eye) { return eye == vr::Eye_Left ? "left" : "right"; }

int main(int argc, char **argv) {
	double seconds = argc > 1 ? atof(argv[1]) : 25.0;
	setvbuf(stdout, NULL, _IONBF, 0);
	printf("smoke: VR_Init(VRApplication_Scene)\n");

	vr::EVRInitError err = vr::VRInitError_None;
	vr::IVRSystem *sys =
		vr::VR_Init(&err, vr::VRApplication_Scene);
	if (!sys) {
		printf("FAIL VR_Init: %d %s\n", err,
			vr::VR_GetVRInitErrorAsEnglishDescription(err));
		return 1;
	}
	printf("smoke: VR_Init ok, runtime handle %p\n", (void *)sys);

	char buf[256];
	vr::ETrackedPropertyError perr;
	const vr::ETrackedDeviceProperty props[] = {
		vr::Prop_TrackingSystemName_String, vr::Prop_ModelNumber_String,
		vr::Prop_ManufacturerName_String, vr::Prop_SerialNumber_String,
	};
	const char *names[] = { "tracking", "model", "maker", "serial" };
	for (int i = 0; i < 4; i++) {
		uint32_t n = sys->GetStringTrackedDeviceProperty(
			vr::k_unTrackedDeviceIndex_Hmd, props[i], buf, sizeof(buf), &perr);
		printf("smoke: hmd.%s = %s (bytes %u err %d)\n", names[i],
			perr == vr::TrackedProp_Success ? buf : "<err>", n, perr);
	}
	uint32_t rw = 0, rh = 0;
	sys->GetRecommendedRenderTargetSize(&rw, &rh);
	printf("smoke: recommended %ux%u per eye\n", rw, rh);
	if (!rw || !rh) {
		printf("FAIL recommended size is zero\n");
		vr::VR_Shutdown();
		return 1;
	}
	vr::IVRCompositor *comp = vr::VRCompositor();
	if (!comp) {
		printf("FAIL VRCompositor() is null\n");
		vr::VR_Shutdown();
		return 1;
	}
	uint32_t n = comp->GetVulkanInstanceExtensionsRequired(NULL, 0);
	char *inst_ext_list = (char *)malloc(n ? n : 1);
	comp->GetVulkanInstanceExtensionsRequired(inst_ext_list, n);
	printf("smoke: instance exts: %s\n", n ? inst_ext_list : "(none)");
	// NOTE: device extensions need a VkPhysicalDevice, so they are queried
	// after instance creation below (xrizer ignores the argument).

	// --- Vulkan: instance/device on the first physical device (single-GPU guest)
	uint32_t n_inst = 0;
	const char **inst_exts = split_exts(n ? inst_ext_list : "", &n_inst);
	VkApplicationInfo app_info = {
		VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ovr-vulkan-smoke", 1,
		"ovr-vulkan-smoke", 1, VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo inst_info = {
		VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app_info,
		0, NULL, n_inst, inst_exts,
	};
	VkInstance inst;
	CHECK_VK(vkCreateInstance(&inst_info, NULL, &inst));
	uint32_t pd_count = 0;
	CHECK_VK(vkEnumeratePhysicalDevices(inst, &pd_count, NULL));
	printf("smoke: %u physical devices\n", pd_count);
	if (!pd_count) {
		printf("FAIL no physical devices\n");
		return 1;
	}
	VkPhysicalDevice *pds = (VkPhysicalDevice *)malloc(pd_count * sizeof(*pds));
	CHECK_VK(vkEnumeratePhysicalDevices(inst, &pd_count, pds));
	VkPhysicalDevice pd = pds[0];
	for (uint32_t i = 0; i < pd_count; i++) {
		VkPhysicalDeviceProperties pr;
		vkGetPhysicalDeviceProperties(pds[i], &pr);
		printf("smoke: pd%u: %s api 0x%x\n", i, pr.deviceName, pr.apiVersion);
	}
	free(pds);
	uint32_t m = comp->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T *)pd, NULL, 0);
	char *dev_ext_list = (char *)malloc(m ? m : 1);
	comp->GetVulkanDeviceExtensionsRequired((VkPhysicalDevice_T *)pd, dev_ext_list, m);
	printf("smoke: device exts: %s\n", m ? dev_ext_list : "(none)");
	uint32_t n_dev = 0;
	const char **dev_exts = split_exts(m ? dev_ext_list : "", &n_dev);
	uint32_t qf_count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, NULL);
	VkQueueFamilyProperties *qfs =
		(VkQueueFamilyProperties *)malloc(qf_count * sizeof(*qfs));
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qfs);
	uint32_t qfi = qf_count;
	for (uint32_t i = 0; i < qf_count; i++)
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qfi = i;
			break;
		}
	free(qfs);
	if (qfi == qf_count) {
		printf("FAIL no graphics queue family\n");
		return 1;
	}
	float prio = 1.0f;
	VkDeviceQueueCreateInfo q_info = {
		VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfi, 1, &prio,
	};
	VkDeviceCreateInfo dev_info = {
		VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &q_info, 0, NULL,
		n_dev, dev_exts, NULL,
	};
	VkDevice dev;
	CHECK_VK(vkCreateDevice(pd, &dev_info, NULL, &dev));
	VkQueue queue;
	vkGetDeviceQueue(dev, qfi, 0, &queue);
	printf("smoke: device ok, queue family %u\n", qfi);

	// --- Two eye images, cleared per frame, left TRANSFER_SRC_OPTIMAL for xrizer
	VkCommandPoolCreateInfo pool_info = {
		VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
		VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfi,
	};
	VkCommandPool pool;
	CHECK_VK(vkCreateCommandPool(dev, &pool_info, NULL, &pool));
	VkCommandBufferAllocateInfo cmd_info = {
		VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
		VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1,
	};
	VkCommandBuffer cmd;
	CHECK_VK(vkAllocateCommandBuffers(dev, &cmd_info, &cmd));
	VkFenceCreateInfo fence_info = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
	VkFence fence;
	CHECK_VK(vkCreateFence(dev, &fence_info, NULL, &fence));

	VkImage eye_img[2];
	VkDeviceMemory eye_mem[2];
	for (int e = 0; e < 2; e++) {
		VkImageCreateInfo img_info = {
			VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
			VK_FORMAT_R8G8B8A8_SRGB, { rw, rh, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
			VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
			VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED,
		};
		CHECK_VK(vkCreateImage(dev, &img_info, NULL, &eye_img[e]));
		VkMemoryRequirements req;
		vkGetImageMemoryRequirements(dev, eye_img[e], &req);
		VkMemoryAllocateInfo mem_info = {
			VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req.size,
			find_mem_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
		};
		CHECK_VK(vkAllocateMemory(dev, &mem_info, NULL, &eye_mem[e]));
		CHECK_VK(vkBindImageMemory(dev, eye_img[e], eye_mem[e], 0));
	}
	printf("smoke: eye images %ux%u srgb\n", rw, rh);

	vr::VRVulkanTextureData_t tex[2];
	for (int e = 0; e < 2; e++) {
		tex[e].m_nImage = (uint64_t)eye_img[e];
		tex[e].m_pDevice = (VkDevice_T *)dev;
		tex[e].m_pPhysicalDevice = (VkPhysicalDevice_T *)pd;
		tex[e].m_pInstance = (VkInstance_T *)inst;
		tex[e].m_pQueue = (VkQueue_T *)queue;
		tex[e].m_nQueueFamilyIndex = qfi;
		tex[e].m_nWidth = rw;
		tex[e].m_nHeight = rh;
		tex[e].m_nFormat = (uint32_t)VK_FORMAT_R8G8B8A8_SRGB;
		tex[e].m_nSampleCount = 1;
	}

	// --- Frame loop
	vr::TrackedDevicePose_t poses[vr::k_unMaxTrackedDeviceCount];
	double t0 = now_s(), last_stat = t0;
	uint64_t frames = 0, submit_errs = 0, invalid_hmd = 0;
	bool first_submit = false;
	while (now_s() - t0 < seconds) {
		uint64_t f = frames;
		float pulse = (float)((f / 30) % 2); // flip colours every ~0.4 s at 72 Hz
		VkClearColorValue clear[2] = {
			{ { 1.0f, pulse, 1.0f, 1.0f } }, // left: magenta/red
			{ { pulse, 1.0f, 1.0f, 1.0f } }, // right: cyan/green
		};
		CHECK_VK(vkResetCommandBuffer(cmd, 0));
		VkCommandBufferBeginInfo begin = {
			VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
			VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL,
		};
		CHECK_VK(vkBeginCommandBuffer(cmd, &begin));
		for (int e = 0; e < 2; e++) {
			VkImageMemoryBarrier to_dst = {
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
				0, VK_ACCESS_TRANSFER_WRITE_BIT,
				f == 0 ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, eye_img[e],
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_dst);
			vkCmdClearColorImage(cmd, eye_img[e], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				&clear[e], 1, &to_dst.subresourceRange);
			VkImageMemoryBarrier to_src = {
				VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
				VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
				VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
				VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, eye_img[e],
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
			};
			vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_src);
		}
		CHECK_VK(vkEndCommandBuffer(cmd));
		VkSubmitInfo sub = {
			VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL,
		};
		CHECK_VK(vkQueueSubmit(queue, 1, &sub, fence));
		VkResult wr = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
		CHECK_VK(vkResetFences(dev, 1, &fence));
		if (wr != VK_SUCCESS) {
			printf("FAIL fence wait -> %d at frame %llu\n", wr,
				(unsigned long long)f);
			return 1;
		}

		vr::EVRCompositorError werr =
			comp->WaitGetPoses(poses, vr::k_unMaxTrackedDeviceCount, NULL, 0);
		if (werr != vr::VRCompositorError_None) {
			printf("smoke: WaitGetPoses err %d at frame %llu\n", werr,
				(unsigned long long)f);
			submit_errs++;
		}
		if (!poses[vr::k_unTrackedDeviceIndex_Hmd].bPoseIsValid)
			invalid_hmd++;
		for (int e = 0; e < 2; e++) {
			vr::Texture_t t;
			t.handle = (void *)&tex[e];
			t.eType = vr::TextureType_Vulkan;
			t.eColorSpace = vr::ColorSpace_Auto;
			vr::EVRCompositorError serr = comp->Submit(
				(vr::EVREye)e, &t, NULL, vr::Submit_Default);
			if (serr != vr::VRCompositorError_None) {
				if (!first_submit || submit_errs < 5)
					printf("smoke: Submit %s err %d at frame %llu\n",
						eye_name((vr::EVREye)e), serr, (unsigned long long)f);
				submit_errs++;
			}
		}
		if (!first_submit) {
			first_submit = true;
			printf("smoke: first submits done (submit_errs %llu)\n",
				(unsigned long long)submit_errs);
		}
		frames++;
		double now = now_s();
		if (now - last_stat >= 1.0) {
			const vr::TrackedDevicePose_t *hmd = &poses[vr::k_unTrackedDeviceIndex_Hmd];
			printf("smoke: t=%.1f frames=%llu fps=%.0f hmd_valid=%d hmd_pos=%.2f,%.2f,%.2f submit_errs=%llu\n",
				now - t0, (unsigned long long)frames, frames / (now - t0),
				hmd->bPoseIsValid, hmd->mDeviceToAbsoluteTracking.m[0][3],
				hmd->mDeviceToAbsoluteTracking.m[1][3],
				hmd->mDeviceToAbsoluteTracking.m[2][3],
				(unsigned long long)submit_errs);
			last_stat = now;
		}
	}
	printf("smoke: done frames=%llu submit_errs=%llu invalid_hmd=%llu\n",
		(unsigned long long)frames, (unsigned long long)submit_errs,
		(unsigned long long)invalid_hmd);
	// NOTE: no vkDestroy*/free of Vulkan objects before VR_Shutdown: xrizer
	// adopted our VkDevice and destroys its own command pool on shutdown.
	vr::VR_Shutdown();
	printf("smoke: VR_Shutdown ok\n");
	return submit_errs && !frames ? 1 : 0;
}

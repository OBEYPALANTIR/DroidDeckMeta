#!/usr/bin/env python3
"""Adds native-buffer import diagnostics and fail-closed limits to the pinned Mesa (mesa.env).

Records go to logcat under the tag "nativeimport":
  - u_gralloc mapper4 backend: a null IMapper 4.0 getService(), each metadata query's HIDL
    transport or mapper error (metadata type name and value), decoder failures, the raw
    PlaneLayouts, where each plane ends against AllocationSize, and the plane->fd association,
    which is inferred (PlaneLayout carries no fd index).
  - vk_android.c: the native handle's ints and every fd (kind, length, dev/inode, /proc link),
    the gralloc provider and what it reports; at memory import, the image's memory requirement
    against the fd being imported.
  - tu_image.cc: VkImage <-> native handle at CreateImage and at deferred-alias bind, Turnip's
    layout, the bound BO (gem id, size, iova) and destruction.
This is a diagnostic driver, not an import fix. It supports one layer, one plane and a native
handle with exactly one fd, described by mapper4 metadata with a LINEAR or QCOM_COMPRESSED
modifier; any other buffer is logged, then refused as diagnostic-unsupported. Also refused: a
plane span past FDL's 32-bit fields, an fd that is not a dma-buf of known length, and a memory
requirement past the dma-buf or its BO. mapper4 metadata is used as the vendor mapper reports it;
nothing here checks it against the kernel.

The original target files are SHA-256 pinned and must resolve inside the source root. Each edit
replaces one exact excerpt that must occur exactly once. All paths, hashes and anchors are checked
before any file is written, so changed inputs fail instead of being half patched.

  patch-native-import.py <mesa-source-dir>
"""
import hashlib
import sys
from pathlib import Path

MAPPER4 = 'src/util/u_gralloc/u_gralloc_imapper4_api.cpp'
VK_ANDROID = 'src/vulkan/runtime/vk_android.c'
TU_IMAGE = 'src/freedreno/vulkan/tu_image.cc'

# Original bytes from Mesa 9fd4c56e2cbcff25d50a63a5420a4f31d67dbebf (mesa.env).
ORIGINAL_SHA256 = {
    MAPPER4: 'e0c6b9d0a53db4698defe098d58d3b553fd0d9ad1ed04ff7354dc46059c1910f',
    VK_ANDROID: '16b40f146c211f1a9a29802975e12654a08a1cd1e3a64c59647b8752ad8f5115',
    TU_IMAGE: '58910c36b2d89c6b9dd798fda8255912edfa986ec13a91ff0c133233b94e8fff',
}

EDITS = [
    (MAPPER4, '#include <system/window.h>\n', '#include <inttypes.h>\n#include <system/window.h>\n'),
    (MAPPER4, '''\
   if (!ret.isOk())
      error = Error::NO_RESOURCES;

   return error;
}
''', '''\
   /* Keep a HIDL transport failure apart from the mapper's own Error. */
   if (!ret.isOk()) {
      mesa_log(MESA_LOG_WARN, "nativeimport",
               "mapper4 get(%s:%" PRId64 ") handle=%p transport: %s", type.name.c_str(),
               type.value, (const void *)buffer, ret.description().c_str());
      error = Error::NO_RESOURCES;
   } else if (error != Error::NONE) {
      mesa_log(MESA_LOG_WARN, "nativeimport", "mapper4 get(%s:%" PRId64 ") handle=%p error=%d",
               type.name.c_str(), type.value, (const void *)buffer, static_cast<int>(error));
   }

   return error;
}

/* Names the decoder that rejected a mapper reply instead of folding it into -EINVAL. */
static bool
decoded(android::status_t status, const char *what, const native_handle_t *buffer)
{
   if (status == android::OK)
      return true;
   mesa_log(MESA_LOG_WARN, "nativeimport", "mapper4 decode(%s) handle=%p status=%d", what,
            (const void *)buffer, static_cast<int>(status));
   return false;
}
'''),
    (MAPPER4, '''\
&plane_layouts);

   if (status != android::OK)
      return std::nullopt;

   return plane_layouts;
}
''', '''\
&plane_layouts);

   if (!decoded(status, "PlaneLayouts", buffer))
      return std::nullopt;

   /* The mapper's own records, before Mesa turns them into fds/offsets/strides. */
   for (size_t i = 0; i < plane_layouts.size(); i++) {
      const PlaneLayout &p = plane_layouts[i];
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "mapper4 handle=%p plane=%zu offset=%" PRId64 " stride=%" PRId64
               " total=%" PRId64 " samples=%" PRId64 "x%" PRId64 " increment_bits=%" PRId64
               " subsampling=%" PRId64 "x%" PRId64,
               (const void *)buffer, i, p.offsetInBytes, p.strideInBytes, p.totalSizeInBytes,
               p.widthInSamples, p.heightInSamples, p.sampleIncrementInBits,
               p.horizontalSubsampling, p.verticalSubsampling);
      for (const PlaneLayoutComponent &c : p.components)
         mesa_log(MESA_LOG_INFO, "nativeimport",
                  "mapper4 handle=%p plane=%zu component=%s:%" PRId64 " offset_bits=%" PRId64
                  " size_bits=%" PRId64,
                  (const void *)buffer, i, c.type.name.c_str(), c.type.value, c.offsetInBits,
                  c.sizeInBits);
   }

   return plane_layouts;
}
'''),
    (MAPPER4, '&drm_fourcc);\n   if (status != android::OK)\n      return -EINVAL;\n',
     '&drm_fourcc);\n   if (!decoded(status, "PixelFormatFourCC", hnd->handle))\n      return -EINVAL;\n'),
    (MAPPER4, '&drm_modifier);\n   if (status != android::OK)\n      return -EINVAL;\n',
     '&drm_modifier);\n   if (!decoded(status, "PixelFormatModifier", hnd->handle))\n'
     '      return -EINVAL;\n'),
    (MAPPER4, '&alloc_size);\n   if (status != android::OK)\n      return -EINVAL;\n',
     '&alloc_size);\n   if (!decoded(status, "AllocationSize", hnd->handle))\n      return -EINVAL;\n'),
    (MAPPER4, '&layer_count);\n   if (status != android::OK)\n      return -EINVAL;\n',
     '&layer_count);\n   if (!decoded(status, "LayerCount", hnd->handle))\n      return -EINVAL;\n'),
    (MAPPER4, '''\
   std::vector<PlaneLayout> &layouts = *layouts_opt;

   out->num_planes = layouts.size();
''', '''\
   std::vector<PlaneLayout> &layouts = *layouts_opt;

   /* out->fds/offsets/strides hold four planes: refuse rather than overrun them. */
   if (layouts.empty() || layouts.size() > ARRAY_SIZE(out->fds)) {
      mesa_log(MESA_LOG_ERROR, "nativeimport", "mapper4 handle=%p planes=%zu unsupported",
               (const void *)hnd->handle, layouts.size());
      return -EINVAL;
   }

   /* totalSizeInBytes is the mapper's own measure of a plane: report where each
    * plane ends against AllocationSize. Reported, not enforced.
    */
   for (size_t i = 0; i < layouts.size(); i++) {
      const PlaneLayout &p = layouts[i];
      bool within = p.offsetInBytes >= 0 && p.totalSizeInBytes >= 0 &&
                    (uint64_t)p.offsetInBytes <= alloc_size &&
                    (uint64_t)p.totalSizeInBytes <= alloc_size - (uint64_t)p.offsetInBytes;
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "mapper4 handle=%p plane=%zu offset=%" PRId64 " total=%" PRId64
               " alloc_size=%" PRIu64 " within_alloc=%d",
               (const void *)hnd->handle, i, p.offsetInBytes, p.totalSizeInBytes, alloc_size,
               static_cast<int>(within));
   }

   out->num_planes = layouts.size();
'''),
    (MAPPER4, '''\
      out->offsets[i] = layouts[i].offsetInBytes;

      /* offset == 0 means layer is located in different dma-buf */
      if (out->offsets[i] == 0 && i > 0)
         fd_index++;

      if (fd_index >= hnd->handle->numFds)
         return -EINVAL;

      out->fds[i] = hnd->handle->data[fd_index];
   }
''', '''\
      out->offsets[i] = layouts[i].offsetInBytes;

      /* A negative or truncated value would describe a different layout. */
      if (out->strides[i] < 0 || out->offsets[i] < 0 ||
          out->strides[i] != layouts[i].strideInBytes ||
          out->offsets[i] != layouts[i].offsetInBytes) {
         mesa_log(MESA_LOG_ERROR, "nativeimport",
                  "mapper4 handle=%p plane=%u offset/stride not representable",
                  (const void *)hnd->handle, i);
         return -EINVAL;
      }

      /* offset == 0 means layer is located in different dma-buf */
      if (out->offsets[i] == 0 && i > 0)
         fd_index++;

      if (fd_index >= hnd->handle->numFds) {
         mesa_log(MESA_LOG_ERROR, "nativeimport",
                  "mapper4 handle=%p plane=%u fd_index=%d numFds=%d", (const void *)hnd->handle,
                  i, fd_index, hnd->handle->numFds);
         return -EINVAL;
      }

      out->fds[i] = hnd->handle->data[fd_index];
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "mapper4 handle=%p plane=%u fd=data[%d] (inferred: PlaneLayout has no fd index)",
               (const void *)hnd->handle, i, fd_index);
   }
'''),
    (MAPPER4, '''\
   auto mapper = IMapper::getService();
   if (!mapper)
      return NULL;
''', '''\
   auto mapper = IMapper::getService();
   if (!mapper) {
      mesa_log(MESA_LOG_ERROR, "nativeimport", "IMapper 4.0 getService() returned null");
      return NULL;
   }
'''),

    (VK_ANDROID, '#include <unistd.h>\n\nstatic struct u_gralloc *_gralloc;\n',
     '#include <inttypes.h>\n#include <stdio.h>\n#include <string.h>\n#include <sys/stat.h>\n'
     '#include <unistd.h>\n\nstatic struct u_gralloc *_gralloc;\n'),
    (VK_ANDROID, '''\
   call_once(&once, vk_android_init_ugralloc_once);
   return _gralloc;
}
''', '''\
   call_once(&once, vk_android_init_ugralloc_once);
   return _gralloc;
}

static const char *
vk_android_gralloc_name(int type)
{
   switch (type) {
   case U_GRALLOC_TYPE_GRALLOC4:
      return "mapper4";
   case U_GRALLOC_TYPE_CROS:
      return "cros";
   case U_GRALLOC_TYPE_LIBDRM:
      return "libdrm";
   case U_GRALLOC_TYPE_QCOM:
      return "qcom";
   case U_GRALLOC_TYPE_FALLBACK:
      return "fallback";
   default:
      return "none";
   }
}

/* What is known about one native-handle fd. Only a regular file (fstat) or a
 * dma-buf (lseek(SEEK_END, 0), which dma-buf answers with its size without
 * moving the file offset) is measured; any other fd keeps length -1. The
 * kernel names dma-buf files "/dmabuf:<name>" ("anon_inode:dmabuf" on older
 * kernels).
 */
struct vk_android_fd_record {
   const char *kind;
   int64_t length;
   int err; /* errno of the last failed fstat, readlink or lseek */
   struct stat st;
   char link[128];
};

static void
vk_android_inspect_fd(int fd, struct vk_android_fd_record *rec)
{
   char path[32];

   memset(rec, 0, sizeof(*rec));
   rec->kind = "other";
   rec->length = -1;

   if (fstat(fd, &rec->st) != 0) {
      rec->err = errno;
      rec->kind = "stat-failed";
      return;
   }

   snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
   ssize_t n = readlink(path, rec->link, sizeof(rec->link) - 1);
   if (n < 0)
      rec->err = errno;
   else
      rec->link[n] = 0;

   if (S_ISREG(rec->st.st_mode)) {
      rec->kind = "regular";
      rec->length = rec->st.st_size;
   } else if (n > 0 && (strncmp(rec->link, "/dmabuf:", 8) == 0 ||
                        strcmp(rec->link, "anon_inode:dmabuf") == 0)) {
      rec->kind = "dmabuf";
      off_t end = lseek(fd, 0, SEEK_END);
      if (end < 0)
         rec->err = errno;
      else
         rec->length = end;
   }
}

/* The native handle as received, its ints and every fd, so one buffer can be
 * followed from layout through import to bind ("nativeimport" logcat tag).
 */
static void
vk_android_log_native_handle(const native_handle_t *handle)
{
   char ints[512] = "";
   size_t len = 0;
   int shown = 0;

   /* One value takes at most 9 characters plus the terminator. */
   while (shown < handle->numInts && len + 10 <= sizeof(ints)) {
      len += (size_t)snprintf(ints + len, sizeof(ints) - len, "%s%x", shown ? " " : "",
                              (unsigned)handle->data[handle->numFds + shown]);
      shown++;
   }
   mesa_log(MESA_LOG_INFO, "nativeimport", "handle=%p numFds=%d numInts=%d ints=[%s]%s",
            (const void *)handle, handle->numFds, handle->numInts, ints,
            shown < handle->numInts ? " truncated" : "");

   for (int i = 0; i < handle->numFds; i++) {
      struct vk_android_fd_record rec;

      vk_android_inspect_fd(handle->data[i], &rec);
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "handle=%p fd[%d]=%d kind=%s length=%" PRId64 " st_size=%" PRId64
               " dev=%" PRIu64 " ino=%" PRIu64 " mode=0%o err=%d link=%s",
               (const void *)handle, i, handle->data[i], rec.kind, rec.length,
               (int64_t)rec.st.st_size, (uint64_t)rec.st.st_dev,
               (uint64_t)rec.st.st_ino, (unsigned)rec.st.st_mode, rec.err, rec.link);
   }
}
'''),
    (VK_ANDROID, '''\
   struct u_gralloc_buffer_basic_info info;
   struct u_gralloc *u_gralloc = vk_android_get_ugralloc();
   assert(u_gralloc);

   if (u_gralloc_get_buffer_basic_info(u_gralloc, in_hnd, &info) != 0) {
      mesa_loge("u_gralloc_get_buffer_basic_info failed");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
''', '''\
   struct u_gralloc_buffer_basic_info info = {0};
   struct u_gralloc *u_gralloc = vk_android_get_ugralloc();
   const native_handle_t *handle = in_hnd->handle;

   /* Diagnostic limits: log the native handle and whatever the gralloc
    * provider reports, then accept only mapper4 metadata for one layer and one
    * plane with a modifier Turnip lays out explicitly. Any other buffer is
    * refused as unsupported by this diagnostic.
    */
   if (!handle) {
      mesa_log(MESA_LOG_ERROR, "nativeimport", "layout refused: null native handle");
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   vk_android_log_native_handle(handle);

   int provider = u_gralloc ? u_gralloc_get_type(u_gralloc) : U_GRALLOC_TYPE_COUNT;
   int ret = u_gralloc ? u_gralloc_get_buffer_basic_info(u_gralloc, in_hnd, &info)
                       : -ENODEV;
   mesa_log(MESA_LOG_INFO, "nativeimport",
            "layout handle=%p provider=%s hal_format=%d pixel_stride=%d basic_info=%d",
            (const void *)handle, vk_android_gralloc_name(provider),
            in_hnd->hal_format, in_hnd->pixel_stride, ret);
   if (ret == 0) {
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "layout handle=%p fourcc=0x%08x modifier=0x%" PRIx64 " planes=%d"
               " alloc_size=%" PRIu64 " layers=%" PRIu64,
               (const void *)handle, info.drm_fourcc, info.modifier,
               info.num_planes, info.alloc_size, info.layer_count);
      for (int i = 0; i < info.num_planes && i < (int)ARRAY_SIZE(info.fds); i++)
         mesa_log(MESA_LOG_INFO, "nativeimport",
                  "layout handle=%p plane=%d fd=%d offset=%d stride=%d fd_association=%s",
                  (const void *)handle, i, info.fds[i], info.offsets[i],
                  info.strides[i], handle->numFds == 1 ? "single-fd" : "inferred");
   }

   const char *unsupported = NULL;
   if (provider != U_GRALLOC_TYPE_GRALLOC4)
      unsupported = "provider is not mapper4";
   else if (ret != 0)
      unsupported = "mapper4 metadata query failed";
   else if (info.layer_count != 1)
      unsupported = "layer count is not 1";
   else if (info.num_planes != 1)
      unsupported = "plane count is not 1";
   else if (info.modifier != DRM_FORMAT_MOD_LINEAR &&
            info.modifier != DRM_FORMAT_MOD_QCOM_COMPRESSED)
      unsupported = "modifier has no explicit Turnip layout";
   if (unsupported) {
      mesa_log(MESA_LOG_ERROR, "nativeimport",
               "layout handle=%p refused, diagnostic unsupported: %s",
               (const void *)handle, unsupported);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
'''),
    (VK_ANDROID, '''\
   /* Query image memory requirements for size and supported memory types */
   VkMemoryRequirements mem_reqs;
   device->dispatch_table.GetImageMemoryRequirements(
      (VkDevice)device, (VkImage)image, &mem_reqs);
''', '''\
   /* Query image memory requirements for size and supported memory types */
   VkMemoryRequirements mem_reqs;
   device->dispatch_table.GetImageMemoryRequirements(
      (VkDevice)device, (VkImage)image, &mem_reqs);

   /* Diagnostic limits: only a native handle with exactly one fd says which
    * fd holds the image, and that fd must be a dma-buf at least as long as the
    * image's memory requirement. Refused before any memory object exists.
    */
   const native_handle_t *handle = anb->handle;
   struct vk_android_fd_record data_fd = {.kind = "none", .length = -1};
   if (handle->numFds > 0)
      vk_android_inspect_fd(dma_buf_fd, &data_fd);
   mesa_log(MESA_LOG_INFO, "nativeimport",
            "import image=%p handle=%p numFds=%d fd=%d kind=%s fd_length=%" PRId64
            " req_size=%" PRIu64 " req_align=%" PRIu64 " req_types=0x%x",
            (const void *)image, (const void *)handle, handle->numFds,
            handle->numFds > 0 ? dma_buf_fd : -1, data_fd.kind, data_fd.length,
            (uint64_t)mem_reqs.size, (uint64_t)mem_reqs.alignment,
            mem_reqs.memoryTypeBits);
   const char *refused = NULL;
   if (handle->numFds != 1)
      refused = "diagnostic unsupported: the handle does not have exactly one fd";
   else if (strcmp(data_fd.kind, "dmabuf") != 0 || data_fd.length <= 0)
      refused = "unknown capacity: the fd is not a dma-buf of known length";
   else if (mem_reqs.size > (uint64_t)data_fd.length)
      refused = "oversized: the memory requirement exceeds the dma-buf";
   if (refused) {
      mesa_log(MESA_LOG_ERROR, "nativeimport", "import image=%p refused, %s",
               (const void *)image, refused);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
'''),
    (VK_ANDROID, '''\
      (VkDevice)device, &alloc_info, alloc, &image->anb_memory);
   if (result != VK_SUCCESS) {
      close(dup_fd);
      return result;
   }

   return VK_SUCCESS;
}
''', '''\
      (VkDevice)device, &alloc_info, alloc, &image->anb_memory);
   if (result != VK_SUCCESS) {
      mesa_log(MESA_LOG_ERROR, "nativeimport",
               "import image=%p AllocateMemory failed: %d", (const void *)image,
               (int)result);
      close(dup_fd);
      return result;
   }

   mesa_log(MESA_LOG_INFO, "nativeimport", "import image=%p memory=%p type=%u",
            (const void *)image, (const void *)image->anb_memory,
            alloc_info.memoryTypeIndex);
   return VK_SUCCESS;
}
'''),

    (TU_IMAGE, '#include "tu_image.h"\n\n', '#include "tu_image.h"\n\n#include <inttypes.h>\n\n'),
    (TU_IMAGE, '''\
         plane_layout.offset = plane_layouts[i].offset;
         plane_layout.pitch = plane_layouts[i].rowPitch;
''', '''\
         /* Diagnostic limits ("nativeimport"): one layer and one plane, as FDL
          * sizes a single layer of an explicit layout and ignores arrayPitch.
          * fdl6_layout_image keeps the offset, the pitch and pitch * rows in 32
          * bits, so bound them in 64 bits first: the pitch must hold a row of
          * blocks, and the plane, its rows rounded up to FDL's tallest tile
          * (32 rows), must end below 4 GiB.
          */
         const VkSubresourceLayout *pl = &plane_layouts[i];
         uint64_t row_bytes = (uint64_t)util_format_get_nblocksx(format, width0) *
                              util_format_get_blocksize(format) *
                              (uint64_t)image->vk.samples;
         uint64_t rows = align64(util_format_get_nblocksy(format, height0), 32);
         uint64_t span = pl->offset <= UINT32_MAX && pl->rowPitch <= UINT32_MAX ?
                         pl->offset + align64(pl->rowPitch * rows, 4096) : UINT64_MAX;
         const char *refused =
            image->vk.array_layers != 1 ? "diagnostic unsupported: more than one layer" :
            tu6_plane_count(image->vk.format) != 1 ?
               "diagnostic unsupported: more than one plane" :
            pl->rowPitch < row_bytes ? "pitch shorter than a row of blocks" :
            span > UINT32_MAX ? "plane span past FDL's 32-bit fields" : nullptr;
         if (refused) {
            mesa_log(MESA_LOG_ERROR, "nativeimport",
                     "explicit layout image=%p refused, %s (layers=%u offset=%" PRIu64
                     " pitch=%" PRIu64 " row_bytes=%" PRIu64 " rows<=%" PRIu64
                     " span<=%" PRIu64 ")",
                     (const void *)image, refused, image->vk.array_layers,
                     (uint64_t)pl->offset, (uint64_t)pl->rowPitch, row_bytes, rows,
                     span);
            return vk_error(device,
                            VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
         }

         plane_layout.offset = plane_layouts[i].offset;
         plane_layout.pitch = plane_layouts[i].rowPitch;
'''),
    (TU_IMAGE, '''\
         assert(plane_layouts); /* can only fail with explicit layout */
         return vk_error(device, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
      }
''', '''\
         assert(plane_layouts); /* can only fail with explicit layout */
         return vk_error(device, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
      }

      /* Offset, UBWC flag data and pixels together must still fit the 32-bit
       * slice offsets FDL recorded them in.
       */
      if (plane_layouts && layout->size > UINT32_MAX) {
         mesa_log(MESA_LOG_ERROR, "nativeimport",
                  "explicit layout image=%p refused, layout size %" PRIu64
                  " past FDL's 32-bit fields",
                  (const void *)image, layout->size);
         return vk_error(device, VK_ERROR_INVALID_DRM_FORMAT_MODIFIER_PLANE_LAYOUT_EXT);
      }
'''),
    (TU_IMAGE, 'TU_GENX(tu_image_init);\n', '''\
TU_GENX(tu_image_init);

/* Native-buffer diagnostics ("nativeimport" logcat tag): joins the VkImage,
 * its native handle, Turnip's layout and the bound BO.
 */
static void
tu_log_native_buffer(const char *stage, const struct tu_image *image,
                     const VkNativeBufferANDROID *anb)
{
   mesa_log(MESA_LOG_INFO, "nativeimport",
            "%s image=%p handle=%p hal_format=%d pixel_stride=%d usage=0x%x"
            " usage2=0x%" PRIx64 "/0x%" PRIx64 " vk_format=%d extent=%ux%u"
            " layers=%u vk_usage=0x%x flags=0x%x",
            stage, (const void *)image, (const void *)anb->handle, anb->format,
            anb->stride, (unsigned)anb->usage, anb->usage2.consumer,
            anb->usage2.producer, (int)image->vk.format, image->vk.extent.width,
            image->vk.extent.height, image->vk.array_layers,
            (unsigned)image->vk.usage, (unsigned)image->vk.create_flags);
}

static void
tu_log_native_layout(const char *stage, const struct tu_image *image,
                     uint64_t modifier)
{
   mesa_log(MESA_LOG_INFO, "nativeimport",
            "%s image=%p modifier=0x%" PRIx64 " total_size=%" PRIu64
            " base_align=%u",
            stage, (const void *)image, modifier, image->total_size,
            image->layout[0].base_align);
   for (uint32_t i = 0; i < tu6_plane_count(image->vk.format); i++) {
      const struct fdl_layout *l = &image->layout[i];
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "%s image=%p plane=%u tile_mode=%u ubwc=%d cpp=%u pitch=%u"
               " offset=%u size0=%u layer_size=%" PRIu64 " ubwc_offset=%u"
               " ubwc_size0=%u ubwc_pitch=%u ubwc_layer_size=%" PRIu64
               " plane_end=%" PRIu64,
               stage, (const void *)image, i, (unsigned)l->tile_mode,
               (int)l->ubwc, (unsigned)l->cpp, fdl_pitch(l, 0),
               l->slices[0].offset, l->slices[0].size0, l->layer_size,
               l->ubwc_slices[0].offset, l->ubwc_slices[0].size0,
               fdl_ubwc_pitch(l, 0), l->ubwc_layer_size, l->size);
   }
}
'''),
    (TU_IMAGE, '''\
      vk_find_struct_const(bind_info->pNext, NATIVE_BUFFER_ANDROID);
''', '''\
      vk_find_struct_const(bind_info->pNext, NATIVE_BUFFER_ANDROID);
   if (!anb) {
      mesa_log(MESA_LOG_ERROR, "nativeimport",
               "bind-alias image=%p refused: no VkNativeBufferANDROID",
               (const void *)img);
      return VK_ERROR_INVALID_EXTERNAL_HANDLE;
   }
   tu_log_native_buffer("bind-alias", img, anb);
'''),
    (TU_IMAGE, '''\
      eci.drmFormatModifier, a_plane_layouts, TU_IMAGE_ID_ASSIGN);
   if (result != VK_SUCCESS)
      return result;
''', '''\
      eci.drmFormatModifier, a_plane_layouts, TU_IMAGE_ID_ASSIGN);
   if (result != VK_SUCCESS) {
      mesa_log(MESA_LOG_ERROR, "nativeimport",
               "bind-alias image=%p tu_image_init failed: %d",
               (const void *)img, (int)result);
      return result;
   }
   tu_log_native_layout("bind-alias", img, eci.drmFormatModifier);
'''),
    (TU_IMAGE, '''\
   if (vk_image_is_android_native_buffer(&image->vk)) {
      result = vk_android_get_anb_layout(
''', '''\
   if (vk_image_is_android_native_buffer(&image->vk)) {
      tu_log_native_buffer("create", image,
                           vk_find_struct_const(pCreateInfo->pNext,
                                                NATIVE_BUFFER_ANDROID));
      result = vk_android_get_anb_layout(
'''),
    (TU_IMAGE, '''\
                                             TU_IMAGE_ID_ASSIGN);
   if (result != VK_SUCCESS)
      goto fail;

   /* This section is removed by the optimizer for non-ANDROID builds */
   if (vk_image_is_android_native_buffer(&image->vk)) {
      result = vk_android_import_anb(&device->vk, pCreateInfo, alloc,
''', '''\
                                             TU_IMAGE_ID_ASSIGN);
   if (result != VK_SUCCESS) {
      if (vk_image_is_android_native_buffer(&image->vk))
         mesa_log(MESA_LOG_ERROR, "nativeimport",
                  "create image=%p tu_image_init failed: %d",
                  (const void *)image, (int)result);
      goto fail;
   }

   /* This section is removed by the optimizer for non-ANDROID builds */
   if (vk_image_is_android_native_buffer(&image->vk)) {
      tu_log_native_layout("create", image, modifier);
      result = vk_android_import_anb(&device->vk, pCreateInfo, alloc,
'''),
    (TU_IMAGE, '''\
   assert(mem);
   image->mem = mem;
''', '''\
   assert(mem);
   if (vk_image_is_android_native_buffer(&image->vk) ||
       vk_image_is_android_native_buffer_alias(&image->vk)) {
      uint64_t bo_size = mem->bo ? mem->bo->size : 0;
      mesa_log(MESA_LOG_INFO, "nativeimport",
               "bind image=%p memory=%p mem_size=%" PRIu64 " bo=%u bo_size=%" PRIu64
               " bo_iova=0x%" PRIx64 " image_iova=0x%" PRIx64 " offset=%" PRIu64
               " total_size=%" PRIu64,
               (const void *)image, (const void *)mem, mem->size,
               mem->bo ? mem->bo->gem_handle : 0, bo_size,
               mem->bo ? mem->bo->iova : 0, mem->iova + offset, offset,
               image->total_size);
      /* The kernel's view of the imported buffer must cover the layout too. */
      if (offset > bo_size || image->total_size > bo_size - offset) {
         mesa_log(MESA_LOG_ERROR, "nativeimport",
                  "bind image=%p refused: offset=%" PRIu64 " total_size=%" PRIu64
                  " bo_size=%" PRIu64,
                  (const void *)image, offset, image->total_size, bo_size);
         return VK_ERROR_INVALID_EXTERNAL_HANDLE;
      }
   }
   image->mem = mem;
'''),
    (TU_IMAGE, '''\
   if (!image)
      return;

   TU_RMV(image_destroy, device, image);
''', '''\
   if (!image)
      return;

   if (vk_image_is_android_native_buffer(&image->vk) ||
       vk_image_is_android_native_buffer_alias(&image->vk))
      mesa_log(MESA_LOG_INFO, "nativeimport", "destroy image=%p",
               (const void *)image);

   TU_RMV(image_destroy, device, image);
'''),
]


def main(source):
    root = Path(source).resolve(strict=True)
    targets, patched = {}, {}
    for path, expected in ORIGINAL_SHA256.items():
        target = (root / path).resolve(strict=True)
        if not target.is_relative_to(root):
            sys.exit(f'{path}: resolves outside the source root')
        data = target.read_bytes()
        if hashlib.sha256(data).hexdigest() != expected:
            sys.exit(f'{path}: SHA-256 mismatch; expected the original file pinned in mesa.env')
        targets[path] = target
        patched[path] = data.decode('utf-8')
    for path, old, new in EDITS:
        text = patched[path]
        count = text.count(old)
        if count != 1:
            sys.exit(f'{path}: anchor found {count} times, expected once:\n{old}')
        patched[path] = text.replace(old, new)
    for path, text in patched.items():
        targets[path].write_text(text, encoding='utf-8')
        print(f'patched {path}')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: patch-native-import.py <mesa-source-dir>')
    main(sys.argv[1])

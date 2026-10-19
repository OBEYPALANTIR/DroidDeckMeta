"""Quest-aware replacement for DD-Turnip's scripts/gralloc_ubwc_detect.py.

Mesa's fallback u_gralloc recognises a QTI private_handle_t only when the first int after
the fds is the 'gmsm' magic. Meta Quest's gralloc (libgralloccore.so, vros 209) inserts one
int before the magic ({fd, fd_metadata, 1, 'gmsm', flags, ...}; 2 fds / 27 ints), so the
check fails and the modifier stays DRM_FORMAT_MOD_INVALID, for which Turnip picks its default
tiled + UBWC layout. On the headset that build faults the GPU on the XR panel swapchain within
seconds of the first frame and the fixed build runs clean (same-base A/B, dossier
features/loop5-driver-fix.md); the buffer-level geometry behind the fault is inferred.
Locate the flags int from whichever layout matches; keep DD-Turnip's gmsm-less read last.
"""
from mesa_edit import replace

replace(
    'src/util/u_gralloc/u_gralloc_fallback.c',
    """   uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
   if (hnd->handle->numInts >= 2 && hnd->handle->data[hnd->handle->numFds] == gmsm) {
      /* This UBWC flag was introduced in a5xx. */
      bool ubwc = hnd->handle->data[hnd->handle->numFds + 1] & 0x08000000;""",
    """   uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
   const int *ints = hnd->handle->data + hnd->handle->numFds;
   int flags_at = -1;
   if (hnd->handle->numInts >= 2 && (uint32_t)ints[0] == gmsm)
      flags_at = 1;                 /* QTI: magic, flags */
   else if (hnd->handle->numInts >= 2 && (uint32_t)ints[1] == gmsm)
      flags_at = hnd->handle->numInts >= 3 ? 2 : -1;   /* Meta Quest: one int, magic, flags */
   else if (hnd->handle->numInts >= 2)
      flags_at = 1;                 /* newer A8xx gralloc writes no magic */
   if (flags_at >= 0) {
      /* This UBWC flag was introduced in a5xx. */
      bool ubwc = ints[flags_at] & 0x08000000;""",
    'UBWC detection for QTI, Meta Quest and gmsm-less handles',
)

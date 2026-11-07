#pragma once

#include <android/hardware_buffer.h>
#include <errno.h>
#include <string.h>

/* AHardwareBuffer arrived in Android 8.0 (API 26). Builds for older systems (Oculus Go runs
 * Android 7.1, API 25) link these weakly; every caller treats a failed allocation or import as
 * "no shared buffers" and keeps its copy path. On API 26+ builds the checks fold away. */

static inline int dd_ahb_supported(void) {
    if (__builtin_available(android 26, *)) return 1;
    return 0;
}

static inline int dd_ahb_allocate(const AHardwareBuffer_Desc *desc, AHardwareBuffer **out) {
    if (__builtin_available(android 26, *)) return AHardwareBuffer_allocate(desc, out);
    if (out) *out = NULL;
    return -ENOSYS;
}

static inline void dd_ahb_acquire(AHardwareBuffer *buffer) {
    if (__builtin_available(android 26, *)) AHardwareBuffer_acquire(buffer);
}

static inline void dd_ahb_release(AHardwareBuffer *buffer) {
    if (__builtin_available(android 26, *)) AHardwareBuffer_release(buffer);
}

static inline void dd_ahb_describe(const AHardwareBuffer *buffer, AHardwareBuffer_Desc *desc) {
    if (__builtin_available(android 26, *)) {
        AHardwareBuffer_describe(buffer, desc);
        return;
    }
    memset(desc, 0, sizeof(*desc));
}

static inline int dd_ahb_lock(AHardwareBuffer *buffer, uint64_t usage, int32_t fence, const ARect *rect, void **address) {
    if (__builtin_available(android 26, *)) return AHardwareBuffer_lock(buffer, usage, fence, rect, address);
    if (address) *address = NULL;
    return -ENOSYS;
}

static inline int dd_ahb_unlock(AHardwareBuffer *buffer, int32_t *fence) {
    if (__builtin_available(android 26, *)) return AHardwareBuffer_unlock(buffer, fence);
    if (fence) *fence = -1;
    return -ENOSYS;
}

static inline int dd_ahb_send_handle(const AHardwareBuffer *buffer, int socket) {
    if (__builtin_available(android 26, *)) return AHardwareBuffer_sendHandleToUnixSocket(buffer, socket);
    return -ENOSYS;
}

static inline int dd_ahb_recv_handle(int socket, AHardwareBuffer **out) {
    if (__builtin_available(android 26, *)) return AHardwareBuffer_recvHandleFromUnixSocket(socket, out);
    if (out) *out = NULL;
    return -ENOSYS;
}

/**
 * BG3SE-macOS - STDString (ls::STDString) read/write
 *
 * See stdstring.h for the layout and how it was established.
 */

#include "stdstring.h"

#include <string.h>

#include "safe_memory.h"

const char *stdstring_read(const void *addr, size_t *out_len) {
    if (!addr || !out_len) return NULL;
    const uint8_t *p = (const uint8_t *)addr;

    uint8_t flag_byte = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)(p + 15), &flag_byte)) return NULL;

    if (!(flag_byte & 0x80)) {
        size_t len = flag_byte & 0x7f;
        if (len > STDSTRING_INLINE_MAX) return NULL;
        *out_len = len;
        return (const char *)p;
    }

    void *data = NULL;
    uint32_t size = 0, cap = 0;
    if (!safe_memory_read_u64((mach_vm_address_t)(p + 0), (uint64_t *)&data)) return NULL;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + 8), &size)) return NULL;
    if (!safe_memory_read_u32((mach_vm_address_t)(p + 12), &cap)) return NULL;
    cap &= 0x7fffffffu;

    if (!data || size > cap || cap > (1u << 24)) return NULL;
    uint8_t probe = 0;
    if (!safe_memory_read_u8((mach_vm_address_t)data, &probe)) return NULL;

    *out_len = (size_t)size;
    return (const char *)data;
}

bool stdstring_write(void *addr, const char *str, size_t len, StdStringAllocFn alloc) {
    if (!addr || (!str && len > 0)) return false;
    uint8_t *p = (uint8_t *)addr;

    if (len <= STDSTRING_WRITE_INLINE_MAX) {
        memset(p, 0, STDSTRING_SIZE);
        if (len) memcpy(p, str, len);
        p[15] = (uint8_t)(len & 0x7f);
        return true;
    }

    if (!alloc) return false;   // caller supplies the allocator; see the header
    char *buf = (char *)alloc(len + 1);
    if (!buf) return false;
    memcpy(buf, str, len);
    buf[len] = '\0';
    *(void **)(p + 0) = buf;
    *(uint32_t *)(p + 8) = (uint32_t)len;
    *(uint32_t *)(p + 12) = (uint32_t)(len + 1) | 0x80000000u;
    return true;
}

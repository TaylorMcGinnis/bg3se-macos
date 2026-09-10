/**
 * BG3SE-macOS - STDString (ls::STDString) read/write
 *
 * STDString is 16 bytes on this build, NOT a std::string. Verified live off a
 * Progression resource:
 *
 *   short:  characters inline at +0, length in (byte[15] & 0x7f)
 *   long:   pointer at +0, uint32 length at +8, uint32 capacity at +12
 *           whose top bit marks the long form
 *
 * The discriminator is the top bit of the last byte either way. Upstream
 * declares these fields as std::basic_string, which would be 24 bytes on libc++
 * and 32 on MSVC; both readings put every field after the first string in a
 * struct at the wrong offset. The sample that settled it held "Barbarian"
 * inline with 0x09 at byte 15, and a 231-character Boosts string out of line
 * with its capacity at +12 carrying the 0x80000000 flag.
 *
 * This lives in core so the static-data resource proxies and the ECS component
 * property layer share one implementation instead of each carrying a copy of a
 * layout this fiddly.
 */

#ifndef BG3SE_CORE_STDSTRING_H
#define BG3SE_CORE_STDSTRING_H

#include <stdbool.h>
#include <stddef.h>

#define STDSTRING_SIZE             16
#define STDSTRING_INLINE_MAX       15   // characters that fit inline
#define STDSTRING_WRITE_INLINE_MAX 14   // leave room for a terminator when we write

/**
 * Allocator for out-of-line strings. The caller supplies it -- normally
 * game_memory_alloc, since the engine owns these buffers -- so this module
 * stays free of any dependency on the offset table and links into the tests.
 */
typedef void *(*StdStringAllocFn)(size_t size);

/**
 * Read an STDString. Returns a pointer to the characters and sets *out_len,
 * or NULL if the address is unreadable or the header is malformed.
 *
 * The returned pointer is either into the string itself (short form) or into
 * the game-owned buffer (long form); it is not owned by the caller and is only
 * valid until the string is next written.
 */
const char *stdstring_read(const void *addr, size_t *out_len);

/**
 * Overwrite an STDString in place. Strings up to STDSTRING_WRITE_INLINE_MAX
 * need no allocation; longer ones are allocated through `alloc`, and the old
 * buffer is intentionally left alone, since it belongs to the engine's
 * allocator and freeing it through ours corrupts the heap.
 *
 * Returns false when the address is NULL, or when the value needs an allocation
 * and `alloc` is NULL or fails.
 */
bool stdstring_write(void *addr, const char *str, size_t len, StdStringAllocFn alloc);

#endif // BG3SE_CORE_STDSTRING_H

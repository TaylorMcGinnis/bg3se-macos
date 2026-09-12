/**
 * BG3SE-macOS - Exception barrier for calls into Osiris
 *
 * Osiris rejects a malformed or mistyped tuple by throwing a C++ exception.
 * Every frame between the throw and our Lua entry point is C, so there is
 * nothing to unwind through and no handler to find: the runtime goes straight
 * to std::terminate and aborts the process.
 *
 * That is what happened on 2026-09-11 -- an Osi.<name> call for a function the
 * engine id cache did not hold was routed into the story tuple-insert path, the
 * engine rejected the tuple, and the game died with SIGABRT. The crash report
 * showed only osi_story_insert and the log's last line was an unrelated cache
 * refresh, so the offending function could not even be identified.
 *
 * Validation in front of the insert is already strict (node resolution,
 * signature read, arity, per-argument type checks all raise before the engine
 * sees anything), so what is left is the engine's own assertions about state we
 * cannot inspect from outside. Those must not be fatal to the whole session, so
 * the call crosses into C++ here and comes back as a boolean.
 */

#ifndef BG3SE_OSIRIS_CALL_GUARD_H
#define BG3SE_OSIRIS_CALL_GUARD_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Invoke a two-argument Osiris entry point (a RETE node's Add/Del) with the
 * C++ exception barrier in place.
 *
 * Returns true when the call completed. On a thrown exception returns false and
 * writes a short description into `err` (always NUL-terminated when errSize > 0)
 * so the caller can raise a Lua error naming what failed.
 *
 * `err` may be NULL if the caller does not want the text.
 */
bool osi_guarded_node_call(void (*fn)(void *, void *), void *node, void *arg,
                           char *err, size_t errSize);

#ifdef __cplusplus
}
#endif

#endif // BG3SE_OSIRIS_CALL_GUARD_H

/**
 * BG3SE-macOS - Exception barrier for calls into Osiris
 *
 * See osi_call_guard.h for why this exists.
 *
 * The game and this dylib both use the system libc++ / libc++abi on macOS, so
 * exceptions thrown inside libOsiris unwind into this translation unit and are
 * caught here. This is the only place in the port that needs to be C++.
 */

#include "osi_call_guard.h"

#include <exception>
#include <cstdio>
#include <cstring>

namespace {

void copy_error(char *err, size_t errSize, const char *text) {
    if (!err || errSize == 0) return;
    std::snprintf(err, errSize, "%s", text ? text : "unknown error");
}

}  // namespace

extern "C" bool osi_guarded_node_call(void (*fn)(void *, void *), void *node, void *arg,
                                      char *err, size_t errSize) {
    if (!fn) {
        copy_error(err, errSize, "entry point is null");
        return false;
    }

    try {
        fn(node, arg);
        if (err && errSize) err[0] = '\0';
        return true;
    } catch (const std::exception &e) {
        copy_error(err, errSize, e.what());
        return false;
    } catch (...) {
        // Osiris throws types we have no declaration for; the fact that it
        // rejected the call is the part the caller can act on.
        copy_error(err, errSize, "Osiris rejected the call");
        return false;
    }
}

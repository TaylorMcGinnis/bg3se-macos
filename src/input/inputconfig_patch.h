#ifndef BG3SE_INPUTCONFIG_PATCH_H
#define BG3SE_INPUTCONFIG_PATCH_H

#include <stdbool.h>
#include <stddef.h>

/**
 * Copy the player's camera movement bindings onto the character movement
 * actions in inputconfig_p1.json.
 *
 * BG3 ships CharacterMoveForward/Backward/Left/Right unbound, and its Options
 * screen has no entry for them, so a player cannot bind them by hand. The
 * camera bindings are the ones a player can edit, so those are the source.
 * This mirrors what Ch4nKyy's BG3WASD does on Windows.
 *
 * Runs from the dylib constructor, before BG3 reads the file. A hook would be
 * installed too late for the first load.
 *
 * Does nothing unless a mod that wants keyboard movement is installed.
 * Returns true when the file was rewritten.
 */
bool inputconfig_patch_movement(void);


/*
 * Exposed for tests only. These carry the two failure modes worth pinning: a
 * result longer than its source, and an empty result that must not be written
 * over a player's bindings. The test links this object rather than including
 * the .c, so the dependency is one CMake actually tracks.
 */

/** Locate "key" : [ ... ] and return the span of the array, brackets included. */
bool inputconfig_find_key_array(const char *buf, size_t len, const char *key,
                                size_t *out_start, size_t *out_end);

/**
 * Rebuild an array body keeping only usable keyboard entries.
 * Returns NULL when nothing usable remains, so callers leave the target alone.
 * Caller frees.
 */
char *inputconfig_filter_keyboard_entries(const char *arr, size_t len);

#endif

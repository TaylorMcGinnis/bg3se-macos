#ifndef BG3SE_INPUTCONFIG_PATCH_H
#define BG3SE_INPUTCONFIG_PATCH_H

#include <stdbool.h>

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

#endif

/**
 * BG3SE-macOS - Campaign (playthrough) identity for persisted variables
 *
 * Mod variables, user variables and PersistentVars were stored in one file per
 * kind for the whole machine, so every playthrough shared them. Upstream keeps
 * this state inside the savegame, so a new game starts clean; ours did not, and
 * a brand-new game came up already holding another playthrough's data
 * (observed 2026-09-11: AppearanceEditEnhanced's OriginCopiedChars and
 * CustomNames carried Karlach entries into a fresh campaign).
 *
 * That is not merely untidy. Mods key this state by character UUID, and origin
 * UUIDs are identical in every playthrough, so a mod asking "have I already
 * done this to this character" gets the wrong answer. AppearanceEditEnhanced's
 * PersistTemplateValues only initialises its entry when absent, so a stale one
 * from another campaign survives and the next resculpt runs against the wrong
 * template values.
 *
 * The key is the campaign's avatar UUID, read from Osiris DB_Avatars. It is
 * minted at character creation and does not change for the life of the
 * playthrough. Osi.GetHostCharacter() is NOT usable here -- it follows whoever
 * the player currently controls (verified: it returned Lae'zel's UUID after
 * switching to her).
 */

#ifndef BG3SE_VARS_CAMPAIGN_KEY_H
#define BG3SE_VARS_CAMPAIGN_KEY_H

#include <stdbool.h>

/** Longest key we store, plus terminator. Avatar UUIDs are 36 characters. */
#define CAMPAIGN_KEY_MAX 64

/**
 * Record the current campaign's key. Accepts a bare UUID or a
 * "Template_<uuid>" name; only the trailing UUID is kept, so the key does not
 * change if the avatar's template name ever differs.
 *
 * Returns true when the key actually changed (including first set), which is
 * the caller's signal to re-point storage and reload.
 */
bool campaign_key_set(const char *avatar_uuid);

/** Forget the current campaign (returning to the main menu). */
void campaign_key_clear(void);

/** The current key, or NULL when no campaign is loaded. */
const char *campaign_key_get(void);

/** True when a campaign key is known. */
bool campaign_key_known(void);

#endif // BG3SE_VARS_CAMPAIGN_KEY_H

/**
 * BG3SE-macOS - Campaign (playthrough) identity for persisted variables
 *
 * See campaign_key.h for why this exists and why the avatar UUID is the key.
 */

#include "campaign_key.h"

#include <string.h>

#include "../core/logging.h"

static char g_CampaignKey[CAMPAIGN_KEY_MAX] = {0};

/* Mods and Osiris hand out character identifiers in two shapes: a bare GUID,
 * and "Template_Name_<guid>". Keep only the trailing GUID so both name the same
 * campaign. */
static const char *trailing_uuid(const char *s, size_t len) {
    const size_t UUID_LEN = 36;
    if (len < UUID_LEN) return NULL;
    const char *tail = s + len - UUID_LEN;
    for (size_t i = 0; i < UUID_LEN; i++) {
        char c = tail[i];
        bool hyphen = (i == 8 || i == 13 || i == 18 || i == 23);
        if (hyphen) {
            if (c != '-') return NULL;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return NULL;
        }
    }
    return tail;
}

bool campaign_key_set(const char *avatar_uuid) {
    if (!avatar_uuid || avatar_uuid[0] == '\0') return false;

    const char *uuid = trailing_uuid(avatar_uuid, strlen(avatar_uuid));
    if (!uuid) {
        LOG_LUA_WARN("Campaign key '%s' is not a UUID; ignoring", avatar_uuid);
        return false;
    }

    if (strncmp(g_CampaignKey, uuid, CAMPAIGN_KEY_MAX - 1) == 0) {
        return false;  // unchanged
    }

    strncpy(g_CampaignKey, uuid, CAMPAIGN_KEY_MAX - 1);
    g_CampaignKey[CAMPAIGN_KEY_MAX - 1] = '\0';
    LOG_LUA_INFO("Campaign key: %s", g_CampaignKey);
    return true;
}

void campaign_key_clear(void) {
    g_CampaignKey[0] = '\0';
}

const char *campaign_key_get(void) {
    return g_CampaignKey[0] ? g_CampaignKey : NULL;
}

bool campaign_key_known(void) {
    return g_CampaignKey[0] != '\0';
}

/**
 * inputconfig_patch.c - point character movement at the camera bindings.
 *
 * The edit is deliberately narrow: each "CharacterMove*" array gets the keys
 * of its "Camera*" counterpart plus its own controller entries. Nothing else
 * is reformatted or reordered, so BG3 keeps ownership of the file's shape.
 *
 * No JSON parser: the only one in this tree builds Lua values, and this runs
 * before Lua exists.
 */

#include "inputconfig_patch.h"
#include "../core/logging.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MOD_DIR_NAME "BG3PlayerImmersiveCamera"

static const char *const kCameraKeys[4] = {
    "CameraForward", "CameraBackward", "CameraLeft", "CameraRight"
};
static const char *const kCharacterKeys[4] = {
    "CharacterMoveForward", "CharacterMoveBackward",
    "CharacterMoveLeft", "CharacterMoveRight"
};
/* The game's controller defaults for the same four actions (controller_1.json). */
static const char *const kCharacterStick[4] = {
    "c:leftstick_ypos", "c:leftstick_yneg",
    "c:leftstick_xneg", "c:leftstick_xpos"
};

/* Skip a JSON string literal starting at *p (which points at the quote). */
static const char *skip_string(const char *p, const char *end) {
    p++;  /* opening quote */
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) p++;
        p++;
    }
    return (p < end) ? p + 1 : end;
}

/* Quoted text is skipped so a bracket inside a string cannot end the array. */
bool inputconfig_find_key_array(const char *buf, size_t len, const char *key,
                           size_t *out_start, size_t *out_end) {
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof(needle)) return false;

    const char *end = buf + len;
    const char *p = buf;
    while ((p = strstr(p, needle)) != NULL) {
        const char *q = p + n;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != ':') { p += n; continue; }
        q++;
        while (q < end && (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')) q++;
        if (q >= end || *q != '[') { p += n; continue; }

        const char *arr = q;
        int depth = 0;
        while (q < end) {
            if (*q == '"') { q = skip_string(q, end); continue; }
            if (*q == '[') depth++;
            else if (*q == ']') { depth--; if (depth == 0) { q++; break; } }
            q++;
        }
        if (depth != 0) return false;
        *out_start = (size_t)(arr - buf);
        *out_end = (size_t)(q - buf);
        return true;
    }
    return false;
}

char *inputconfig_filter_keyboard_entries(const char *arr, size_t len) {
    /* Separators are written as ", " where the source may have ",", so the
     * result can outgrow the input by a byte per entry. */
    size_t cap = len * 2 + 8;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t w = 0;
    out[w++] = '[';
    bool first = true;

    const char *p = arr, *end = arr + len;
    while (p < end) {
        if (*p != '"') { p++; continue; }
        const char *s = p;
        p = skip_string(p, end);
        size_t slen = (size_t)(p - s);              /* includes both quotes */
        if (slen < 3) continue;
        if (strncmp(s + 1, "key:", 4) != 0) continue;          /* keyboard only */
        if (slen >= 12 && strncmp(s + 1, "key:unknown", 11) == 0) continue;

        if (!first) { out[w++] = ','; out[w++] = ' '; }
        memcpy(out + w, s, slen); w += slen;
        first = false;
    }
    out[w++] = ']';
    out[w] = '\0';

    /* An empty result would clear character movement, which the Options
     * screen cannot restore; leave the previous value alone instead. */
    if (first) {
        free(out);
        return NULL;
    }
    return out;
}

/* Writing the character array overrides the game's controller default, so the
 * stick binding must be carried over or the controller cannot move. */
char *inputconfig_keep_controller_entries(const char *keys, const char *existing,
                                          size_t existing_len, const char *fallback) {
    size_t keys_len = strlen(keys);
    size_t cap = keys_len + existing_len * 2 + strlen(fallback) + 16;
    char *out = malloc(cap);
    if (!out) return NULL;
    memcpy(out, keys, keys_len - 1);                /* drop the closing ']' */
    size_t w = keys_len - 1;
    bool kept = false;

    const char *p = existing, *end = existing + existing_len;
    while (p < end) {
        if (*p != '"') { p++; continue; }
        const char *s = p;
        p = skip_string(p, end);
        size_t slen = (size_t)(p - s);              /* includes both quotes */
        if (slen < 5 || strncmp(s + 1, "c:", 2) != 0) continue;
        out[w++] = ','; out[w++] = ' ';
        memcpy(out + w, s, slen); w += slen;
        kept = true;
    }
    if (!kept) {
        w += (size_t)snprintf(out + w, cap - w, ", \"%s\"", fallback);
    }
    out[w++] = ']';
    out[w] = '\0';
    return out;
}

static char *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0 || sz > (long)(4 * 1024 * 1024)) { fclose(f); return NULL; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[got] = '\0';
    *out_len = got;
    return buf;
}

/* Temp file then rename, so a crash cannot leave a half-written config. */
static bool write_file_atomic(const char *path, const char *data, size_t len) {
    char tmp[1024];
    if (snprintf(tmp, sizeof(tmp), "%s.bg3se-tmp", path) >= (int)sizeof(tmp)) return false;
    FILE *f = fopen(tmp, "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0) ok = false;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return false; }
    return true;
}

/* Pick the profile that actually owns an inputconfig; fall back to Public. */
static bool find_inputconfig(char *out, size_t out_size) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char profiles[900];
    if (snprintf(profiles, sizeof(profiles),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles",
                 home) >= (int)sizeof(profiles)) {
        return false;
    }

    DIR *d = opendir(profiles);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.') continue;
            char candidate[1024];
            if (snprintf(candidate, sizeof(candidate), "%s/%s/inputconfig_p1.json",
                         profiles, e->d_name) >= (int)sizeof(candidate)) {
                continue;
            }
            struct stat st;
            if (stat(candidate, &st) == 0 && S_ISREG(st.st_mode)) {
                closedir(d);
                snprintf(out, out_size, "%s", candidate);
                return true;
            }
        }
        closedir(d);
    }

    return snprintf(out, out_size, "%s/Public/inputconfig_p1.json", profiles)
           < (int)out_size;
}

/*
 * Only touch the player's bindings when they have enabled the mod, i.e. when
 * modsettings.lsx names it. An unpacked mod folder also counts, for working
 * copies.
 */
static bool movement_mod_installed(void) {
    const char *home = getenv("HOME");
    if (!home) return false;

    char path[1024];
    struct stat st;

    if (snprintf(path, sizeof(path),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/Mods/%s",
                 home, MOD_DIR_NAME) < (int)sizeof(path) &&
        stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        return true;
    }

    if (snprintf(path, sizeof(path),
                 "%s/Documents/Larian Studios/Baldur's Gate 3/PlayerProfiles",
                 home) >= (int)sizeof(path)) {
        return false;
    }

    DIR *d = opendir(path);
    if (!d) return false;

    bool enabled = false;
    struct dirent *e;
    while (!enabled && (e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char settings[1200];
        if (snprintf(settings, sizeof(settings), "%s/%s/modsettings.lsx",
                     path, e->d_name) >= (int)sizeof(settings)) {
            continue;
        }
        size_t len = 0;
        char *buf = read_file(settings, &len);
        if (!buf) continue;
        enabled = strstr(buf, "\"" MOD_DIR_NAME "\"") != NULL;
        free(buf);
    }
    closedir(d);
    return enabled;
}

bool inputconfig_patch_movement(void) {
    if (!movement_mod_installed()) return false;

    char path[1024];
    if (!find_inputconfig(path, sizeof(path))) return false;

    size_t len = 0;
    char *buf = read_file(path, &len);
    if (!buf) {
        LOG_INPUT_DEBUG("[InputConfig] no config at %s", path);
        return false;
    }

    bool changed = false;

    for (int i = 0; i < 4; i++) {
        size_t cs, ce;
        if (!inputconfig_find_key_array(buf, len, kCameraKeys[i], &cs, &ce)) continue;

        char *want = inputconfig_filter_keyboard_entries(buf + cs, ce - cs);
        if (!want) {
            LOG_INPUT_DEBUG("[InputConfig] %s has no keyboard binding; %s left alone",
                            kCameraKeys[i], kCharacterKeys[i]);
            continue;
        }

        size_t ts, te;
        if (!inputconfig_find_key_array(buf, len, kCharacterKeys[i], &ts, &te)) {
            free(want);
            LOG_INPUT_DEBUG("[InputConfig] %s absent; skipped", kCharacterKeys[i]);
            continue;
        }

        char *merged = inputconfig_keep_controller_entries(want, buf + ts, te - ts,
                                                           kCharacterStick[i]);
        free(want);
        if (!merged) break;
        want = merged;

        size_t want_len = strlen(want);
        if (want_len == te - ts && memcmp(buf + ts, want, want_len) == 0) {
            free(want);
            continue;  /* already correct */
        }

        size_t new_len = len - (te - ts) + want_len;
        char *next = malloc(new_len + 1);
        if (!next) { free(want); break; }
        memcpy(next, buf, ts);
        memcpy(next + ts, want, want_len);
        memcpy(next + ts + want_len, buf + te, len - te);
        next[new_len] = '\0';

        free(buf); free(want);
        buf = next; len = new_len;
        changed = true;
    }

    if (changed) {
        if (write_file_atomic(path, buf, len)) {
            LOG_INPUT_INFO("[InputConfig] character movement now follows the camera bindings");
        } else {
            LOG_INPUT_ERROR("[InputConfig] could not write %s", path);
            changed = false;
        }
    }

    free(buf);
    return changed;
}

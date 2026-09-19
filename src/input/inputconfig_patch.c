/**
 * inputconfig_patch.c - point character movement at the camera bindings.
 *
 * The edit is deliberately narrow. It reads four "Camera*" arrays and writes
 * them to the matching "CharacterMove*" arrays. It does not reformat or
 * reorder anything else, so BG3 keeps ownership of the file's shape.
 *
 * A full JSON parser is not used on purpose: the only parser in this tree
 * builds Lua values, and this runs before Lua exists.
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

/* Skip a JSON string literal starting at *p (which points at the quote). */
static const char *skip_string(const char *p, const char *end) {
    p++;  /* opening quote */
    while (p < end && *p != '"') {
        if (*p == '\\' && p + 1 < end) p++;
        p++;
    }
    return (p < end) ? p + 1 : end;
}

/*
 * Find "key" : [ ... ] and return the span of the array, brackets included.
 * Quoted text is skipped so a bracket inside a string cannot confuse this.
 */
static bool find_key_array(const char *buf, size_t len, const char *key,
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

/*
 * Rebuild an array body, keeping only usable keyboard entries.
 * Controller entries and the engine's unbound sentinels are dropped: the
 * character action only needs the keys.
 */
static char *filter_keyboard_entries(const char *arr, size_t len) {
    size_t cap = len + 8;
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
 * The mod ships as a .pak, so an unpacked folder is no longer proof of
 * anything. Ask the load order instead: modsettings.lsx names the mod only
 * when the player has actually enabled it, which is the permission we want
 * before touching their bindings. The unpacked folder is still accepted, for
 * anyone running the mod from a working copy.
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
        if (!find_key_array(buf, len, kCameraKeys[i], &cs, &ce)) continue;

        char *want = filter_keyboard_entries(buf + cs, ce - cs);
        if (!want) continue;

        size_t ts, te;
        if (!find_key_array(buf, len, kCharacterKeys[i], &ts, &te)) {
            free(want);
            LOG_INPUT_DEBUG("[InputConfig] %s absent; skipped", kCharacterKeys[i]);
            continue;
        }

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

/*
 * Tier 0: PersistentVars store round trip.
 *
 * Regression for the "null" store: persist_save_all() took lua_gettop() after
 * luaL_buffinit(), which in Lua 5.4 pushes a placeholder slot, so every mod's
 * file held the 4-byte "null" that the restore then rejected. Mods therefore
 * started from empty PersistentVars on every load (TransmogEnhanced granted
 * its control items again each time).
 *
 * The store lives under $HOME, so HOME is pointed at a scratch directory
 * before the module initialises. No game state is involved: the tick's
 * session gate is not exercised here, so game_state_get_current() is stubbed.
 */

#include "test_harness.h"
#include "lua_persistentvars.h"
#include "../../src/game/game_state.h"

#include <lauxlib.h>
#include <lualib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

ServerGameState game_state_get_current(void) { return SERVER_STATE_RUNNING; }

static char s_home[512];

static void use_scratch_home(void) {
    if (s_home[0]) return;
    snprintf(s_home, sizeof(s_home), "/tmp/bg3se_persist_XXXXXX");
    ASSERT_NOT_NULL(mkdtemp(s_home));
    setenv("HOME", s_home, 1);
    /* persist_init() creates only its own two levels; a real home has these. */
    char cmd[600];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s/Library/Application Support'", s_home);
    ASSERT_EQ(system(cmd), 0);
}

static char *store_read(const char *modtable) {
    char path[1024];
    snprintf(path, sizeof(path),
             "%s/Library/Application Support/BG3SE/persistentvars/%s.json", s_home, modtable);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    static char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static lua_State *fresh_state(const char *setup) {
    lua_State *L = luaL_newstate();
    ASSERT_NOT_NULL(L);
    luaL_openlibs(L);
    ASSERT_EQ(luaL_dostring(L, setup), LUA_OK);
    return L;
}

TEST(persist_writes_the_table_not_the_buffer_placeholder) {
    use_scratch_home();
    lua_State *L = fresh_state(
        "Mods = { TransmogEnhanced = { PersistentVars = {"
        "  ControlItems = { 'TMOG_Appearance', 'TMOG_Undo', 'TMOG_Settings' },"
        "  Version = 3 } } }");

    int top = lua_gettop(L);
    persist_save_all(L);
    ASSERT_EQ(lua_gettop(L), top);           /* stack balanced */

    const char *json = store_read("TransmogEnhanced");
    ASSERT_NOT_NULL(json);
    ASSERT_TRUE(json[0] == '{');             /* not "null", not "{}" */
    ASSERT_NOT_NULL(strstr(json, "\"ControlItems\":[\"TMOG_Appearance\",\"TMOG_Undo\",\"TMOG_Settings\"]"));
    ASSERT_NOT_NULL(strstr(json, "\"Version\":3"));
    lua_close(L);
}

TEST(persist_restores_over_the_bootstrap_template) {
    use_scratch_home();
    /* A fresh VM, as on the next launch: the mod's bootstrap has reset its
     * vars to the empty template, then the session loads and we restore. */
    lua_State *L = fresh_state("Mods = { TransmogEnhanced = { PersistentVars = {} } }");

    int top = lua_gettop(L);
    persist_restore_all(L);
    ASSERT_EQ(lua_gettop(L), top);
    ASSERT_TRUE(persist_is_loaded());

    ASSERT_EQ(luaL_dostring(L,
        "local pv = Mods.TransmogEnhanced.PersistentVars\n"
        "assert(#pv.ControlItems == 3, 'ControlItems lost')\n"
        "assert(pv.ControlItems[2] == 'TMOG_Undo')\n"
        "assert(pv.Version == 3)\n"), LUA_OK);
    lua_close(L);
}

TEST(persist_never_overwrites_data_with_empty_or_null) {
    use_scratch_home();
    /* The dedup cache remembers the last write; a new session drops it so the
     * empty-table guard, not the cache, is what protects the file here. */
    persist_session_reset();
    lua_State *L = fresh_state("Mods = { TransmogEnhanced = { PersistentVars = {} } }");
    persist_save_all(L);
    const char *json = store_read("TransmogEnhanced");
    ASSERT_NOT_NULL(json);
    ASSERT_NOT_NULL(strstr(json, "\"Version\":3"));
    lua_close(L);
}

void register_persistentvars_tests(void) {
    printf("PersistentVars store:\n");
    RUN_TEST(persist_writes_the_table_not_the_buffer_placeholder);
    RUN_TEST(persist_restores_over_the_bootstrap_template);
    RUN_TEST(persist_never_overwrites_data_with_empty_or_null);
    if (s_home[0]) {
        char cmd[600];
        snprintf(cmd, sizeof(cmd), "rm -rf '%s'", s_home);
        (void)system(cmd);
    }
}

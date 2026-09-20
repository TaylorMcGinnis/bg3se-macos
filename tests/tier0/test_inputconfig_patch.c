/*
 * Tier 0 tests: inputconfig_patch.c -- the camera-to-character binding copy.
 *
 * BG3 ships CharacterMoveForward and its siblings unbound and offers no UI to
 * bind them, so the extender derives them from the camera bindings. That means
 * it rewrites a file the player owns, and two of its failure modes are silent:
 * a buffer that overruns on a long binding list, and an empty result that would
 * clear movement with no way back through the Options screen.
 *
 * The helpers are exposed in the header and this links the real object. An
 * earlier version included the .c instead; CMake did not track that as a
 * dependency, so a deliberately reintroduced bug still reported PASS.
 */

#include "test_harness.h"

#include <string.h>

#include "inputconfig_patch.h"
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------- */
/* Locating a key's array                                            */
/* ---------------------------------------------------------------- */

static const char *FOUR_KEYS =
    "{\n"
    "   \"CameraForward\" : [ \"key:w\", \"key:up\" ],\n"
    "   \"CameraBackward\" : [ \"key:s\", \"key:down\" ],\n"
    "   \"CharacterMoveForward\" : [],\n"
    "   \"PhotoModeCameraForward\" : [ \"c:leftstick_ypos\", \"key:w\" ]\n"
    "}\n";

static char *filter_for(const char *json, const char *key) {
    size_t s = 0, e = 0;
    if (!inputconfig_find_key_array(json, strlen(json), key, &s, &e)) return NULL;
    return inputconfig_filter_keyboard_entries(json + s, e - s);
}

TEST(finds_the_named_array) {
    size_t s = 0, e = 0;
    ASSERT_TRUE(inputconfig_find_key_array(FOUR_KEYS, strlen(FOUR_KEYS), "CameraForward", &s, &e));
    ASSERT_EQ(FOUR_KEYS[s], '[');
    ASSERT_EQ(FOUR_KEYS[e - 1], ']');
}

/* "CameraForward" is a prefix of "PhotoModeCameraForward" only in the other
 * direction, but a sloppy search would match the longer key's suffix. */
TEST(does_not_match_a_longer_key) {
    size_t s = 0, e = 0;
    ASSERT_TRUE(inputconfig_find_key_array(FOUR_KEYS, strlen(FOUR_KEYS), "PhotoModeCameraForward", &s, &e));
    char *got = inputconfig_filter_keyboard_entries(FOUR_KEYS + s, e - s);
    ASSERT_TRUE(got != NULL);
    ASSERT_STR_EQ(got, "[\"key:w\"]");   /* controller entry dropped */
    free(got);
}

TEST(missing_key_is_not_found) {
    size_t s = 0, e = 0;
    ASSERT_FALSE(inputconfig_find_key_array(FOUR_KEYS, strlen(FOUR_KEYS), "NoSuchAction", &s, &e));
}

/* ---------------------------------------------------------------- */
/* Choosing which entries to copy                                    */
/* ---------------------------------------------------------------- */

TEST(copies_keyboard_entries_in_order) {
    char *got = filter_for(FOUR_KEYS, "CameraForward");
    ASSERT_TRUE(got != NULL);
    ASSERT_STR_EQ(got, "[\"key:w\", \"key:up\"]");
    free(got);
}

TEST(drops_controller_entries) {
    char *got = filter_for("{\"A\":[\"c:leftstick_yneg\",\"key:w\"]}", "A");
    ASSERT_TRUE(got != NULL);
    ASSERT_STR_EQ(got, "[\"key:w\"]");
    free(got);
}

/* The engine writes these sentinels into a slot the player has cleared. */
TEST(drops_unbound_sentinels) {
    char *a = filter_for("{\"A\":[\"INVALID:unknown\",\"key:w\"]}", "A");
    ASSERT_TRUE(a != NULL);
    ASSERT_STR_EQ(a, "[\"key:w\"]");
    free(a);

    char *b = filter_for("{\"A\":[\"key:unknown\",\"key:w\"]}", "A");
    ASSERT_TRUE(b != NULL);
    ASSERT_STR_EQ(b, "[\"key:w\"]");
    free(b);
}

/*
 * Regression. An empty result was copied over the character action, clearing
 * the player's movement. The Options screen has no entry for those actions, so
 * there was no way back except rebinding the camera. Leaving the previous
 * value alone is the safer failure.
 */
TEST(refuses_to_produce_an_empty_binding) {
    ASSERT_TRUE(filter_for("{\"A\":[]}", "A") == NULL);
    ASSERT_TRUE(filter_for("{\"A\":[\"c:leftstick_yneg\"]}", "A") == NULL);
    ASSERT_TRUE(filter_for("{\"A\":[\"INVALID:unknown\"]}", "A") == NULL);
    ASSERT_TRUE(filter_for("{\"A\":[\"key:unknown\"]}", "A") == NULL);
}

/*
 * Regression. The output buffer was sized len + 8. Separators are written as
 * ", " while the source may hold only ",", so the result grows by a byte per
 * entry and overran past eight bindings.
 *
 * This case only bites under a sanitizer: malloc rounds the allocation up, so
 * a normal build still reports PASS with the bug reintroduced. Verified with
 * -DBG3SE_TEST_ASAN=ON, which reports heap-buffer-overflow in
 * inputconfig_filter_keyboard_entries.
 */
TEST(survives_more_bindings_than_the_old_buffer_held) {
    char json[1024];
    size_t n = (size_t)snprintf(json, sizeof(json), "{\"A\":[");
    for (int i = 0; i < 16; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n, "%s\"key:f%d\"", i ? "," : "", i + 1);
    }
    snprintf(json + n, sizeof(json) - n, "]}");

    char *got = filter_for(json, "A");
    ASSERT_TRUE(got != NULL);
    ASSERT_EQ(got[0], '[');
    ASSERT_EQ(got[strlen(got) - 1], ']');
    /* every entry survived */
    for (int i = 0; i < 16; i++) {
        char want[16];
        snprintf(want, sizeof(want), "\"key:f%d\"", i + 1);
        ASSERT_TRUE(strstr(got, want) != NULL);
    }
    free(got);
}

/* A bracket inside a string must not end the array early. */
TEST(brackets_inside_strings_do_not_confuse_the_scan) {
    char *got = filter_for("{\"A\":[\"key:[\",\"key:w\"]}", "A");
    ASSERT_TRUE(got != NULL);
    ASSERT_TRUE(strstr(got, "\"key:w\"") != NULL);
    free(got);
}

void register_inputconfig_patch_tests(void) {
    RUN_TEST(finds_the_named_array);
    RUN_TEST(does_not_match_a_longer_key);
    RUN_TEST(missing_key_is_not_found);
    RUN_TEST(copies_keyboard_entries_in_order);
    RUN_TEST(drops_controller_entries);
    RUN_TEST(drops_unbound_sentinels);
    RUN_TEST(refuses_to_produce_an_empty_binding);
    RUN_TEST(survives_more_bindings_than_the_old_buffer_held);
    RUN_TEST(brackets_inside_strings_do_not_confuse_the_scan);
}

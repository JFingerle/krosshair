/*
 * Unit tests for the hotkey key-name mapping (include/keys.h).
 *
 * kh_key_from_name is a static inline in the header, so this translation
 * unit simply includes it and drives the mapping table directly.
 */
#define _GNU_SOURCE

#include <stdint.h>

#include "test.h"
#include "../include/keys.h"

static void test_known_names(void)
{
        CHECK_EQ(kh_key_from_name("a"), KEY_A);
        CHECK_EQ(kh_key_from_name("z"), KEY_Z);
        CHECK_EQ(kh_key_from_name("1"), KEY_1);
        CHECK_EQ(kh_key_from_name("space"), KEY_SPACE);
        CHECK_EQ(kh_key_from_name("escape"), KEY_ESC);
        CHECK_EQ(kh_key_from_name("return"), KEY_ENTER);
        CHECK_EQ(kh_key_from_name("shift_l"), KEY_LEFTSHIFT);
        CHECK_EQ(kh_key_from_name("shift_r"), KEY_RIGHTSHIFT);
        CHECK_EQ(kh_key_from_name("ctrl_l"), KEY_LEFTCTRL);
        CHECK_EQ(kh_key_from_name("alt_l"), KEY_LEFTALT);
        CHECK_EQ(kh_key_from_name("f1"), KEY_F1);
        CHECK_EQ(kh_key_from_name("f12"), KEY_F12);
        CHECK_EQ(kh_key_from_name("up"), KEY_UP);
        CHECK_EQ(kh_key_from_name("tab"), KEY_TAB);
}

/* The mapping is case-insensitive. */
static void test_case_insensitive(void)
{
        CHECK_EQ(kh_key_from_name("A"), KEY_A);
        CHECK_EQ(kh_key_from_name("F7"), KEY_F7);
        CHECK_EQ(kh_key_from_name("SPACE"), KEY_SPACE);
        CHECK_EQ(kh_key_from_name("Shift_L"), KEY_LEFTSHIFT);
}

static void test_unknown_names(void)
{
        CHECK_EQ(kh_key_from_name(""), -1);
        CHECK_EQ(kh_key_from_name("bogus_key"), -1);
        CHECK_EQ(kh_key_from_name("shift_r+f7"), -1); /* no token splitting here */
        CHECK_EQ(kh_key_from_name(NULL), -1);
}

int main(void)
{
        fprintf(stderr, "# test_keys\n");
        test_known_names();
        test_case_insensitive();
        test_unknown_names();
        fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
        return test_failed ? 1 : 0;
}

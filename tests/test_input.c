/*
 * Unit tests for the pure hotkey/combo logic in src/input.c.
 *
 * parse_hotkey, kh_key_index, kh_apply_event, kh_elapsed_ms,
 * kh_update_combo and kh_select_timeout_ms are exposed via the
 * INPUT_API macro in UNIT_TEST builds (see tests/test_input_api.h).
 * The evdev device scanning / background-thread code is not exercised.
 */
#define _GNU_SOURCE

#include <time.h>
#include <unistd.h>

#include "test.h"
#include "test_input_api.h"

/* Required-key bitmask for the "A+ESCAPE" combo used below. */
#define TEST_COMBO_BITS ((1 << 0) | (1 << 1))

static void set_hotkey_env(const char* value)
{
    if (value)
        setenv("KROSSHAIR_HOTKEY_TOGGLE", value, 1);
    else
        unsetenv("KROSSHAIR_HOTKEY_TOGGLE");
}

/* Set (or clear) the env var and re-parse it into the required-key list. */
static void parse_hotkey_env(const char* value)
{
    set_hotkey_env(value);
    parse_hotkey();
}

/* Build one synthetic input event and feed it to kh_apply_event. */
static void apply_event(int type, int code, int value)
{
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.value = value;
    kh_apply_event(&ev);
}

/* Unset (or empty) env var falls back to the default SHIFT_R+F9. */
static void test_default_hotkey_unset(void)
{
    parse_hotkey_env(NULL);
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_RIGHTSHIFT);
    CHECK_EQ(kh_input_test_required_key(1), KEY_F9);
}

static void test_default_hotkey_empty(void)
{
    parse_hotkey_env("");
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_RIGHTSHIFT);
    CHECK_EQ(kh_input_test_required_key(1), KEY_F9);
}

static void test_custom_hotkey(void)
{
    parse_hotkey_env("A+ESCAPE");
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_A);
    CHECK_EQ(kh_input_test_required_key(1), KEY_ESC);
}

static void test_single_key_hotkey(void)
{
    parse_hotkey_env("F5");
    CHECK_EQ(kh_input_test_required_count(), 1);
    CHECK_EQ(kh_input_test_required_key(0), KEY_F5);
}

static void test_case_insensitive(void)
{
    parse_hotkey_env("a+Escape");
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_A);
    CHECK_EQ(kh_input_test_required_key(1), KEY_ESC);
}

static void test_whitespace_trimming(void)
{
    parse_hotkey_env("  A +\tESCAPE \r");
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_A);
    CHECK_EQ(kh_input_test_required_key(1), KEY_ESC);
}

/* Only empty tokens: nothing valid, so the default combo is used. */
static void test_only_empty_tokens_fall_back(void)
{
    parse_hotkey_env("++");
    CHECK_EQ(kh_input_test_required_count(), 2);
    CHECK_EQ(kh_input_test_required_key(0), KEY_RIGHTSHIFT);
    CHECK_EQ(kh_input_test_required_key(1), KEY_F9);
}

static void test_unknown_tokens_skipped(void)
{
    parse_hotkey_env("BOGUS+F7");
    CHECK_EQ(kh_input_test_required_count(), 1);
    CHECK_EQ(kh_input_test_required_key(0), KEY_F7);
}

static void test_duplicate_tokens_skipped(void)
{
    parse_hotkey_env("F7+F7");
    CHECK_EQ(kh_input_test_required_count(), 1);
    CHECK_EQ(kh_input_test_required_key(0), KEY_F7);
}

/* More tokens than KROSSHAIR_MAX_KEYS: capped, in order of appearance. */
static void test_max_keys_capped(void)
{
    parse_hotkey_env("A+B+C+D+E+F+G+H+1+2");
    CHECK_EQ(kh_input_test_required_count(), 8);
    CHECK_EQ(kh_input_test_required_key(0), KEY_A);
    CHECK_EQ(kh_input_test_required_key(7), KEY_H);
    CHECK_EQ(kh_input_test_required_key(8), -1);
}

static void test_key_index(void)
{
    parse_hotkey_env("A+ESCAPE");
    CHECK_EQ(kh_key_index(KEY_A), 0);
    CHECK_EQ(kh_key_index(KEY_ESC), 1);
    CHECK_EQ(kh_key_index(KEY_B), -1);
    CHECK_EQ(kh_key_index(KEY_0), -1);
}

static void test_apply_event(void)
{
    parse_hotkey_env("A+ESCAPE");
    kh_input_test_reset();

    apply_event(EV_KEY, KEY_A, 1);
    CHECK_EQ(kh_input_test_keys_down(), 1);

    apply_event(EV_KEY, KEY_ESC, 1);
    CHECK_EQ(kh_input_test_keys_down(), 3);

    apply_event(EV_KEY, KEY_A, 0);
    CHECK_EQ(kh_input_test_keys_down(), 2);

    apply_event(EV_KEY, KEY_ESC, 0);
    CHECK_EQ(kh_input_test_keys_down(), 0);

    /* Key outside the required list: ignored. */
    apply_event(EV_KEY, KEY_B, 1);
    CHECK_EQ(kh_input_test_keys_down(), 0);

    /* Non-key event type: ignored. */
    apply_event(EV_SYN, KEY_A, 1);
    CHECK_EQ(kh_input_test_keys_down(), 0);

    /* Autorepeat (value 2) counts as pressed. */
    apply_event(EV_KEY, KEY_A, 2);
    CHECK_EQ(kh_input_test_keys_down(), 1);

    kh_input_test_reset();
}

static void test_elapsed_ms(void)
{
    struct timespec t0 = { 100, 0 };
    CHECK_EQ(kh_elapsed_ms(&t0, &t0), 0);

    struct timespec one_sec_later = { 101, 0 };
    CHECK_EQ(kh_elapsed_ms(&t0, &one_sec_later), 1000);

    struct timespec same_second = { 100, 250000000 };
    CHECK_EQ(kh_elapsed_ms(&t0, &same_second), 250);

    /* Crossing a second boundary: 900 ms -> 1 s 100 ms = 200 ms. */
    struct timespec start = { 100, 900000000 };
    struct timespec end = { 101, 100000000 };
    CHECK_EQ(kh_elapsed_ms(&start, &end), 200);
}

/*
 * The 100 / 50 literals are KROSSHAIR_RESYNC_TICK_MS and
 * KROSSHAIR_HOTKEY_HOLD_MS from input.c.
 */
static void test_select_timeout(void)
{
    /* No active combo: the 100 ms resync tick. */
    kh_input_test_reset();
    CHECK_EQ(kh_select_timeout_ms(), 100);

    /* Combo already fired: back to the tick. */
    kh_input_test_set_combo(1, 1);
    CHECK_EQ(kh_select_timeout_ms(), 100);

    /* Held, not fired, timer just started: ~50 ms remain. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    kh_input_test_set_combo(1, 0);
    kh_input_test_set_combo_down_ts(&now);
    long t = kh_select_timeout_ms();
    CHECK(t >= 45 && t <= 50);

    /* Timer already past the 50 ms boundary: wake immediately. */
    now.tv_nsec -= 200 * 1000000;
    if (now.tv_nsec < 0) {
        now.tv_nsec += 1000000000;
        now.tv_sec--;
    }
    kh_input_test_set_combo_down_ts(&now);
    CHECK_EQ(kh_select_timeout_ms(), 0);
}

static void test_combo_hold_fires(void)
{
    parse_hotkey_env("A+ESCAPE");
    kh_input_test_reset();
    crosshair_visible = 1;

    /* Partial press: combo not active. */
    kh_input_test_set_keys_down(1);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_active(), 0);
    CHECK_EQ(kh_input_test_combo_fired(), 0);
    CHECK_EQ(crosshair_visible, 1);

    /* Full press: combo becomes active, no fire yet. */
    kh_input_test_set_keys_down(TEST_COMBO_BITS);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_active(), 1);
    CHECK_EQ(kh_input_test_combo_fired(), 0);
    CHECK_EQ(crosshair_visible, 1);

    /* Still within the 50 ms hold window: no fire. */
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_fired(), 0);

    /* After the hold: fires once, toggling visibility. */
    usleep(60 * 1000);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_fired(), 1);
    CHECK_EQ(crosshair_visible, 0);

    /* Debounced: further iterations while held do not re-fire. */
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_fired(), 1);
    CHECK_EQ(crosshair_visible, 0);

    /* Releasing any key cancels the hold. */
    kh_input_test_set_keys_down(1);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_active(), 0);
    CHECK_EQ(kh_input_test_combo_fired(), 0);
    CHECK_EQ(crosshair_visible, 0);

    /* Re-pressing fires again and toggles back. */
    kh_input_test_set_keys_down(TEST_COMBO_BITS);
    kh_update_combo(TEST_COMBO_BITS);
    usleep(60 * 1000);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_fired(), 1);
    CHECK_EQ(crosshair_visible, 1);
}

static void test_combo_cancel_before_hold(void)
{
    parse_hotkey_env("A+ESCAPE");
    kh_input_test_reset();
    crosshair_visible = 1;

    kh_input_test_set_keys_down(TEST_COMBO_BITS);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_active(), 1);

    /* Release before the 50 ms hold completes: nothing fires. */
    usleep(20 * 1000);
    kh_input_test_set_keys_down(0);
    kh_update_combo(TEST_COMBO_BITS);
    CHECK_EQ(kh_input_test_combo_active(), 0);
    CHECK_EQ(kh_input_test_combo_fired(), 0);
    CHECK_EQ(crosshair_visible, 1);
}

int main(void)
{
    fprintf(stderr, "# test_input\n");
    test_default_hotkey_unset();
    test_default_hotkey_empty();
    test_custom_hotkey();
    test_single_key_hotkey();
    test_case_insensitive();
    test_whitespace_trimming();
    test_only_empty_tokens_fall_back();
    test_unknown_tokens_skipped();
    test_duplicate_tokens_skipped();
    test_max_keys_capped();
    test_key_index();
    test_apply_event();
    test_elapsed_ms();
    test_select_timeout();
    test_combo_hold_fires();
    test_combo_cancel_before_hold();
    set_hotkey_env(NULL);
    fprintf(stderr, "  -> %d passed, %d failed\n", test_passed, test_failed);
    return test_failed ? 1 : 0;
}

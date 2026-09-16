#ifndef KROSSHAIR_TEST_INPUT_API_H
#define KROSSHAIR_TEST_INPUT_API_H

/*
 * Declarations of the test-only exports that src/input.c exposes when
 * compiled with -DUNIT_TEST. Types must stay in sync with input.c.
 */

#include <time.h>
#include <linux/input.h>

extern void parse_hotkey(void);
extern int kh_key_index(int code);
extern void kh_apply_event(const struct input_event* ev);
extern long kh_elapsed_ms(const struct timespec* start,
                          const struct timespec* now);
extern void kh_update_combo(int combo_bits);
extern long kh_select_timeout_ms(void);

/* State hooks (see the UNIT_TEST block in input.c). */
extern void kh_input_test_reset(void);
extern int kh_input_test_required_count(void);
extern int kh_input_test_required_key(int index);
extern int kh_input_test_keys_down(void);
extern void kh_input_test_set_keys_down(int mask);
extern int kh_input_test_combo_active(void);
extern int kh_input_test_combo_fired(void);
extern void kh_input_test_set_combo(int active, int fired);
extern void kh_input_test_set_combo_down_ts(const struct timespec* ts);

/* Visibility flag toggled by the hotkey (non-static in input.c). */
extern volatile int crosshair_visible;

#endif /* KROSSHAIR_TEST_INPUT_API_H */

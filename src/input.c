/*
 * Input subsystem.
 *
 * Scans /dev/input for keyboard devices and runs a background thread
 * that watches hotkey press/release events (evdev) to toggle the
 * crosshair's visibility (crosshair_visible) and handle the
 * show-once behavior.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <linux/input.h>
#include "../include/keys.h"
#include "../include/krosshair.h"

/*
 * UNIT_TEST build: expose the pure hotkey/combo logic so
 * tests/test_input.c can drive it directly. Release builds
 * keep it static.
 */
#ifdef UNIT_TEST
#define INPUT_API
#else
#define INPUT_API static
#endif

/* 1 while the crosshair overlay is visible (toggled by the hotkey). */
volatile int crosshair_visible = 1;

#define KROSSHAIR_MAX_KEYS 8
static int kh_required_keys[KROSSHAIR_MAX_KEYS];
static int kh_required_key_count;
static char kh_hotkey_display[256];

#define KROSSHAIR_MAX_INPUT_DEVS 16
static int kh_input_fds[KROSSHAIR_MAX_INPUT_DEVS];
static int kh_input_fd_count;

/* Minimum hold (ms) before the hotkey fires; a short press toggles, a tap under
 * this duration cancels. */
#define KROSSHAIR_HOTKEY_HOLD_MS 50

/* Default 100 ms select() tick; doubles as the device rescan interval. */
#define KROSSHAIR_RESYNC_TICK_MS 100

static int kh_keys_down;        /* bitmask of required keys currently pressed */
static int kh_combo_active;     /* 1 while the full combo is held */
static int kh_combo_fired;      /* 1 after we toggled for the current hold (debounce) */
static struct timespec kh_combo_down_ts; /* CLOCK_MONOTONIC moment the combo first became fully held */
static pthread_once_t kh_input_once;

/* Returns the bit index (0..count-1) of `code` within the required-key list, or -1. */
INPUT_API int kh_key_index(int code)
{
        for (int i = 0; i < kh_required_key_count; ++i)
                if (kh_required_keys[i] == code)
                        return i;
        return -1;
}

/*
 * Parse the hotkey environment variable into the required-key list.
 *
 * Reads KROSSHAIR_HOTKEY_TOGGLE (default "SHIFT_R+F9"); tokens are split
 * on '+', whitespace-trimmed and resolved via kh_key_from_name(). Unknown
 * and duplicate tokens are skipped; if nothing remains, the default
 * combo is used.
 */
INPUT_API void parse_hotkey(void)
{
        const char* hotkey = getenv("KROSSHAIR_HOTKEY_TOGGLE");
        if (!hotkey || hotkey[0] == '\0')
                hotkey = "SHIFT_R+F9";

        strncpy(kh_hotkey_display, hotkey, sizeof(kh_hotkey_display) - 1);
        kh_hotkey_display[sizeof(kh_hotkey_display) - 1] = '\0';

        char scratch[256];
        strncpy(scratch, hotkey, sizeof(scratch) - 1);
        scratch[sizeof(scratch) - 1] = '\0';

        int count = 0;
        char* token = strtok(scratch, "+");
        while (token) {
                /* trim leading and trailing whitespace */
                while (*token == ' ' || *token == '\t')
                        token++;
                char* end = token + strlen(token);
                while (end > token &&
                              (end[-1] == ' ' || end[-1] == '\t' ||
                                end[-1] == '\r' || end[-1] == '\n'))
                        end--;
                *end = '\0';

                if (*token) {
                        int code = kh_key_from_name(token);
                        if (code < 0) {
                                KROSSHAIR_LOG("[KROSSHAIR] unknown hotkey token '%s', skipping\n", token);
                        } else {
                        /*
                          * Check the list being built (not the global counter, which
                          * only updates at the end of the parse).
                          */
                        int dup = 0;
                        for (int i = 0; i < count; ++i) {
                                if (kh_required_keys[i] == code) {
                                        dup = 1;
                                        break;
                                }
                        }
                        if (!dup && count < KROSSHAIR_MAX_KEYS)
                                kh_required_keys[count++] = code;
                        }
                }
                token = strtok(NULL, "+");
        }
        kh_required_key_count = count;

        if (count == 0) {
                kh_required_keys[0] = KEY_RIGHTSHIFT;
                kh_required_keys[1] = KEY_F9;
                kh_required_key_count = 2;
                KROSSHAIR_LOG("[KROSSHAIR] no valid hotkey tokens, using default SHIFT_R+F9\n");
        }
}

/* Close all open input device file descriptors. */
static void close_devices(void)
{
        for (int i = 0; i < kh_input_fd_count; ++i)
                close(kh_input_fds[i]);
        kh_input_fd_count = 0;
}

/*
 * Open every /dev/input/event* node that answers the evdev version ioctl.
 * Previously opened devices are closed first, so this is safe to call
 * repeatedly (the rescan tick uses it to pick up new/virtual keyboards).
 *
 * Returns the number of devices now open.
 */
static int scan_devices(void)
{
        close_devices();
        int count = 0;
        DIR* dir = opendir("/dev/input");
        if (!dir)
                return 0;
        struct dirent* ent;
        while ((ent = readdir(dir))) {
                if (strncmp(ent->d_name, "event", 5) != 0)
                        continue;
                if (count >= KROSSHAIR_MAX_INPUT_DEVS)
                        break;
                char path[300];
                snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
                int fd = open(path, O_RDONLY | O_NONBLOCK);
                if (fd < 0)
                        continue;
                int version = 0;
                if (ioctl(fd, EVIOCGVERSION, &version) < 0) {
                        close(fd);
                        continue;
                }
                kh_input_fds[count++] = fd;
        }
        closedir(dir);
        kh_input_fd_count = count;
        return count;
}

/* Applies a single EV_KEY event to the required-keys bitmask. */
INPUT_API void kh_apply_event(const struct input_event* ev)
{
        if (ev->type != EV_KEY)
                return;
        int bit = kh_key_index(ev->code);
        if (bit < 0)
                return;
        if (ev->value)
                kh_keys_down |= (1 << bit);
        else
                kh_keys_down &= ~(1 << bit);
}

/*
 * Snap kh_keys_down to the kernel's authoritative per-device key state.
 * Self-heals any drift (a key UP event lost during a rescan/reopen), so a
 * stuck bit can never persist past the next 100 ms tick.
 */
static void kh_resync_state(void)
{
        int mask = 0;
        unsigned char key_state[(KEY_MAX / 8) + 1];
        for (int d = 0; d < kh_input_fd_count; ++d) {
                if (ioctl(kh_input_fds[d], EVIOCGKEY(sizeof key_state), key_state) < 0)
                        continue;
                for (int k = 0; k < kh_required_key_count; ++k) {
                        int code = kh_required_keys[k];
                        if ((key_state[code / 8] >> (code % 8)) & 1)
                                mask |= (1 << k);
                }
        }
        kh_keys_down = mask;
}

/* Milliseconds elapsed between two CLOCK_MONOTONIC timestamps. */
INPUT_API long kh_elapsed_ms(const struct timespec* start, const struct timespec* now)
{
        return (now->tv_sec - start->tv_sec) * 1000 +
                      (now->tv_nsec - start->tv_nsec) / 1000000;
}

/*
 * Compute the select() timeout in ms.
 *
 * Normally the 100 ms resync tick; but while the combo is held and not yet
 * fired, wake exactly at the hold boundary even though held keys emit no
 * new events. No busy-wait.
 */
INPUT_API long kh_select_timeout_ms(void)
{
        if (!kh_combo_active || kh_combo_fired)
                return KROSSHAIR_RESYNC_TICK_MS;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long held_ms = kh_elapsed_ms(&kh_combo_down_ts, &now);
        long remain = KROSSHAIR_HOTKEY_HOLD_MS - held_ms;
        if (remain < 0)
                remain = 0;
        return remain < KROSSHAIR_RESYNC_TICK_MS ? remain : KROSSHAIR_RESYNC_TICK_MS;
}

/*
 * Read all pending events from one input device, applying each to the
 * required-keys bitmask.
 *
 * fd  The device file descriptor.
 *
 * Returns 1 if the device should be removed from the watch list
 * (unreadable or vanished), 0 otherwise.
 */
static int kh_drain_device(int fd)
{
        for (;;) {
                struct input_event ev;
                ssize_t n = read(fd, &ev, sizeof(ev));
                if (n < 0) {
                        if (errno == EAGAIN || errno == EINTR)
                                return 0; /* buffer drained */
                        close(fd);
                        return 1;
                }
                if (n == 0) { /* EOF: device vanished */
                        close(fd);
                        return 1;
                }
                if (n < (ssize_t)sizeof(ev))
                        return 0; /* partial frame; nothing more queued */
                kh_apply_event(&ev);
        }
}

/*
 * Run the combo hold-gate state machine once per loop iteration.
 *
 * combo_bits  Bitmask with a bit per required key, all set.
 *
 * The hotkey fires (toggling crosshair visibility) once the full combo has
 * been held for KROSSHAIR_HOTKEY_HOLD_MS; releasing any key before that
 * cancels the hold. Runs on every iteration (event and tick paths) so the
 * hold is evaluated even while held keys emit no new events.
 */
INPUT_API void kh_update_combo(int combo_bits)
{
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long held_ms = kh_elapsed_ms(&kh_combo_down_ts, &now);
        if ((kh_keys_down & combo_bits) == combo_bits) {
                if (!kh_combo_active) {
                        kh_combo_active = 1;
                        kh_combo_fired = 0;
                        kh_combo_down_ts = now;
                } else if (!kh_combo_fired && held_ms >= KROSSHAIR_HOTKEY_HOLD_MS) {
                        crosshair_visible ^= 1;
                        kh_combo_fired = 1;
                        fprintf(stderr, "[KH] Hotkey %s pressed - crosshair will be %s\n",
                                        kh_hotkey_display, crosshair_visible ? "shown" : "hidden");
                        KROSSHAIR_LOG("[KROSSHAIR] hotkey fired -> crosshair %s\n",
                                                    crosshair_visible ? "ON" : "OFF");
                }
        } else {
                kh_combo_active = 0;
                kh_combo_fired = 0;
        }
}

/*
 * Add all open input devices to a select() set.
 *
 * ready_set  The fd_set to fill.
 *
 * Returns the highest file descriptor, for select's nfds argument.
 */
static int kh_build_fd_set(fd_set* ready_set)
{
        FD_ZERO(ready_set);
        int maxfd = 0;
        for (int i = 0; i < kh_input_fd_count; ++i) {
                FD_SET(kh_input_fds[i], ready_set);
                if (kh_input_fds[i] > maxfd)
                        maxfd = kh_input_fds[i];
        }
        return maxfd;
}

/*
 * Background thread body.
 *
 * Loops on select() over the open evdev devices. On a ready fd it drains
 * and applies events; on a timeout it rescans /dev/input (100 ms tick,
 * catches new / Proton virtual keyboards) and re-syncs the key bitmask
 * from the kernel. After either path it evaluates the combo hold-gate.
 */
static void* input_thread_main(void* arg)
{
        (void)arg;
        int device_count = scan_devices();
        if (device_count == 0) {
                KROSSHAIR_LOG("[KROSSHAIR] no EV_KEY input devices; hotkey disabled\n");
                return NULL;
        }

        fprintf(stderr, "[KH] Krosshair loaded. Hotkey to toggle: '%s' (change via env var 'KROSSHAIR_HOTKEY_TOGGLE').\n", kh_hotkey_display);

        /* bitmask with one bit per required key, all set */
        int combo_bits = (1 << kh_required_key_count) - 1;

        for (;;) {
                fd_set ready_set;
                int maxfd = kh_build_fd_set(&ready_set);

                long timeout_ms = kh_select_timeout_ms();
                struct timeval tv;
                tv.tv_sec = timeout_ms / 1000;
                tv.tv_usec = (timeout_ms % 1000) * 1000;

                int ready = select(maxfd + 1, &ready_set, NULL, NULL, &tv);
                if (ready <= 0) {
                        /* Only rescan on the full 100 ms tick (catches new / Proton
                          * virtual keyboards). The short dynamic hold-timeout just needs
                          * a lightweight state re-check; a full device reopen is slow in
                          * the flatpak sandbox and defeats the 25 ms wake. */
                        if (timeout_ms >= KROSSHAIR_RESYNC_TICK_MS)
                                scan_devices();
                        kh_resync_state(); /* self-heal: snap bitmask to kernel reality */
                } else {
                        for (int i = 0; i < kh_input_fd_count; ++i) {
                                int fd = kh_input_fds[i];
                                if (!FD_ISSET(fd, &ready_set))
                                        continue;
                                if (kh_drain_device(fd)) {
                                        kh_input_fds[i] = kh_input_fds[kh_input_fd_count - 1];
                                        kh_input_fd_count--;
                                        break;
                                }
                        }
                }

                kh_update_combo(combo_bits);
        }
        return NULL;
}

/* One-shot init: parse the hotkey, then spawn the detached input thread. */
static void kh_input_init_once(void)
{
        parse_hotkey();
        pthread_t th;
        if (pthread_create(&th, NULL, input_thread_main, NULL) != 0) {
                KROSSHAIR_LOG("[KROSSHAIR] failed to create input thread; hotkey disabled\n");
                return;
        }
        pthread_detach(th);
}

/*
 * Public entry point; safe to call from multiple threads.
 * Runs kh_input_init_once exactly once.
 */
void init_input_thread(void)
{
                pthread_once(&kh_input_once, kh_input_init_once);
}

#ifdef UNIT_TEST
/* Test hooks for the pure hotkey/combo logic (tests/test_input.c). */

/* Clear the key bitmask and combo state (required-key list untouched). */
void kh_input_test_reset(void)
{
        kh_keys_down = 0;
        kh_combo_active = 0;
        kh_combo_fired = 0;
        kh_combo_down_ts.tv_sec = 0;
        kh_combo_down_ts.tv_nsec = 0;
}

/* Number of required keys set by parse_hotkey. */
int kh_input_test_required_count(void)
{
        return kh_required_key_count;
}

/* Required key at `index`, or -1 if out of range. */
int kh_input_test_required_key(int index)
{
        if (index < 0 || index >= kh_required_key_count)
                return -1;
        return kh_required_keys[index];
}

/* Current required-keys bitmask (one bit per required key). */
int kh_input_test_keys_down(void)
{
        return kh_keys_down;
}

/* Overwrite the required-keys bitmask (synthetic press/release). */
void kh_input_test_set_keys_down(int mask)
{
        kh_keys_down = mask;
}

/* 1 while the full combo is held. */
int kh_input_test_combo_active(void)
{
        return kh_combo_active;
}

/* 1 after the hotkey fired for the current hold. */
int kh_input_test_combo_fired(void)
{
        return kh_combo_fired;
}

/* Set the combo active/fired flags directly. */
void kh_input_test_set_combo(int active, int fired)
{
        kh_combo_active = active;
        kh_combo_fired = fired;
}

/* Overwrite the combo-down timestamp (CLOCK_MONOTONIC). */
void kh_input_test_set_combo_down_ts(const struct timespec* ts)
{
        kh_combo_down_ts = *ts;
}
#endif

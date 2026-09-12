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

static int kh_keys_down;        /* bitmask of required keys currently pressed */
static int kh_combo_active;     /* 1 while the full combo is held */
static int kh_combo_fired;      /* 1 after we toggled for the current hold (debounce) */
static struct timespec kh_combo_down_ts; /* CLOCK_MONOTONIC moment the combo first became fully held */
static pthread_once_t kh_input_once;

/* Returns the bit index (0..count-1) of `code` within the required-key list, or -1. */
static int kh_key_index(int code)
{
    for (int i = 0; i < kh_required_key_count; ++i)
        if (kh_required_keys[i] == code)
            return i;
    return -1;
}

static void parse_hotkey(void)
{
    char buf[256];
    const char* src = getenv("KROSSHAIR_HOTKEY_TOGGLE");
    if (!src || src[0] == '\0')
        src = "SHIFT_R+F9";

    strncpy(kh_hotkey_display, src, sizeof(kh_hotkey_display) - 1);
    kh_hotkey_display[sizeof(kh_hotkey_display) - 1] = '\0';

    strncpy(buf, src, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    int count = 0;
    char* tok = strtok(buf, "+");
    while (tok) {
        while (*tok == ' ' || *tok == '\t')
            tok++;
        char* end = tok + strlen(tok);
        while (end > tok &&
               (end[-1] == ' ' || end[-1] == '\t' ||
                end[-1] == '\r' || end[-1] == '\n'))
            end--;
        *end = '\0';

        if (*tok) {
            int code = kh_key_from_name(tok);
            if (code < 0) {
                KROSSHAIR_LOG("[KROSSHAIR] unknown hotkey token '%s', skipping\n", tok);
            } else {
                int dup = 0;
                for (int i = 0; i < count; ++i)
                    if (kh_required_keys[i] == code)
                        dup = 1;
                if (!dup && count < KROSSHAIR_MAX_KEYS)
                    kh_required_keys[count++] = code;
            }
        }
        tok = strtok(NULL, "+");
    }
    kh_required_key_count = count;

    if (count == 0) {
        kh_required_keys[0] = KEY_RIGHTSHIFT;
        kh_required_keys[1] = KEY_F9;
        kh_required_key_count = 2;
        KROSSHAIR_LOG("[KROSSHAIR] no valid hotkey tokens, using default SHIFT_R+F9\n");
    }
}

static void close_devices(void)
{
    for (int i = 0; i < kh_input_fd_count; ++i)
        close(kh_input_fds[i]);
    kh_input_fd_count = 0;
}

static int scan_devices(void)
{
    close_devices();
    int count = 0;
    DIR* d = opendir("/dev/input");
    if (!d)
        return 0;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (strncmp(ent->d_name, "event", 5) != 0)
            continue;
        if (count >= KROSSHAIR_MAX_INPUT_DEVS)
            break;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        int ver = 0;
        if (ioctl(fd, EVIOCGVERSION, &ver) < 0) {
            close(fd);
            continue;
        }
        kh_input_fds[count++] = fd;
    }
    closedir(d);
    kh_input_fd_count = count;
    return count;
}

/* Applies a single EV_KEY event to the required-keys bitmask. */
static void kh_apply_event(const struct input_event* ev)
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

static void* input_thread_main(void* arg)
{
    (void)arg;
    int opened = scan_devices();
    if (opened == 0) {
        KROSSHAIR_LOG("[KROSSHAIR] no EV_KEY input devices; hotkey disabled\n");
        return NULL;
    }

    struct timespec kh_t0;
    clock_gettime(CLOCK_MONOTONIC, &kh_t0);
    fprintf(stderr, "[KH] Krosshair loaded. Hotkey to toggle: '%s' (change via env var 'KROSSHAIR_HOTKEY_TOGGLE').\n", kh_hotkey_display);

    int need_bits = (1 << kh_required_key_count) - 1;

    for (;;) {
        fd_set set;
        FD_ZERO(&set);
        int maxfd = 0;
        for (int i = 0; i < kh_input_fd_count; ++i) {
            FD_SET(kh_input_fds[i], &set);
            if (kh_input_fds[i] > maxfd)
                maxfd = kh_input_fds[i];
        }

        /* Dynamic timeout: default 100 ms rescan tick. While the combo is held
         * and not yet fired, wake exactly at the hold boundary even though held
         * keys emit no new events. No busy-wait. */
        struct timeval tv;
        long timeout_ms = 100;
        if (kh_combo_active && !kh_combo_fired) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            long held_ms = (t.tv_sec - kh_combo_down_ts.tv_sec) * 1000 +
                           (t.tv_nsec - kh_combo_down_ts.tv_nsec) / 1000000;
            long remain = KROSSHAIR_HOTKEY_HOLD_MS - held_ms;
            if (remain < 0)
                remain = 0;
            timeout_ms = remain < 100 ? remain : 100;
        }
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000; /* 100 ms default: timeout doubles as rescan tick */

        int r = select(maxfd + 1, &set, NULL, NULL, &tv);
        if (r <= 0) {
            /* Only rescan on the full 100 ms tick (catches new / Proton
             * virtual keyboards). The short dynamic hold-timeout just needs
             * a lightweight state re-check; a full device reopen is slow in
             * the flatpak sandbox and defeats the 25 ms wake. */
            if (timeout_ms >= 100)
                scan_devices();
            kh_resync_state(); /* self-heal: snap bitmask to kernel reality */
        } else {
            for (int i = 0; i < kh_input_fd_count; ++i) {
                int fd = kh_input_fds[i];
                if (!FD_ISSET(fd, &set))
                    continue;
                int gone = 0;
                for (;;) {
                    struct input_event ev;
                    ssize_t n = read(fd, &ev, sizeof(ev));
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EINTR)
                            break; /* buffer drained */
                        close(fd);
                        gone = 1;
                        break;
                    }
                    if (n == 0) { /* EOF: device vanished */
                        close(fd);
                        gone = 1;
                        break;
                    }
                    if (n < (ssize_t)sizeof(ev))
                        break; /* partial frame; nothing more queued */
                    kh_apply_event(&ev);
                }
                if (gone) {
                    kh_input_fds[i] = kh_input_fds[kh_input_fd_count - 1];
                    kh_input_fd_count--;
                    break;
                }
            }
        }

        /* Combo hold-gate: run on EVERY iteration (event and tick paths) so the
         * hold is evaluated even while held keys emit no new events. Toggles once
         * after a short hold; releasing before that cancels. */
        {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long held_ms = (now.tv_sec - kh_combo_down_ts.tv_sec) * 1000 +
                           (now.tv_nsec - kh_combo_down_ts.tv_nsec) / 1000000;
            if ((kh_keys_down & need_bits) == need_bits) {
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
    }
    return NULL;
}

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


void init_input_thread(void)
{
	pthread_once(&kh_input_once, kh_input_init_once);
}

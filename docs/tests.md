# Test suite

All tests are located in `tests/`.

Use the following command to run all tests:

```
make test
```

The target builds `lib/krosshair.so` first (`test: all`), then builds
and runs every `tests/test_*.c`, failing on the first failing suite.

## Test framework

We use a lightweight custom harness located at `tests/test.h`
(`CHECK`, `CHECK_EQ`, `CHECK_PTR`, `CHECK_STR`, `TESTS_MAIN`). A suite
exits non-zero if any check failed.

Requirements are the same as the layer build (Vulkan headers), plus
`libvulkan` for the end-to-end binary.

## Unit suites

Every unit suite is compiled against **all layer sources with
`-DUNIT_TEST`** (see the exposure convention below), so the suites can
drive normally-static helpers directly. `tests/test_mock_icd.c` is
excluded from this loop — it is an application linked against `libvulkan`,
not a unit test (see "End-to-end" below).

| Suite | What it covers |
| --- | --- |
| `test_dispatch.c` | the command lookup table in `src/layer.c`: every entry of `name_to_funcptr_map`, and the two `Get*ProcAddr` entry points with NULL handles (must short-circuit on intercepted names before touching any chain) |
| `test_objects.c` | object bookkeeping in `src/objects.c`: `vk_map_set` / `vk_map_get` / `vk_map_delete` and the thread-safe `map_object` / `find_object_data` / `unmap_object` wrappers, including update-in-place and map-full edge cases |
| `test_keys.c` | the hotkey key-name mapping in `include/keys.h` (`kh_key_from_name` is a static inline, so the suite includes and drives the table directly) |
| `test_apng.c` | APNG decoding in `src/apng.c` using inputs synthesized in memory (a minimal PNG writer plus APNG acTL/fcTL/fdAT chunks — no fixture files, no zlib) |
| `test_layer_chain.c` | the layer entry points in `src/layer.c` with the "next" chain stubbed: hand-built `VkLayerInstanceCreateInfo` / `VkLayerDeviceCreateInfo` chains whose `Get*ProcAddr` point at recording stubs |
| `test_crosshair.c` | the device-independent parts of `src/crosshair.c`: push-constant defaults and `.cfg` parsing, quad vertex setup, file helpers (mtime, whole-file read, change detection), image path resolution, animation state setup, and image decoding (static PNG, animated APNG atlas, content sniffing, garbage/missing files) |
| `test_input.c` | the pure hotkey/combo logic in `src/input.c`: `parse_hotkey`, `kh_key_index`, `kh_apply_event`, `kh_elapsed_ms`, `kh_update_combo`, `kh_select_timeout_ms`. The evdev scanning / background-thread code is not exercised |
| `test_gpu.c` | `vk_memory_type()` in `src/gpu.c`: a stub `GetPhysicalDeviceMemoryProperties` vtable returns a synthesized `VkMemoryProperties`, and the suite asserts the selected type index for various requirement/flag combinations |
| `test_render.c` | the overlay render path in `src/render.c`: fence wait/ready handling, swapchain layout transitions, framebuffer-copy recording, crosshair and dynamic-mask upload sequences, quad-buffer creation, and the submit topology (single same-family submit vs two cross-engine submits) — driven against a recording stub vtable with the built-in crosshair (HOME pointed at an empty directory) |
| `test_perflog.c` | the memory tracking in `src/perflog.c`: `KROSSHAIR_PERFLOGGING` env-var parsing (unset/`1`/`0`/other), the device-memory handle→size registry (alloc/free by handle, untracked and NULL handles, double-free), and the host-memory running total |

Adding a new suite requires no Makefile change: the `test_*.c` wildcard
picks it up (linked with `-lm -lpthread`).

## The UNIT_TEST exposure convention

Helpers that unit tests need but that must stay private in release builds
are marked with a per-file API macro:

```c
#ifdef UNIT_TEST
#define LAYER_API
#else
#define LAYER_API static
#endif
```

`LAYER_API` (`src/layer.c`), `CROSSHAIR_API` (`src/crosshair.c`),
`INPUT_API` (`src/input.c`) and `RENDER_API` (`src/render.c`) follow this
pattern; the matching declarations live in `tests/test_layer_api.h`,
`tests/test_input_api.h` and `tests/test_render_api.h` (types must stay in
sync with the sources). `src/gpu.c`'s `vk_memory_type` needs no macro
because it is non-static already.

Release builds keep everything static — no test symbols ship in
`lib/krosshair.so`.

To expose a new helper: mark it with the file's API macro, declare it in
the matching `tests/test_*_api.h`, and add a suite.

## End-to-end: the mock ICD

`tests/mock_icd.c` is a minimal Vulkan ICD loaded by the **real Vulkan
loader** beneath the layer, so the end-to-end angle exercises the full
instance → device → swapchain → acquire → submit → present cycle through
the loader's trampolines and the layer's actual interceptors (the layer
intercepts only ~10 commands).

Mechanics the mock has to get right for modern loaders:

- **New loader-ICD protocol**: the mock directly exports
  `vk_icdNegotiateLoaderICDInterfaceVersion` (negotiating down to the
  `CURRENT_LOADER_ICD_INTERFACE_VERSION` its headers support) and
  `vk_icdGetInstanceProcAddr`; the loader dlsym's the former first and
  resolves everything else through the latter.
- **Handle contract**: every handle is a real heap object whose first
  field is `VK_LOADER_DATA` (`ICD_LOADER_MAGIC`); the loader overwrites
  that slot with its dispatch data, so the objects must stay alive.
- **Counters**: the mock counts the calls it receives and exposes them via
  `mock_icd_get_stats(submits, cmdbufs, presents, swapchains, acquires,
  queue_queries, max_alloc)` (returns 0 on success). The test dlopens the
  same `.so` the loader dlopened (dlopen dedupes by inode) to read the
  shared counters.

Manifests (generated by the Makefile at build time; the loader dlopens
`library_path` **verbatim**, so both carry absolute paths):

- `build/mock_icd/mock_icd.json` — the ICD manifest, selected with
  `VK_DRIVER_FILES`.
- `build/vk_layer_path/krosshair.json` — the layer manifest, selected with
  `VK_LAYER_PATH`. The checked-in `krosshair.json` points at the install
  location, so the test uses this build-tree copy instead (no
  `make install` needed).
- Some packaged loaders (Debian 1.4.309, Arch 1.4.357) enumerate the
  manifest but silently skip layers found via `VK_LAYER_PATH`, so the
  target also copies the layer manifest to
  `~/.config/vulkan/implicit_layer.d/` for the run and removes it
  afterwards.

The test app (`tests/test_mock_icd.c`) asserts on the counters: the app
submits one empty `vkQueueSubmit`, and the layer must add its own overlay
submit on present — `submits`, `cmdbufs`, `presents`, `swapchains`,
`acquires` and `queue_queries` together prove the layer actually rendered
and forwarded. If `/proc/self/maps` shows `krosshair.so` was never mapped
(the loader-skip case above), the end-to-end angle prints a notice and
skips with exit 0 instead of failing.

Two runs:

1. **Default angle** — built-in 50x50 crosshair.
2. **Custom-image angle** (`KROSSHAIR_E2E_CUSTOM_IMG=1`) — the test
   generates a small 3-frame 11x13 APNG in memory (shared builder in
   `tests/apng_fixture.h`), writes it to a temp file with an `.apng`
   extension, and points `KROSSHAIR_IMG` at it. The layer decodes it into
   an 11x(13*3) vertical frame atlas and uploads it; that upload buffer
   becomes the largest single device-memory allocation (the mock reports
   the fixed 1024-byte image requirement, so the buffer stands out),
   which differs from the built-in crosshair's — the mock's max-allocation
   counter therefore identifies which image the layer loaded.

Mock object lifecycle: every handle the mock creates is tracked on a list
and freed on the matching destroy / `vkFreeMemory` call. This loader
generation never invokes the ICD's `vkDestroyDevice`, so device-scoped
objects the layer leaves to implicit destruction are freed by an unload-
time cleanup (`__attribute__((destructor))` in `mock_icd.c`). With that in
place both e2e runs are valgrind-clean (`--leak-check=full`: 0 leaks, 0
errors) — the conclusion of the 2026-09 leak audit, in which every loss
record traced to the mock's own backing, none to the layer.

## Environment variables

| Variable | Set by | Meaning |
| --- | --- | --- |
| `KROSSHAIR=1` | Makefile test target | enables the layer (the manifest's `enable_environment`) |
| `DISABLE_KROSSHAIR=1` | — | disables it (the manifest's `disable_environment`) |
| `VK_DRIVER_FILES` | Makefile test target | pins the mock ICD as the only driver |
| `VK_LAYER_PATH` | Makefile test target | points the loader at the build-tree layer manifest |
| `MOCK_ICD_SO` | Makefile test target | absolute path of `libmock_icd.so`; the test dlopens it to read `mock_icd_get_stats` |
| `MOCK_ICD_DEBUG` | manually | makes the mock ICD print its `[mock-icd] ...` dispatch trace (gpa misses, extension queries, negotiation); hidden by default to keep `make test` output clean |
| `KROSSHAIR_E2E_CUSTOM_IMG` | Makefile (second e2e run) | tells the test to generate and use the custom APNG |
| `KROSSHAIR_IMG` | the test itself | the custom image path handed to the layer |
| `KROSSHAIR_HOTKEY_TOGGLE` | `test_input` | the hotkey under test |
| `KROSSHAIR_PERFLOGGING=1` | manually | makes the layer print `[KH] perf: ...` lines with running device- and host-memory totals (for leak testing, e.g. under valgrind) |

## CI

Both workflows (`.github/workflows/pr.yml` and `release.yml`) run
`make test` as a dedicated "Run tests" step (after the dependency install,
before the binary and Flatpak builds).

## Not covered

- The render path's logic is covered by `test_render.c`'s stub vtable, but
  no unit test runs against a real driver — actual GPU behavior is checked
  only through the mock's call counters; `KROSSHAIR=1 vkcube` remains the
  real-hardware smoke test (see the README).
- Shaders are committed as SPIR-V artifacts with no regeneration target
  and no test (see `AGENTS.md`).

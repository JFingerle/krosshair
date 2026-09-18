# AGENTS.md

krosshair is a **Vulkan implicit layer** (a crosshair overlay for games), not a standalone app.
There is no binary to run — it compiles to a shared library `lib/krosshair.so` that the Vulkan
app loader picks up via `krosshair.json` when `KROSSHAIR=1` is set.

## Build

```
make all       # dev build: lib/krosshair.so (adds -ggdb)
make release   # used by PKGBUILD and CI (no -ggdb)
```

- Language standard is **C99** (`-std=c99`), always compiled with `-Wall`. Keep the build warning-free.
- Requires **Vulkan headers** to be installed (`vulkan-headers` on Arch / `libvulkan-dev` on Debian).
  A bare container will fail to build without them.
- **Tests:** `make test` runs the unit suites plus an end-to-end run of the
  layer through the real Vulkan loader against a mock ICD (see
  `docs/tests.md`). There is no linter/typecheck; a dev build must also stay
  warning-free under `-Wall`.
- Entry points live in `src/layer.c` (`overlay_CreateInstance`, `overlay_CreateDevice`,
  `overlay_GetInstanceProcAddr`, `overlay_GetDeviceProcAddr`). The huge command table in
  `src/dispatch.c` is built with the `DISPATCH_LOAD(table, gpa, scope, NAME)` macro — search for
  a command by its entry name (e.g. `AcquireNextImageKHR`), **not** by `vkAcquireNextImageKHR`.

## Documentation

- Project documentation lives in `docs/` — currently `docs/tests.md`, which
  covers the unit-test suites and the mock-ICD end-to-end setup.

## Shaders (codegen gotcha)

GLSL sources are in `shaders/*.vert` / `shaders/*.frag`. They are compiled to SPIR-V and
committed as artifacts: the `.spv` files in `shaders/` and the byte-array headers
`include/shaders.h` + `shaders/dynamic_spv.h` (both included from `src/gpu.c`).
**There is no Makefile target to regenerate them.** If you edit a `.vert`/`.frag`, you must
recompile GLSL→SPIR-V and regenerate the C byte array manually, or the built layer keeps the
old shader.

## Style (see `.clang-format`)

- **`UseTab: Never`** — 8-space indentation, no tabs. (Tabs slip in and break the build style;
  scan for them after edits.)
- `IndentWidth: 8`, `ColumnLimit: 80`, pointer alignment left (`void*`/`char*`, not `void *`).
- Every function carries a leading `/* ... */` doc comment describing purpose and parameters —
  match this convention when adding functions.

## Conventions

- **Conventional commit messages** with a scope in parens: `fix(gpu): ...`,
  `refactor(layer): ...`, `docs(crosshair): ...`, `chore(input): ...`.
- Runtime env vars (from `krosshair.json` and README): `KROSSHAIR=1` (enable),
  `DISABLE_KROSSHAIR=1` (disable), `KROSSHAIR_IMG` (custom crosshair),
  `KROSSHAIR_HOTKEY_TOGGLE` (e.g. `SHIFT_R+F7`), `KROSSHAIR_PERFLOGGING=1`
  (memory-tracking diagnostics on stderr).
- `KROSSHAIR_LOG(...)` is a **no-op unless `KROSSHAIR_DEBUG` is defined**; `[KH]`/`[KROSSHAIR_ERROR]`
  lines on stderr are the runtime diagnostics users see.

## Packaging (context only)

- `PKGBUILD` + `krosshair.install` = Arch/AUR (builds via `make release`, installs to
  `usr/lib/krosshair/` + the implicit layer json).
- `flatpak/*.yml` builds a Vulkan-layer Flatpak extension via `make flatpak-build` /
  `flatpak-install` (needs `flatpak-builder` + Flathub remote).

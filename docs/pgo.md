# Faster-Godot build-level performance: flags & (later) PGO

## WS1 — standard compiler/linker flags

Behavior-preserving flags evaluated for the desktop builds (the fork already used
several on web/android). Each was gated **empirically**: build green + boot +
behavior unchanged + neutral-or-better on the WS0 benchmark harness, measured
**flag-OFF vs flag-ON at the same config** (`target=editor optimize=speed
debug_symbols=no`, mono on Windows). Binary size is the headline signal; frame-time
deltas on a high-end CPU (RTX 4080 SUPER) sat within harness noise.

Process note that paid off: **isolate flags and measure** — bundling hides
regressions. `/Ob3` looked harmless batched with `/Gy /Gw`; isolated, it showed
+13.3 MB for zero speed.

### Windows (MSVC editor) — measured flag-by-flag

| config | engine `.exe` bytes | Δ vs baseline | decision |
|---|---|---|---|
| baseline (no WS1 flags) | 150,416,896 | — | — |
| `/Gy /Gw` | 150,183,424 | **−233,472 (−0.22 MB)** | **KEEP** — small shrink, frame-neutral |
| `/Gy /Gw /Ob3` | 164,350,464 | +13,933,568 (+13.3 MB) | `/Ob3` **DROPPED** — +13.3 MB, 0 measured speed |
| `/Gy /Gw` + `/OPT:ICF` | 150,183,424 | identical to `/Gy /Gw` | `/OPT:ICF` **DROPPED** — verified-applied but folds nothing (MSVC already folds same-named COMDATs; no LTCG to enable content-folding) |

Frame time: the low-variance corpus scenes (gdscript_logic, heldout_logic,
forward_plus_3d) stayed flat (±1.5%) across every config — no flag bought measurable
speed on this CPU. The high-variance scenes (animation, canvas_2d, physics) swung
from run-to-run noise, not flag effects (canvas_2d moved +19% for `/Gy /Gw` *alone*,
which cannot be a real codegen change — link-granularity flags don't alter code).

**Shipped Windows flag set: `/Gy` + `/Gw`** (`platform/windows/detect.py`), applied
to editor + templates. `/Ob3` and `/OPT:ICF` were evaluated and rejected as
neutral-to-negative on MSVC. (`/OPT:ICF` may matter under WS2's clang/lld via the
separate `-Wl,--icf=safe`; the MSVC `/OPT:ICF` does not.)

### Linux (GCC) — compile/link validated on WSL2-Debian

`platform/linuxbsd/detect.py` gains `-ffunction-sections -fdata-sections` +
`-Wl,--gc-sections` (dead-section elimination), `-fno-semantic-interposition`, and
`-fvisibility=hidden -fvisibility-inlines-hidden`. Validated on **Debian 13 (trixie)
via WSL2**: the `linuxbsd` editor **compiles and links clean** with the full flag set
and launches (`--headless --version` OK; binary `4.6.3.stable.custom_build`). A clean
link is the load-bearing signal that `-fvisibility=hidden` does not break engine
symbol resolution.

Not measured (and why it's acceptable): Linux **runtime perf / binary-size delta**
(WSL2 has no trustworthy Vulkan/perf path, and repeated 9p-mounted Linux rebuilds are
impractical — the Linux build is a one-time compile/link gate, not a per-change step),
and a **live GDExtension load** (GDExtensions use the function-pointer interface and do
not import engine symbols, so hidden visibility on the engine binary is low-risk by
design). Linux ICF (`--icf=safe`) is deferred to WS2 (needs gold/lld).

Caveat to revisit before graduating `-fvisibility=hidden` from GATED (WS2): it is
applied to *all* `linuxbsd` builds, including the fork-unused
`library_type=shared_library` path. If the engine is ever built as a shared library,
its public C API would need explicit `visibility("default")` annotations to stay
exported — so that path needs its own compile check when the flag is validated.

## WS2 — clang + ThinLTO release templates

The release templates now build with **`clang-cl` + `lld-link` + `llvm-lib`** (Windows) and
**clang + lld** (Linux), with **ThinLTO** (`lto=thin`). The editor/dev toolchain stays MSVC/GCC.
This is the pipeline's biggest win: unlike the WS1 flags (neutral on a high-end CPU), clang+ThinLTO
delivers broad, well-beyond-noise speedups — it gives the fork working cross-translation-unit
inlining (MSVC LTO never did) plus the GDScript-VM computed-goto dispatch that only exists under
`__clang__`.

### Build recipe

```
# Shipped release templates: clang + ThinLTO. The EDITOR stays MSVC/GCC.
scons platform=windows  target=template_release use_llvm=yes lto=thin production=yes module_mono_enabled=yes ...
scons platform=linuxbsd target=template_release use_llvm=yes lto=thin linker=lld production=yes ...
```
On Windows the standalone LLVM toolchain must be installed and ahead of any mingw LLVM on PATH
(`lld-link` resolution matters). `lto=auto` maps to *none* for the MSVC-style driver, so ThinLTO
must be requested explicitly with `lto=thin`. `use_llvm` builds carry a `.llvm` suffix and coexist
with the MSVC build.

### Green-build spike (Windows clang-cl)

Bringing the fork up green under clang-cl needed exactly **one** source change:
`modules/raycast/SCsub` excludes Embree's MSVC-only `__SSE4_1__/__SSE4_2__` defines for `use_llvm`
— clang-cl enforces intrinsic target features like real clang, so it takes Embree's SSE2 fallback
paths like the GCC build (real MSVC tolerated the define/flag mismatch). Everything else worked
first try: `lld-link` accepted the MSVC-style LINKFLAGS, the prebuilt Rapier Rust static lib and
the mono glue linked cleanly, and clang-cl accepts the WS1 `/Gy /Gw`. The portable **AVX2 baseline
is preserved** — clang-cl receives the `-m` ISA flags and defines every fork-baseline macro
(`__AVX2__` `__FMA__` `__F16C__` `__BMI2__` `__SSE4_2__` `__AES__` `__PCLMUL__` `__LZCNT__`
`__POPCNT__`), verified with a `/Zs` compile probe.

### Measured: clang+ThinLTO vs MSVC (editor proxy, same config, RTX 4080 SUPER)

Each scene's primary metric median; **negative = clang+ThinLTO faster**:

| scene | metric | MSVC | clang+ThinLTO | delta |
|---|---|---:|---:|---:|
| gdscript_logic (VM) | wall | 1.967 | 1.602 | **−18.6%** |
| heldout_logic (VM) | wall | 1.724 | 1.338 | **−22.4%** |
| forward_plus_3d | cpu_render | 0.341 | 0.300 | **−12.0%** |
| heldout_3d | cpu_render | 0.198 | 0.173 | **−12.6%** |
| canvas_2d | cpu_render | 0.723 | 0.490 | −32.2% |
| particles | cpu_render | 0.056 | 0.045 | −19.6% |
| animation | wall | 0.419 | 0.381 | −9.1% |
| physics | wall | 0.578 | 0.531 | −8.1% |
| resource_loading | wall | 2.932 | 2.721 | −7.2% |
| physics_light | wall | 0.264 | 0.273 | +3.4% (noise) |

**Faster on 9/10 scenes**, and the *low-variance, trustworthy* scenes (gdscript_logic,
heldout_logic, forward_plus_3d, heldout_3d) show **12–22%** — far beyond their ~3–8% run-to-run
noise. The two **GDScript-VM scenes lead** (−18.6% / −22.4%): the **computed-goto unlock**
(clang-cl defines `__clang__`, so `gdscript_vm.cpp:253` takes the computed-goto branch instead of
MSVC's `switch`) compounded with ThinLTO cross-TU inlining.

**Size cost:** the clang+ThinLTO editor is +38.9 MB (150→191 MB) from aggressive inlining; the
production-stripped `template_release` (the shipped artifact) is **77.6 MB** and runs
(`--headless --version` OK). For a perf fork, 12–22% on reliable workloads for a one-time
template-size cost is a clear win.

(Cosmetic follow-up: clang-cl ignores a bare `-ffp-contract=fast`; it uses clang's default
`fp-contract=on`, which already does FMA contraction within expressions — marginal, not changed.)

### Linux (clang + ThinLTO + ICF) — compile/link/boot gate on WSL2

The `linuxbsd` editor built with **clang + lld + ThinLTO** (`use_llvm=yes lto=thin linker=lld`)
**compiles, links, and boots** on Debian 13 via WSL2 (`--headless --version` →
`4.6.3.stable.custom_build.de6169c08`). This also enables **identical-code folding**:
`platform/linuxbsd/detect.py` adds `-Wl,--icf=safe`, guarded to `use_llvm and linker == "lld"` —
the ICF deferred from WS1 (which had neither gold nor lld), now unblocked because WS2's linker *is*
lld. A clean link with `--icf=safe` is the load-bearing signal that folding doesn't merge
address-significant functions.

Not measured, same rationale as WS1: Linux **runtime perf / binary-size delta** (WSL2 has no
trustworthy Vulkan/perf path; the Linux build is a one-time compile/link/boot gate, not a
per-change step). The Windows clang+ThinLTO numbers above carry the perf story; clang codegen on
Linux is the same family of wins.

## WS3 — PGO

To be written when that workstream lands (PGO is clang-only, so it builds on WS2's toolchain). The
4-rung measurement ladder + benchmark corpus live in the local `feat/ws0-benchmark-harness` branch
under `misc/pgo/`.

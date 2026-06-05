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

## WS2 / WS3 — clang+ThinLTO and PGO

To be written when those workstreams land. The 4-rung measurement ladder + benchmark
corpus live in the local `feat/ws0-benchmark-harness` branch under `misc/pgo/`.

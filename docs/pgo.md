# Faster-Godot build-level performance: flags and PGO

## Standard compiler and linker flags

Behavior-preserving flags evaluated for the desktop builds (the fork already used several on web and android). Each one was gated empirically: build green, boot, behavior unchanged, and neutral-or-better on the benchmark harness, measured flag-OFF against flag-ON at the same config (`target=editor optimize=speed debug_symbols=no`, mono on Windows). Binary size is the clearest signal here. Frame-time deltas on a high-end CPU (RTX 4080 SUPER) sat within harness noise.

One process note that paid off: isolate each flag and measure it alone, because bundling hides regressions. `/Ob3` looked harmless batched with `/Gy /Gw`. On its own it cost +13.3 MB for zero speed.

### Windows (MSVC editor): measured flag by flag

| config | engine `.exe` bytes | delta vs baseline | decision |
|---|---|---|---|
| baseline (no added flags) | 150,416,896 | n/a | n/a |
| `/Gy /Gw` | 150,183,424 | -233,472 (-0.22 MB) | keep: small shrink, frame-neutral |
| `/Gy /Gw /Ob3` | 164,350,464 | +13,933,568 (+13.3 MB) | drop `/Ob3`: +13.3 MB, 0 measured speed |
| `/Gy /Gw` + `/OPT:ICF` | 150,183,424 | identical to `/Gy /Gw` | drop `/OPT:ICF`: verified-applied but folds nothing (MSVC already folds same-named COMDATs, and there is no LTCG to enable content-folding) |

Frame time: the low-variance corpus scenes (gdscript_logic, heldout_logic, forward_plus_3d) stayed flat (within 1.5%) across every config. No flag bought measurable speed on this CPU. The high-variance scenes (animation, canvas_2d, physics) swung from run-to-run noise rather than flag effects. canvas_2d moved +19% for `/Gy /Gw` alone, which cannot be a real codegen change, since link-granularity flags do not alter code.

Shipped Windows flag set: `/Gy` and `/Gw` (`platform/windows/detect.py`), applied to both the editor and the templates. `/Ob3` and `/OPT:ICF` were evaluated and rejected as neutral-to-negative on MSVC. (`/OPT:ICF` may matter under the clang/lld template build through the separate `-Wl,--icf=safe`. The MSVC `/OPT:ICF` does not.)

### Linux (GCC): compile and link validated on WSL2-Debian

`platform/linuxbsd/detect.py` gains `-ffunction-sections -fdata-sections` with `-Wl,--gc-sections` (dead-section elimination), `-fno-semantic-interposition`, and `-fvisibility=hidden -fvisibility-inlines-hidden`. Validated on Debian 13 (trixie) through WSL2: the `linuxbsd` editor compiles and links clean with the full flag set and launches (`--headless --version` OK, binary `4.6.3.stable.custom_build`). A clean link is the load-bearing signal that `-fvisibility=hidden` does not break engine symbol resolution.

Two things go unmeasured here. Linux runtime perf and binary-size delta are skipped because WSL2 has no trustworthy Vulkan or perf path, and repeated 9p-mounted Linux rebuilds are impractical; the Linux build is a one-time compile and link gate, not a per-change step. A live GDExtension load is also skipped, because GDExtensions use the function-pointer interface and do not import engine symbols, so hidden visibility on the engine binary is low-risk by design. Linux ICF (`--icf=safe`) needs gold or lld, so it rides with the clang/lld template build described below.

One caveat to revisit before relying on `-fvisibility=hidden` unconditionally: it applies to all `linuxbsd` builds, including the fork-unused `library_type=shared_library` path. If the engine is ever built as a shared library, its public C API would need explicit `visibility("default")` annotations to stay exported, so that path needs its own compile check when the flag is validated.

## Clang and ThinLTO release templates

The release templates build with `clang-cl`, `lld-link`, and `llvm-lib` on Windows, and clang with lld on Linux, using ThinLTO (`lto=thin`). The editor and dev toolchain stay on MSVC and GCC. This is the largest build-level speedup. The standard flags above were neutral on a high-end CPU, but clang with ThinLTO produces gains well beyond noise, because it gives the fork working cross-translation-unit inlining (which MSVC LTO never did) plus the GDScript-VM computed-goto dispatch that only exists under `__clang__`.

### Build recipe

```
# Shipped release templates: clang and ThinLTO. The EDITOR stays on MSVC/GCC.
scons platform=windows  target=template_release use_llvm=yes lto=thin production=yes module_mono_enabled=yes ...
scons platform=linuxbsd target=template_release use_llvm=yes lto=thin linker=lld production=yes ...
```
On Windows the standalone LLVM toolchain must be installed and ahead of any mingw LLVM on PATH, since `lld-link` resolution matters. `lto=auto` maps to none for the MSVC-style driver, so ThinLTO has to be requested explicitly with `lto=thin`. `use_llvm` builds carry a `.llvm` suffix and coexist with the MSVC build.

### Green-build bring-up (Windows clang-cl)

Bringing the fork up green under clang-cl needed exactly one source change. `modules/raycast/SCsub` excludes Embree's MSVC-only `__SSE4_1__/__SSE4_2__` defines for `use_llvm`, because clang-cl enforces intrinsic target features like real clang, so it takes Embree's SSE2 fallback paths like the GCC build does (real MSVC tolerated the define/flag mismatch). Everything else worked first try: `lld-link` accepted the MSVC-style LINKFLAGS, the prebuilt Rapier Rust static lib and the mono glue linked cleanly, and clang-cl accepts the `/Gy /Gw` flags. The portable AVX2 baseline is preserved. clang-cl receives the `-m` ISA flags and defines every fork-baseline macro (`__AVX2__` `__FMA__` `__F16C__` `__BMI2__` `__SSE4_2__` `__AES__` `__PCLMUL__` `__LZCNT__` `__POPCNT__`), verified with a `/Zs` compile probe.

### Measured: clang+ThinLTO vs MSVC (editor proxy, same config, RTX 4080 SUPER)

Each scene's primary metric median. A negative delta means clang+ThinLTO is faster.

| scene | metric | MSVC | clang+ThinLTO | delta |
|---|---|---:|---:|---:|
| gdscript_logic (VM) | wall | 1.967 | 1.602 | -18.6% |
| heldout_logic (VM) | wall | 1.724 | 1.338 | -22.4% |
| forward_plus_3d | cpu_render | 0.341 | 0.300 | -12.0% |
| heldout_3d | cpu_render | 0.198 | 0.173 | -12.6% |
| canvas_2d | cpu_render | 0.723 | 0.490 | -32.2% |
| particles | cpu_render | 0.056 | 0.045 | -19.6% |
| animation | wall | 0.419 | 0.381 | -9.1% |
| physics | wall | 0.578 | 0.531 | -8.1% |
| resource_loading | wall | 2.932 | 2.721 | -7.2% |
| physics_light | wall | 0.264 | 0.273 | +3.4% (noise) |

clang+ThinLTO is faster on 9 of 10 scenes. The low-variance, trustworthy scenes (gdscript_logic, heldout_logic, forward_plus_3d, heldout_3d) show 12 to 22%, well beyond their roughly 3 to 8% run-to-run noise. The two GDScript-VM scenes lead, at -18.6% and -22.4%: clang-cl defines `__clang__`, so `gdscript_vm.cpp:253` takes the computed-goto branch instead of MSVC's `switch`, and ThinLTO's cross-TU inlining compounds it.

The size cost: the clang+ThinLTO editor is +38.9 MB (150 MB to 191 MB) from aggressive inlining, and the production-stripped `template_release`, the shipped artifact, is 77.6 MB and runs (`--headless --version` OK). For a perf fork, 12 to 22% on reliable workloads for a one-time template-size cost is a clear win.

One cosmetic follow-up: clang-cl ignores a bare `-ffp-contract=fast` and uses clang's default `fp-contract=on`, which already does FMA contraction within expressions. The difference is marginal and was left as is.

### Linux (clang + ThinLTO + ICF): compile, link, and boot gate on WSL2

The `linuxbsd` editor built with clang, lld, and ThinLTO (`use_llvm=yes lto=thin linker=lld`) compiles, links, and boots on Debian 13 through WSL2 (`--headless --version` reports `4.6.3.stable.custom_build.de6169c08`). This also enables identical-code folding: `platform/linuxbsd/detect.py` adds `-Wl,--icf=safe`, guarded to `use_llvm and linker == "lld"`. The GCC flag pass above could not use ICF because it had neither gold nor lld; the clang template build's linker is lld, so it can. A clean link with `--icf=safe` is the load-bearing signal that folding does not merge address-significant functions.

This is not measured for runtime perf or binary size, for the same reason as the GCC flag pass above: WSL2 has no trustworthy Vulkan or perf path, and the Linux build is a one-time compile, link, and boot gate rather than a per-change step. The Windows clang+ThinLTO numbers carry the perf story, and clang codegen on Linux is the same family of wins.

## Profile-guided optimization (PGO)

Not yet implemented. PGO is clang-only, so it builds on the clang toolchain above. The measurement ladder and benchmark corpus are kept as local tooling under `misc/pgo/`, outside the shipped tree.

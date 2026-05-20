# x86_64 AVX2/FMA/F16C/POPCNT Baseline

## Goal

Treat modern desktop x86_64 CPUs with AVX2, FMA, F16C, and POPCNT as the
baseline contract, then let the compiler and bundled third-party libraries
optimize around that contract. BMI2/LZCNT/TZCNT remain conditional unless a
library already provides safe runtime dispatch.

## Code Changes

- `SConstruct`
  - Adds the default-on `faster_godot` build option.
  - Requires `platform=windows` or `platform=linuxbsd`.
  - Forces `arch=x86_64` when `arch=auto`.
  - Rejects non-`x86_64` builds.
  - Adds `-mavx2 -mfma -mf16c -mpopcnt -ffp-contract=fast` for
    GCC/Clang/MinGW.
  - Adds `/arch:AVX2 /fp:fast` for MSVC.
  - Adds the `.faster_godot` binary suffix.
- `platform/windows/cpu_feature_validation.c`
  - Checks SSE4.2, POPCNT, AVX, AVX2, FMA, F16C, OSXSAVE, and XCR0 AVX state
    before entering the main executable.
- `platform/windows/SCsub`
  - Compiles the CPU-validation shim without AVX2/FMA/F16C/POPCNT flags so the
    validator can run safely before the main binary uses those instructions.
- `platform/linuxbsd/godot_linuxbsd.cpp`
  - Adds Linux startup validation for SSE4.2, POPCNT, AVX, AVX2, FMA, F16C,
    OSXSAVE, and XCR0 AVX state.
- `platform/linuxbsd/SCsub`
  - Compiles the Linux startup validation translation unit without
    AVX2/FMA/F16C/POPCNT flags.
- `core/SCsub`
  - Explicitly enables zstd's `DYNAMIC_BMI2` path for Faster-Godot on x86_64
    when the compiler supports target attributes. zstd checks CPUID once per
    context and calls BMI2/LZCNT/TZCNT entry points only on CPUs that support
    them.
- `modules/astcenc/SCsub`
  - Defines ASTC encoder AVX2, F16C, and POPCNT feature knobs for MSVC Faster
    builds, matching the feature macros GCC/Clang receive from the command
    line. This mainly speeds editor/import ASTC work.
- `modules/webp/SCsub`
  - Defines libwebp's MSVC SSE4.1 and AVX2 feature knobs for Faster builds so
    the bundled lossless WebP AVX2/SSE4.1 DSP code is compiled and can dispatch
    at runtime.
- `thirdparty/etcpak/ProcessRGB.cpp`
  - Replaces `_tzcnt_u32` use in the AVX2 path with a first-set-bit helper that
    does not imply BMI.
- `thirdparty/jolt_physics/Jolt/Core/Core.h`
  - Prevents AVX2 alone from implying F16C, LZCNT, or TZCNT for GCC/Clang.
- `modules/raycast/SCsub`
  - Prevents the global AVX2/FMA/F16C/POPCNT flags from leaking into Embree's
    base dispatch objects, which need their own ISA rules.

## Pros

- Gives the compiler a stronger vectorization and instruction scheduling target.
- Allows FMA contraction in hot numeric code.
- Enables F16C/POPCNT-controlled fast paths in bundled libraries without adding
  per-call feature checks.
- Lets zstd use BMI2-specialized entropy paths conditionally on CPUs that expose
  BMI2, which can improve compressed asset and PCK load/decode paths.
- Makes unsupported CPUs fail with a clear startup message instead of undefined
  instruction crashes.
- Aligns the runtime CPU guard with the optimized third-party code that is
  actually linked into the binary.

## Cons

- Older x86_64 CPUs cannot run Faster-Godot binaries.
- Floating-point behavior may differ from official Godot because fast FP and FMA
  contraction are enabled.
- F16C and POPCNT are part of the Faster-Godot runtime contract. BMI2 is not a
  global contract; only runtime-dispatched library paths use it.
- This profile is not intended for deterministic cross-platform simulation.

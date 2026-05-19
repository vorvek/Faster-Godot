# x86_64 AVX2/FMA Baseline

## Goal

Treat modern desktop x86_64 CPUs with AVX2 and FMA as the baseline contract,
then let the compiler optimize the engine around that contract.

## Code Changes

- `SConstruct`
  - Adds the default-on `faster_godot` build option.
  - Requires `platform=windows` or `platform=linuxbsd`.
  - Forces `arch=x86_64` when `arch=auto`.
  - Rejects non-`x86_64` builds.
  - Adds `-mavx2 -mfma -ffp-contract=fast` for GCC/Clang/MinGW.
  - Adds `/arch:AVX2 /fp:fast` for MSVC.
  - Adds the `.faster_godot` binary suffix.
- `platform/windows/cpu_feature_validation.c`
  - Checks SSE4.2, AVX, AVX2, FMA, OSXSAVE, and XCR0 AVX state before entering
    the main executable.
- `platform/windows/SCsub`
  - Compiles the CPU-validation shim without AVX2/FMA flags so the validator can
    run safely before the main binary uses those instructions.
- `platform/linuxbsd/godot_linuxbsd.cpp`
  - Adds Linux startup validation for SSE4.2, AVX, AVX2, FMA, OSXSAVE, and XCR0
    AVX state.
- `platform/linuxbsd/SCsub`
  - Compiles the Linux startup validation translation unit without AVX2/FMA
    flags.
- `thirdparty/etcpak/ProcessRGB.cpp`
  - Replaces `_tzcnt_u32` use in the AVX2 path with a first-set-bit helper that
    does not imply BMI.
- `thirdparty/jolt_physics/Jolt/Core/Core.h`
  - Prevents AVX2 alone from implying F16C, LZCNT, or TZCNT for GCC/Clang.
- `modules/raycast/SCsub`
  - Prevents the global AVX2/FMA flags from leaking into Embree's base dispatch
    objects, which need their own ISA rules.

## Pros

- Gives the compiler a stronger vectorization and instruction scheduling target.
- Allows FMA contraction in hot numeric code.
- Makes unsupported CPUs fail with a clear startup message instead of undefined
  instruction crashes.
- Aligns the runtime CPU guard with the optimized third-party code that is
  actually linked into the binary.

## Cons

- Older x86_64 CPUs cannot run Faster-Godot binaries.
- Floating-point behavior may differ from official Godot because fast FP and FMA
  contraction are enabled.
- This profile is not intended for deterministic cross-platform simulation.

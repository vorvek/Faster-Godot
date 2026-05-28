# x86_64 AVX2/FMA3 Desktop Baseline

## Goal

Treat modern desktop x86_64 CPUs with SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C,
AES, PCLMUL, BMI1, BMI2, and LZCNT as the baseline contract, then let the
compiler and bundled third-party libraries optimize around that contract.

`-mfma` is the GCC/Clang flag for the common three-operand FMA3 instruction
set. Faster-Godot does not require AMD's separate FMA4 or XOP extensions.

## Code Changes

- `SConstruct`
  - Always builds the Faster-Godot profile; the legacy `faster_godot` argument
    is ignored if passed.
  - Requires `platform=windows` or `platform=linuxbsd`.
  - Forces `arch=x86_64` when `arch=auto`.
  - Rejects non-`x86_64` builds.
  - Rejects non-`single` precision builds.
  - Adds `-mavx2 -mfma -mf16c -mpopcnt -msse4.2 -mpclmul -maes -mbmi -mbmi2
    -mlzcnt -ffp-contract=fast` for GCC/Clang/MinGW.
  - Adds `/arch:AVX2 /fp:fast` for MSVC.
  - Adds the `.faster_godot` binary suffix.
- `core/math/simd_defs.h`
  - Centralizes the core math SIMD contract and makes missing GCC/Clang
    AVX2/FMA3/F16C/POPCNT/SSE4.2/AES/PCLMUL/BMI1/BMI2/LZCNT flags a
    compile-time error.
- `core/math`
  - Uses AVX2/FMA3/F16C directly in selected BVH, AABB convex-plane,
    Transform3D vector-array, half-float decode, and EDF/SDF loops.
- `platform/windows/cpu_feature_validation.c`
  - Checks SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C, AES, PCLMUL, BMI1, BMI2,
    LZCNT, OSXSAVE, and XCR0 AVX state before entering the main executable.
- `platform/windows/SCsub`
  - Compiles the CPU-validation shim without the Faster-Godot x86 ISA flags so
    the validator can run safely before the main binary uses those
    instructions.
- `platform/linuxbsd/godot_linuxbsd.cpp`
  - Adds Linux startup validation for SSE4.2, POPCNT, AVX, AVX2, FMA3, F16C,
    AES, PCLMUL, BMI1, BMI2, LZCNT, OSXSAVE, and XCR0 AVX state.
- `platform/linuxbsd/SCsub`
  - Compiles the Linux startup validation translation unit without
    the Faster-Godot x86 ISA flags.
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
  - Prevents the global Faster-Godot x86 ISA flags from leaking into Embree's
    base dispatch objects, which need their own ISA rules.

## Research Notes

- GCC documents `-mavx2` as enabling MMX, SSE, SSE2, SSE3, SSSE3, SSE4.1,
  SSE4.2, AVX, and AVX2 code generation. Faster-Godot still passes
  `-msse4.2` explicitly so the baseline is visible in SCons and in feature
  macro validation.
- GCC/Clang expose AES-NI and carry-less multiply separately as `-maes` and
  `-mpclmul`; Mbed TLS uses the intrinsic AES/GCM path for GCC-like compilers
  only when both `__AES__` and `__PCLMUL__` are available.
- MSVC's x64 `/arch:AVX2` enables AVX2 and FMA3 code generation and can also
  enable associated non-vector instructions such as BMI. MSVC does not expose
  the same complete GNU-style feature macro set, so selected bundled libraries
  still receive their own MSVC feature defines where needed.
- SSE4A, FMA4, and XOP are AMD-specific/deprecated side branches rather than a
  common modern desktop baseline, so they remain out of the required contract.
- RDRAND, RDSEED, MOVBE, FSGSBASE, and prefetch/cache-control extensions are
  not SIMD hot-path primitives for Godot's current codebase, so they are not
  required globally.

## Pros

- Gives the compiler a stronger vectorization and instruction scheduling target.
- Allows FMA contraction in hot numeric code.
- Enables F16C/POPCNT/AES/PCLMUL-controlled fast paths in bundled libraries
  without adding per-call feature checks.
- Lets zstd use BMI2-specialized entropy paths by default, which can improve
  compressed asset and PCK load/decode paths.
- Makes unsupported CPUs fail with a clear startup message instead of undefined
  instruction crashes.
- Aligns the runtime CPU guard with the optimized third-party code that is
  actually linked into the binary.

## Cons

- Older x86_64 CPUs cannot run Faster-Godot binaries.
- Floating-point behavior may differ from official Godot because fast FP and FMA
  contraction are enabled.
- F16C, POPCNT, AES, PCLMUL, BMI1, BMI2, and LZCNT are part of the Faster-Godot
  runtime contract.
- This profile is not intended for deterministic cross-platform simulation.

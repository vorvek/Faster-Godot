# Rapier 2D Source Build

- Upstream: https://github.com/appsinacup/godot-rapier-physics
- Version: v0.8.32 (3f3d8e4f39495182c0bf1650538dfdee7f9e5b6c, 2026)
- Build profile: Rust nightly-2025-12-12 with features `simd-nightly,serde-serialize,parallel,experimental-threads,register-docs,single-dim2,api-custom-json`

This directory is a trimmed copy of the upstream Rust source needed to build
the 2D static library. SCons builds the static library into
`bin/rapier_2d_static` and links it into the engine.

`extension_api.faster_godot_4_6_3.json` is generated from the reduced
Faster-Godot API and is passed to godot-rust through `api-custom-json`, so the
Rust bindings do not expect classes or methods removed from this fork.

The build is intentionally offline:

- `vendor/` contains the crates.io dependencies from `Cargo.lock`.
- `vendor_git/rapier` contains `dimforge/rapier` commit
  `a14c947143e5f597ac4c040e26015766bd9befb9`.
- `vendor_git/salva` contains `ughuuu/salva` commit
  `b30c622534ac6c0c1010f88192b8834e95262e69`.

Vendored godot-rust has local compatibility patches for this fork's reduced
Godot 4.6 extension API:

- Use pre-generated GDExtension Rust bindings instead of build-time libclang.
- Remove an obsolete `FileAccess.create_temp` enum special case.
- Accept namespaced enum constants emitted by this fork's API JSON.
- Skip generated rendering methods whose raytracing-only parameter types are
  absent from the reduced API.

`RUST_CRATE_LICENSES.md` records the full `Cargo.lock` license inventory.
`RELEASE_NOTICES.md` lists the notice and source materials that must accompany
binary releases using this built-in module.

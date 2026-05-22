# Rapier 2D Module

This module embeds the Godot-Rapier 2D GDExtension as a built-in Faster-Godot
module and links the static library built from `thirdparty/rapier_2d`.

The module intentionally does not download artifacts during SCons. The Rust
source, crates.io dependencies, and pinned Rapier/Salva git dependencies are
vendored under `thirdparty/rapier_2d`, and Cargo is run with `--offline`.

When bumping Godot-Rapier, update the source tree, `Cargo.lock`, `vendor/`,
`vendor_git/`, `extension_api.faster_godot_4_6_3.json`, and
`RUST_CRATE_LICENSES.md` together.

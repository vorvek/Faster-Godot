# Rapier 2D Release Notice Checklist

This directory vendors the source used to build the built-in Rapier 2D static
library. Binary releases that include `module_rapier_2d_enabled=yes` must ship
or otherwise provide the following notice and source materials:

- Godot/Faster-Godot `COPYRIGHT.txt`.
- `thirdparty/rapier_2d/LICENSE` for appsinacup/godot-rapier-physics.
- `thirdparty/rapier_2d/THIRDPARTY.txt` from appsinacup/godot-rapier-physics.
- `thirdparty/rapier_2d/RUST_CRATE_LICENSES.md`, generated from the full
  `Cargo.lock`.
- The vendored MPL-2.0 godot-rust crate sources under
  `thirdparty/rapier_2d/vendor/godot*` and their license metadata, or an
  equivalent source offer that satisfies MPL-2.0 for those covered files.
- Apache-2.0 sources and license metadata for Rapier and Salva under
  `thirdparty/rapier_2d/vendor_git/rapier` and
  `thirdparty/rapier_2d/vendor_git/salva`.
- Vendored crates.io source and license files under
  `thirdparty/rapier_2d/vendor`.

The SCons build uses Cargo with `--locked --offline` and the feature set listed
in `README.md`. If that feature set or lockfile changes, regenerate
`RUST_CRATE_LICENSES.md` and re-check this file before publishing binaries.

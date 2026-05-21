# SceneTree Hot-Path Tuning

## Intent

Reduce per-node dispatch overhead during normal scene processing and group
calls while preserving the existing mutation safety when nodes are removed
during those traversals.

## Changes

- `scene/main/scene_tree.{h,cpp}`
  - Adds a dirty flag for `nodes_removed_on_group_call` so process and group
    loops skip the removed-node hash lookup until a node is actually removed
    while a traversal lock is active.
  - Keeps the existing removed-node set and clears both the set and dirty flag
    when the outermost traversal lock exits.

## Pros

- Avoids one `HashSet::has()` probe per processed or group-called node in the
  common frame where no node is removed during traversal.
- Keeps the previous skip behavior for nodes removed by notifications, group
  calls, or process callbacks during traversal.

## Validation

- Windows editor dev build.
- RTGI runtime smoke suite.
- Static adversarial review of process traversal, group calls, nested traversal
  locks, and removal semantics.

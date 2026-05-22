// This file contains compatibility methods that are safe to keep in builds where
// native deprecated APIs are disabled. They only forward to current generated APIs.

using System;
using System.ComponentModel;

namespace Godot;

partial class Node3D
{
    /// <inheritdoc cref="LookAt(Vector3, Nullable{Vector3}, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void LookAt(Vector3 target, Nullable<Vector3> up)
    {
        LookAt(target, up, useModelFront: false);
    }

    /// <inheritdoc cref="LookAtFromPosition(Vector3, Vector3, Nullable{Vector3}, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void LookAtFromPosition(Vector3 position, Vector3 target, Nullable<Vector3> up)
    {
        LookAtFromPosition(position, target, up, useModelFront: false);
    }
}

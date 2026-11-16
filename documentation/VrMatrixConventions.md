# VR matrix conventions

How the headset's matrices map onto the runtime's camera. The code is in `src/dxvk/rtx_render/rtx_vr_math.h`.

## Matrix layout

`Matrix4` holds four columns and multiplies column vectors. For an affine transform the translation is column 3,
and `A * B` applies `B` first. `OpenVrMatrix4` stores the same math as a row-major array.
`matrixFromRowMajor()` copies `rows[row * 4 + column]` to `matrix[column][row]`. It does not transpose the math.

## Handedness

OpenVR is right-handed with +X right, +Y up and -Z forward, in metres.
`flipHandedness()` mirrors the Z axis (`S * M * S` with `S = diag(1, 1, -1, 1)`) to express a transform in a
left-handed basis. The camera accepts both handedness through `isLHS`, so a conversion is needed only where a
headset transform is combined with a game transform of the other handedness.

## Projection

`IVRSystem::GetProjectionRaw` returns tangents. The vertical ones are reported with the Y axis flipped, so top is
negative and bottom is positive. `projectionFromRawBounds()` maps the left, right, top and bottom planes to
normalized device coordinates of -1 and +1, the near plane to depth 0 and the far plane to depth 1.

The sign of the vertical tangents comes from the Valve header's `ComposeProjection` code and from other
implementations, not from a measurement. If the picture in the headset is mirrored vertically or its vertical
centre is off, this is the first place to check.

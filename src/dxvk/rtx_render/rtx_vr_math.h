#pragma once

#include <array>
#include <cstddef>

#include "../../util/util_matrix.h"

// Matrix helpers for feeding OpenVR poses and projections into the RTX camera.
//
// Matrix4 stores four columns and multiplies column vectors, so (A * B) * v applies B first and the
// translation of an affine transform lives in column 3. See documentation/VrMatrixConventions.md.

namespace dxvk {

  // Converts a row-major 4x4 array, such as OpenVrMatrix4::elements, to a Matrix4.
  inline Matrix4 matrixFromRowMajor(const std::array<float, 16>& rows) {
    Matrix4 result;
    for (std::size_t column = 0; column < 4; column++) {
      for (std::size_t row = 0; row < 4; row++) {
        result[column][row] = rows[row * 4 + column];
      }
    }
    return result;
  }

  // Converts between a right-handed and a left-handed basis by mirroring the Z axis.
  // Applying it twice returns the original matrix.
  inline Matrix4 flipHandedness(const Matrix4& matrix) {
    Matrix4 result = matrix;
    for (std::size_t column = 0; column < 4; column++) {
      for (std::size_t row = 0; row < 4; row++) {
        const bool mirroredRow = (row == 2);
        const bool mirroredColumn = (column == 2);
        if (mirroredRow != mirroredColumn) {
          result[column][row] = -matrix[column][row];
        }
      }
    }
    return result;
  }

  // Builds a projection from the tangents returned by IVRSystem::GetProjectionRaw.
  // OpenVR reports the vertical tangents with the Y axis flipped, so top is negative and bottom is positive.
  // Depth maps the near plane to 0 and the far plane to 1. Clip Y points up.
  // The camera looks along +Z when isLHS is set and along -Z otherwise.
  inline Matrix4 projectionFromRawBounds(
    float left, float right, float top, float bottom,
    float nearPlane, float farPlane, bool isLHS) {
    const float forwardSign = isLHS ? 1.0f : -1.0f;
    const float upperTangent = -top;
    const float lowerTangent = -bottom;
    const float width = right - left;
    const float height = upperTangent - lowerTangent;
    const float depth = farPlane - nearPlane;

    Matrix4 result;
    result[0] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    result[1] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    result[2] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    result[3] = Vector4(0.0f, 0.0f, 0.0f, 0.0f);
    result[0][0] = 2.0f / width;
    result[1][1] = 2.0f / height;
    result[2][0] = -forwardSign * (right + left) / width;
    result[2][1] = -forwardSign * (upperTangent + lowerTangent) / height;
    result[2][2] = forwardSign * farPlane / depth;
    result[2][3] = forwardSign;
    result[3][2] = -nearPlane * farPlane / depth;
    return result;
  }

}

// Copyright (C) 2019  Joseph Artsimovich <joseph.artsimovich@gmail.com>, 4lex4 <4lex49@zoho.com>
// Use of this source code is governed by the GNU GPLv3 license that can be found in the LICENSE file.

#define _ISOC99SOURCE  // For std::copysign()

#include "SavGolKernel.h"

#include <QPoint>
#include <QSize>
#include <cassert>
#include <cmath>
#include <stdexcept>


namespace imageproc {
namespace {
int calcNumTerms(const int horDegree, const int vertDegree) {
  return (horDegree + 1) * (vertDegree + 1);
}
}  // anonymous namespace

SavGolKernel::SavGolKernel(const QSize& size, const QPoint& origin, const int horDegree, const int vertDegree)
    : m_horDegree(horDegree),
      m_vertDegree(vertDegree),
      m_width(size.width()),
      m_height(size.height()),
      m_numTerms(calcNumTerms(horDegree, vertDegree)),
      m_numDataPoints(size.width() * size.height()) {
  if (size.isEmpty()) {
    throw std::invalid_argument("SavGolKernel: invalid size");
  }
  if (horDegree < 0) {
    throw std::invalid_argument("SavGolKernel: invalid horDegree");
  }
  if (vertDegree < 0) {
    throw std::invalid_argument("SavGolKernel: invalid vertDegree");
  }
  if (m_numTerms > m_numDataPoints) {
    throw std::invalid_argument("SavGolKernel: too high degree for this amount of data");
  }

  // Allocate memory.
  m_dataPoints.resize(m_numDataPoints, 0.0);
  m_coeffs.resize(m_numTerms);
  AlignedArray<float, 4>(m_numDataPoints).swap(m_kernel);
  // Prepare equations.
  m_equations.reserve(m_numTerms * m_numDataPoints);
  for (int y = 1; y <= m_height; ++y) {
    for (int x = 1; x <= m_width; ++x) {
      double pow1 = 1.0;
      for (int i = 0; i <= m_vertDegree; ++i) {
        double pow2 = pow1;
        for (int j = 0; j <= m_horDegree; ++j) {
          m_equations.push_back(pow2);
          pow2 *= x;
        }
        pow1 *= y;
      }
    }
  }

  QR();
  recalcForOrigin(origin);
}

/**
 * Perform a QR factorization of m_equations by Givens rotations.
 * We store R in place of m_equations, and we don't store Q anywhere,
 * but we do store the rotations in the order they were performed.
 */
void SavGolKernel::QR() {
  m_rotations.clear();
  m_rotations.reserve(m_numTerms * (m_numTerms - 1) / 2 + (m_numDataPoints - m_numTerms) * m_numTerms);

  int jj = 0;  // j * m_numTerms + j
  for (int j = 0; j < m_numTerms; ++j, jj += m_numTerms + 1) {
    int ij = jj + m_numTerms;  // i * m_numTerms + j
    for (int i = j + 1; i < m_numDataPoints; ++i, ij += m_numTerms) {
      const double a = m_equations[jj];
      const double b = m_equations[ij];

      if (b == 0.0) {
        m_rotations.emplace_back(1.0, 0.0);
        continue;
      }

      double sin, cos;

      if (a == 0.0) {
        cos = 0.0;
        sin = std::copysign(1.0, b);
        m_equations[jj] = std::fabs(b);
      } else if (std::fabs(b) > std::fabs(a)) {
        const double t = a / b;
        const double u = std::copysign(std::sqrt(1.0 + t * t), b);
        sin = 1.0 / u;
        cos = sin * t;
        m_equations[jj] = b * u;
      } else {
        const double t = b / a;
        const double u = std::copysign(std::sqrt(1.0 + t * t), a);
        cos = 1.0 / u;
        sin = cos * t;
        m_equations[jj] = a * u;
      }
      m_equations[ij] = 0.0;

      m_rotations.emplace_back(sin, cos);

      int ik = ij + 1;  // i * m_numTerms + k
      int jk = jj + 1;  // j * m_numTerms + k
      for (int k = j + 1; k < m_numTerms; ++k, ++ik, ++jk) {
        const double temp = cos * m_equations[jk] + sin * m_equations[ik];
        m_equations[ik] = cos * m_equations[ik] - sin * m_equations[jk];
        m_equations[jk] = temp;
      }
    }
  }
}  // SavGolKernel::QR

void SavGolKernel::recalcForOrigin(const QPoint& origin) {
  std::fill(m_dataPoints.begin(), m_dataPoints.end(), 0.0);
  m_dataPoints[origin.y() * m_width + origin.x()] = 1.0;

  // Rotate data points.
  double* const dp = &m_dataPoints[0];
  std::vector<Rotation>::const_iterator rot(m_rotations.begin());
  for (int j = 0; j < m_numTerms; ++j) {
    for (int i = j + 1; i < m_numDataPoints; ++i, ++rot) {
      const double temp = rot->cos * dp[j] + rot->sin * dp[i];
      dp[i] = rot->cos * dp[i] - rot->sin * dp[j];
      dp[j] = temp;
    }
  }
  // Solve R*x = d by back-substitution.
  int ii = m_numTerms * m_numTerms - 1;  // i * m_numTerms + i
  for (int i = m_numTerms - 1; i >= 0; --i, ii -= m_numTerms + 1) {
    double sum = dp[i];
    int ik = ii + 1;
    for (int k = i + 1; k < m_numTerms; ++k, ++ik) {
      sum -= m_equations[ik] * m_coeffs[k];
    }

    assert(m_equations[ii] != 0.0);
    m_coeffs[i] = sum / m_equations[ii];
  }

  int ki = 0;
  for (int y = 1; y <= m_height; ++y) {
    for (int x = 1; x <= m_width; ++x) {
      double sum = 0.0;
      double pow1 = 1.0;
      int ci = 0;
      for (int i = 0; i <= m_vertDegree; ++i) {
        double pow2 = pow1;
        for (int j = 0; j <= m_horDegree; ++j) {
          sum += pow2 * m_coeffs[ci];
          ++ci;
          pow2 *= x;
        }
        pow1 *= y;
      }
      m_kernel[ki] = (float) sum;
      ++ki;
    }
  }
}  // SavGolKernel::recalcForOrigin
}  // namespace imageproc
/******************************************************************************
 *
 *  Copyright 2006-2015 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This file contains simple pairing algorithms using Elliptic Curve
 *Cryptography for private public key
 *
 ******************************************************************************/

#pragma once

#include <cstdbool>

#include "p_256_multprecision.h"

typedef struct {
  uint32_t x[KEY_LENGTH_DWORDS_P256];
  uint32_t y[KEY_LENGTH_DWORDS_P256];
  uint32_t z[KEY_LENGTH_DWORDS_P256];
} Point;

typedef struct {
  // curve's coefficients
  uint32_t a[KEY_LENGTH_DWORDS_P256];
  uint32_t b[KEY_LENGTH_DWORDS_P256];

  // whether a is -3
  int a_minus3;

  // prime modulus
  uint32_t p[KEY_LENGTH_DWORDS_P256];

  // Omega, p = 2^m -omega
  uint32_t omega[KEY_LENGTH_DWORDS_P256];

  // base point, a point on E of order r
  Point G;
} elliptic_curve_t;

extern elliptic_curve_t curve;
extern elliptic_curve_t curve_p256;

bool ECC_ValidatePoint(const Point& p);

void ECC_PointMult_Bin_NAF(Point* q, Point* p, uint32_t* n);

#define ECC_PointMult(q, p, n) ECC_PointMult_Bin_NAF(q, p, n)

void p_256_init_curve();

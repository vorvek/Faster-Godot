/**************************************************************************/
/*  simd_defs.h                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

#include "core/math/math_defs.h"

#ifdef REAL_T_IS_DOUBLE
#error "Faster-Godot SIMD math paths require precision=single."
#endif

#if !defined(_MSC_VER) && (!defined(__AVX2__) || !defined(__FMA__) || !defined(__F16C__) || !defined(__POPCNT__))
#error "Faster-Godot SIMD math paths require AVX2, FMA, F16C, and POPCNT compiler flags."
#endif

#define MATH_SIMD_AVX2_FLOAT 1
#define MATH_SIMD_FMA_FLOAT 1
#define MATH_SIMD_F16C_FLOAT 1

#include <immintrin.h>

namespace Math {

_ALWAYS_INLINE_ __m256 simd_fmadd_ps(__m256 p_a, __m256 p_b, __m256 p_c) {
	return _mm256_fmadd_ps(p_a, p_b, p_c);
}

} // namespace Math

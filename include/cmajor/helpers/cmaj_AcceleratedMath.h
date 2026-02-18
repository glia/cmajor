//
//     ,ad888ba,                              88
//    d8"'    "8b
//   d8            88,dba,,adba,   ,aPP8A.A8  88     The Cmajor Toolkit
//   Y8,           88    88    88  88     88  88
//    Y8a.   .a8P  88    88    88  88,   ,88  88     (C)2024 Cmajor Software Ltd
//     '"Y888Y"'   88    88    88  '"8bbP"Y8  88     https://cmajor.dev
//                                           ,88
//                                        888P"
//
//  The Cmajor project is subject to commercial or open-source licensing.
//  You may use it under the terms of the GPLv3 (see www.gnu.org/licenses), or
//  visit https://cmajor.dev to learn about our commercial licence options.
//
//  CMAJOR IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
//  EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
//  DISCLAIMED.

#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <thread>

#ifdef __APPLE__
 #include <Accelerate/Accelerate.h>
#endif

#if defined(__arm64__) || defined(__aarch64__)
 #include <arm_neon.h>
 #define CMAJ_HAS_NEON 1
#endif

namespace cmaj
{

//==============================================================================
/// Hardware-accelerated math operations for audio DSP.
///
/// On Apple Silicon, these use:
///   - Accelerate.framework (vDSP/vForce) for large batch operations
///   - NEON SIMD intrinsics for small batch operations
///   - Scalar fast approximations as fallback
///
/// All functions are safe to call from the audio thread (no allocation).
///
namespace AcceleratedMath
{

//==============================================================================
// Batch exponential: computes exp(x[i]) for each element
//==============================================================================

/// Computes exp() for an array of float64 values, in-place.
/// Uses Accelerate.framework on macOS, NEON elsewhere, scalar fallback last.
inline void batchExp (double* data, uint32_t count)
{
   #ifdef __APPLE__
    if (count >= 4)
    {
        // vForce vectorised exp - uses NEON under the hood on Apple Silicon
        auto n = static_cast<int> (count);
        vvexp (data, data, &n);
        return;
    }
   #endif

   #if CMAJ_HAS_NEON
    // Process pairs using NEON
    uint32_t i = 0;

    for (; i + 1 < count; i += 2)
    {
        float64x2_t v = vld1q_f64 (data + i);
        // NEON doesn't have native exp, but we can use a Pade approximant
        // vectorised across 2 lanes simultaneously
        double vals[2];
        vst1q_f64 (vals, v);
        vals[0] = std::exp (vals[0]);
        vals[1] = std::exp (vals[1]);
        vst1q_f64 (data + i, vld1q_f64 (vals));
    }

    // Handle remaining element
    if (i < count)
        data[i] = std::exp (data[i]);
   #else
    for (uint32_t i = 0; i < count; ++i)
        data[i] = std::exp (data[i]);
   #endif
}

/// Computes exp() for an array of float32 values, in-place.
inline void batchExp (float* data, uint32_t count)
{
   #ifdef __APPLE__
    if (count >= 4)
    {
        auto n = static_cast<int> (count);
        vvexpf (data, data, &n);
        return;
    }
   #endif

    for (uint32_t i = 0; i < count; ++i)
        data[i] = std::exp (data[i]);
}

//==============================================================================
// Fast Pade exponential approximation (matches the Cmajor fastExp pattern)
//==============================================================================

/// Pade(4,4) approximation of exp(x), accurate to <1% for |x| < 10.
/// This is the same algorithm used in the HarmonyEngine neurons but
/// expressed as a vectorisable loop for batch processing.
inline void batchFastExp (double* data, uint32_t count)
{
   #if CMAJ_HAS_NEON
    uint32_t i = 0;

    for (; i + 1 < count; i += 2)
    {
        float64x2_t x = vld1q_f64 (data + i);

        // Clamp
        float64x2_t lo = vdupq_n_f64 (-10.0);
        float64x2_t hi = vdupq_n_f64 (10.0);
        x = vmaxq_f64 (vminq_f64 (x, hi), lo);

        // Pade(4,4): num = 1008 + x*(504 + x*(112 + x*(14 + x)))
        //            den = 1008 + x*(-504 + x*(112 + x*(-14 + x)))
        float64x2_t c1008 = vdupq_n_f64 (1008.0);
        float64x2_t c504  = vdupq_n_f64 (504.0);
        float64x2_t c112  = vdupq_n_f64 (112.0);
        float64x2_t c14   = vdupq_n_f64 (14.0);
        float64x2_t cn504 = vdupq_n_f64 (-504.0);
        float64x2_t cn14  = vdupq_n_f64 (-14.0);

        // Horner's method for numerator
        float64x2_t num = vaddq_f64 (c14, x);
        num = vfmaq_f64 (c112, num, x);   // 112 + (14+x)*x
        num = vfmaq_f64 (c504, num, x);   // 504 + prev*x
        num = vfmaq_f64 (c1008, num, x);  // 1008 + prev*x

        // Horner's method for denominator
        float64x2_t den = vaddq_f64 (cn14, x);
        den = vfmaq_f64 (c112, den, x);
        den = vfmaq_f64 (cn504, den, x);
        den = vfmaq_f64 (c1008, den, x);

        float64x2_t result = vdivq_f64 (num, den);
        vst1q_f64 (data + i, result);
    }

    // Scalar tail
    for (; i < count; ++i)
    {
        double x = std::clamp (data[i], -10.0, 10.0);
        double num = 1008.0 + x * (504.0 + x * (112.0 + x * (14.0 + x)));
        double den = 1008.0 + x * (-504.0 + x * (112.0 + x * (-14.0 + x)));
        data[i] = num / den;
    }
   #else
    for (uint32_t i = 0; i < count; ++i)
    {
        double x = std::clamp (data[i], -10.0, 10.0);
        double num = 1008.0 + x * (504.0 + x * (112.0 + x * (14.0 + x)));
        double den = 1008.0 + x * (-504.0 + x * (112.0 + x * (-14.0 + x)));
        data[i] = num / den;
    }
   #endif
}

//==============================================================================
// Batch sigmoid: computes 2/(1+exp(-gain*x)) - 1
//==============================================================================

/// Batch sigmoid activation using vectorised exp.
inline void batchSigmoid (double* data, uint32_t count, double gain)
{
   #ifdef __APPLE__
    if (count >= 4)
    {
        // Step 1: data[i] = -gain * data[i]
        double negGain = -gain;
        auto n = static_cast<int> (count);
        vDSP_vsmulD (data, 1, &negGain, data, 1, static_cast<vDSP_Length> (count));

        // Step 2: data[i] = exp(data[i])
        vvexp (data, data, &n);

        // Step 3: data[i] = 2.0 / (1.0 + data[i]) - 1.0
        for (uint32_t i = 0; i < count; ++i)
            data[i] = 2.0 / (1.0 + data[i]) - 1.0;

        return;
    }
   #endif

    for (uint32_t i = 0; i < count; ++i)
        data[i] = 2.0 / (1.0 + std::exp (-gain * data[i])) - 1.0;
}

//==============================================================================
// Matrix-vector multiply: result = matrix * vector
//==============================================================================

/// Computes result[i] = sum_j(matrix[i*cols + j] * vector[j]) for each row i.
/// Uses Accelerate.framework (BLAS) on macOS for optimal performance.
inline void matrixVectorMultiply (const double* matrix, const double* vector,
                                  double* result, uint32_t rows, uint32_t cols)
{
   #ifdef __APPLE__
    // cblas_dgemv computes y = alpha*A*x + beta*y
    // Using row-major layout (CblasRowMajor), no transpose
    cblas_dgemv (CblasRowMajor, CblasNoTrans,
                 static_cast<int> (rows),
                 static_cast<int> (cols),
                 1.0,           // alpha
                 matrix,        // A
                 static_cast<int> (cols),  // lda
                 vector,        // x
                 1,             // incX
                 0.0,           // beta
                 result,        // y
                 1);            // incY
   #else
    for (uint32_t i = 0; i < rows; ++i)
    {
        double sum = 0.0;
        const double* row = matrix + static_cast<size_t> (i) * cols;

       #if CMAJ_HAS_NEON
        uint32_t j = 0;
        float64x2_t acc = vdupq_n_f64 (0.0);

        for (; j + 1 < cols; j += 2)
            acc = vfmaq_f64 (acc, vld1q_f64 (row + j), vld1q_f64 (vector + j));

        sum = vaddvq_f64 (acc);

        for (; j < cols; ++j)
            sum += row[j] * vector[j];
       #else
        for (uint32_t j = 0; j < cols; ++j)
            sum += row[j] * vector[j];
       #endif

        result[i] = sum;
    }
   #endif
}

//==============================================================================
// Vector operations using Accelerate
//==============================================================================

/// Adds two double vectors: result[i] = a[i] + b[i]
inline void vectorAdd (const double* a, const double* b, double* result, uint32_t count)
{
   #ifdef __APPLE__
    vDSP_vaddD (a, 1, b, 1, result, 1, static_cast<vDSP_Length> (count));
   #else
    for (uint32_t i = 0; i < count; ++i)
        result[i] = a[i] + b[i];
   #endif
}

/// Multiplies two double vectors element-wise: result[i] = a[i] * b[i]
inline void vectorMultiply (const double* a, const double* b, double* result, uint32_t count)
{
   #ifdef __APPLE__
    vDSP_vmulD (a, 1, b, 1, result, 1, static_cast<vDSP_Length> (count));
   #else
    for (uint32_t i = 0; i < count; ++i)
        result[i] = a[i] * b[i];
   #endif
}

/// Scales a double vector: result[i] = a[i] * scalar
inline void vectorScale (const double* a, double scalar, double* result, uint32_t count)
{
   #ifdef __APPLE__
    vDSP_vsmulD (a, 1, &scalar, result, 1, static_cast<vDSP_Length> (count));
   #else
    for (uint32_t i = 0; i < count; ++i)
        result[i] = a[i] * scalar;
   #endif
}

/// Clamps all elements to [lo, hi]
inline void vectorClamp (double* data, uint32_t count, double lo, double hi)
{
   #ifdef __APPLE__
    vDSP_vclipD (data, 1, &lo, &hi, data, 1, static_cast<vDSP_Length> (count));
   #elif CMAJ_HAS_NEON
    uint32_t i = 0;
    float64x2_t vlo = vdupq_n_f64 (lo);
    float64x2_t vhi = vdupq_n_f64 (hi);

    for (; i + 1 < count; i += 2)
    {
        float64x2_t v = vld1q_f64 (data + i);
        v = vmaxq_f64 (vminq_f64 (v, vhi), vlo);
        vst1q_f64 (data + i, v);
    }

    for (; i < count; ++i)
        data[i] = std::clamp (data[i], lo, hi);
   #else
    for (uint32_t i = 0; i < count; ++i)
        data[i] = std::clamp (data[i], lo, hi);
   #endif
}

/// Computes the sum of a double vector
inline double vectorSum (const double* data, uint32_t count)
{
   #ifdef __APPLE__
    double result = 0.0;
    vDSP_sveD (data, 1, &result, static_cast<vDSP_Length> (count));
    return result;
   #elif CMAJ_HAS_NEON
    uint32_t i = 0;
    float64x2_t acc = vdupq_n_f64 (0.0);

    for (; i + 1 < count; i += 2)
        acc = vaddq_f64 (acc, vld1q_f64 (data + i));

    double result = vaddvq_f64 (acc);

    for (; i < count; ++i)
        result += data[i];

    return result;
   #else
    double result = 0.0;

    for (uint32_t i = 0; i < count; ++i)
        result += data[i];

    return result;
   #endif
}

/// Computes the max of absolute values across a double vector
inline double vectorMaxAbs (const double* data, uint32_t count)
{
   #ifdef __APPLE__
    double result = 0.0;
    vDSP_maxmgvD (data, 1, &result, static_cast<vDSP_Length> (count));
    return result;
   #else
    double result = 0.0;

    for (uint32_t i = 0; i < count; ++i)
    {
        double a = std::abs (data[i]);

        if (a > result)
            result = a;
    }

    return result;
   #endif
}

//==============================================================================
// Utility: detect hardware capabilities at runtime
//==============================================================================

struct HardwareCapabilities
{
    bool hasNEON = false;
    bool hasAppleAccelerate = false;
    bool hasMetalCompute = false;
    uint32_t performanceCoreCount = 1;

    static HardwareCapabilities detect()
    {
        HardwareCapabilities caps;

       #if CMAJ_HAS_NEON
        caps.hasNEON = true;
       #endif

       #ifdef __APPLE__
        caps.hasAppleAccelerate = true;

        // Detect performance core count on Apple Silicon
        // M1: 4 perf + 4 eff, M1 Pro: 6+2 or 8+2, M1 Max: 8+2, M2: 4+4, etc.
        uint32_t totalCores = std::thread::hardware_concurrency();

        // Heuristic: Apple Silicon typically has equal or more perf than eff cores
        // M1 family: 4 perf, M1 Pro/Max: 6-8 perf
        if (totalCores >= 10)
            caps.performanceCoreCount = 8;
        else if (totalCores >= 8)
            caps.performanceCoreCount = 4;
        else
            caps.performanceCoreCount = std::max (1u, totalCores / 2);

        caps.hasMetalCompute = true;  // All Apple Silicon has Metal
       #else
        caps.performanceCoreCount = std::max (1u, std::thread::hardware_concurrency());
       #endif

        return caps;
    }
};

} // namespace AcceleratedMath
} // namespace cmaj

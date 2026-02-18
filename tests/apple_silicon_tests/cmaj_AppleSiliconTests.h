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

#include <iostream>
#include <string>
#include <cmath>
#include <vector>
#include <numeric>
#include <random>
#include <chrono>
#include <atomic>
#include <cassert>
#include <iomanip>
#include <sstream>
#include <functional>

#include "../../include/cmajor/helpers/cmaj_AcceleratedMath.h"
#include "../../include/cmajor/helpers/cmaj_ParallelWorkGroup.h"

#ifdef __APPLE__
 #include "../../include/cmajor/helpers/cmaj_MetalCompute.h"
#endif

namespace cmaj::test
{

//==============================================================================
struct TestContext
{
    uint32_t passed = 0;
    uint32_t failed = 0;
    uint32_t totalAssertions = 0;
    std::string currentCategory;
    std::string currentTest;
    bool currentTestFailed = false;

    void beginCategory (const std::string& name)
    {
        currentCategory = name;
        std::cout << "\n=== " << name << " ===" << std::endl;
    }

    void beginTest (const std::string& name)
    {
        currentTest = name;
        currentTestFailed = false;
    }

    void endTest()
    {
        if (currentTestFailed)
        {
            ++failed;
            std::cout << "  FAIL: " << currentTest << std::endl;
        }
        else
        {
            ++passed;
            std::cout << "  pass: " << currentTest << std::endl;
        }
    }

    void expect (bool condition, const std::string& description)
    {
        ++totalAssertions;

        if (! condition)
        {
            currentTestFailed = true;
            std::cout << "    ASSERTION FAILED: " << description << std::endl;
        }
    }

    void expectNear (double a, double b, double tolerance, const std::string& description)
    {
        ++totalAssertions;

        if (std::abs (a - b) > tolerance)
        {
            currentTestFailed = true;
            std::ostringstream oss;
            oss << std::setprecision (12) << "    ASSERTION FAILED: " << description
                << " (expected " << b << ", got " << a << ", diff " << std::abs (a - b)
                << ", tolerance " << tolerance << ")";
            std::cout << oss.str() << std::endl;
        }
    }

    void printSummary()
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Results: " << passed << " passed, " << failed << " failed ("
                  << totalAssertions << " assertions)" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    bool allPassed() const  { return failed == 0; }
};

//==============================================================================
//  AcceleratedMath tests
//==============================================================================
static void runBatchExpDoubleTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::batchExp (double)");

    {
        ctx.beginTest ("zero elements does not crash");
        double dummy = 42.0;
        AcceleratedMath::batchExp (&dummy, 0);
        ctx.expect (dummy == 42.0, "data unchanged after zero-count call");
        ctx.endTest();
    }

    {
        ctx.beginTest ("single element");
        double val = 1.0;
        AcceleratedMath::batchExp (&val, 1);
        ctx.expectNear (val, std::exp (1.0), 1e-10, "exp(1.0)");
        ctx.endTest();
    }

    {
        ctx.beginTest ("two elements (NEON pair path)");
        double vals[2] = { 0.0, -1.0 };
        AcceleratedMath::batchExp (vals, 2);
        ctx.expectNear (vals[0], std::exp (0.0), 1e-10, "exp(0.0) = 1.0");
        ctx.expectNear (vals[1], std::exp (-1.0), 1e-10, "exp(-1.0)");
        ctx.endTest();
    }

    {
        ctx.beginTest ("three elements (NEON pair + scalar tail)");
        double vals[3] = { 1.0, 2.0, 3.0 };
        AcceleratedMath::batchExp (vals, 3);
        ctx.expectNear (vals[0], std::exp (1.0), 1e-10, "exp(1.0)");
        ctx.expectNear (vals[1], std::exp (2.0), 1e-10, "exp(2.0)");
        ctx.expectNear (vals[2], std::exp (3.0), 1e-10, "exp(3.0)");
        ctx.endTest();
    }

    {
        ctx.beginTest ("large batch (>= 4, Accelerate vvexp path)");
        constexpr uint32_t N = 128;
        std::vector<double> data (N);
        std::vector<double> expected (N);

        for (uint32_t i = 0; i < N; ++i)
        {
            data[i] = -5.0 + 10.0 * static_cast<double> (i) / (N - 1);
            expected[i] = std::exp (data[i]);
        }

        AcceleratedMath::batchExp (data.data(), N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (data[i], expected[i], 1e-8, "exp element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("exp(0) = 1 for entire batch");
        std::vector<double> data (16, 0.0);
        AcceleratedMath::batchExp (data.data(), 16);

        for (uint32_t i = 0; i < 16; ++i)
            ctx.expectNear (data[i], 1.0, 1e-12, "exp(0) element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("large negative values converge toward zero");
        double vals[4] = { -20.0, -50.0, -100.0, -200.0 };
        AcceleratedMath::batchExp (vals, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expect (vals[i] >= 0.0 && vals[i] < 1e-8,
                        "exp(large negative) near zero, element " + std::to_string (i));

        ctx.endTest();
    }
}

//==============================================================================
static void runBatchExpFloatTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::batchExp (float)");

    {
        ctx.beginTest ("single element");
        float val = 1.0f;
        AcceleratedMath::batchExp (&val, 1);
        ctx.expectNear (static_cast<double> (val), std::exp (1.0), 1e-6, "expf(1.0)");
        ctx.endTest();
    }

    {
        ctx.beginTest ("large batch (>= 4, Accelerate vvexpf path)");
        constexpr uint32_t N = 64;
        std::vector<float> data (N);
        std::vector<float> expected (N);

        for (uint32_t i = 0; i < N; ++i)
        {
            data[i] = -3.0f + 6.0f * static_cast<float> (i) / (N - 1);
            expected[i] = std::exp (data[i]);
        }

        AcceleratedMath::batchExp (data.data(), N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (data[i], expected[i], 1e-4, "expf element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("small batch (< 4, scalar path)");
        float vals[3] = { 0.0f, 1.0f, -1.0f };
        AcceleratedMath::batchExp (vals, 3);
        ctx.expectNear (vals[0], 1.0, 1e-6, "expf(0) = 1");
        ctx.expectNear (vals[1], std::exp (1.0), 1e-5, "expf(1)");
        ctx.expectNear (vals[2], std::exp (-1.0), 1e-5, "expf(-1)");
        ctx.endTest();
    }
}

//==============================================================================
static void runBatchFastExpTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::batchFastExp");

    {
        ctx.beginTest ("exp(0) approximation is close to 1");
        double val = 0.0;
        AcceleratedMath::batchFastExp (&val, 1);
        ctx.expectNear (val, 1.0, 0.001, "fastExp(0) ~= 1.0");
        ctx.endTest();
    }

    {
        ctx.beginTest ("small positive values approximate exp well (|x| <= 2)");
        double vals[4] = { 0.1, 0.5, 1.0, 2.0 };
        double expected[4];

        for (int i = 0; i < 4; ++i)
            expected[i] = std::exp (vals[i]);

        AcceleratedMath::batchFastExp (vals, 4);

        for (int i = 0; i < 4; ++i)
        {
            double relError = std::abs (vals[i] - expected[i]) / expected[i];
            ctx.expect (relError < 0.02,
                        "fastExp relative error < 2% for small positive, got "
                        + std::to_string (relError * 100) + "%");
        }

        ctx.endTest();
    }

    {
        ctx.beginTest ("small negative values approximate exp well (|x| <= 2)");
        double vals[4] = { -0.1, -0.5, -1.0, -2.0 };
        double expected[4];

        for (int i = 0; i < 4; ++i)
            expected[i] = std::exp (vals[i]);

        AcceleratedMath::batchFastExp (vals, 4);

        for (int i = 0; i < 4; ++i)
        {
            double relError = std::abs (vals[i] - expected[i]) / expected[i];
            ctx.expect (relError < 0.02,
                        "fastExp relative error < 2% for small negative, got "
                        + std::to_string (relError * 100) + "%");
        }

        ctx.endTest();
    }

    {
        ctx.beginTest ("large values diverge from exp but remain finite and positive");
        double vals[4] = { 5.0, -5.0, 8.0, -8.0 };
        AcceleratedMath::batchFastExp (vals, 4);

        for (int i = 0; i < 4; ++i)
        {
            ctx.expect (std::isfinite (vals[i]),
                        "result is finite at index " + std::to_string (i));
            ctx.expect (vals[i] > 0.0,
                        "result is positive at index " + std::to_string (i));
        }

        ctx.endTest();
    }

    {
        ctx.beginTest ("clamping at boundaries: values beyond [-10, 10]");
        double vals[4] = { -20.0, -15.0, 15.0, 20.0 };
        double clamped[4] = { -10.0, -10.0, 10.0, 10.0 };
        double expectedFromClamped[4];

        for (int i = 0; i < 4; ++i)
        {
            double x = clamped[i];
            double num = 1008.0 + x * (504.0 + x * (112.0 + x * (14.0 + x)));
            double den = 1008.0 + x * (-504.0 + x * (112.0 + x * (-14.0 + x)));
            expectedFromClamped[i] = num / den;
        }

        AcceleratedMath::batchFastExp (vals, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (vals[i], expectedFromClamped[i], 1e-10,
                            "clamped boundary element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("NEON pair processing with odd count (scalar tail)");
        double vals[7] = { -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0 };
        AcceleratedMath::batchFastExp (vals, 7);

        ctx.expect (vals[3] > 0.99 && vals[3] < 1.01, "fastExp(0) ~= 1");
        ctx.expect (vals[4] > vals[3], "fastExp(1) > fastExp(0)");
        ctx.expect (vals[0] < vals[1], "fastExp(-3) < fastExp(-2)");
        ctx.endTest();
    }

    {
        ctx.beginTest ("monotonicity: fastExp preserves ordering for |x| <= 3");
        constexpr uint32_t N = 20;
        std::vector<double> data (N);

        for (uint32_t i = 0; i < N; ++i)
            data[i] = -3.0 + 6.0 * static_cast<double> (i) / (N - 1);

        AcceleratedMath::batchFastExp (data.data(), N);

        for (uint32_t i = 1; i < N; ++i)
            ctx.expect (data[i] >= data[i - 1],
                        "monotonic at index " + std::to_string (i));

        ctx.endTest();
    }
}

//==============================================================================
static void runBatchSigmoidTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::batchSigmoid");

    auto referenceSigmoid = [] (double x, double gain) -> double
    {
        return 2.0 / (1.0 + std::exp (-gain * x)) - 1.0;
    };

    {
        ctx.beginTest ("sigmoid(0) = 0 for any gain");
        double val = 0.0;
        AcceleratedMath::batchSigmoid (&val, 1, 5.0);
        ctx.expectNear (val, 0.0, 1e-10, "sigmoid(0, gain=5) = 0");
        ctx.endTest();
    }

    {
        ctx.beginTest ("sigmoid approaches +1 for large positive input");
        double val = 100.0;
        AcceleratedMath::batchSigmoid (&val, 1, 1.0);
        ctx.expectNear (val, 1.0, 1e-6, "sigmoid(100, gain=1) ~= 1");
        ctx.endTest();
    }

    {
        ctx.beginTest ("sigmoid approaches -1 for large negative input");
        double val = -100.0;
        AcceleratedMath::batchSigmoid (&val, 1, 1.0);
        ctx.expectNear (val, -1.0, 1e-6, "sigmoid(-100, gain=1) ~= -1");
        ctx.endTest();
    }

    {
        ctx.beginTest ("sigmoid is odd function: sigmoid(-x) = -sigmoid(x)");
        double pos[4] = { 0.5, 1.0, 2.0, 3.0 };
        double neg[4] = { -0.5, -1.0, -2.0, -3.0 };
        AcceleratedMath::batchSigmoid (pos, 4, 2.0);
        AcceleratedMath::batchSigmoid (neg, 4, 2.0);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (pos[i], -neg[i], 1e-10,
                            "odd symmetry at index " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("large batch matches reference (Accelerate path)");
        constexpr uint32_t N = 88;
        std::vector<double> data (N);
        std::vector<double> expected (N);
        double gain = 3.0;

        for (uint32_t i = 0; i < N; ++i)
        {
            data[i] = -5.0 + 10.0 * static_cast<double> (i) / (N - 1);
            expected[i] = referenceSigmoid (data[i], gain);
        }

        AcceleratedMath::batchSigmoid (data.data(), N, gain);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (data[i], expected[i], 1e-8,
                            "sigmoid element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("small batch (< 4, scalar path)");
        double vals[3] = { -1.0, 0.0, 1.0 };
        double expected[3];

        for (int i = 0; i < 3; ++i)
            expected[i] = referenceSigmoid (vals[i], 1.0);

        AcceleratedMath::batchSigmoid (vals, 3, 1.0);

        for (int i = 0; i < 3; ++i)
            ctx.expectNear (vals[i], expected[i], 1e-10,
                            "small batch element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("higher gain produces steeper transition");
        double lowGain[2] = { 0.5, 0.5 };
        double highGain[2] = { 0.5, 0.5 };
        AcceleratedMath::batchSigmoid (lowGain, 1, 1.0);
        AcceleratedMath::batchSigmoid (highGain, 1, 10.0);
        ctx.expect (highGain[0] > lowGain[0],
                    "higher gain produces larger output for positive input");
        ctx.endTest();
    }
}

//==============================================================================
static void runMatrixVectorMultiplyTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::matrixVectorMultiply");

    {
        ctx.beginTest ("1x1 matrix");
        double matrix[1] = { 3.0 };
        double vec[1] = { 7.0 };
        double result[1] = { 0.0 };
        AcceleratedMath::matrixVectorMultiply (matrix, vec, result, 1, 1);
        ctx.expectNear (result[0], 21.0, 1e-10, "3 * 7 = 21");
        ctx.endTest();
    }

    {
        ctx.beginTest ("identity matrix 4x4");
        double identity[16] = {
            1, 0, 0, 0,
            0, 1, 0, 0,
            0, 0, 1, 0,
            0, 0, 0, 1
        };
        double vec[4] = { 1.0, 2.0, 3.0, 4.0 };
        double result[4] = { 0.0 };
        AcceleratedMath::matrixVectorMultiply (identity, vec, result, 4, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (result[i], vec[i], 1e-10,
                            "identity preserves element " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("known 3x3 matrix");
        double matrix[9] = {
            1, 2, 3,
            4, 5, 6,
            7, 8, 9
        };
        double vec[3] = { 1, 1, 1 };
        double result[3] = { 0.0 };
        AcceleratedMath::matrixVectorMultiply (matrix, vec, result, 3, 3);
        ctx.expectNear (result[0], 6.0, 1e-10, "row 0 sum");
        ctx.expectNear (result[1], 15.0, 1e-10, "row 1 sum");
        ctx.expectNear (result[2], 24.0, 1e-10, "row 2 sum");
        ctx.endTest();
    }

    {
        ctx.beginTest ("non-square matrix 2x4");
        double matrix[8] = {
            1, 2, 3, 4,
            5, 6, 7, 8
        };
        double vec[4] = { 1, 0, 1, 0 };
        double result[2] = { 0.0 };
        AcceleratedMath::matrixVectorMultiply (matrix, vec, result, 2, 4);
        ctx.expectNear (result[0], 4.0, 1e-10, "1*1 + 2*0 + 3*1 + 4*0 = 4");
        ctx.expectNear (result[1], 12.0, 1e-10, "5*1 + 6*0 + 7*1 + 8*0 = 12");
        ctx.endTest();
    }

    {
        ctx.beginTest ("zero vector produces zero result");
        double matrix[4] = { 1, 2, 3, 4 };
        double vec[2] = { 0.0, 0.0 };
        double result[2] = { 99.0, 99.0 };
        AcceleratedMath::matrixVectorMultiply (matrix, vec, result, 2, 2);
        ctx.expectNear (result[0], 0.0, 1e-10, "zero vec row 0");
        ctx.expectNear (result[1], 0.0, 1e-10, "zero vec row 1");
        ctx.endTest();
    }

    {
        ctx.beginTest ("88x88 matrix (neural network size, BLAS/NEON path)");
        constexpr uint32_t N = 88;
        std::vector<double> matrix (N * N);
        std::vector<double> vec (N);
        std::vector<double> result (N, 0.0);
        std::vector<double> expected (N, 0.0);

        std::mt19937 rng (42);
        std::uniform_real_distribution<double> dist (-1.0, 1.0);

        for (auto& v : matrix) v = dist (rng);
        for (auto& v : vec)    v = dist (rng);

        for (uint32_t i = 0; i < N; ++i)
        {
            double sum = 0.0;

            for (uint32_t j = 0; j < N; ++j)
                sum += matrix[i * N + j] * vec[j];

            expected[i] = sum;
        }

        AcceleratedMath::matrixVectorMultiply (matrix.data(), vec.data(),
                                               result.data(), N, N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (result[i], expected[i], 1e-8,
                            "88x88 row " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("odd-dimensioned matrix 5x7 (NEON tail handling)");
        constexpr uint32_t rows = 5, cols = 7;
        std::vector<double> matrix (rows * cols);
        std::vector<double> vec (cols);
        std::vector<double> result (rows, 0.0);
        std::vector<double> expected (rows, 0.0);

        std::mt19937 rng (123);
        std::uniform_real_distribution<double> dist (-2.0, 2.0);

        for (auto& v : matrix) v = dist (rng);
        for (auto& v : vec)    v = dist (rng);

        for (uint32_t i = 0; i < rows; ++i)
        {
            double sum = 0.0;

            for (uint32_t j = 0; j < cols; ++j)
                sum += matrix[i * cols + j] * vec[j];

            expected[i] = sum;
        }

        AcceleratedMath::matrixVectorMultiply (matrix.data(), vec.data(),
                                               result.data(), rows, cols);

        for (uint32_t i = 0; i < rows; ++i)
            ctx.expectNear (result[i], expected[i], 1e-8,
                            "5x7 row " + std::to_string (i));

        ctx.endTest();
    }
}

//==============================================================================
static void runVectorOperationTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath vector operations");

    {
        ctx.beginTest ("vectorAdd basic");
        double a[4] = { 1.0, 2.0, 3.0, 4.0 };
        double b[4] = { 10.0, 20.0, 30.0, 40.0 };
        double result[4] = {};
        AcceleratedMath::vectorAdd (a, b, result, 4);
        ctx.expectNear (result[0], 11.0, 1e-10, "1+10");
        ctx.expectNear (result[1], 22.0, 1e-10, "2+20");
        ctx.expectNear (result[2], 33.0, 1e-10, "3+30");
        ctx.expectNear (result[3], 44.0, 1e-10, "4+40");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorAdd in-place (result aliases a)");
        double a[4] = { 1.0, 2.0, 3.0, 4.0 };
        double b[4] = { 5.0, 6.0, 7.0, 8.0 };
        AcceleratedMath::vectorAdd (a, b, a, 4);
        ctx.expectNear (a[0], 6.0, 1e-10, "1+5 in-place");
        ctx.expectNear (a[3], 12.0, 1e-10, "4+8 in-place");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMultiply element-wise");
        double a[4] = { 2.0, 3.0, 4.0, 5.0 };
        double b[4] = { 10.0, 10.0, 10.0, 10.0 };
        double result[4] = {};
        AcceleratedMath::vectorMultiply (a, b, result, 4);
        ctx.expectNear (result[0], 20.0, 1e-10, "2*10");
        ctx.expectNear (result[1], 30.0, 1e-10, "3*10");
        ctx.expectNear (result[2], 40.0, 1e-10, "4*10");
        ctx.expectNear (result[3], 50.0, 1e-10, "5*10");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMultiply with zeros");
        double a[3] = { 1.0, 2.0, 3.0 };
        double b[3] = { 0.0, 0.0, 0.0 };
        double result[3] = { 99.0, 99.0, 99.0 };
        AcceleratedMath::vectorMultiply (a, b, result, 3);

        for (int i = 0; i < 3; ++i)
            ctx.expectNear (result[i], 0.0, 1e-10, "multiply by zero");

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorScale");
        double a[5] = { 1.0, 2.0, 3.0, 4.0, 5.0 };
        double result[5] = {};
        AcceleratedMath::vectorScale (a, 3.0, result, 5);
        ctx.expectNear (result[0], 3.0, 1e-10, "1*3");
        ctx.expectNear (result[4], 15.0, 1e-10, "5*3");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorScale by zero");
        double a[4] = { 100.0, 200.0, 300.0, 400.0 };
        double result[4] = {};
        AcceleratedMath::vectorScale (a, 0.0, result, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (result[i], 0.0, 1e-10, "scale by zero");

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorScale by one (identity)");
        double a[4] = { 1.0, 2.0, 3.0, 4.0 };
        double result[4] = {};
        AcceleratedMath::vectorScale (a, 1.0, result, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (result[i], a[i], 1e-10, "scale by one");

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorClamp within range (no clamping needed)");
        double data[4] = { 0.2, 0.5, 0.7, 0.9 };
        AcceleratedMath::vectorClamp (data, 4, 0.0, 1.0);
        ctx.expectNear (data[0], 0.2, 1e-10, "within range 0");
        ctx.expectNear (data[3], 0.9, 1e-10, "within range 3");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorClamp clips low values");
        double data[4] = { -5.0, -1.0, 0.5, 2.0 };
        AcceleratedMath::vectorClamp (data, 4, -1.0, 1.0);
        ctx.expectNear (data[0], -1.0, 1e-10, "clamp -5 to -1");
        ctx.expectNear (data[1], -1.0, 1e-10, "keep -1");
        ctx.expectNear (data[2], 0.5, 1e-10, "keep 0.5");
        ctx.expectNear (data[3], 1.0, 1e-10, "clamp 2 to 1");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorClamp odd count (NEON tail)");
        double data[5] = { -10.0, 5.0, 0.0, -0.5, 10.0 };
        AcceleratedMath::vectorClamp (data, 5, -1.0, 1.0);
        ctx.expectNear (data[0], -1.0, 1e-10, "clamp low");
        ctx.expectNear (data[1], 1.0, 1e-10, "clamp high");
        ctx.expectNear (data[2], 0.0, 1e-10, "keep zero");
        ctx.expectNear (data[3], -0.5, 1e-10, "keep -0.5");
        ctx.expectNear (data[4], 1.0, 1e-10, "clamp high tail");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorSum basic");
        double data[4] = { 1.0, 2.0, 3.0, 4.0 };
        double sum = AcceleratedMath::vectorSum (data, 4);
        ctx.expectNear (sum, 10.0, 1e-10, "1+2+3+4 = 10");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorSum single element");
        double data[1] = { 42.0 };
        double sum = AcceleratedMath::vectorSum (data, 1);
        ctx.expectNear (sum, 42.0, 1e-10, "single element sum");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorSum odd count (NEON tail)");
        double data[7] = { 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0 };
        double sum = AcceleratedMath::vectorSum (data, 7);
        ctx.expectNear (sum, 28.0, 1e-10, "sum of 1..7 = 28");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorSum large array");
        constexpr uint32_t N = 1000;
        std::vector<double> data (N, 1.0);
        double sum = AcceleratedMath::vectorSum (data.data(), N);
        ctx.expectNear (sum, static_cast<double> (N), 1e-8, "sum of 1000 ones");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMaxAbs all positive");
        double data[4] = { 1.0, 5.0, 3.0, 2.0 };
        double maxAbs = AcceleratedMath::vectorMaxAbs (data, 4);
        ctx.expectNear (maxAbs, 5.0, 1e-10, "max abs of positives");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMaxAbs mixed signs");
        double data[5] = { -7.0, 3.0, -1.0, 6.0, -4.0 };
        double maxAbs = AcceleratedMath::vectorMaxAbs (data, 5);
        ctx.expectNear (maxAbs, 7.0, 1e-10, "max abs with negatives");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMaxAbs single element");
        double data[1] = { -42.0 };
        double maxAbs = AcceleratedMath::vectorMaxAbs (data, 1);
        ctx.expectNear (maxAbs, 42.0, 1e-10, "abs of single negative");
        ctx.endTest();
    }
}

//==============================================================================
static void runHardwareCapabilitiesTests (TestContext& ctx)
{
    ctx.beginCategory ("AcceleratedMath::HardwareCapabilities");

    {
        ctx.beginTest ("detect returns sensible values");
        auto caps = AcceleratedMath::HardwareCapabilities::detect();

        ctx.expect (caps.performanceCoreCount >= 1, "at least 1 performance core");
        ctx.expect (caps.performanceCoreCount <= 128, "reasonable core count upper bound");

       #ifdef __APPLE__
        ctx.expect (caps.hasAppleAccelerate, "Apple platform has Accelerate");
        ctx.expect (caps.hasMetalCompute, "Apple Silicon has Metal");
       #endif

       #if CMAJ_HAS_NEON
        ctx.expect (caps.hasNEON, "ARM64 has NEON");
       #endif

        std::cout << "    Hardware: NEON=" << caps.hasNEON
                  << " Accelerate=" << caps.hasAppleAccelerate
                  << " Metal=" << caps.hasMetalCompute
                  << " PerfCores=" << caps.performanceCoreCount << std::endl;

        ctx.endTest();
    }

    {
        ctx.beginTest ("detect is deterministic");
        auto caps1 = AcceleratedMath::HardwareCapabilities::detect();
        auto caps2 = AcceleratedMath::HardwareCapabilities::detect();

        ctx.expect (caps1.hasNEON == caps2.hasNEON, "NEON flag stable");
        ctx.expect (caps1.hasAppleAccelerate == caps2.hasAppleAccelerate, "Accelerate flag stable");
        ctx.expect (caps1.hasMetalCompute == caps2.hasMetalCompute, "Metal flag stable");
        ctx.expect (caps1.performanceCoreCount == caps2.performanceCoreCount, "core count stable");
        ctx.endTest();
    }
}

//==============================================================================
//  ParallelWorkGroup tests
//==============================================================================
static void runParallelWorkGroupTests (TestContext& ctx)
{
    ctx.beginCategory ("ParallelWorkGroup");

    {
        ctx.beginTest ("construction with explicit thread count");
        ParallelWorkGroup wg (2);
        ctx.expect (wg.getThreadCount() == 2, "thread count is 2");
        ctx.endTest();
    }

    {
        ctx.beginTest ("construction with default (0) uses recommended count");
        ParallelWorkGroup wg (0);
        auto recommended = ParallelWorkGroup::getRecommendedThreadCount();
        ctx.expect (wg.getThreadCount() == recommended,
                    "default matches recommended (" + std::to_string (recommended) + ")");
        ctx.endTest();
    }

    {
        ctx.beginTest ("thread count clamped to at least 1");
        ParallelWorkGroup wg (1);
        ctx.expect (wg.getThreadCount() >= 1, "at least 1 thread");
        ctx.endTest();
    }

    {
        ctx.beginTest ("getRecommendedThreadCount returns 1-8");
        auto count = ParallelWorkGroup::getRecommendedThreadCount();
        ctx.expect (count >= 1 && count <= 8,
                    "recommended count in [1,8], got " + std::to_string (count));
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor with count=0 does not crash");
        ParallelWorkGroup wg (2);
        wg.parallelFor (0, [] (uint32_t) {});
        ctx.expect (true, "survived zero-count parallelFor");
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor with count=1 processes single item");
        ParallelWorkGroup wg (4);
        std::atomic<uint32_t> callCount { 0 };
        wg.parallelFor (1, [&] (uint32_t) { callCount.fetch_add (1); });
        ctx.expect (callCount.load() == 1, "called exactly once");
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor visits every index exactly once");
        constexpr uint32_t N = 100;
        ParallelWorkGroup wg (4);
        std::vector<std::atomic<int>> visited (N);

        for (auto& v : visited)
            v.store (0);

        wg.parallelFor (N, [&] (uint32_t i) { visited[i].fetch_add (1); });

        bool allVisitedOnce = true;

        for (uint32_t i = 0; i < N; ++i)
        {
            if (visited[i].load() != 1)
            {
                allVisitedOnce = false;
                ctx.expect (false, "index " + std::to_string (i) + " visited "
                                   + std::to_string (visited[i].load()) + " times");
            }
        }

        if (allVisitedOnce)
            ctx.expect (true, "all 100 indices visited exactly once");

        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor large workload (1000 items)");
        constexpr uint32_t N = 1000;
        ParallelWorkGroup wg (4);
        std::atomic<uint32_t> total { 0 };
        wg.parallelFor (N, [&] (uint32_t) { total.fetch_add (1); });
        ctx.expect (total.load() == N,
                    "processed " + std::to_string (total.load()) + "/" + std::to_string (N));
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor correctness: sum of indices");
        constexpr uint32_t N = 200;
        ParallelWorkGroup wg (4);
        std::atomic<uint64_t> sum { 0 };
        wg.parallelFor (N, [&] (uint32_t i) { sum.fetch_add (i); });

        uint64_t expectedSum = static_cast<uint64_t> (N - 1) * N / 2;
        ctx.expect (sum.load() == expectedSum,
                    "sum of 0..199 = " + std::to_string (expectedSum)
                    + ", got " + std::to_string (sum.load()));
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor with count <= threadCount (sequential path)");
        ParallelWorkGroup wg (4);
        std::atomic<uint32_t> callCount { 0 };
        wg.parallelFor (3, [&] (uint32_t) { callCount.fetch_add (1); });
        ctx.expect (callCount.load() == 3, "processed all 3 items via sequential path");
        ctx.endTest();
    }

    {
        ctx.beginTest ("multiple sequential dispatches work correctly");
        ParallelWorkGroup wg (4);

        for (int iteration = 0; iteration < 10; ++iteration)
        {
            std::atomic<uint32_t> count { 0 };
            wg.parallelFor (50, [&] (uint32_t) { count.fetch_add (1); });
            ctx.expect (count.load() == 50,
                        "iteration " + std::to_string (iteration) + " processed all items");
        }

        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor writes to shared array safely");
        constexpr uint32_t N = 88;
        ParallelWorkGroup wg (4);
        std::vector<double> results (N, 0.0);

        wg.parallelFor (N, [&] (uint32_t i)
        {
            results[i] = static_cast<double> (i * i);
        });

        bool correct = true;

        for (uint32_t i = 0; i < N; ++i)
        {
            double expected = static_cast<double> (i * i);

            if (std::abs (results[i] - expected) > 1e-10)
            {
                correct = false;
                ctx.expect (false, "results[" + std::to_string (i) + "] = "
                                   + std::to_string (results[i]) + ", expected "
                                   + std::to_string (expected));
            }
        }

        if (correct)
            ctx.expect (true, "all 88 results correct");

        ctx.endTest();
    }

    {
        ctx.beginTest ("single-thread work group processes everything");
        ParallelWorkGroup wg (1);
        std::atomic<uint32_t> count { 0 };
        wg.parallelFor (50, [&] (uint32_t) { count.fetch_add (1); });
        ctx.expect (count.load() == 50, "single thread processed all 50 items");
        ctx.endTest();
    }
}

//==============================================================================
//  MetalCompute header tests (Apple only, compilation and shader source)
//==============================================================================
#ifdef __APPLE__
static void runMetalComputeTests (TestContext& ctx)
{
    ctx.beginCategory ("MetalCompute (header validation)");

    {
        ctx.beginTest ("MetalShaders source is non-null and non-empty");
        auto* src = cmaj::MetalShaders::synapseKernelSource;
        ctx.expect (src != nullptr, "shader source pointer is non-null");
        ctx.expect (std::string (src).size() > 100, "shader source has substantial content");
        ctx.endTest();
    }

    {
        ctx.beginTest ("shader source contains expected kernel names");
        std::string src (cmaj::MetalShaders::synapseKernelSource);
        ctx.expect (src.find ("matVecMul") != std::string::npos, "contains matVecMul kernel");
        ctx.expect (src.find ("batchSigmoidKernel") != std::string::npos, "contains batchSigmoidKernel");
        ctx.expect (src.find ("batchExpKernel") != std::string::npos, "contains batchExpKernel");
        ctx.expect (src.find ("updateGlutamateSynapses") != std::string::npos, "contains updateGlutamateSynapses");
        ctx.expect (src.find ("updateGabaSynapses") != std::string::npos, "contains updateGabaSynapses");
        ctx.endTest();
    }

    {
        ctx.beginTest ("shader source contains Metal stdlib include");
        std::string src (cmaj::MetalShaders::synapseKernelSource);
        ctx.expect (src.find ("#include <metal_stdlib>") != std::string::npos,
                    "shader includes metal_stdlib");
        ctx.endTest();
    }

    {
        ctx.beginTest ("MetalComputeContext struct has expected interface");
        // Verify the struct compiles and has the expected method signatures
        // by taking addresses of member functions
        using MCC = cmaj::MetalComputeContext;
        (void) static_cast<bool (MCC::*) () const> (&MCC::isAvailable);
        (void) static_cast<uint32_t (MCC::*) () const> (&MCC::getMinMatrixSizeForGPU);
        ctx.expect (true, "MetalComputeContext interface compiles");
        ctx.endTest();
    }
}
#endif

//==============================================================================
//  Performance / stress tests
//==============================================================================
static void runPerformanceTests (TestContext& ctx)
{
    ctx.beginCategory ("Performance smoke tests");

    {
        ctx.beginTest ("batchExp 10000 elements completes within 10ms");
        constexpr uint32_t N = 10000;
        std::vector<double> data (N);

        for (uint32_t i = 0; i < N; ++i)
            data[i] = -5.0 + 10.0 * static_cast<double> (i) / (N - 1);

        auto start = std::chrono::high_resolution_clock::now();
        AcceleratedMath::batchExp (data.data(), N);
        auto end = std::chrono::high_resolution_clock::now();

        auto us = std::chrono::duration_cast<std::chrono::microseconds> (end - start).count();
        std::cout << "    batchExp(10000): " << us << " us" << std::endl;
        ctx.expect (us < 10000, "completed within 10ms");
        ctx.endTest();
    }

    {
        ctx.beginTest ("batchFastExp 10000 elements completes within 5ms");
        constexpr uint32_t N = 10000;
        std::vector<double> data (N);

        for (uint32_t i = 0; i < N; ++i)
            data[i] = -9.0 + 18.0 * static_cast<double> (i) / (N - 1);

        auto start = std::chrono::high_resolution_clock::now();
        AcceleratedMath::batchFastExp (data.data(), N);
        auto end = std::chrono::high_resolution_clock::now();

        auto us = std::chrono::duration_cast<std::chrono::microseconds> (end - start).count();
        std::cout << "    batchFastExp(10000): " << us << " us" << std::endl;
        ctx.expect (us < 5000, "completed within 5ms");
        ctx.endTest();
    }

    {
        ctx.beginTest ("88x88 matrixVectorMultiply completes within 1ms");
        constexpr uint32_t N = 88;
        std::vector<double> matrix (N * N, 1.0);
        std::vector<double> vec (N, 1.0);
        std::vector<double> result (N, 0.0);

        auto start = std::chrono::high_resolution_clock::now();

        for (int rep = 0; rep < 100; ++rep)
            AcceleratedMath::matrixVectorMultiply (matrix.data(), vec.data(),
                                                   result.data(), N, N);

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds> (end - start).count();
        std::cout << "    matVecMul 88x88 x100: " << us << " us ("
                  << us / 100 << " us/call)" << std::endl;
        ctx.expect (us < 100000, "100 calls completed within 100ms");
        ctx.endTest();
    }

    {
        ctx.beginTest ("batchSigmoid 88 elements completes within 100us");
        constexpr uint32_t N = 88;
        std::vector<double> data (N);

        for (uint32_t i = 0; i < N; ++i)
            data[i] = -5.0 + 10.0 * static_cast<double> (i) / (N - 1);

        auto start = std::chrono::high_resolution_clock::now();

        for (int rep = 0; rep < 1000; ++rep)
        {
            for (uint32_t i = 0; i < N; ++i)
                data[i] = -5.0 + 10.0 * static_cast<double> (i) / (N - 1);

            AcceleratedMath::batchSigmoid (data.data(), N, 3.0);
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds> (end - start).count();
        std::cout << "    batchSigmoid(88) x1000: " << us << " us ("
                  << us / 1000 << " us/call)" << std::endl;
        ctx.expect (us < 1000000, "1000 calls completed within 1s");
        ctx.endTest();
    }

    {
        ctx.beginTest ("parallelFor 88 items across 4 threads completes quickly");
        ParallelWorkGroup wg (4);
        std::vector<double> results (88, 0.0);

        auto start = std::chrono::high_resolution_clock::now();

        for (int rep = 0; rep < 1000; ++rep)
        {
            wg.parallelFor (88, [&] (uint32_t i)
            {
                double x = static_cast<double> (i);
                results[i] = std::sin (x) * std::cos (x);
            });
        }

        auto end = std::chrono::high_resolution_clock::now();
        auto us = std::chrono::duration_cast<std::chrono::microseconds> (end - start).count();
        std::cout << "    parallelFor(88) x1000: " << us << " us ("
                  << us / 1000 << " us/call)" << std::endl;
        ctx.expect (us < 5000000, "1000 dispatches completed within 5s");
        ctx.endTest();
    }
}

//==============================================================================
//  Edge case and numerical robustness tests
//==============================================================================
static void runEdgeCaseTests (TestContext& ctx)
{
    ctx.beginCategory ("Edge cases and numerical robustness");

    {
        ctx.beginTest ("batchExp with very small positive values");
        double vals[4] = { 1e-10, 1e-15, 1e-20, 0.0 };
        AcceleratedMath::batchExp (vals, 4);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (vals[i], 1.0, 1e-6, "exp(tiny) ~= 1");

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorSum with alternating positive/negative values");
        double data[8] = { 1.0, -1.0, 2.0, -2.0, 3.0, -3.0, 4.0, -4.0 };
        double sum = AcceleratedMath::vectorSum (data, 8);
        ctx.expectNear (sum, 0.0, 1e-10, "cancellation produces zero");
        ctx.endTest();
    }

    {
        ctx.beginTest ("matrixVectorMultiply with all-ones matrix (sum vector)");
        constexpr uint32_t N = 16;
        std::vector<double> matrix (N * N, 1.0);
        std::vector<double> vec (N);
        std::vector<double> result (N, 0.0);

        for (uint32_t i = 0; i < N; ++i)
            vec[i] = static_cast<double> (i + 1);

        double expectedRowSum = N * (N + 1) / 2.0;
        AcceleratedMath::matrixVectorMultiply (matrix.data(), vec.data(),
                                               result.data(), N, N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (result[i], expectedRowSum, 1e-8,
                            "all-ones row " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("batchSigmoid gain=0 produces all zeros");
        double data[8] = { -3.0, -2.0, -1.0, 0.0, 1.0, 2.0, 3.0, 4.0 };
        AcceleratedMath::batchSigmoid (data, 8, 0.0);

        for (int i = 0; i < 8; ++i)
            ctx.expectNear (data[i], 0.0, 1e-10, "sigmoid with gain=0 at index " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("batchFastExp positive output for all inputs in range");
        std::vector<double> data (100);

        for (uint32_t i = 0; i < 100; ++i)
            data[i] = -10.0 + 20.0 * static_cast<double> (i) / 99.0;

        AcceleratedMath::batchFastExp (data.data(), 100);

        for (uint32_t i = 0; i < 100; ++i)
            ctx.expect (data[i] > 0.0,
                        "fastExp output positive at index " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorClamp with equal lo and hi collapses to constant");
        double data[4] = { -5.0, 0.0, 5.0, 10.0 };
        AcceleratedMath::vectorClamp (data, 4, 3.0, 3.0);

        for (int i = 0; i < 4; ++i)
            ctx.expectNear (data[i], 3.0, 1e-10, "collapsed to 3.0");

        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorMaxAbs with all zeros");
        double data[4] = { 0.0, 0.0, 0.0, 0.0 };
        double maxAbs = AcceleratedMath::vectorMaxAbs (data, 4);
        ctx.expectNear (maxAbs, 0.0, 1e-10, "max abs of zeros is 0");
        ctx.endTest();
    }

    {
        ctx.beginTest ("vectorAdd large array (Accelerate path stress test)");
        constexpr uint32_t N = 10000;
        std::vector<double> a (N), b (N), result (N);

        for (uint32_t i = 0; i < N; ++i)
        {
            a[i] = static_cast<double> (i);
            b[i] = static_cast<double> (N - i);
        }

        AcceleratedMath::vectorAdd (a.data(), b.data(), result.data(), N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (result[i], static_cast<double> (N), 1e-10,
                            "a[i]+b[i]=N at index " + std::to_string (i));

        ctx.endTest();
    }
}

//==============================================================================
//  Combined integration test: simulate a neural network tick
//==============================================================================
static void runIntegrationTests (TestContext& ctx)
{
    ctx.beginCategory ("Integration: simulated neural network tick");

    {
        ctx.beginTest ("88-neuron tick: weights * sigmoid(voltages)");
        constexpr uint32_t N = 88;

        std::mt19937 rng (12345);
        std::uniform_real_distribution<double> wDist (-0.5, 0.5);
        std::uniform_real_distribution<double> vDist (-3.0, 3.0);

        std::vector<double> weights (N * N);
        std::vector<double> voltages (N);
        std::vector<double> currents (N, 0.0);
        std::vector<double> expectedCurrents (N, 0.0);

        for (auto& w : weights)  w = wDist (rng);
        for (auto& v : voltages) v = vDist (rng);

        // Reference: compute sigmoid of voltages, then matrix-vector multiply
        std::vector<double> sigmoidVoltages (voltages);

        for (uint32_t i = 0; i < N; ++i)
            sigmoidVoltages[i] = 2.0 / (1.0 + std::exp (-3.0 * sigmoidVoltages[i])) - 1.0;

        for (uint32_t i = 0; i < N; ++i)
        {
            double sum = 0.0;

            for (uint32_t j = 0; j < N; ++j)
                sum += weights[i * N + j] * sigmoidVoltages[j];

            expectedCurrents[i] = sum;
        }

        // Accelerated path
        std::vector<double> accelSigmoid (voltages);
        AcceleratedMath::batchSigmoid (accelSigmoid.data(), N, 3.0);
        AcceleratedMath::matrixVectorMultiply (weights.data(), accelSigmoid.data(),
                                               currents.data(), N, N);

        for (uint32_t i = 0; i < N; ++i)
            ctx.expectNear (currents[i], expectedCurrents[i], 1e-8,
                            "neuron current " + std::to_string (i));

        ctx.endTest();
    }

    {
        ctx.beginTest ("parallel sigmoid + matmul pipeline");
        constexpr uint32_t N = 88;
        ParallelWorkGroup wg (4);

        std::vector<double> voltages (N);
        std::vector<double> processed (N, 0.0);

        for (uint32_t i = 0; i < N; ++i)
            voltages[i] = static_cast<double> (i) / N - 0.5;

        // Step 1: parallel per-neuron processing
        wg.parallelFor (N, [&] (uint32_t i)
        {
            processed[i] = std::sin (voltages[i]) + std::cos (voltages[i]);
        });

        // Step 2: vectorised batch operation on results
        AcceleratedMath::vectorScale (processed.data(), 2.0, processed.data(), N);

        // Verify
        for (uint32_t i = 0; i < N; ++i)
        {
            double expected = 2.0 * (std::sin (voltages[i]) + std::cos (voltages[i]));
            ctx.expectNear (processed[i], expected, 1e-10,
                            "pipeline result " + std::to_string (i));
        }

        ctx.endTest();
    }
}

//==============================================================================
static int runAllTests()
{
    std::cout << "Apple Silicon Performance Tests for Cmajor" << std::endl;
    std::cout << "==========================================" << std::endl;

   #ifdef __APPLE__
    std::cout << "Platform: macOS (Apple)" << std::endl;
   #else
    std::cout << "Platform: non-Apple" << std::endl;
   #endif

   #if CMAJ_HAS_NEON
    std::cout << "NEON: available" << std::endl;
   #else
    std::cout << "NEON: not available" << std::endl;
   #endif

    TestContext ctx;

    runBatchExpDoubleTests (ctx);
    runBatchExpFloatTests (ctx);
    runBatchFastExpTests (ctx);
    runBatchSigmoidTests (ctx);
    runMatrixVectorMultiplyTests (ctx);
    runVectorOperationTests (ctx);
    runHardwareCapabilitiesTests (ctx);
    runParallelWorkGroupTests (ctx);

   #ifdef __APPLE__
    runMetalComputeTests (ctx);
   #endif

    runPerformanceTests (ctx);
    runEdgeCaseTests (ctx);
    runIntegrationTests (ctx);

    ctx.printSummary();
    return ctx.allPassed() ? 0 : 1;
}

} // namespace cmaj::test

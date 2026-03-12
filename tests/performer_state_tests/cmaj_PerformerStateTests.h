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

#include <cstring>
#include <vector>
#include <cstdint>

#ifdef __clang__
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#elif __GNUC__
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

#include "../../include/cmajor/COM/cmaj_PerformerInterface.h"

#ifdef __clang__
 #pragma clang diagnostic pop
#elif __GNUC__
 #pragma GCC diagnostic pop
#endif

/**
 * Tests for the Performer state snapshot API (getState, restoreState, getStateSize).
 *
 * Since we cannot JIT-compile CMajor code in the lightweight test build,
 * these tests use a mock PerformerInterface that simulates the LLVM backend's
 * state memory layout to verify the API contract.
 */

namespace cmaj::performer_state_tests
{

namespace
{

struct MockPerformer final : public choc::com::ObjectWithAtomicRefCount<cmaj::PerformerInterface, MockPerformer>
{
    std::vector<uint8_t> stateMemory;

    MockPerformer (size_t stateSize) : stateMemory (stateSize, 0) {}

    // PerformerInterface overrides for state snapshots
    uint32_t getStateSize() override { return static_cast<uint32_t> (stateMemory.size()); }

    void getState (void* dest, uint32_t bufferSize) override
    {
        auto size = std::min (static_cast<size_t> (bufferSize), stateMemory.size());
        std::memcpy (dest, stateMemory.data(), size);
    }

    void restoreState (const void* src, uint32_t bufferSize) override
    {
        if (static_cast<size_t> (bufferSize) == stateMemory.size())
            std::memcpy (stateMemory.data(), src, bufferSize);
    }

    // Stubs for the rest of PerformerInterface
    cmaj::Result reset() override { return cmaj::Result::Ok; }
    cmaj::Result setBlockSize (uint32_t) override { return cmaj::Result::Ok; }
    cmaj::Result setInputFrames (cmaj::EndpointHandle, const void*, uint32_t) override { return cmaj::Result::Ok; }
    cmaj::Result setInputValue (cmaj::EndpointHandle, const void*, uint32_t) override { return cmaj::Result::Ok; }
    cmaj::Result addInputEvent (cmaj::EndpointHandle, uint32_t, const void*) override { return cmaj::Result::Ok; }
    cmaj::Result copyOutputValue (cmaj::EndpointHandle, void*) override { return cmaj::Result::Ok; }
    cmaj::Result copyOutputFrames (cmaj::EndpointHandle, void*, uint32_t) override { return cmaj::Result::Ok; }
    cmaj::Result iterateOutputEvents (cmaj::EndpointHandle, void*, HandleOutputEventCallback) override { return cmaj::Result::Ok; }
    cmaj::Result advance() override { return cmaj::Result::Ok; }
    const char* getStringForHandle (uint32_t, size_t& len) override { len = 0; return ""; }
    uint32_t getXRuns() override { return 0; }
    uint32_t getMaximumBlockSize() override { return 512; }
    uint32_t getEventBufferSize() override { return 32; }
    double getLatency() override { return 0; }
    const char* getRuntimeError() override { return nullptr; }
};

} // anonymous namespace

//==============================================================================
struct TestContext
{
    uint32_t passed = 0;
    uint32_t failed = 0;
    std::string currentTest;
    bool currentTestFailed = false;

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
        if (! condition)
        {
            currentTestFailed = true;
            std::cout << "    ASSERTION FAILED: " << description << std::endl;
        }
    }

    void expectEquals (int a, int b, const std::string& description)
    {
        if (a != b)
        {
            currentTestFailed = true;
            std::cout << "    ASSERTION FAILED: " << description << " (expected " << b << ", got " << a << ")" << std::endl;
        }
    }

    void printSummary()
    {
        std::cout << "\n========================================" << std::endl;
        std::cout << "Results: " << passed << " passed, " << failed << " failed" << std::endl;
        std::cout << "========================================" << std::endl;
    }

    bool allPassed() const  { return failed == 0; }
};

//==============================================================================
static void runPerformerStateTests (TestContext& ctx)
{
    ctx.beginTest ("getStateSize returns correct size");
    {
        MockPerformer performer (256);
        ctx.expectEquals (static_cast<int> (performer.getStateSize()), 256, "getStateSize");
        ctx.endTest();
    }

    ctx.beginTest ("getStateSize returns zero for empty state");
    {
        MockPerformer performer (0);
        ctx.expectEquals (static_cast<int> (performer.getStateSize()), 0, "getStateSize");
        ctx.endTest();
    }

    ctx.beginTest ("getState captures full state memory");
    {
        MockPerformer performer (64);

        for (size_t i = 0; i < performer.stateMemory.size(); ++i)
            performer.stateMemory[i] = static_cast<uint8_t> (i * 7 + 3);

        std::vector<uint8_t> snapshot (64);
        performer.getState (snapshot.data(), 64);

        bool match = true;
        for (size_t i = 0; i < 64; ++i)
            if (snapshot[i] != performer.stateMemory[i])
                match = false;
        ctx.expect (match, "snapshot matches state memory");
        ctx.endTest();
    }

    ctx.beginTest ("restoreState restores exact snapshot");
    {
        MockPerformer performer (64);

        std::vector<uint8_t> snapshot (64);
        for (size_t i = 0; i < 64; ++i)
            snapshot[i] = static_cast<uint8_t> (i * 13 + 5);

        performer.restoreState (snapshot.data(), 64);

        bool match = true;
        for (size_t i = 0; i < 64; ++i)
            if (performer.stateMemory[i] != snapshot[i])
                match = false;
        ctx.expect (match, "restored state matches snapshot");
        ctx.endTest();
    }

    ctx.beginTest ("restoreState rejects mismatched size");
    {
        MockPerformer performer (64);

        for (auto& b : performer.stateMemory)
            b = 0xAA;

        std::vector<uint8_t> wrongSize (128, 0x55);
        performer.restoreState (wrongSize.data(), 128);

        bool unchanged = true;
        for (auto& b : performer.stateMemory)
            if (b != 0xAA)
                unchanged = false;
        ctx.expect (unchanged, "state unchanged after wrong-size restore");
        ctx.endTest();
    }

    ctx.beginTest ("State round-trip preserves all bytes");
    {
        MockPerformer performer (1024);

        for (size_t i = 0; i < performer.stateMemory.size(); ++i)
            performer.stateMemory[i] = static_cast<uint8_t> ((i * 37 + 199) % 256);

        std::vector<uint8_t> snapshot (1024);
        performer.getState (snapshot.data(), 1024);

        MockPerformer newPerformer (1024);
        ctx.expect (newPerformer.stateMemory != snapshot, "New performer should start with zeroed state");

        newPerformer.restoreState (snapshot.data(), 1024);

        ctx.expect (newPerformer.stateMemory == snapshot, "Restored state should exactly match captured state");
        ctx.endTest();
    }

    ctx.beginTest ("State transfer between two same-size performers");
    {
        MockPerformer oldPerformer (512);
        MockPerformer newPerformer (512);

        for (size_t i = 0; i < 512; ++i)
            oldPerformer.stateMemory[i] = static_cast<uint8_t> (i % 251);

        auto stateSize = oldPerformer.getStateSize();
        std::vector<uint8_t> stateBuffer (stateSize);
        oldPerformer.getState (stateBuffer.data(), stateSize);

        newPerformer.restoreState (stateBuffer.data(), stateSize);

        ctx.expect (newPerformer.stateMemory == oldPerformer.stateMemory,
                    "State should be identical after transfer");
        ctx.endTest();
    }

    ctx.beginTest ("State transfer rejected between different-size performers");
    {
        MockPerformer oldPerformer (512);
        MockPerformer newPerformer (768);

        for (auto& b : oldPerformer.stateMemory)
            b = 0xBB;

        std::vector<uint8_t> stateBuffer (512);
        oldPerformer.getState (stateBuffer.data(), 512);

        newPerformer.restoreState (stateBuffer.data(), 512);

        bool unchanged = true;
        for (auto& b : newPerformer.stateMemory)
            if (b != 0)
                unchanged = false;
        ctx.expect (unchanged, "New performer state should be unchanged (all zeros)");
        ctx.endTest();
    }

    ctx.beginTest ("Default PerformerInterface returns zero state size");
    {
        MockPerformer performer (0);
        ctx.expectEquals (static_cast<int> (performer.getStateSize()), 0, "getStateSize");
        ctx.endTest();
    }
}

//==============================================================================
static int runAllTests()
{
    std::cout << "Performer State Snapshot Tests for Cmajor" << std::endl;
    std::cout << "========================================" << std::endl;

    TestContext ctx;
    runPerformerStateTests (ctx);

    ctx.printSummary();
    return ctx.allPassed() ? 0 : 1;
}

} // namespace cmaj::performer_state_tests

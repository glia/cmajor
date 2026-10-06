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

#include "../../../../include/cmajor/API/cmaj_Engine.h"
#include "choc/platform/choc_UnitTest.h"

namespace cmaj::performer_state_tests
{

/// A processor whose only state is a frame counter, so that a snapshot taken
/// after N frames is observable: the next output frame must be N.
static constexpr const char* counterSource = R"(
    processor Counter [[ main ]]
    {
        output stream float32 out;
        float32 count = 0;

        void main()
        {
            loop
            {
                out <- count;
                count += 1.0f;
                advance();
            }
        }
    }
)";

struct LinkedCounter
{
    cmaj::Engine engine;
    cmaj::EndpointHandle outHandle = {};
    cmaj::DiagnosticMessageList messages;
    bool ok = false;

    LinkedCounter (uint32_t maxBlockSize)
    {
        engine = cmaj::Engine::create();
        cmaj::Program program;

        if (! program.parse (messages, "Counter.cmajor", counterSource))
            return;

        engine.setBuildSettings (cmaj::BuildSettings().setFrequency (44100.0)
                                                      .setMaxBlockSize (maxBlockSize));

        if (! engine.load (messages, program, {}, {}))
            return;

        outHandle = engine.getEndpointHandle ("out");

        if (! engine.link (messages, {}))
            return;

        ok = outHandle != 0;
    }

    /// Renders one block and returns the first output sample.
    static float renderBlock (cmaj::Performer& p, cmaj::EndpointHandle out, uint32_t numFrames)
    {
        auto block = choc::buffer::InterleavedBuffer<float> (1, numFrames);
        p.setBlockSize (numFrames);
        p.advance();
        p.copyOutputFrames (out, block);
        return block.getSample (0, 0);
    }
};

inline void runUnitTests (choc::test::TestProgress& progress)
{
    CHOC_CATEGORY (PerformerState);

    {
        CHOC_TEST (UnlinkedPerformerHasNoState)

        cmaj::Performer empty;
        CHOC_EXPECT_EQ (empty.getStateSize(), 0u);
        CHOC_EXPECT_TRUE (empty.getState().empty());
        CHOC_EXPECT_FALSE (empty.restoreState ({}));
        CHOC_EXPECT_FALSE (empty.restoreState (std::vector<uint8_t> (16)));
    }

    {
        CHOC_TEST (SnapshotRestoresCounterOnSamePerformer)

        LinkedCounter counter (4);

        if (! counter.ok)
        {
            CHOC_FAIL ("Failed to build the counter program: " + counter.messages.toString());
            return;
        }

        auto performer = counter.engine.createPerformer();
        CHOC_EXPECT_TRUE (performer);

        auto stateSize = performer.getStateSize();
        CHOC_EXPECT_TRUE (stateSize > 0);

        // two blocks of 4 frames: the counter reaches 8
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 0.0f, 0.0001f);
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 4.0f, 0.0001f);

        auto snapshot = performer.getState();
        CHOC_EXPECT_EQ (snapshot.size(), static_cast<size_t> (stateSize));

        // keep going: 8, then 12
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 8.0f, 0.0001f);
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 12.0f, 0.0001f);

        // ...and rewind to the snapshot: the next block must start at 8 again
        CHOC_EXPECT_TRUE (performer.restoreState (snapshot));
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 8.0f, 0.0001f);
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 12.0f, 0.0001f);
    }

    {
        CHOC_TEST (SnapshotTransfersBetweenPerformersOfTheSameProgram)

        LinkedCounter counter (4);

        if (! counter.ok)
        {
            CHOC_FAIL ("Failed to build the counter program: " + counter.messages.toString());
            return;
        }

        auto a = counter.engine.createPerformer();
        auto b = counter.engine.createPerformer();
        CHOC_EXPECT_TRUE (a);
        CHOC_EXPECT_TRUE (b);
        CHOC_EXPECT_EQ (a.getStateSize(), b.getStateSize());

        for (int i = 0; i < 3; ++i)
            LinkedCounter::renderBlock (a, counter.outHandle, 4);   // a is at 12

        CHOC_EXPECT_TRUE (b.restoreState (a.getState()));
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (b, counter.outHandle, 4), 12.0f, 0.0001f);
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (b, counter.outHandle, 4), 16.0f, 0.0001f);

        // a was not affected by b's restore
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (a, counter.outHandle, 4), 12.0f, 0.0001f);
    }

    {
        CHOC_TEST (WrongSizeSnapshotIsRejected)

        LinkedCounter counter (4);

        if (! counter.ok)
        {
            CHOC_FAIL ("Failed to build the counter program: " + counter.messages.toString());
            return;
        }

        auto performer = counter.engine.createPerformer();
        LinkedCounter::renderBlock (performer, counter.outHandle, 4);   // at 4

        std::vector<uint8_t> tooSmall (performer.getStateSize() / 2);
        std::vector<uint8_t> tooBig (performer.getStateSize() + 1);
        CHOC_EXPECT_FALSE (performer.restoreState (tooSmall));
        CHOC_EXPECT_FALSE (performer.restoreState (tooBig));
        CHOC_EXPECT_FALSE (performer.restoreState ({}));

        // state untouched
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 4.0f, 0.0001f);
    }

    {
        CHOC_TEST (ResetAfterRestoreStillReturnsToInitialState)

        LinkedCounter counter (4);

        if (! counter.ok)
        {
            CHOC_FAIL ("Failed to build the counter program: " + counter.messages.toString());
            return;
        }

        auto performer = counter.engine.createPerformer();
        LinkedCounter::renderBlock (performer, counter.outHandle, 4);
        auto snapshot = performer.getState();
        LinkedCounter::renderBlock (performer, counter.outHandle, 4);
        CHOC_EXPECT_TRUE (performer.restoreState (snapshot));
        performer.reset();
        CHOC_EXPECT_NEAR (LinkedCounter::renderBlock (performer, counter.outHandle, 4), 0.0f, 0.0001f);
    }
}

} // namespace cmaj::performer_state_tests

// Copyright 2016 Dolphin Emulator Project / 2017 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <catch2/catch.hpp>

#include <array>
#include <bitset>
#include <cstdlib>
#include <memory>
#include <string>

#include "common/file_util.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace {
// Numbers are chosen randomly to make sure the correct one is given.
constexpr std::array<u64, 5> CB_IDS{{42, 144, 93, 1026, UINT64_C(0xFFFF7FFFF7FFFF)}};
constexpr int MAX_SLICE_LENGTH = 10000; // Copied from CoreTiming internals

std::bitset<CB_IDS.size()> callbacks_ran_flags;
u64 expected_callback = 0;
s64 lateness = 0;

template <unsigned int IDX>
void CallbackTemplate(u64 userdata, s64 cycles_late) {
    static_assert(IDX < CB_IDS.size(), "IDX out of range");
    callbacks_ran_flags.set(IDX);
    REQUIRE(CB_IDS[IDX] == userdata);
    REQUIRE(CB_IDS[IDX] == expected_callback);
    REQUIRE(lateness == cycles_late);
}

u64 callbacks_done = 0;

void EmptyCallback(u64 userdata, s64 cycles_late) {
    ++callbacks_done;
}

struct ScopeInit final {
    ScopeInit() {
        core_timing.Initialize();
    }
    ~ScopeInit() {
        core_timing.Shutdown();
    }

    Core::Timing::CoreTiming core_timing;
};

void AdvanceAndCheck(Core::Timing::CoreTiming& core_timing, u32 idx, u32 context = 0,
                     int expected_lateness = 0, int cpu_downcount = 0) {
    callbacks_ran_flags = 0;
    expected_callback = CB_IDS[idx];
    lateness = expected_lateness;

    // Pretend we executed X cycles of instructions.
    core_timing.SwitchContext(context);
    core_timing.AddTicks(core_timing.GetDowncount() - cpu_downcount);
    core_timing.Advance();
    core_timing.SwitchContext((context + 1) % 4);

    REQUIRE(decltype(callbacks_ran_flags)().set(idx) == callbacks_ran_flags);
}
} // Anonymous namespace

TEST_CASE("CoreTiming[BasicOrder]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;

    std::shared_ptr<Core::Timing::EventType> cb_a =
        Core::Timing::CreateEvent("callbackA", CallbackTemplate<0>);
    std::shared_ptr<Core::Timing::EventType> cb_b =
        Core::Timing::CreateEvent("callbackB", CallbackTemplate<1>);
    std::shared_ptr<Core::Timing::EventType> cb_c =
        Core::Timing::CreateEvent("callbackC", CallbackTemplate<2>);
    std::shared_ptr<Core::Timing::EventType> cb_d =
        Core::Timing::CreateEvent("callbackD", CallbackTemplate<3>);
    std::shared_ptr<Core::Timing::EventType> cb_e =
        Core::Timing::CreateEvent("callbackE", CallbackTemplate<4>);

    // Enter slice 0
    core_timing.ResetRun();

    // D -> B -> C -> A -> E
    core_timing.SwitchContext(0);
    core_timing.ScheduleEvent(1000, cb_a, CB_IDS[0]);
    REQUIRE(1000 == core_timing.GetDowncount());
    core_timing.ScheduleEvent(500, cb_b, CB_IDS[1]);
    REQUIRE(500 == core_timing.GetDowncount());
    core_timing.ScheduleEvent(800, cb_c, CB_IDS[2]);
    REQUIRE(500 == core_timing.GetDowncount());
    core_timing.ScheduleEvent(100, cb_d, CB_IDS[3]);
    REQUIRE(100 == core_timing.GetDowncount());
    core_timing.ScheduleEvent(1200, cb_e, CB_IDS[4]);
    REQUIRE(100 == core_timing.GetDowncount());

    AdvanceAndCheck(core_timing, 3, 0);
    AdvanceAndCheck(core_timing, 1, 1);
    AdvanceAndCheck(core_timing, 2, 2);
    AdvanceAndCheck(core_timing, 0, 3);
    AdvanceAndCheck(core_timing, 4, 0);
}

TEST_CASE("CoreTiming[FairSharing]", "[core]") {

    ScopeInit guard;
    auto& core_timing = guard.core_timing;

    std::shared_ptr<Core::Timing::EventType> empty_callback =
        Core::Timing::CreateEvent("empty_callback", EmptyCallback);

    callbacks_done = 0;
    u64 MAX_CALLBACKS = 10;
    for (std::size_t i = 0; i < 10; i++) {
        core_timing.ScheduleEvent(i * 3333U, empty_callback, 0);
    }

    const s64 advances = MAX_SLICE_LENGTH / 10;
    core_timing.ResetRun();
    u64 current_time = core_timing.GetTicks();
    bool keep_running{};
    do {
        keep_running = false;
        for (u32 active_core = 0; active_core < 4; ++active_core) {
            core_timing.SwitchContext(active_core);
            if (core_timing.CanCurrentContextRun()) {
                core_timing.AddTicks(std::min<s64>(advances, core_timing.GetDowncount()));
                core_timing.Advance();
            }
            keep_running |= core_timing.CanCurrentContextRun();
        }
    } while (keep_running);
    u64 current_time_2 = core_timing.GetTicks();

    REQUIRE(MAX_CALLBACKS == callbacks_done);
    REQUIRE(current_time_2 == current_time + MAX_SLICE_LENGTH * 4);
}

TEST_CASE("Core::Timing[PredictableLateness]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;

    std::shared_ptr<Core::Timing::EventType> cb_a =
        Core::Timing::CreateEvent("callbackA", CallbackTemplate<0>);
    std::shared_ptr<Core::Timing::EventType> cb_b =
        Core::Timing::CreateEvent("callbackB", CallbackTemplate<1>);

    // Enter slice 0
    core_timing.ResetRun();

    core_timing.ScheduleEvent(100, cb_a, CB_IDS[0]);
    core_timing.ScheduleEvent(200, cb_b, CB_IDS[1]);

    AdvanceAndCheck(core_timing, 0, 0, 10, -10); // (100 - 10)
    AdvanceAndCheck(core_timing, 1, 1, 50, -50);
}

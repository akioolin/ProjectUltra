#include "sim/simulated_station.hpp"

#include <cassert>
#include <iostream>

namespace {

constexpr uint32_t kSampleRate = 48000;
constexpr size_t kTrSwitchSamples = kSampleRate * 20 / 1000;
constexpr size_t kCooldownSamples = kSampleRate * 100 / 1000;

void expectState(const RadioPttStateMachine& ptt,
                 PttState expected,
                 bool blackout,
                 bool ready_for_tx) {
    assert(ptt.state() == expected);
    assert(ptt.isInRxBlackout() == blackout);
    assert(ptt.isReadyForNextTx() == ready_for_tx);
}

}  // namespace

int main() {
    RadioPttStateMachine ptt(kSampleRate);
    expectState(ptt, PttState::RX, false, true);

    ptt.noteTxQueued(960);
    expectState(ptt, PttState::TX, true, false);

    ptt.noteTxDrained(480);
    expectState(ptt, PttState::TX, true, false);

    ptt.noteTxDrained(480);
    expectState(ptt, PttState::TX_TR_SWITCH, true, false);

    ptt.advanceSamples(kTrSwitchSamples - 1);
    expectState(ptt, PttState::TX_TR_SWITCH, true, false);

    ptt.advanceSamples(1);
    expectState(ptt, PttState::TX_COOLDOWN, false, false);

    ptt.advanceSamples(kCooldownSamples - 1);
    expectState(ptt, PttState::TX_COOLDOWN, false, false);

    ptt.advanceSamples(1);
    expectState(ptt, PttState::RX, false, true);

    ptt.setRecoveryTimings(0, 50);
    ptt.noteTxQueued(480);
    ptt.noteTxDrained(480);
    expectState(ptt, PttState::TX_COOLDOWN, false, false);
    ptt.advanceSamples(kSampleRate * 50 / 1000);
    expectState(ptt, PttState::RX, false, true);

    ptt.setRecoveryTimings(0, 0);
    ptt.noteTxQueued(480);
    ptt.noteTxDrained(480);
    expectState(ptt, PttState::RX, false, true);

    ptt.setRecoveryTimings(20, 100);
    ptt.noteTxSampleBlock(true);
    expectState(ptt, PttState::TX, true, false);
    ptt.noteTxSampleBlock(false, true);
    expectState(ptt, PttState::TX, true, false);
    ptt.noteTxSampleBlock(false, false);
    expectState(ptt, PttState::TX_TR_SWITCH, true, false);
    ptt.advanceSamples(kTrSwitchSamples);
    expectState(ptt, PttState::TX_COOLDOWN, false, false);
    ptt.advanceSamples(kCooldownSamples);
    expectState(ptt, PttState::RX, false, true);

    std::cout << "simulated radio PTT state separates TX deafness, T/R switch, and cooldown\n";
    return 0;
}

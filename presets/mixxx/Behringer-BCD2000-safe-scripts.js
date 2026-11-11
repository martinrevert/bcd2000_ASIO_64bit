/**
 * Behringer BCD2000 Safe Controller Scripts for Mixxx
 *
 * Based on the original Mixxx BCD2000 mapping by Golzo and the Mixxx Development Team.
 * Adapted for Behringer BCD2000 64-bit ASIO and MIDI Driver Suite.
 *
 * Copyright (C) 2011-2024 Golzo & The Mixxx Development Team
 * Copyright (C) 2024-2026 Martin Revert <martinrevert@gmail.com> and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

function BehringerBCD2000Safe() {}

BehringerBCD2000Safe.debug = false;
BehringerBCD2000Safe.escratch = [false, false];

// Jog wheel sensitivity and hardware acceleration
BehringerBCD2000Safe.UseAcceleration = true;
BehringerBCD2000Safe.JogSensitivity = 0.2;

// Track per-CC soft-takeover state so knobs physically left at 0 never
// unexpectedly cut audio on startup or on an accidental slight nudge.
BehringerBCD2000Safe.engaged = {};
BehringerBCD2000Safe.prevParam = {};

BehringerBCD2000Safe.knobTargets = {
    0x00: { group: "[Channel1]", key: "volume" },
    0x01: { group: "[Master]", key: "crossfader" },
    0x02: { group: "[Channel2]", key: "volume" },
    0x03: { group: "[EqualizerRack1_[Channel1]_Effect1]", key: "parameter1" },
    0x04: { group: "[EqualizerRack1_[Channel1]_Effect1]", key: "parameter2" },
    0x05: { group: "[EqualizerRack1_[Channel1]_Effect1]", key: "parameter3" },
    0x06: { group: "[Channel1]", key: "pregain" },
    0x07: { group: "[EqualizerRack1_[Channel2]_Effect1]", key: "parameter1" },
    0x08: { group: "[EqualizerRack1_[Channel2]_Effect1]", key: "parameter2" },
    0x09: { group: "[EqualizerRack1_[Channel2]_Effect1]", key: "parameter3" },
    0x0A: { group: "[Channel2]", key: "pregain" },
    0x0B: { group: "[Channel1]", key: "rate" },
    0x0C: { group: "[Channel2]", key: "rate" },
    0x10: { group: "[Master]", key: "gain" },
    0x11: { group: "[Master]", key: "headMix" }
};

BehringerBCD2000Safe.init = function (id, debugging) {
    BehringerBCD2000Safe.reset();
    BehringerBCD2000Safe.engaged = {};
    BehringerBCD2000Safe.prevParam = {};
    BehringerBCD2000Safe.escratch = [false, false];

    // NOTE: Intentionally do NOT send (0xB0, 0x64, 0x7F) on init.
    // Sending 0x64 asks the BCD2000 to dump every knob/slider at startup,
    // which immediately sets Mixxx EQs/Master to 0 if hardware pots are at 0.
    // Combined with safeKnob() below, audio starts intact regardless of pot positions.

    // Enable Mixxx soft-takeover for subsequent GUI/deck changes as well
    for (var cc in BehringerBCD2000Safe.knobTargets) {
        if (Object.prototype.hasOwnProperty.call(BehringerBCD2000Safe.knobTargets, cc)) {
            var target = BehringerBCD2000Safe.knobTargets[cc];
            engine.softTakeover(target.group, target.key, true);
        }
    }

    // Set jog wheel hardware acceleration
    if (BehringerBCD2000Safe.UseAcceleration) {
        midi.sendShortMsg(0xB0, 0x63, 0x7F);
    } else {
        midi.sendShortMsg(0xB0, 0x63, 0x00);
    }
};

BehringerBCD2000Safe.shutdown = function () {
    BehringerBCD2000Safe.reset();

    // Restore default jog acceleration on shutdown
    if (!BehringerBCD2000Safe.UseAcceleration) {
        midi.sendShortMsg(0xB0, 0x63, 0x7F);
    }
};

BehringerBCD2000Safe.reset = function () {
    // Turn off all panel LEDs
    for (var i = 0; i <= 25; i++) {
        midi.sendShortMsg(0xB0, i, 0x00);
    }
};

BehringerBCD2000Safe.getDeck = function (group) {
    if (group === "[Channel1]") {
        return 0;
    } else if (group === "[Channel2]") {
        return 1;
    }
    print("BehringerBCD2000Safe: Invalid group: " + group);
    return -1;
};

BehringerBCD2000Safe.safeKnob = function (channel, control, value, status, group) {
    var target = BehringerBCD2000Safe.knobTargets[control];
    if (!target) {
        return;
    }

    var param = value / 127.0;

    if (!BehringerBCD2000Safe.engaged[control]) {
        var current = engine.getParameter(target.group, target.key);
        var prev = BehringerBCD2000Safe.prevParam[control];
        var closeEnough = Math.abs(param - current) <= 0.06;
        var crossed = (prev !== undefined) &&
            ((prev < current && param >= current) || (prev > current && param <= current));

        BehringerBCD2000Safe.prevParam[control] = param;

        if (!closeEnough && !crossed) {
            return;
        }
        BehringerBCD2000Safe.engaged[control] = true;
    }

    engine.setParameter(target.group, target.key, param);
};

BehringerBCD2000Safe.jogWheel = function (channel, control, value, status, group) {
    var deck = BehringerBCD2000Safe.getDeck(group);
    if (deck < 0) {
        return;
    }

    if (BehringerBCD2000Safe.escratch[deck]) {
        var scratchValue = (value >= 65) ? (value - 0x40) : (value - 0x41);
        engine.scratchTick(deck + 1, scratchValue);
        if (BehringerBCD2000Safe.debug) {
            print(group + " scratch tick: " + scratchValue);
        }
    } else {
        var delta = (value >= 65) ? (value - 0x40) : (value - 0x41);
        var jogValue = delta * BehringerBCD2000Safe.JogSensitivity;
        engine.setValue(group, "jog", jogValue);
        if (BehringerBCD2000Safe.debug) {
            print(group + " jog adjust: " + jogValue);
        }
    }
};

BehringerBCD2000Safe.scratchButton = function (channel, control, value, status, group) {
    if (value !== 0x7F) {
        return;
    }

    var deck = BehringerBCD2000Safe.getDeck(group);
    if (deck < 0) {
        return;
    }

    BehringerBCD2000Safe.escratch[deck] = !BehringerBCD2000Safe.escratch[deck];

    if (BehringerBCD2000Safe.escratch[deck]) {
        midi.sendShortMsg(0xB0, (deck === 0) ? 0x13 : 0x0B, 0x7F);
        engine.scratchEnable(deck + 1, 100, 33 + 1 / 3, 1.0 / 8.0, (1.0 / 8.0) / 32.0);
    } else {
        midi.sendShortMsg(0xB0, (deck === 0) ? 0x13 : 0x0B, 0x00);
        engine.scratchDisable(deck + 1);
    }
};

// SPDX-FileCopyrightText: 2026 gdxsv contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

// Timing statistics of a rollback battle for P2PMatchingReport: rollbacks and
// their time per frame, Present and input intervals, and the frame limiter's
// slack. Recorded while a GGPO session runs (the recording hooks are in
// gdxsv_ggpo_hooks.h) in fixed-size histograms, without locks or allocation;
// those of the last session are kept until the next one or a reset.

namespace proto {
class P2PMatchingReport;
}

// Drops the samples of the last session.
void gdxsv_net_report_reset();
// A frame was presented (vsync blocking included).
void gdxsv_net_report_present();
// The frame limiter woke up; remainingUs: the time it slept, negative when the frame was late.
void gdxsv_net_report_wake(int64_t remainingUs);
// Sets the timing fields of the report.
void gdxsv_net_report_fill(proto::P2PMatchingReport* report);

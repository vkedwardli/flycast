#pragma once

// Sleeps out the rest of the FixedFrequency frame period. Called once per
// rendered frame from the main UI loop.
void gdxsv_frame_limiter_wait();

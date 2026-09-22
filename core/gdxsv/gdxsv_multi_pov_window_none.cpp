// Fallback backend for targets with no desktop windows to arrange - Android,
// the Switch, UWP. 4-player replay spawns desktop processes and tiles their
// windows, neither of which those platforms have, so the grid reports itself
// unavailable and the feature simply never engages there.
//
// This is not a placeholder for Windows, Linux or macOS: each of those has a
// real implementation alongside this file.
#include "gdxsv_multi_pov_window.h"

bool gdxsv_multi_pov_window_available() { return false; }

GdxsvMultiPovRect gdxsv_multi_pov_window_get_frame() { return {}; }

void gdxsv_multi_pov_window_set_frame(const GdxsvMultiPovRect&) {}

bool gdxsv_multi_pov_window_is_maximized() { return false; }

void gdxsv_multi_pov_window_unmaximize() {}

GdxsvMultiPovRect gdxsv_multi_pov_window_work_area() { return {}; }

void gdxsv_multi_pov_window_set_borderless(bool) {}


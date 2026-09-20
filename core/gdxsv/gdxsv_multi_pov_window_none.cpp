// Fallback backend for targets with no desktop windows to arrange - Android,
// the Switch, UWP. 4-player replay spawns desktop processes and tiles their
// windows, neither of which those platforms have, so the grid reports itself
// unavailable and the feature simply never engages there.
//
// This is not a placeholder for Windows, Linux or macOS: each of those has a
// real implementation alongside this file.
#include "gdxsv_multi_pov_window.h"

namespace gdxsv_multi_pov {
namespace window {

bool Available() { return false; }

WindowRect GetFrame() { return {}; }

void SetFrame(const WindowRect&) {}

bool IsMaximized() { return false; }

void Unmaximize() {}

WindowRect WorkArea() { return {}; }

void SetBorderless(bool) {}

}  // namespace window
}  // namespace gdxsv_multi_pov

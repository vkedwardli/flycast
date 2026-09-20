#pragma once
#include "gdxsv_multi_pov.h"

// Window control for the 4-player replay grid: what makes four OS windows
// behave as one.
//
// The host's window is the one the user interacts with. Everything else is
// derived from it: the grid covers twice the host's width and height, the
// guests take the other three quadrants, and when the host is maximized the
// grid becomes the display's work area so the four tile it exactly.
namespace gdxsv_multi_pov {

// The per-platform half. One implementation per platform - Win32, Cocoa and
// SDL for Linux - selected at build time; a platform with no desktop windows
// at all (Android, Switch, UWP) links a version that reports unavailable, so
// the policy above never has to know which it is.
//
// Rectangles are the window's *content* area in desktop coordinates, with the
// origin at the top-left of the primary display: the one convention all three
// platforms can be pinned to, and the one the grid math is written in.
namespace window {

// False when this build or this run has no window to move (headless, or a
// platform without desktop windows). Everything else is only called when this
// is true.
bool Available();

WindowRect GetFrame();

// Moves and resizes without raising or focusing: a follower must never steal
// the keyboard from the screen the user is driving.
void SetFrame(const WindowRect& rect);

bool IsMaximized();

// Leaves the maximized state, keeping the window on the same display. The
// host does this before taking its own quadrant - a maximized window cannot
// also be a quarter of the screen.
void Unmaximize();

// Usable area of the display the window is on: the whole screen less the task
// bar, dock, menu bar or panel. This is what "maximized" tiles.
WindowRect WorkArea();

// Guests drop their decorations so the grid reads as one window.
void SetBorderless(bool borderless);

}  // namespace window

// Runs every frame on the UI thread (window calls are main-thread-only on
// macOS and Windows alike): the host publishes where the grid is, the guests
// put themselves in it. A no-op outside a 4-screen session.
void WindowTick();

}  // namespace gdxsv_multi_pov

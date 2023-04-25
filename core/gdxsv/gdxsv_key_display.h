#pragma once

#include <array>
#include <deque>

class GdxsvKeyDisplay {
public:
    enum KeyCode {
        UP = 0x2000,
        DOWN = 0x1000,
        LEFT = 0x0800,
        RIGHT = 0x0400,
        SEARCH = 0x0040,
        JUMP = 0x0080,
        SHOOT = 0x0200,
        COMBAT = 0x0100,
        COOP = 0x0020,
    };

    struct KeyInput {
        int code;
        int frame;
    };

    void DisplayOSD();
    void AppendInput(int player, KeyInput input);
    void SetCurrentFrame(int frame);

private:
    int current_frame;
    std::array<std::deque<KeyInput>, 4> history;
};
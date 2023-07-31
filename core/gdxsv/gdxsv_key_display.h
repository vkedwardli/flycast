#pragma once

#include <array>
#include <deque>

#include "types.h"

class GdxsvKeyDisplay {
   public:
	void DisplayOSD();
	void AppendInput(int player, u16 mcs_key);
	void SetDisplayPlayer(int player);
	void Clear();

	void enabled(bool enabled) { enabled_ = enabled; }
	bool enabled() const { return enabled_; }

   private:
	struct McsPadInput {
		int frame;
		u16 code;
	};

	bool enabled_ = false;
	int display_player_ = 0;
	std::array<std::deque<McsPadInput>, 4> history_;
};

#pragma once
#include <cstdint>
#include <string>

// Keeps several spectator instances on one machine playing the same frame.
//
// Each instance publishes the frame it has reached into a shared mmap'd file
// and holds back for peers that are behind. Nothing goes over the network: the
// instances are local and a frame index means the same thing to all of them.
class GdxsvSpectateSync {
   public:
	~GdxsvSpectateSync();

	// Joins the named group, claiming a slot. No-op if group is empty, which
	// is how a lone spectator opts out.
	void Join(const std::string& group);
	bool Active() const { return slot_ != nullptr; }

	// Publishes the frame this instance has reached.
	void Publish(int32_t frame);

	// How far ahead of the slowest live peer this instance is, in position
	// units. 0 when alone, when a peer is still catching up, or when we are
	// not ahead - so it is safe to feed straight into the frame period.
	int32_t LeadOverSlowest(int32_t frame) const;

	// Holds this instance back while any live peer is more than kSyncSlack
	// behind, so the group cannot drift apart.
	//
	// Returns true if the group synced up, false if it gave up: alone, a
	// peer too far away to wait for (still catching up), or the deadline
	// passed. Playback continues either way - a spectator must never hang.
	bool WaitForPeers(int32_t frame, int max_wait_ms);

   private:
	void* map_ = nullptr;
	size_t map_size_ = 0;
	struct Slot* slot_ = nullptr;
};

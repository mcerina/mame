// license:BSD-3-Clause
/***************************************************************************

    rbtest.h

    Rollback feasibility harness (netplay milestone M1, "local rollback").

    Enabled by the MAME_RBTEST environment variable, e.g.
      MAME_RBTEST="mode=rollback,players=2,latency=4,jitter=3,window=10,frames=10800,out=/out/kod"

    A deterministic input bot drives every player. In "rollback" mode player 1
    is local and the others are fake remote peers whose inputs arrive late;
    missing inputs are predicted (last known value) and corrected with a real
    rollback: load the in-memory state of the first mispredicted frame and
    resimulate up to the present in one host frame, with video, audio and
    throttle disabled. A hash of the whole save state is logged for every
    confirmed frame, so the run can be compared bit for bit with a "plain"
    run that uses the same inputs and never rolls back.

***************************************************************************/

#ifndef MAME_EMU_RBTEST_H
#define MAME_EMU_RBTEST_H

#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>


class rollback_tester
{
public:
	enum class mode { PLAIN, SAVEALL, ROLLBACK, SYNCTEST };

	static std::unique_ptr<rollback_tester> create(running_machine &machine);
	rollback_tester(running_machine &machine, const std::string &config);
	~rollback_tester();

	// hooks
	void frame_inputs_begin();          // ioport frame_update start (end of the current frame)
	void frame_inputs_port(ioport_port &port); // per port, after the OS sample
	void post_timeslice();              // main loop, after every timeslice
	bool resimulating() const { return m_resim; }

private:
	static constexpr int MAXP = 8;
	enum { S_UP, S_DOWN, S_LEFT, S_RIGHT, S_B1, S_B2, S_B3, S_B4, S_COIN, S_START, S_COUNT };

	struct slot
	{
		s64 frame = -1;
		bool valid = false;
		std::vector<u8> data;
	};

	struct frame_record
	{
		s64 frame = -1;
		u16 used[MAXP];                  // semantic input actually applied per player
		std::vector<ioport_value> other; // non-player digital bits per port (OS sample)
	};

	struct stats
	{
		std::vector<float> v;
		void add(double x) { v.push_back(float(x)); }
		std::string json(const char *name);
	};

	// input model
	u16 truth(int player, s64 frame) const;
	u16 best(int player, s64 frame) const;
	ioport_value compose(int portidx, const u16 *inputs, ioport_value other) const;
	void apply_frame_to_ports(s64 frame);
	void update_arrivals(s64 boundary);

	// state ring
	slot &slot_for(s64 frame) { return m_slots[frame % m_slots.size()]; }
	frame_record &rec_for(s64 frame) { return m_recs[frame % m_recs.size()]; }
	bool save_slot(s64 frame);
	bool load_slot(s64 frame);
	u64 hash_buffer(const std::vector<u8> &buf);
	u64 hash_logic();
	static u64 hash_raw(const std::vector<u8> &buf);
	void log_confirmed(s64 upto);
	void run_one_frame();
	void finish();

	running_machine &m_machine;
	mode m_mode = mode::PLAIN;
	int m_players = 2;
	int m_latency = 4;
	int m_jitter = 0;
	int m_delay = 0;
	int m_window = 10;
	int m_hash_every = 1;
	u32 m_seed = 1;
	s64 m_frames = 3600;
	std::string m_out;
	std::vector<s64> m_dump;
	std::vector<s64> m_snap;
	int m_check = 4;                          // synctest: frames to roll back every frame
	std::vector<std::pair<s64, u64>> m_first_hash; // synctest: first-pass logic hash per slot
	u64 m_st_checked = 0, m_st_mismatch = 0;
	bool m_st_dumped = false;
	FILE *m_stlog = nullptr;
	// tap: log maincpu I/O + shared RAM accesses during one frame, per pass
	s64 m_tapframe = -1;
	int m_tappass = 0;
	FILE *m_tapf = nullptr;
	std::vector<memory_passthrough_handler> m_taps;
	void tap_log(char rw, offs_t offset, u32 data, u32 mask);
	void tap_frame_start();
	std::vector<std::string> m_exclude; // extra video-derived save items (substring match), driver specific
	int m_profile = 0;
	unsigned m_fieldcount = 0;
	frame_record *m_prep = nullptr;
	bool m_keep_other = false;
	u64 m_extra_depth = 0;

	size_t m_state_size = 0;
	std::vector<slot> m_slots;
	std::vector<frame_record> m_recs;
	std::vector<ioport_port *> m_ports;
	std::unordered_map<ioport_port *, int> m_portidx;
	std::vector<std::array<std::array<ioport_value, S_COUNT>, MAXP>> m_mask; // [port][player][sem]
	std::vector<ioport_value> m_playermask;   // all player-owned bits per port

	// remote arrival model
	s64 m_known[MAXP];          // last frame whose true input is known, per player
	s64 m_last_arrival[MAXP];

	s64 m_cur = 0;              // frame currently being (or about to be) emulated
	bool m_frame_ended = false;
	bool m_resim = false;
	bool m_done = false;
	s64 m_logged = 0;
	std::vector<u8> m_scratch;
	std::vector<std::array<size_t, 5>> m_timer_norm; // m_index, m_enabled, m_expire.seconds, m_start.attoseconds, m_start.seconds
	std::vector<std::pair<size_t, size_t>> m_video_norm; // video-derived state (tilemaps, screens): excluded from the logic hash
	std::vector<std::pair<size_t, size_t>> m_zero_norm; // video-only fields (offset, size)
	FILE *m_hashlog = nullptr;

	// statistics
	stats m_save_us, m_load_us, m_resim_frame_us, m_frame_us, m_rollback_us, m_depth;
	u64 m_saves_failed = 0, m_rollbacks = 0, m_deferred = 0, m_unrecoverable = 0, m_mispredicted_frames = 0;
	std::chrono::steady_clock::time_point m_frame_start;
	std::chrono::steady_clock::time_point m_run_start;
};

#endif // MAME_EMU_RBTEST_H

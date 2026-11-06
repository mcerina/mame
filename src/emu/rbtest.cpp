// license:BSD-3-Clause
/***************************************************************************

    rbtest.cpp

    Rollback feasibility harness, see rbtest.h.

***************************************************************************/

#include "emu.h"
#include "rbtest.h"

#include "sound.h"
#include "video.h"

#include <algorithm>
#include <cstdlib>
#include <sstream>
#include <string_view>


namespace {

inline u64 mix64(u64 x)
{
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

inline u32 h3(u32 seed, u64 a, u64 b, u64 c)
{
	return u32(mix64(mix64(mix64(seed + 0x9e3779b97f4a7c15ULL) ^ a) ^ (b * 0x632be59bd9b4e019ULL)) ^ c);
}

using clk = std::chrono::steady_clock;
inline double us_since(clk::time_point t) { return std::chrono::duration<double, std::micro>(clk::now() - t).count(); }

} // anonymous namespace


std::unique_ptr<rollback_tester> rollback_tester::create(running_machine &machine)
{
	const char *cfg = std::getenv("MAME_RBTEST");
	if (!cfg || !*cfg)
		return nullptr;
	return std::make_unique<rollback_tester>(machine, cfg);
}


rollback_tester::rollback_tester(running_machine &machine, const std::string &config)
	: m_machine(machine)
{
	// parse key=value,key=value
	std::istringstream ss(config);
	std::string kv;
	while (std::getline(ss, kv, ','))
	{
		auto eq = kv.find('=');
		if (eq == std::string::npos)
			continue;
		std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
		if (k == "mode") m_mode = (v == "rollback") ? mode::ROLLBACK : (v == "saveall") ? mode::SAVEALL : (v == "synctest") ? mode::SYNCTEST : mode::PLAIN;
		else if (k == "check") m_check = std::max(1, std::stoi(v));
		else if (k == "draw") m_resim_draw = std::stoi(v) != 0;
		else if (k == "tapframe") m_tapframe = std::stoll(v);
		else if (k == "players") m_players = std::clamp(std::stoi(v), 1, MAXP);
		else if (k == "latency") m_latency = std::stoi(v);
		else if (k == "jitter") m_jitter = std::stoi(v);
		else if (k == "delay") m_delay = std::stoi(v);
		else if (k == "window") m_window = std::max(1, std::stoi(v));
		else if (k == "hash_every") m_hash_every = std::max(1, std::stoi(v));
		else if (k == "seed") m_seed = u32(std::stoul(v));
		else if (k == "frames") m_frames = std::stoll(v);
		else if (k == "out") m_out = v;
		else if (k == "dump") m_dump.push_back(std::stoll(v));
		else if (k == "profile") m_profile = std::stoi(v);
		else if (k == "snap") m_snap.push_back(std::stoll(v));
		else if (k == "exclude") { std::istringstream es(v); std::string e; while (std::getline(es, e, ';')) if (!e.empty()) m_exclude.push_back(e); }
	}

	// map player-owned input bits per port
	for (auto &p : machine.ioport().ports())
	{
		m_portidx[p.second.get()] = int(m_ports.size());
		m_ports.push_back(p.second.get());
	}
	m_mask.resize(m_ports.size());
	m_playermask.assign(m_ports.size(), 0);
	for (auto &pm : m_mask)
		for (auto &a : pm)
			a.fill(0);
	for (size_t i = 0; i < m_ports.size(); i++)
	{
		for (ioport_field &field : m_ports[i]->fields())
		{
			const ioport_type t = field.type();
			int player = -1, sem = -1;
			if (t >= IPT_COIN1 && t < IPT_COIN1 + MAXP) { player = t - IPT_COIN1; sem = S_COIN; }
			else if (t >= IPT_START1 && t < IPT_START1 + MAXP) { player = t - IPT_START1; sem = S_START; }
			else if (t > IPT_DIGITAL_JOYSTICK_FIRST && t < IPT_DIGITAL_JOYSTICK_LAST)
			{
				player = field.player();
				if (t == IPT_JOYSTICK_UP) sem = S_UP;
				else if (t == IPT_JOYSTICK_DOWN) sem = S_DOWN;
				else if (t == IPT_JOYSTICK_LEFT) sem = S_LEFT;
				else if (t == IPT_JOYSTICK_RIGHT) sem = S_RIGHT;
			}
			else if (t >= IPT_BUTTON1 && t <= IPT_BUTTON16)
			{
				player = field.player();
				if (t - IPT_BUTTON1 < 4) sem = S_B1 + (t - IPT_BUTTON1);
			}
			if (player < 0 || player >= MAXP)
				continue;
			m_playermask[i] |= field.mask();
			if (sem >= 0)
				m_mask[i][player][sem] |= field.mask();
			m_fieldcount++;
		}
	}

	m_state_size = ram_state::get_size(machine.save());
	if (m_mode == mode::SYNCTEST)
		m_window = std::max(m_window, m_check + 1);
	m_slots.resize(m_window + 3);
	m_first_hash.assign(m_slots.size(), { -1, 0 });
	for (auto &s : m_slots)
		s.data.resize(m_state_size);
	m_recs.resize(m_window + 8);
	for (auto &r : m_recs)
	{
		r.other.assign(m_ports.size(), 0);
		std::fill(std::begin(r.used), std::end(r.used), 0);
	}
	for (int p = 0; p < MAXP; p++)
	{
		m_known[p] = 0;
		m_last_arrival[p] = 0;
	}

	// walk the save state layout: write it out (to attribute a divergence to a
	// named item) and collect the items the desync hash ignores, by category:
	// - state derived from drawing (tilemaps, screen bookkeeping, plus driver
	//   items named with exclude=), not updated while resimulating
	// - frontend state (the Lua engine re-arms its own timer on load)
	FILE *lay = m_out.empty() ? nullptr : std::fopen((m_out + ".layout.tsv").c_str(), "w");
	size_t off = 32; // HEADER_SIZE in save.cpp
	void *base;
	u32 valsize, valcount, blockcount, stride;
	for (int i = 0; ; i++)
	{
		const char *name = machine.save().indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (!name)
			break;
		const size_t sz = size_t(valsize) * valcount * blockcount;
		if (lay)
			std::fprintf(lay, "%zu\t%zu\t%s\n", off, sz, name);
		const std::string_view n(name);
		bool skip = n.substr(0, 8) == "tilemap/" || n.substr(0, 13) == "Video Screen/" || n.find("lua_engine") != std::string_view::npos;
		for (auto &e : m_exclude)
			skip = skip || n.find(e) != std::string_view::npos;
		if (skip)
			m_hash_skip.emplace_back(off, sz);
		off += sz;
	}
	if (lay)
		std::fclose(lay);

	if (!m_out.empty())
	{
		m_hashlog = std::fopen((m_out + ".hash.csv").c_str(), "w");
		if (m_mode == mode::SYNCTEST)
			m_stlog = std::fopen((m_out + ".synctest.txt").c_str(), "w");
	}

	if (m_tapframe >= 0)
	{
		cpu_device *cpu = machine.root_device().subdevice<cpu_device>("maincpu");
		if (cpu && cpu->space(AS_PROGRAM).data_width() == 16)
		{
			address_space &sp = cpu->space(AS_PROGRAM);
			auto r = [this] (offs_t o, u16 &d, u16 m) { tap_log('r', o, d, m); };
			auto w = [this] (offs_t o, u16 &d, u16 m) { tap_log('w', o, d, m); };
			m_taps.push_back(sp.install_readwrite_tap(0x800000, 0x8fffff, "rbtest_io", r, w));
			m_taps.push_back(sp.install_readwrite_tap(0xf00000, 0xf1ffff, "rbtest_shared", r, w));
		}
	}

	osd_printf_info("rbtest: mode=%d players=%d latency=%d jitter=%d delay=%d window=%d frames=%d state=%u bytes, %u player fields in %u ports\n",
			int(m_mode), m_players, m_latency, m_jitter, m_delay, m_window, int(m_frames), unsigned(m_state_size), unsigned(m_fieldcount), unsigned(m_ports.size()));
	m_run_start = m_frame_start = clk::now();
}


rollback_tester::~rollback_tester()
{
	finish();
}


//-------------------------------------------------
//  input model
//-------------------------------------------------

// deterministic "human-like" bot: coin+start periodically, held directions,
// mashed attack, occasional jump / special
u16 rollback_tester::truth(int player, s64 frame) const
{
	if (player >= m_players)
		return 0;
	const s64 g = frame - m_delay;
	if (g < 0)
		return 0;
	u16 r = 0;
	const s64 phase = g % 1800;
	if (phase >= 120 + 20 * player && phase < 126 + 20 * player) r |= 1 << S_COIN;
	if (phase >= 300 + 20 * player && phase < 306 + 20 * player) r |= 1 << S_START;

	// profile 0 = aggressive (direction every ~10 frames, attack toggling every 3 frames)
	// profile 1 = calmer   (direction every ~20 frames, attack toggling every 6 frames)
	const int dseg = m_profile ? 20 : 10, aseg = m_profile ? 6 : 3;
	static const u8 dirs[9] = {
		0,
		1 << S_UP, (1 << S_UP) | (1 << S_RIGHT), 1 << S_RIGHT, (1 << S_RIGHT) | (1 << S_DOWN),
		1 << S_DOWN, (1 << S_DOWN) | (1 << S_LEFT), 1 << S_LEFT, (1 << S_LEFT) | (1 << S_UP) };
	r |= dirs[h3(m_seed, player, g / dseg, 1) % 9];
	if (h3(m_seed, player, g / aseg, 2) & 1) r |= 1 << S_B1;
	if (h3(m_seed, player, g / 4, 3) % 10 == 0) r |= 1 << S_B2;
	if (h3(m_seed, player, g / 8, 4) % 30 == 0) r |= 1 << S_B3;
	return r;
}

u16 rollback_tester::best(int player, s64 frame) const
{
	if (m_mode != mode::ROLLBACK || player == 0 || player >= m_players || frame <= m_known[player])
		return truth(player, frame);
	return truth(player, m_known[player]); // predict: last known input
}

ioport_value rollback_tester::compose(int portidx, const u16 *inputs, ioport_value other) const
{
	ioport_value v = other & ~m_playermask[portidx];
	for (int p = 0; p < MAXP; p++)
		for (int s = 0; s < S_COUNT; s++)
			if (inputs[p] & (1 << s))
				v |= m_mask[portidx][p][s];
	return v;
}

void rollback_tester::update_arrivals(s64 boundary)
{
	for (int p = 1; p < m_players; p++)
	{
		while (true)
		{
			const s64 f = m_known[p] + 1;
			const int lat = m_latency + (m_jitter ? int(h3(m_seed, p, f, 7) % (m_jitter + 1)) : 0);
			s64 arr = std::max<s64>(m_last_arrival[p], f - m_delay + lat);
			arr = std::min<s64>(arr, f + m_window - 1); // beyond the window a real peer would stall
			if (arr > boundary)
				break;
			m_known[p] = f;
			m_last_arrival[p] = arr;
		}
	}
}


//-------------------------------------------------
//  ioport hooks
//-------------------------------------------------

void rollback_tester::frame_inputs_begin()
{
	if (m_done)
		return;
	// frame m_cur has just ended; prepare inputs for m_cur + 1
	m_frame_ended = true;
	const s64 f = m_cur + 1;
	frame_record &r = rec_for(f);
	const bool keep_other = m_resim && r.frame == f;
	r.frame = f;
	for (int p = 0; p < MAXP; p++)
		r.used[p] = best(p, f);
	m_keep_other = keep_other;
	m_prep = &r;
}

void rollback_tester::frame_inputs_port(ioport_port &port)
{
	if (m_done || !m_prep)
		return;
	const int idx = m_portidx[&port];
	if (!m_keep_other)
		m_prep->other[idx] = port.live().digital & ~m_playermask[idx];
	port.live().digital = compose(idx, m_prep->used, m_prep->other[idx]);
}

// after a load, the live port values are those of the present: put back
// the ones of the loaded frame (with corrected inputs) and re-drive device lines
void rollback_tester::apply_frame_to_ports(s64 frame)
{
	frame_record &r = rec_for(frame);
	if (r.frame != frame)
	{
		r.frame = frame;
		std::fill(r.other.begin(), r.other.end(), 0);
	}
	for (int p = 0; p < MAXP; p++)
		r.used[p] = best(p, frame);
	for (size_t i = 0; i < m_ports.size(); i++)
	{
		ioport_port &port = *m_ports[i];
		port.live().digital = compose(int(i), r.used, r.other[i]);
		const ioport_value nv = port.read();
		for (auto &dyn : port.live().writelist)
			if (dyn.field().type() != IPT_OUTPUT)
				dyn.write(nv);
	}
}


//-------------------------------------------------
//  state ring
//-------------------------------------------------

bool rollback_tester::save_slot(s64 frame)
{
	slot &s = slot_for(frame);
	s.frame = frame;
	s.valid = false;
	if (!m_machine.scheduler().can_save())
	{
		m_saves_failed++;
		return false;
	}
	auto t = clk::now();
	s.valid = m_machine.save().write_buffer(s.data.data(), s.data.size()) == STATERR_NONE;
	m_save_us.add(us_since(t));
	return s.valid;
}

bool rollback_tester::load_slot(s64 frame)
{
	slot &s = slot_for(frame);
	auto t = clk::now();
	bool ok = m_machine.save().read_buffer(s.data.data(), s.data.size()) == STATERR_NONE;
	m_load_us.add(us_since(t));
	return ok;
}

// hash of a save state without the items in m_hash_skip
u64 rollback_tester::state_hash(const std::vector<u8> &state)
{
	m_scratch = state;
	for (auto &z : m_hash_skip)
		memset(&m_scratch[z.first], 0, z.second);
	u64 h = 0x84222325cbf29ce4ULL;
	const size_t n = m_scratch.size() / 8;
	const u8 *p = m_scratch.data();
	for (size_t i = 0; i < n; i++, p += 8)
	{
		u64 w;
		memcpy(&w, p, 8);
		h ^= w * 0x9e3779b97f4a7c15ULL;
		h = ((h << 31) | (h >> 33)) * 0xc2b2ae3d27d4eb4fULL;
	}
	for (size_t i = n * 8; i < m_scratch.size(); i++)
		h = (h ^ m_scratch[i]) * 0x100000001b3ULL;
	return mix64(h);
}

void rollback_tester::dump_state(const std::vector<u8> &state, const std::string &suffix)
{
	if (m_out.empty())
		return;
	FILE *f = std::fopen((m_out + suffix).c_str(), "wb");
	if (f)
	{
		std::fwrite(state.data(), 1, state.size(), f);
		std::fclose(f);
	}
}

void rollback_tester::log_confirmed(s64 upto)
{
	for (s64 c = m_logged + 1; c <= upto; c++)
	{
		const slot &s = slot_for(c);
		const bool ok = s.frame == c && s.valid;
		if (m_hashlog && (c % m_hash_every) == 0)
		{
			if (ok)
				std::fprintf(m_hashlog, "%lld,%016llx\n", (long long)c, (unsigned long long)state_hash(s.data));
			else
				std::fprintf(m_hashlog, "%lld,NA\n", (long long)c);
		}
		if (ok && std::find(m_dump.begin(), m_dump.end(), c) != m_dump.end())
			dump_state(s.data, util::string_format(".%lld.state", (long long)c));
	}
	m_logged = std::max(m_logged, upto);
}


//-------------------------------------------------
//  tap (diagnostics)
//-------------------------------------------------

void rollback_tester::tap_log(char rw, offs_t offset, u32 data, u32 mask)
{
	if (m_tapf)
		std::fprintf(m_tapf, "%s %c %06x %04x %04x pc=%06x\n", m_machine.time().as_string(9), rw, offset, data, mask,
				unsigned(m_machine.root_device().subdevice<cpu_device>("maincpu")->pc()));
}

void rollback_tester::tap_frame_start()
{
	if (m_tapf)
	{
		std::fclose(m_tapf);
		m_tapf = nullptr;
	}
	if (m_tapframe >= 0 && m_cur == m_tapframe && !m_out.empty())
		m_tapf = std::fopen(util::string_format("%s.tap%d.txt", m_out, m_tappass++).c_str(), "w");
}

//-------------------------------------------------
//  main loop hook
//-------------------------------------------------

void rollback_tester::run_one_frame()
{
	tap_frame_start();
	m_frame_ended = false;
	for (int guard = 0; !m_frame_ended && guard < 1000000; guard++)
	{
		if (m_machine.exit_pending() || m_machine.hard_reset_pending())
			return;
		m_machine.scheduler().timeslice();
	}
}

// load the snapshot of frame `from` and re-run frames from..to-1 with the
// best known inputs (video, audio and throttle off), saving every frame;
// `check` is called after each save with the frame number
template <typename T>
void rollback_tester::resimulate(s64 from, s64 to, T &&check)
{
	auto t = clk::now();
	load_slot(from);
	apply_frame_to_ports(from);
	m_resim = true;
	m_machine.video().set_resimulating(!m_resim_draw);
	m_machine.sound().set_discard_output(true);
	for (m_cur = from; m_cur < to; )
	{
		auto tf = clk::now();
		run_one_frame();
		m_cur++;
		check(m_cur, true);
		save_slot(m_cur);
		m_resim_frame_us.add(us_since(tf));
		check(m_cur, false);
	}
	m_frame_ended = false;
	m_machine.sound().set_discard_output(false);
	m_machine.video().set_resimulating(false);
	m_resim = false;
	m_rollback_us.add(us_since(t));
	m_depth.add(double(to - from));
	m_rollbacks++;
}

// GGPO-style sync test: every frame, roll back m_check frames and resimulate
// with the same inputs; a state that differs from the first pass depends on
// something the save state does not hold
void rollback_tester::sync_test(s64 b)
{
	const size_t n = m_slots.size();
	m_first_hash[b % n] = { b, state_hash(slot_for(b).data) };
	const s64 j = b - m_check;
	if (j < 1 || slot_for(j).frame != j || !slot_for(j).valid)
		return;
	std::vector<u8> first_pass;
	resimulate(j, b, [&] (s64 f, bool before_save)
	{
		if (before_save)
		{
			if (!m_st_dumped)
				first_pass = slot_for(f).data;
			return;
		}
		const slot &sl = slot_for(f);
		auto &fh = m_first_hash[f % n];
		if (!sl.valid || fh.first != f)
			return;
		m_st_checked++;
		const u64 h = state_hash(sl.data);
		if (h == fh.second)
			return;
		m_st_mismatch++;
		if (m_stlog)
			std::fprintf(m_stlog, "frame %lld differs after reload of %lld (boundary %lld)\n", (long long)f, (long long)j, (long long)b);
		if (!m_st_dumped)
		{
			m_st_dumped = true;
			dump_state(first_pass, util::string_format(".stA.%lld.state", (long long)f));
			dump_state(sl.data, util::string_format(".stB.%lld.state", (long long)f));
		}
		fh.second = h;
	});
}

// fake peer: roll back to the first frame whose applied remote inputs
// differ from what is known now
void rollback_tester::rollback_if_needed(s64 b)
{
	const s64 oldest = std::max<s64>(1, b - m_window);
	s64 k = -1;
	for (s64 f = oldest; f <= b && k < 0; f++)
	{
		const frame_record &r = rec_for(f);
		if (r.frame != f)
			continue;
		for (int p = 1; p < m_players; p++)
			if (r.used[p] != best(p, f)) { k = f; break; }
	}
	if (k < 0)
		return;

	// newest valid snapshot not after k (a save can fail on anonymous timers)
	s64 j = k;
	while (j >= oldest && !(slot_for(j).frame == j && slot_for(j).valid))
		j--;
	if (j < oldest)
	{
		m_unrecoverable++;
		return;
	}
	if (j < k)
		m_extra_depth++;
	resimulate(j, b, [] (s64, bool) { });
}

void rollback_tester::post_timeslice()
{
	if (!m_frame_ended || m_done)
		return;
	m_frame_ended = false;
	m_frame_us.add(us_since(m_frame_start));

	const s64 b = ++m_cur; // boundary before frame b (inputs for b already applied)
	s64 confirmed = b;

	switch (m_mode)
	{
	case mode::PLAIN:
		if ((b % m_hash_every) == 0 || std::find(m_dump.begin(), m_dump.end(), b) != m_dump.end())
			save_slot(b);
		break;

	case mode::SAVEALL:
		save_slot(b);
		break;

	case mode::SYNCTEST:
		if (save_slot(b))
			sync_test(b);
		break;

	case mode::ROLLBACK:
		update_arrivals(b);
		// with anonymous timers pending neither save nor load is safe: retry next frame
		if (save_slot(b))
			rollback_if_needed(b);
		else
			m_deferred++;
		for (int p = 1; p < m_players; p++)
			confirmed = std::min(confirmed, m_known[p]);
		break;
	}
	log_confirmed(confirmed);

	if (std::find(m_snap.begin(), m_snap.end(), b) != m_snap.end())
		m_machine.video().save_active_screen_snapshots();

	if (b >= m_frames)
	{
		finish();
		m_machine.schedule_exit();
	}
	tap_frame_start();
	m_frame_start = clk::now();
}


//-------------------------------------------------
//  summary
//-------------------------------------------------

std::string rollback_tester::stats::json(const char *name)
{
	std::vector<float> s = v;
	std::sort(s.begin(), s.end());
	auto pct = [&s] (double q) { return s.empty() ? 0.0 : double(s[std::min(s.size() - 1, size_t(q * (s.size() - 1) + 0.5))]); };
	double sum = 0;
	for (float x : s) sum += x;
	return util::string_format("\"%s\":{\"n\":%u,\"mean\":%.2f,\"p50\":%.2f,\"p90\":%.2f,\"p99\":%.2f,\"max\":%.2f}",
			name, unsigned(s.size()), s.empty() ? 0.0 : sum / s.size(), pct(0.5), pct(0.9), pct(0.99), s.empty() ? 0.0 : double(s.back()));
}

void rollback_tester::finish()
{
	if (m_done)
		return;
	m_done = true;
	if (m_hashlog)
	{
		std::fclose(m_hashlog);
		m_hashlog = nullptr;
	}
	if (m_stlog)
	{
		std::fclose(m_stlog);
		m_stlog = nullptr;
	}
	if (m_out.empty())
		return;
	FILE *f = std::fopen((m_out + ".json").c_str(), "w");
	if (!f)
		return;
	static const char *const modes[] = { "plain", "saveall", "rollback", "synctest" };
	std::fprintf(f, "{\"system\":\"%s\",\"mode\":\"%s\",\"players\":%d,\"latency\":%d,\"jitter\":%d,\"delay\":%d,\"window\":%d,\"profile\":%d,\"seed\":%u,"
			"\"synctest_checked\":%llu,\"synctest_mismatch\":%llu,\"frames\":%lld,\"state_size\":%u,\"wall_s\":%.2f,\"saves_failed\":%llu,\"deferred\":%llu,\"rollbacks\":%llu,\"unrecoverable\":%llu,\"extra_depth\":%llu,\n",
			m_machine.system().name, modes[int(m_mode)], m_players, m_latency, m_jitter, m_delay, m_window, m_profile, m_seed,
			(unsigned long long)m_st_checked, (unsigned long long)m_st_mismatch,
			(long long)m_cur, unsigned(m_state_size), us_since(m_run_start) / 1e6,
			(unsigned long long)m_saves_failed, (unsigned long long)m_deferred, (unsigned long long)m_rollbacks,
			(unsigned long long)m_unrecoverable, (unsigned long long)m_extra_depth);
	std::fprintf(f, "%s,\n%s,\n%s,\n%s,\n%s,\n%s}\n",
			m_save_us.json("save_us").c_str(), m_load_us.json("load_us").c_str(), m_resim_frame_us.json("resim_frame_us").c_str(),
			m_frame_us.json("frame_us").c_str(), m_rollback_us.json("rollback_us").c_str(), m_depth.json("depth").c_str());
	std::fclose(f);
}

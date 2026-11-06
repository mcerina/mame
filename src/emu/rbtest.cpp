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
#include <map>
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

	if (!m_out.empty())
	{
		m_hashlog = std::fopen((m_out + ".hash.csv").c_str(), "w");
		if (m_mode == mode::SYNCTEST)
			m_stlog = std::fopen((m_out + ".synctest.txt").c_str(), "w");

		// save state layout, to attribute any divergence to a named item
		FILE *lay = std::fopen((m_out + ".layout.tsv").c_str(), "w");
		std::map<std::string, std::array<size_t, 5>> timers;
		if (lay)
		{
			size_t off = 32; // HEADER_SIZE in save.cpp
			void *base;
			u32 valsize, valcount, blockcount, stride;
			for (int i = 0; ; i++)
			{
				const char *name = machine.save().indexed_item(i, base, valsize, valcount, blockcount, stride);
				if (!name)
					break;
				size_t sz = size_t(valsize) * valcount * blockcount;
				std::fprintf(lay, "%zu\t%zu\t%s\n", off, sz, name);
				std::string_view n(name);
				if (n.substr(0, 6) == "timer/")
				{
					auto slash = n.rfind('/');
					std::string prefix(n.substr(0, slash));
					if (n.substr(slash) == "/m_enabled") timers[prefix][1] = off;
					else if (n.substr(slash) == "/m_index") timers[prefix][0] = off;
					else if (n.substr(slash) == "/m_expire.seconds") timers[prefix][2] = off;
					else if (n.substr(slash) == "/m_start.attoseconds") timers[prefix][3] = off;
					else if (n.substr(slash) == "/m_start.seconds") timers[prefix][4] = off;
				}
				else if (n.size() > 20 && n.substr(n.size() - 20) == "/m_last_partial_scan")
					m_zero_norm.emplace_back(off, sz);
				bool excluded = false;
				for (auto &e : m_exclude) if (n.find(e) != std::string_view::npos) excluded = true;
				if (excluded || n.substr(0, 8) == "tilemap/" || n.substr(0, 13) == "Video Screen/")
					m_video_norm.emplace_back(off, sz);
				off += sz;
			}
			std::fclose(lay);
		}
		for (auto &t : timers)
			if (t.second[0] && t.second[1] && t.second[2] && t.second[3] && t.second[4])
				m_timer_norm.push_back(t.second);
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

	osd_printf_info("rbtest: norm timers=%u zero=%u video=%u\n", unsigned(m_timer_norm.size()), unsigned(m_zero_norm.size()), unsigned(m_video_norm.size()));
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

u64 rollback_tester::hash_logic()
{
	// call right after hash_buffer(): m_scratch holds the canonical state
	for (auto &z : m_video_norm)
		memset(&m_scratch[z.first], 0, z.second);
	return hash_raw(m_scratch);
}

u64 rollback_tester::hash_buffer(const std::vector<u8> &src)
{
	// m_index of a timer that cannot fire is just its position in the
	// scheduler list, which depends on allocation order/addresses (ASLR):
	// normalise it so equal emulation states hash equal
	m_scratch = src;
	std::vector<u8> &buf = m_scratch;
	for (auto &t : m_timer_norm)
	{
		s32 secs;
		memcpy(&secs, &buf[t[2]], 4);
		if (!buf[t[1]] || secs >= ATTOTIME_MAX_SECONDS)
		{
			// a timer that cannot fire: list position, enable flag and start
			// time do not influence emulation (e.g. lua_engine resets its own
			// timer in postload)
			memset(&buf[t[0]], 0, 4);
			buf[t[1]] = 0;
			memset(&buf[t[3]], 0, 8);
			memset(&buf[t[4]], 0, 4);
		}
	}
	// screen partial-update bookkeeping is left behind by skipped draws during
	// resimulation and reset at the start of every frame: video only
	for (auto &z : m_zero_norm)
		memset(&buf[z.first], 0, z.second);
	return hash_raw(buf);
}

u64 rollback_tester::hash_raw(const std::vector<u8> &buf)
{
	u64 h = 0x84222325cbf29ce4ULL;
	size_t n = buf.size() / 8;
	const u8 *p = buf.data();
	for (size_t i = 0; i < n; i++, p += 8)
	{
		u64 w;
		memcpy(&w, p, 8);
		h = (h ^ (w * 0x9e3779b97f4a7c15ULL));
		h = ((h << 31) | (h >> 33)) * 0xc2b2ae3d27d4eb4fULL;
	}
	for (size_t i = n * 8; i < buf.size(); i++)
		h = (h ^ buf[i]) * 0x100000001b3ULL;
	return mix64(h);
}

void rollback_tester::log_confirmed(s64 upto)
{
	for (s64 c = m_logged + 1; c <= upto; c++)
	{
		const bool dump = std::find(m_dump.begin(), m_dump.end(), c) != m_dump.end();
		if ((c % m_hash_every) != 0 && !dump)
			continue;
		slot &s = slot_for(c);
		const bool ok = s.frame == c && s.valid;
		if (m_hashlog && (c % m_hash_every) == 0)
		{
			if (ok)
			{
				const u64 hc = hash_buffer(s.data);
				const u64 hl = hash_logic();
				std::fprintf(m_hashlog, "%lld,%016llx,%016llx\n", (long long)c, (unsigned long long)hc, (unsigned long long)hl);
			}
			else
				std::fprintf(m_hashlog, "%lld,NA,NA\n", (long long)c);
		}
		if (dump && ok && !m_out.empty())
		{
			if (c == m_cur)
				m_machine.scheduler().dump_timers();
			FILE *f = std::fopen(util::string_format("%s.%lld.state", m_out, (long long)c).c_str(), "wb");
			if (f)
			{
				std::fwrite(s.data.data(), 1, s.data.size(), f);
				std::fclose(f);
			}
		}
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

void rollback_tester::post_timeslice()
{
	if (!m_frame_ended || m_done)
		return;
	m_frame_ended = false;
	m_frame_us.add(us_since(m_frame_start));

	const s64 b = ++m_cur; // boundary before frame b (inputs for b already applied)

	if (m_mode == mode::PLAIN)
	{
		const bool dump = std::find(m_dump.begin(), m_dump.end(), b) != m_dump.end();
		if ((b % m_hash_every) == 0 || dump)
			save_slot(b);
		log_confirmed(b);
	}
	else
	{
		if (m_mode == mode::ROLLBACK)
			update_arrivals(b);

		const bool saved = save_slot(b);

		if (m_mode == mode::SYNCTEST && saved)
		{
			// GGPO-style sync test: every frame, roll back m_check frames and
			// resimulate with the same inputs; any state that differs from the
			// first pass depends on something the save state does not hold
			const size_t n = m_slots.size();
			hash_buffer(slot_for(b).data);
			m_first_hash[b % n] = { b, hash_logic() };
			const s64 j = b - m_check;
			if (j >= 1 && slot_for(j).frame == j && slot_for(j).valid)
			{
				auto t = clk::now();
				load_slot(j);
				apply_frame_to_ports(j);
				m_resim = true;
				m_machine.video().set_resimulating(true);
				m_machine.sound().set_discard_output(true);
				for (m_cur = j; m_cur < b; )
				{
					auto tf = clk::now();
					run_one_frame();
					m_cur++;
					std::vector<u8> before;
					if (!m_st_dumped)
						before = slot_for(m_cur).data;
					save_slot(m_cur);
					m_resim_frame_us.add(us_since(tf));
					slot &sl = slot_for(m_cur);
					auto &fh = m_first_hash[m_cur % n];
					if (sl.valid && fh.first == m_cur)
					{
						hash_buffer(sl.data);
						const u64 h = hash_logic();
						m_st_checked++;
						if (h != fh.second)
						{
							m_st_mismatch++;
							if (m_stlog)
								std::fprintf(m_stlog, "frame %lld differs after reload of %lld (boundary %lld)\n", (long long)m_cur, (long long)j, (long long)b);
							if (!m_st_dumped && !m_out.empty())
							{
								m_st_dumped = true;
								for (int w = 0; w < 2; w++)
								{
									FILE *f = std::fopen(util::string_format("%s.st%s.%lld.state", m_out, w ? "B" : "A", (long long)m_cur).c_str(), "wb");
									if (f) { const auto &d = w ? sl.data : before; std::fwrite(d.data(), 1, d.size(), f); std::fclose(f); }
								}
							}
							fh.second = h;
						}
					}
				}
				m_frame_ended = false;
				m_machine.sound().set_discard_output(false);
				m_machine.video().set_resimulating(false);
				m_resim = false;
				m_rollback_us.add(us_since(t));
				m_rollbacks++;
			}
		}

		if (m_mode == mode::ROLLBACK)
		{
			if (!saved)
			{
				// anonymous timers pending: neither save nor load is safe now
				m_deferred++;
			}
			else
			{
				// earliest frame whose applied inputs differ from the best knowledge
				s64 k = -1;
				for (s64 f = std::max<s64>(1, b - m_window); f <= b && k < 0; f++)
				{
					frame_record &r = rec_for(f);
					if (r.frame != f)
						continue;
					for (int p = 1; p < m_players; p++)
						if (r.used[p] != best(p, f)) { k = f; break; }
				}
				if (k >= 0)
				{
					// newest valid snapshot not after k
					s64 j = k;
					while (j >= std::max<s64>(1, b - m_window) && !(slot_for(j).frame == j && slot_for(j).valid))
						j--;
					if (j < std::max<s64>(1, b - m_window))
						m_unrecoverable++;
					else
					{
						auto t = clk::now();
						load_slot(j);
						apply_frame_to_ports(j);
						m_resim = true;
						m_machine.video().set_resimulating(true);
						m_machine.sound().set_discard_output(true);
						for (m_cur = j; m_cur < b; )
						{
							auto tf = clk::now();
							run_one_frame();
							m_cur++;
							save_slot(m_cur);
							m_resim_frame_us.add(us_since(tf));
						}
						m_frame_ended = false;
						m_machine.sound().set_discard_output(false);
						m_machine.video().set_resimulating(false);
						m_resim = false;
						m_rollback_us.add(us_since(t));
						m_depth.add(double(b - j));
						m_rollbacks++;
						if (j < k) m_extra_depth++;
					}
				}
			}
		}

		s64 upto = b;
		if (m_mode == mode::ROLLBACK)
			for (int p = 1; p < m_players; p++)
				upto = std::min(upto, m_known[p]);
		log_confirmed(upto);
	}

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

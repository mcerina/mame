// license:BSD-3-Clause
/***************************************************************************

    netplay.cpp

    Rollback netplay session, see netplay.h.

    Frame numbering: frame b starts at boundary b, which is where its inputs
    are applied and its state is saved. Everything runs on the main thread:
    the socket is polled at every frame boundary.

    Test hooks come from the MAME_NETPLAY_TEST environment variable (key=value
    separated by commas):
      bot=<seed>        a deterministic bot plays instead of the local player
      frames=<n>        leave at frame n
      log=<path>        write <path>.hash.csv and <path>.json
      sim=<ms>:<ms>:<%> delay, jitter and loss of every packet sent
      fault=<frame>     corrupt one byte of RAM at this frame
      dump=<from>:<to>  write <path>.<frame>.state for these confirmed frames
      draw=1            draw while resimulating: state computed while drawing then stops differing

***************************************************************************/

#include "emu.h"
#include "netplay.h"

#include "emuopts.h"
#include "main.h"
#include "osdepend.h"
#include "screen.h"
#include "sound.h"
#include "video.h"

#include "asio.h"
#include "multibyte.h"

#include <zstd.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <sstream>
#include <string_view>
#include <thread>
#include <vector>


namespace {

using udp = asio::ip::udp;

constexpr u32 MAGIC = 0x4d4e5031;
constexpr u32 PROTOCOL = 2;
constexpr int MAX_PLAYERS = 4;
constexpr int INPUT_RING = 1024;        // frames of input kept: must cover a state transfer
constexpr int MAX_INPUTS_PER_PACKET = 64;
constexpr u32 CHUNK_SIZE = 1200;
constexpr u32 CHUNK_WINDOW = 256;       // chunks in flight, and what an acknowledgement describes
constexpr u32 CHUNKS_PER_MS = 2;        // about 2.4 MB/s
constexpr u32 CHUNK_RESEND_MS = 200;    // wait for the acknowledgements before sending again
constexpr u32 PEER_TIMEOUT_MS = 10000;
constexpr u32 JOIN_TIMEOUT_MS = 300000;  // time to exchange the addresses by chat
constexpr u32 RESYNC_RETRY_MS = 5000;
constexpr u32 STUN_COOKIE = 0x2112a442;
constexpr int STUN_TRIES = 8;
constexpr u32 STUN_RETRY_MS = 300;
constexpr u32 PUNCH_MS = 250;
constexpr s64 START_FRAME = 1;
constexpr s64 NO_FRAME = std::numeric_limits<s64>::max();
constexpr size_t STATE_HEADER_SIZE = 32; // HEADER_SIZE in save.cpp

enum packet_type : u8 { P_HELLO = 1, P_WELCOME, P_REJECT, P_START, P_GO, P_INPUT, P_HASH, P_RESYNC, P_CHUNK, P_CHUNK_ACK, P_BYE, P_PUNCH };

// an address as people paste it: without spaces or a final full stop
std::string clean_address(std::string text)
{
	text.erase(std::remove_if(text.begin(), text.end(), [] (char c) { return std::isspace(u8(c)); }), text.end());
	while (!text.empty() && text.back() == '.')
		text.pop_back();
	return text;
}

u64 hash_bytes(u64 h, const void *data, size_t size)
{
	const u8 *p = reinterpret_cast<const u8 *>(data);
	for (size_t i = 0; i < size; i++)
		h = (h ^ p[i]) * 0x100000001b3ULL;
	return h;
}

template <typename T> u64 hash_value(u64 h, T value) { return hash_bytes(h, &value, sizeof(value)); }

constexpr u64 HASH_SEED = 0xcbf29ce484222325ULL;

inline u64 mix64(u64 x)
{
	x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
	x ^= x >> 27; x *= 0x94d049bb133111ebULL;
	x ^= x >> 31;
	return x;
}

u32 now_ms() { return u32(osd_ticks() / (osd_ticks_per_second() / 1000)); }


// little-endian packet writer and bounds-checked reader: everything read comes from the network
class packet_writer
{
public:
	packet_writer(packet_type type, u32 session, int sender)
	{
		put<u32>(MAGIC);
		put<u32>(session);
		put<u8>(type);
		put<u8>(u8(sender));
	}

	template <typename T> void put(T value)
	{
		for (size_t i = 0; i < sizeof(T); i++)
			m_data.push_back(u8(u64(value) >> (i * 8)));
	}

	void put_string(std::string_view s)
	{
		put<u16>(u16(s.size()));
		m_data.insert(m_data.end(), s.begin(), s.end());
	}

	void put_bytes(const u8 *data, size_t size) { m_data.insert(m_data.end(), data, data + size); }

	const std::vector<u8> &data() const { return m_data; }

private:
	std::vector<u8> m_data;
};

class packet_reader
{
public:
	packet_reader(const u8 *data, size_t size) : m_data(data), m_size(size) { }

	template <typename T> T get()
	{
		if (m_size < sizeof(T))
		{
			m_ok = false;
			return T(0);
		}
		u64 v = 0;
		for (size_t i = 0; i < sizeof(T); i++)
			v |= u64(m_data[i]) << (i * 8);
		m_data += sizeof(T);
		m_size -= sizeof(T);
		return T(v);
	}

	std::string get_string()
	{
		const size_t n = get<u16>();
		if (n > m_size)
		{
			m_ok = false;
			return std::string();
		}
		std::string s(reinterpret_cast<const char *>(m_data), n);
		m_data += n;
		m_size -= n;
		return s;
	}

	const u8 *remaining() const { return m_data; }
	size_t remaining_size() const { return m_size; }
	bool ok() const { return m_ok; }

private:
	const u8 *m_data;
	size_t m_size;
	bool m_ok = true;
};


class netplay_session : public netplay_manager
{
public:
	netplay_session(running_machine &machine, bool host);
	~netplay_session();

	bool in_session() const override { return m_phase == phase::RUNNING; }
	void frame_inputs_begin() override;
	void frame_inputs_port(ioport_port &port) override;
	void post_timeslice() override;

private:
	enum class phase { BOOT, LOBBY, RUNNING, ENDED };

	struct slot
	{
		s64 frame = -1;
		bool valid = false;
		std::vector<u8> data;
	};

	// one input of one player's bit in one port
	struct port_field
	{
		u8 bit;
		u8 player;
		ioport_value mask;
	};

	struct transfer_out
	{
		bool active = false;
		std::shared_ptr<const std::vector<u8>> data;
		u32 id = 0;
		s64 frame = 0;
		u32 total = 0;
		u32 next_missing = 0;
		std::array<u64, CHUNK_WINDOW / 64> received_after = { }; // the chunks received after next_missing
		u32 send_pos = 0;
		u32 last_tick_ms = 0;
		u32 last_pass_ms = 0;       // when it last went back to the first missing chunk
	};

	struct transfer_in
	{
		bool active = false;
		bool ready = false;         // m_raw holds the whole state
		u32 id = 0;
		u32 done_id = 0;
		s64 frame = 0;
		u32 total = 0;
		u32 compressed = 0;
		u32 next_missing = 0;
		std::vector<u8> have;
		std::vector<u8> data;
		std::vector<u8> raw;
	};

	struct peer
	{
		bool connected = false;
		udp::endpoint endpoint;
		u32 last_rx_ms = 0;
		s64 acked = START_FRAME;    // last of my inputs they received
		s64 frame = START_FRAME;    // their frame when they last sent
		u32 frame_rx_ms = 0;
		u32 echo_time = 0;          // their last send time, sent back to measure the round trip
		u32 echo_rx_ms = 0;
		float rtt = 0;
		float advantage = 0;        // how many frames I am ahead of them
		float their_advantage = 0;  // how many frames they say they are ahead of me
		transfer_out tx;
		bool session_ready = false; // got the starting state
		bool resync_wanted = false;
		s64 resync_frame = 0;       // its present: the state sent can't be newer
		s64 resync_check = 0;       // the checked frame that differed
	};

	struct hash_entry
	{
		s64 frame = -1;
		u64 mine = 0;
		u64 host = 0;
		bool have_mine = false;
		bool have_host = false;
	};

	struct checked_state
	{
		s64 frame = -1;
		std::vector<u8> data;
	};

	// a STUN binding request: the server answers with the address it saw
	struct stun_query
	{
		udp::endpoint server;
		std::array<u8, 12> id;
		udp::endpoint mapped;
		bool done = false;
	};

	// lines typed on the console, filled by a thread that may outlive the session
	struct console_lines
	{
		std::mutex lock;
		std::vector<std::string> lines;
	};

	struct delayed_packet
	{
		u32 time;
		udp::endpoint endpoint;
		std::vector<u8> data;
	};

	// setup
	void map_inputs();
	void compute_signature();
	void start_session_buffers();
	void compute_hash_skip();

	// inputs
	u32 best(int player, s64 frame) const;
	u32 frame_word(int player, s64 frame) const { return player == m_me ? m_input[player][frame % INPUT_RING] : m_used[player][frame % INPUT_RING]; }
	u32 bot_word(s64 frame) const;
	ioport_value compose(int port, s64 frame) const;
	void apply_frame_to_ports(s64 frame);

	// states
	slot &slot_for(s64 frame) { return m_slots[frame % m_slots.size()]; }
	bool slot_valid(s64 frame) { const slot &s = slot_for(frame); return s.frame == frame && s.valid; }
	bool save_slot(s64 frame);
	void resimulate(s64 from, s64 to);
	void run_one_frame();
	void rollback(s64 boundary);
	void corrupt_state(s64 frame);

	// session
	void run_lobby();
	void lobby_tick();
	void run_frame();
	void stall_tick();
	void finish_lobby();

	// desync detection and repair
	u64 state_hash(const std::vector<u8> &state);
	void advance_confirmed(s64 boundary);
	void hash_frame(s64 frame);
	void compare_hash(hash_entry &entry);
	void request_resync(s64 boundary, s64 frame);
	void apply_received_state(s64 boundary);
	void log_differences(const std::vector<u8> &mine, const std::vector<u8> &host);
	void serve_resyncs();

	// time sync
	void update_time_sync(s64 boundary);

	// transport
	void send(const udp::endpoint &to, const packet_writer &packet);
	void send_all(const packet_writer &packet);
	void poll();
	void flush_simulated();
	void handle_packet(const udp::endpoint &from, const u8 *data, size_t size);
	void handle_hello(const udp::endpoint &from, packet_reader &in);
	void start_stun();
	void stun_tick(u32 now);
	bool handle_stun(const udp::endpoint &from, const u8 *data, size_t size);
	void read_console();
	void punch_tick(u32 now);
	void handle_welcome(packet_reader &in);
	void handle_start(packet_reader &in);
	void handle_input(int sender, packet_reader &in);
	void handle_hash(packet_reader &in);
	void handle_chunk(packet_reader &in);
	void handle_chunk_ack(int sender, packet_reader &in);
	void send_inputs();
	void send_welcome(int player);
	void send_start(int player);
	void send_chunk_ack();
	void start_transfer(int player, s64 frame, const std::vector<u8> &state);
	void tick_transfers();
	void check_timeouts();
	void end_session(const char *reason);

	void write_log();

	running_machine &m_machine;
	phase m_phase = phase::BOOT;
	const bool m_host;
	std::string m_end_reason;

	// session parameters: the host's options, sent to the clients
	u32 m_session = 0;
	int m_me = 0;
	int m_players = 2;
	int m_delay = 1;
	int m_window = 10;
	int m_hash_every = 30;
	std::string m_exclude;
	u64 m_base_time = 0;

	// signature that must match for a client to join
	u64 m_layout_hash = 0, m_rom_hash = 0, m_input_hash = 0;
	size_t m_state_size = 0;

	// input mapping
	std::vector<ioport_port *> m_ports;
	bool m_input_callbacks = false;     // inputs with a change callback, which writes machine state
	std::unordered_map<ioport_port *, int> m_port_index;
	std::vector<std::vector<port_field>> m_port_fields;
	std::vector<ioport_type> m_bit_type;

	// inputs per player and frame
	std::vector<u32> m_input[MAX_PLAYERS];
	std::vector<u32> m_used[MAX_PLAYERS];   // remote players only: what the frame applied
	s64 m_known[MAX_PLAYERS];

	// states
	std::vector<slot> m_slots;
	std::vector<std::pair<size_t, size_t>> m_hash_skip;
	std::vector<u8> m_scratch;
	s64 m_cur = 0;
	bool m_frame_ended = false;
	bool m_resim = false;
	s64 m_rollback_to = NO_FRAME;
	s64 m_floor = START_FRAME;      // no rollback before this frame: the state came from the host
	s64 m_confirmed = START_FRAME;
	std::vector<hash_entry> m_hashes;
	checked_state m_checked[2];     // the states of the last hashed frames: a repair starts from the one that differed
	bool m_resync_pending = false;
	u32 m_resync_ms = 0;

	// time sync
	float m_frame_ms = 1000.0f / 60;
	int m_base_speed = 1000;
	bool m_slowed = false;

	// transport
	asio::io_context m_io;
	udp::socket m_socket;
	udp::endpoint m_host_endpoint;
	peer m_peers[MAX_PLAYERS];
	transfer_in m_rx;
	bool m_send_ack = false;
	u32 m_next_transfer_id = 1;
	bool m_all_joined = false;
	u32 m_lobby_start_ms = 0;
	u32 m_last_hello_ms = 0;
	u32 m_last_draw_ms = 0;

	// NAT traversal: the public address, and the players' addresses the host
	// sends to until they join, so that its router lets their packets in
	std::vector<stun_query> m_stun;
	int m_stun_tries = 0;
	u32 m_stun_ms = 0;
	bool m_stun_reported = false;
	std::vector<udp::endpoint> m_punch;
	u32 m_punch_ms = 0;
	std::shared_ptr<console_lines> m_console;
	bool m_welcomed = false;
	bool m_started = false;
	bool m_go = false;
	bool m_loaded = false;
	u64 m_tx_bytes = 0, m_rx_bytes = 0;

	// test hooks
	u32 m_bot_seed = 0;
	s64 m_test_frames = 0;
	s64 m_test_fault = -1;
	long long m_test_dump_from = 0, m_test_dump_to = -1;
	bool m_test_draw = false;
	size_t m_fault_offset = 0;
	std::string m_log_path;
	FILE *m_hash_log = nullptr;
	int m_sim_delay = 0, m_sim_jitter = 0, m_sim_loss = 0;
	std::mt19937 m_sim_rng;
	std::deque<delayed_packet> m_sim_queue;

	// statistics
	u64 m_rollbacks = 0, m_resim_frames = 0, m_unrecoverable = 0, m_saves_failed = 0, m_deferred = 0;
	u64 m_desyncs = 0, m_resyncs = 0, m_hash_compared = 0, m_stall_ms = 0, m_slow_frames = 0;
	u64 m_depth_histogram[64] = { };
	double m_rollback_ms_total = 0, m_rollback_ms_max = 0;
	osd_ticks_t m_session_start = 0;
};


//-------------------------------------------------
//  construction
//-------------------------------------------------

netplay_session::netplay_session(running_machine &machine, bool host)
	: m_machine(machine)
	, m_host(host)
	, m_socket(m_io)
	, m_sim_rng(u32(osd_ticks()))
{
	if (machine.options().cheat())
		throw emu_fatalerror("Netplay: cheats must be off");

	map_inputs();
	compute_signature();

	if (host)
	{
		m_players = std::clamp(machine.options().netplay_players(), 2, MAX_PLAYERS);
		m_delay = std::clamp(machine.options().netplay_delay(), 0, 8);
		m_window = std::clamp(machine.options().netplay_window(), 1, 60);
		m_hash_every = std::clamp(machine.options().netplay_hash_every(), 1, 3600);
		m_exclude = machine.options().netplay_exclude();
		m_session = u32(mix64(osd_ticks() ^ u64(::time(nullptr)))) | 1;
		const int port = machine.options().netplay_host_port();
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), u16(port)));
		osd_printf_info("Netplay: hosting on UDP port %d, waiting for %d more player(s)\n", port, m_players - 1);
	}
	else
	{
		const std::string target = clean_address(machine.options().netplay_join());
		const auto colon = target.rfind(':');
		if (colon == std::string::npos)
			throw emu_fatalerror("Netplay: -netplay_join needs host:port");
		asio::error_code ec;
		udp::resolver resolver(m_io);
		auto results = resolver.resolve(udp::v4(), target.substr(0, colon), target.substr(colon + 1), ec);
		if (ec || results.empty())
			throw emu_fatalerror("Netplay: cannot resolve %s", target);
		m_host_endpoint = *results.begin();
		m_socket.open(udp::v4());
		m_socket.bind(udp::endpoint(udp::v4(), 0));
		osd_printf_info("Netplay: joining %s\n", target);
	}
	m_socket.non_blocking(true);
	start_stun();

	if (screen_device *screen = screen_device_enumerator(machine.root_device()).first())
		m_frame_ms = float(screen->frame_period().as_double() * 1000.0);
	m_base_speed = machine.video().speed_factor();

	// test hooks
	if (const char *cfg = std::getenv("MAME_NETPLAY_TEST"))
	{
		std::istringstream ss(cfg);
		std::string kv;
		while (std::getline(ss, kv, ','))
		{
			const auto eq = kv.find('=');
			if (eq == std::string::npos)
				continue;
			const std::string k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == "bot") m_bot_seed = u32(std::stoul(v));
			else if (k == "frames") m_test_frames = std::stoll(v);
			else if (k == "fault") m_test_fault = std::stoll(v);
			else if (k == "draw") m_test_draw = std::stoi(v) != 0;
			else if (k == "log") m_log_path = v;
			else if (k == "dump") std::sscanf(v.c_str(), "%lld:%lld", &m_test_dump_from, &m_test_dump_to);
			else if (k == "sim") std::sscanf(v.c_str(), "%d:%d:%d", &m_sim_delay, &m_sim_jitter, &m_sim_loss);
		}
		if (!m_log_path.empty())
			m_hash_log = std::fopen((m_log_path + ".hash.csv").c_str(), "w");
	}

	for (int p = 0; p < MAX_PLAYERS; p++)
	{
		m_input[p].assign(INPUT_RING, 0);
		m_used[p].assign(INPUT_RING, 0);
		m_known[p] = START_FRAME;
	}
}


netplay_session::~netplay_session()
{
	if (m_phase == phase::RUNNING || m_phase == phase::LOBBY)
	{
		packet_writer out(P_BYE, m_session, m_me);
		for (int i = 0; i < 3; i++)
			send_all(out);
	}
	write_log();
	if (m_hash_log)
		std::fclose(m_hash_log);
	if (m_slowed)
		m_machine.video().set_speed_factor(m_base_speed);
}


// every player has the same inputs in the packet, one bit per input type;
// the local player's controls are those of player 1, whatever seat they got
void netplay_session::map_inputs()
{
	for (auto &p : m_machine.ioport().ports())
	{
		m_port_index[p.second.get()] = int(m_ports.size());
		m_ports.push_back(p.second.get());
		for (auto &dyn : p.second->live().writelist)
			m_input_callbacks = m_input_callbacks || dyn.field().type() != IPT_OUTPUT;
	}
	m_port_fields.resize(m_ports.size());

	for (size_t i = 0; i < m_ports.size(); i++)
	{
		for (ioport_field &field : m_ports[i]->fields())
		{
			if (field.is_analog())
				throw emu_fatalerror("Netplay: analog controls are not supported yet (%s)", field.name());

			const ioport_type t = field.type();
			ioport_type type = t;
			int player = field.player();
			if (t >= IPT_COIN1 && t <= IPT_COIN12) { type = IPT_COIN1; player = t - IPT_COIN1; }
			else if (t >= IPT_START1 && t <= IPT_START10) { type = IPT_START1; player = t - IPT_START1; }
			else if (!(t > IPT_DIGITAL_JOYSTICK_FIRST && t < IPT_DIGITAL_JOYSTICK_LAST) && !(t >= IPT_BUTTON1 && t <= IPT_BUTTON16))
				continue; // service, tilt...: nobody's, and always off in a session
			if (player >= MAX_PLAYERS)
				continue;

			auto it = std::find(m_bit_type.begin(), m_bit_type.end(), type);
			if (it == m_bit_type.end())
			{
				if (m_bit_type.size() == 32)
					throw emu_fatalerror("Netplay: too many different inputs");
				it = m_bit_type.insert(m_bit_type.end(), type);
			}
			m_port_fields[i].push_back({ u8(it - m_bit_type.begin()), u8(player), field.mask() });
		}
	}
}


// what must be the same on all instances, and is not in the state
void netplay_session::compute_signature()
{
	save_manager &save = m_machine.save();
	m_state_size = ram_state::get_size(save);

	m_layout_hash = HASH_SEED;
	void *base;
	u32 valsize, valcount, blockcount, stride;
	for (int i = 0; ; i++)
	{
		const char *name = save.indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (!name)
			break;
		m_layout_hash = hash_bytes(m_layout_hash, name, strlen(name));
		m_layout_hash = hash_value(m_layout_hash, u64(valsize) * valcount * blockcount);
	}

	std::vector<std::string> names;
	for (auto &r : m_machine.memory().regions())
		names.push_back(r.first);
	std::sort(names.begin(), names.end());
	m_rom_hash = HASH_SEED;
	for (auto &name : names)
	{
		memory_region *region = m_machine.memory().regions().find(name)->second.get();
		m_rom_hash = hash_bytes(m_rom_hash, name.data(), name.size());
		m_rom_hash = hash_bytes(m_rom_hash, region->base(), region->bytes());
	}

	m_input_hash = HASH_SEED;
	for (ioport_port *port : m_ports)
	{
		m_input_hash = hash_bytes(m_input_hash, port->tag(), strlen(port->tag()));
		m_input_hash = hash_value(m_input_hash, u32(port->live().defvalue));
	}
	for (ioport_type t : m_bit_type)
		m_input_hash = hash_value(m_input_hash, u32(t));
}


// once the session parameters are known
void netplay_session::start_session_buffers()
{
	m_slots.resize(m_window + 3);
	for (auto &s : m_slots)
		s.data.resize(m_state_size);
	m_hashes.assign(64, hash_entry());
	compute_hash_skip();
	for (int p = 0; p < m_players; p++)
		m_known[p] = START_FRAME + m_delay; // nobody has inputs before the first boundary
}


// items the desync hash ignores: state derived from drawing, which depends
// on whether a frame was drawn or resimulated, sound samples, and frontend state
void netplay_session::compute_hash_skip()
{
	std::vector<std::string> exclude;
	std::istringstream es(m_exclude);
	std::string e;
	while (std::getline(es, e, ';'))
		if (!e.empty())
			exclude.push_back(e);

	size_t offset = STATE_HEADER_SIZE;
	void *base;
	u32 valsize, valcount, blockcount, stride;
	for (int i = 0; ; i++)
	{
		const char *name = m_machine.save().indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (!name)
			break;
		const size_t size = size_t(valsize) * valcount * blockcount;
		const std::string_view n(name);
		bool skip = n.substr(0, 8) == "tilemap/" || n.substr(0, 13) == "Video Screen/" || n.find("lua_engine") != std::string_view::npos;

		// sound samples are computed in floating point: two builds for different
		// compilers or CPUs may round them differently; the sound chips' own
		// state stays in the hash
		skip = skip || n.substr(0, 7) == "stream.";
		for (auto &x : exclude)
			skip = skip || n.find(x) != std::string_view::npos;
		if (skip)
			m_hash_skip.emplace_back(offset, size);
		else if (!m_fault_offset && n.substr(0, 7) == "memory/" && size >= 256)
			m_fault_offset = offset + size / 2;
		offset += size;
	}
}


//-------------------------------------------------
//  inputs
//-------------------------------------------------

// what is known for a remote player, or else the last known value
u32 netplay_session::best(int player, s64 frame) const
{
	return m_input[player][std::min(frame, m_known[player]) % INPUT_RING];
}

// deterministic bot: same idea as the rollback harness, on any set of inputs
u32 netplay_session::bot_word(s64 frame) const
{
	u32 word = 0;
	for (size_t bit = 0; bit < m_bit_type.size(); bit++)
	{
		const ioport_type t = m_bit_type[bit];
		const u64 base = mix64(u64(m_bot_seed) * 0x9e3779b97f4a7c15ULL + m_me * 131 + bit);
		bool on;
		if (t == IPT_COIN1)
			on = (frame % 1800) >= 120u + 20 * m_me && (frame % 1800) < 126u + 20 * m_me;
		else if (t == IPT_START1)
			on = (frame % 1800) >= 300u + 20 * m_me && (frame % 1800) < 306u + 20 * m_me;
		else if (t >= IPT_BUTTON1 && t <= IPT_BUTTON16)
			on = (mix64(base ^ u64(frame / 3)) % (t == IPT_BUTTON1 ? 2 : 10)) == 0;
		else
			on = (mix64(base ^ u64(frame / 10)) % 4) == 0;
		if (on)
			word |= u32(1) << bit;
	}
	return word;
}

ioport_value netplay_session::compose(int port, s64 frame) const
{
	ioport_value v = 0;
	for (const port_field &f : m_port_fields[port])
		if (f.player < m_players && BIT(frame_word(f.player, frame), f.bit))
			v |= f.mask;
	return v;
}

void netplay_session::frame_inputs_begin()
{
	if (m_phase == phase::BOOT)
		m_frame_ended = true;
	if (m_phase != phase::RUNNING)
		return;

	// frame m_cur has just ended: prepare the inputs of m_cur + 1
	m_frame_ended = true;
	const s64 f = m_cur + 1;
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			m_used[p][f % INPUT_RING] = best(p, f);
	if (!m_resim)
		m_input[m_me][(f + m_delay) % INPUT_RING] = m_bot_seed ? bot_word(f + m_delay) : 0;
}

void netplay_session::frame_inputs_port(ioport_port &port)
{
	if (m_phase != phase::RUNNING)
	{
		port.live().digital = 0;
		return;
	}

	const int index = m_port_index[&port];
	const s64 f = m_cur + 1;
	if (!m_resim && !m_bot_seed)
	{
		u32 &word = m_input[m_me][(f + m_delay) % INPUT_RING];
		for (const port_field &field : m_port_fields[index])
			if (field.player == 0 && (port.live().digital & field.mask))
				word |= u32(1) << field.bit;
	}
	port.live().digital = compose(index, f);
}

// after a load, the live port values are those of the present: put back the
// ones of the loaded frame, with the best known inputs
void netplay_session::apply_frame_to_ports(s64 frame)
{
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			m_used[p][frame % INPUT_RING] = best(p, frame);
	auto dis = m_machine.disable_side_effects();
	for (size_t i = 0; i < m_ports.size(); i++)
	{
		ioport_port &port = *m_ports[i];
		port.live().digital = compose(int(i), frame);
		const ioport_value value = port.read();
		for (auto &dyn : port.live().writelist)
			if (dyn.field().type() != IPT_OUTPUT)
				dyn.write(value);
	}
}


//-------------------------------------------------
//  states
//-------------------------------------------------

bool netplay_session::save_slot(s64 frame)
{
	slot &s = slot_for(frame);
	s.frame = frame;
	s.valid = false;
	if (!m_machine.scheduler().can_save())
	{
		m_saves_failed++;
		return false;
	}
	s.valid = m_machine.save().write_buffer(s.data.data(), s.data.size()) == STATERR_NONE;
	return s.valid;
}

void netplay_session::run_one_frame()
{
	m_frame_ended = false;
	for (int guard = 0; !m_frame_ended && guard < 1000000; guard++)
	{
		if (m_machine.exit_pending() || m_machine.hard_reset_pending())
			return;
		m_machine.scheduler().timeslice();
	}
}

// load the state of frame `from` and run up to `to` with the best known
// inputs, in one host frame: no video, audio or throttle
void netplay_session::resimulate(s64 from, s64 to)
{
	const osd_ticks_t start = osd_ticks();
	m_machine.save().read_buffer(slot_for(from).data.data(), m_state_size);
	apply_frame_to_ports(from);

	// the slot was saved after the input callbacks ran with the inputs predicted
	// then: save it again, or its hash differs from the other machines' one
	if (m_input_callbacks)
		save_slot(from);
	m_resim = true;
	m_machine.video().set_resimulating(!m_test_draw);
	m_machine.sound().set_discard_output(true);
	for (m_cur = from; m_cur < to && !m_machine.exit_pending(); )
	{
		run_one_frame();
		m_cur++;
		save_slot(m_cur);
		m_resim_frames++;
	}
	m_frame_ended = false;
	m_machine.sound().set_discard_output(false);
	m_machine.video().set_resimulating(false);
	m_resim = false;

	// the state of the present was saved with the inputs predicted then
	if (from == to)
		save_slot(to);

	const double ms = double(osd_ticks() - start) * 1000.0 / osd_ticks_per_second();
	m_rollback_ms_total += ms;
	m_rollback_ms_max = std::max(m_rollback_ms_max, ms);
	m_depth_histogram[std::min<s64>(to - from, std::size(m_depth_histogram) - 1)]++;
	m_rollbacks++;
}

void netplay_session::rollback(s64 boundary)
{
	const s64 oldest = std::max(m_floor, boundary - m_window);
	s64 frame = m_rollback_to;
	m_rollback_to = NO_FRAME;

	// newest valid snapshot not after the first wrong frame (a save can fail on anonymous timers)
	while (frame >= oldest && !slot_valid(frame))
		frame--;
	if (frame < oldest)
	{
		// state lost: the hash comparison finds out and repairs it
		m_unrecoverable++;
		return;
	}
	resimulate(frame, boundary);
}

void netplay_session::corrupt_state(s64 frame)
{
	if (!m_fault_offset || !slot_valid(frame))
		return;
	slot &s = slot_for(frame);
	s.data[m_fault_offset] ^= 0x5a;
	m_machine.save().read_buffer(s.data.data(), m_state_size);
	osd_printf_info("Netplay test: corrupted the state at frame %d\n", int(frame));
}


//-------------------------------------------------
//  lobby: wait for all players, then give them
//  the host's state
//-------------------------------------------------

void netplay_session::run_lobby()
{
	m_phase = phase::LOBBY;
	m_lobby_start_ms = now_ms();
	if (m_host)
	{
		m_me = 0;
		start_session_buffers();
		m_cur = START_FRAME;
		save_slot(START_FRAME);
		m_base_time = u64(m_machine.base_time());
		m_peers[0].connected = true;
		m_machine.popmessage("Netplay: waiting for %d more player(s)", m_players - 1);
	}
	else
		m_machine.popmessage("Netplay: joining");

	while (m_phase == phase::LOBBY && !m_machine.exit_pending())
		lobby_tick();
}

void netplay_session::lobby_tick()
{
	poll();
	send_chunk_ack();

	const u32 now = now_ms();
	stun_tick(now);
	if (m_host)
	{
		punch_tick(now);
		tick_transfers();
		bool ready = m_all_joined;
		for (int p = 1; p < m_players; p++)
			ready = ready && m_peers[p].session_ready;
		if (ready)
			finish_lobby();
	}
	else
	{
		if (!m_started && now - m_last_hello_ms >= 250)
		{
			m_last_hello_ms = now;
			packet_writer out(P_HELLO, 0, 0);
			out.put<u32>(PROTOCOL);
			out.put_string(emulator_info::get_build_version());
			out.put_string(m_machine.system().name);
			out.put<u32>(u32(m_state_size));
			out.put<u64>(m_layout_hash);
			out.put<u64>(m_rom_hash);
			out.put<u64>(m_input_hash);
			send(m_host_endpoint, out);
		}
		if (!m_welcomed && now - m_lobby_start_ms >= JOIN_TIMEOUT_MS)
		{
			end_session("no answer from the host");
			return;
		}
		if (m_rx.ready && !m_loaded)
		{
			// the machine is loaded when the session starts
			slot &s = slot_for(START_FRAME);
			s.data = std::move(m_rx.raw);
			s.frame = START_FRAME;
			s.valid = true;
			m_rx.ready = false;
			m_loaded = true;
			m_cur = START_FRAME;
		}
		if (m_loaded && m_started && now - m_last_hello_ms >= 100)
		{
			// resend the acknowledgement until the host answers
			m_last_hello_ms = now;
			m_send_ack = true;
			send_chunk_ack();
		}
		if (m_loaded && m_go && m_started)
			finish_lobby();
	}
	flush_simulated();

	// keep the UI alive; this does not run the machine, and the state is
	// loaded again at the end
	if (now - m_last_draw_ms >= 16)
	{
		m_last_draw_ms = now;
		if (m_phase == phase::LOBBY)
			m_machine.video().frame_update();
	}
	else
		osd_sleep(osd_ticks_per_second() / 1000);
}

void netplay_session::finish_lobby()
{
	if (m_host)
	{
		packet_writer out(P_GO, m_session, m_me);
		for (int i = 0; i < 3; i++)
			send_all(out);
	}
	m_cur = START_FRAME;
	m_machine.save().read_buffer(slot_for(START_FRAME).data.data(), m_state_size);
	m_phase = phase::RUNNING;
	apply_frame_to_ports(START_FRAME);
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		m_peers[p].last_rx_ms = now;
		m_peers[p].frame_rx_ms = now;
	}
	m_session_start = osd_ticks();
	m_machine.popmessage("Netplay: player %d of %d", m_me + 1, m_players);
	osd_printf_info("Netplay: session started as player %d of %d, delay %d, window %d\n", m_me + 1, m_players, m_delay, m_window);
}


//-------------------------------------------------
//  one frame of the session
//-------------------------------------------------

void netplay_session::post_timeslice()
{
	if (!m_frame_ended)
		return;
	m_frame_ended = false;

	if (m_phase == phase::BOOT)
	{
		// the first boundary with a state that can be saved
		if (m_machine.scheduler().can_save())
			run_lobby();
	}
	else if (m_phase == phase::RUNNING)
		run_frame();
}

void netplay_session::run_frame()
{
	s64 b = ++m_cur;
	m_known[m_me] = b + m_delay;
	const bool saved = save_slot(b);
	if (!saved)
		m_deferred++;
	if (b == m_test_fault)
		corrupt_state(b);

	poll();
	send_chunk_ack();
	send_inputs();

	while (m_phase == phase::RUNNING && !m_machine.exit_pending())
	{
		// with anonymous timers pending neither save nor load is safe: retry at the next boundary
		if (saved)
		{
			if (m_rx.ready)
			{
				apply_received_state(b);
				b = m_cur;
			}
			else if (m_rollback_to != NO_FRAME)
				rollback(b);
		}
		advance_confirmed(b);

		// too far ahead of the slowest player: wait, the state to roll back to would be lost
		s64 slowest = b;
		for (int p = 0; p < m_players; p++)
			if (p != m_me)
				slowest = std::min(slowest, m_known[p]);
		if (b - slowest < m_window)
			break;
		stall_tick();
		if (!saved && m_rollback_to != NO_FRAME)
			break;
	}

	serve_resyncs();
	tick_transfers();
	flush_simulated();
	update_time_sync(b);
	check_timeouts();

	if (m_test_frames && b >= m_test_frames)
		end_session("test finished");
}

void netplay_session::stall_tick()
{
	const u32 start = now_ms();
	osd_sleep(osd_ticks_per_second() / 1000);
	poll();
	send_chunk_ack();
	send_inputs();
	tick_transfers();
	flush_simulated();
	check_timeouts();
	m_machine.osd().update(true);
	m_stall_ms += now_ms() - start;
}


//-------------------------------------------------
//  desync detection and repair
//-------------------------------------------------

u64 netplay_session::state_hash(const std::vector<u8> &state)
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
	return mix64(hash_bytes(h, p, m_scratch.size() - n * 8));
}

void netplay_session::advance_confirmed(s64 boundary)
{
	s64 confirmed = boundary;
	for (int p = 0; p < m_players; p++)
		confirmed = std::min(confirmed, m_known[p]);
	if (m_rollback_to != NO_FRAME)
		confirmed = std::min(confirmed, m_rollback_to - 1);
	for (s64 f = m_confirmed + 1; f <= confirmed; f++)
		if (f % m_hash_every == 0)
			hash_frame(f);
	m_confirmed = std::max(m_confirmed, confirmed);
}

void netplay_session::hash_frame(s64 frame)
{
	if (!slot_valid(frame))
		return;
	checked_state &c = m_checked[(frame / m_hash_every) % 2];
	c.frame = frame;
	c.data = slot_for(frame).data;
	const u64 h = state_hash(c.data);
	if (frame >= m_test_dump_from && frame <= m_test_dump_to)
		if (FILE *f = std::fopen(util::string_format("%s.%d.state", m_log_path, int(frame)).c_str(), "wb"))
		{
			std::fwrite(slot_for(frame).data.data(), 1, m_state_size, f);
			std::fclose(f);
		}
	if (m_hash_log)
		std::fprintf(m_hash_log, "%lld,%016llx\n", (long long)frame, (unsigned long long)h);
	if (m_host)
	{
		packet_writer out(P_HASH, m_session, m_me);
		out.put<s32>(s32(frame));
		out.put<u64>(h);
		send_all(out);
	}
	else
	{
		hash_entry &e = m_hashes[(frame / m_hash_every) % m_hashes.size()];
		if (e.frame != frame)
			e = hash_entry();
		e.frame = frame;
		e.mine = h;
		e.have_mine = true;
		compare_hash(e);
	}
}

void netplay_session::compare_hash(hash_entry &e)
{
	if (!e.have_mine || !e.have_host)
		return;
	m_hash_compared++;
	const bool same = e.mine == e.host;
	e.have_mine = e.have_host = false;

	if (same)
		return;

	m_desyncs++;
	osd_printf_info("Netplay: desync at frame %d\n", int(e.frame));
	request_resync(m_cur, e.frame);
}

void netplay_session::request_resync(s64 boundary, s64 frame)
{
	const u32 now = now_ms();
	if (m_resync_pending && now - m_resync_ms < RESYNC_RETRY_MS)
		return;
	m_resync_pending = true;
	m_resync_ms = now;
	m_machine.popmessage("Netplay: repairing a desync");
	packet_writer out(P_RESYNC, m_session, m_me);
	out.put<s32>(s32(boundary));
	out.put<s32>(s32(frame));
	send(m_peers[0].endpoint, out);
}

// the host's state of an earlier frame: load it and catch up, as in a rollback
void netplay_session::apply_received_state(s64 boundary)
{
	const s64 frame = m_rx.frame;
	m_rx.ready = false;
	if (frame > boundary || boundary - frame >= INPUT_RING - 1)
		return; // too old to catch up from: the next hash mismatch asks again

	// the host sends the state of the frame that differed, if it still has it
	const checked_state &c = m_checked[(frame / m_hash_every) % 2];
	if (c.frame == frame)
		log_differences(c.data, m_rx.raw);
	else if (slot_valid(frame))
		log_differences(slot_for(frame).data, m_rx.raw);

	slot &s = slot_for(frame);
	s.data = std::move(m_rx.raw);
	s.frame = frame;
	s.valid = true;
	m_floor = frame;
	m_confirmed = frame;
	m_rollback_to = NO_FRAME;
	m_hashes.assign(m_hashes.size(), hash_entry());
	resimulate(frame, boundary);
	m_resync_pending = false;
	m_resyncs++;
	m_machine.popmessage("Netplay: desync repaired");
	osd_printf_info("Netplay: state of frame %d loaded at frame %d\n", int(frame), int(boundary));
}

// what a desync touched: the only way to find out from a session in the field
void netplay_session::log_differences(const std::vector<u8> &mine, const std::vector<u8> &host)
{
	std::string names;
	int count = 0;
	size_t offset = STATE_HEADER_SIZE;
	void *base;
	u32 valsize, valcount, blockcount, stride;
	for (int i = 0; ; i++)
	{
		const char *name = m_machine.save().indexed_item(i, base, valsize, valcount, blockcount, stride);
		if (!name)
			break;
		const size_t size = size_t(valsize) * valcount * blockcount;
		const bool skipped = std::find(m_hash_skip.begin(), m_hash_skip.end(), std::make_pair(offset, size)) != m_hash_skip.end();
		if (!skipped && memcmp(&mine[offset], &host[offset], size) && count++ < 16)
			names.append(names.empty() ? "" : ";").append(name);
		offset += size;
	}
	osd_printf_info("Netplay: %d items differ: %s\n", count, names);
}

// host: send the state of the frame that differed, so that the client can
// tell what differed, or else the newest confirmed one
void netplay_session::serve_resyncs()
{
	if (!m_host)
		return;
	for (int p = 1; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (!pr.resync_wanted || pr.tx.active)
			continue;
		const s64 newest = std::min(m_confirmed, pr.resync_frame);
		const checked_state &c = m_checked[(pr.resync_check / m_hash_every) % 2];
		if (c.frame == pr.resync_check && c.frame <= newest)
		{
			pr.resync_wanted = false;
			start_transfer(p, c.frame, c.data);
			continue;
		}
		const s64 oldest = std::max<s64>(START_FRAME, newest - s64(m_slots.size()) + 1);
		for (s64 f = newest; f >= oldest; f--)
		{
			if (slot_valid(f))
			{
				pr.resync_wanted = false;
				start_transfer(p, f, slot_for(f).data);
				break;
			}
		}
	}
}


//-------------------------------------------------
//  time sync: whoever runs ahead slows down a
//  little, so that nobody has to stall
//-------------------------------------------------

void netplay_session::update_time_sync(s64 boundary)
{
	const u32 now = now_ms();
	float ahead = 0;
	for (int p = 0; p < m_players; p++)
	{
		if (p == m_me)
			continue;
		peer &pr = m_peers[p];
		const float remote = float(pr.frame) + (float(now - pr.frame_rx_ms) + pr.rtt / 2) / m_frame_ms;
		pr.advantage += (float(boundary) - remote - pr.advantage) * 0.05f;
		ahead = std::max(ahead, (pr.advantage - pr.their_advantage) / 2);
	}

	const bool slow = m_slowed ? ahead > 0.25f : ahead > 1.0f;
	if (slow != m_slowed)
	{
		m_slowed = slow;
		m_machine.video().set_speed_factor(slow ? m_base_speed * 98 / 100 : m_base_speed);
	}
	if (m_slowed)
		m_slow_frames++;
}


//-------------------------------------------------
//  NAT traversal: each player learns its public
//  address from STUN servers and sends it to the
//  others by chat; the host then sends to the
//  players' addresses, which opens its router to
//  their packets, as their own packets to the host
//  open theirs (UDP hole punching)
//-------------------------------------------------

void netplay_session::start_stun()
{
	std::istringstream servers(m_machine.options().netplay_stun());
	std::string server;
	while (std::getline(servers, server, ';'))
	{
		const auto colon = server.rfind(':');
		if (server.empty() || colon == std::string::npos)
			continue;
		asio::error_code ec;
		udp::resolver resolver(m_io);
		auto results = resolver.resolve(udp::v4(), server.substr(0, colon), server.substr(colon + 1), ec);
		if (ec || results.empty())
		{
			osd_printf_verbose("Netplay: cannot resolve the STUN server %s\n", server);
			continue;
		}
		stun_query &q = m_stun.emplace_back();
		q.server = *results.begin();
		for (u8 &b : q.id)
			b = u8(m_sim_rng());
	}
	if (m_host)
	{
		// the players' addresses are typed or pasted on the console
		auto console = std::make_shared<console_lines>();
		m_console = console;
		std::thread([console] ()
		{
			std::string line;
			while (std::getline(std::cin, line))
			{
				std::lock_guard<std::mutex> guard(console->lock);
				console->lines.push_back(line);
			}
		}).detach();
	}
}

void netplay_session::stun_tick(u32 now)
{
	if (m_stun_reported || m_stun.empty() || now - m_stun_ms < STUN_RETRY_MS)
		return;
	if (m_stun_tries++ == STUN_TRIES)
	{
		m_stun_reported = true;
		bool any = false;
		for (const stun_query &q : m_stun)
			any = any || q.done;
		if (!any)
			osd_printf_info("Netplay: no answer from the STUN servers, so your public address is unknown\n");
		return;
	}
	m_stun_ms = now;
	for (const stun_query &q : m_stun)
	{
		if (q.done)
			continue;
		u8 request[20] = { 0x00, 0x01, 0x00, 0x00 };
		put_u32be(request + 4, STUN_COOKIE);
		std::copy(q.id.begin(), q.id.end(), request + 8);
		asio::error_code ec;
		m_socket.send_to(asio::buffer(request), q.server, 0, ec);
	}
}

// a binding response: its XOR-MAPPED-ADDRESS is our address seen from outside
bool netplay_session::handle_stun(const udp::endpoint &from, const u8 *data, size_t size)
{
	auto const server = std::find_if(m_stun.begin(), m_stun.end(), [&from] (const stun_query &q) { return q.server == from; });
	if (server == m_stun.end())
		return false;
	for (stun_query &q : m_stun)
	{
		if (q.done || from != q.server || size < 20 || get_u32be(data + 4) != STUN_COOKIE || !std::equal(q.id.begin(), q.id.end(), data + 8) || get_u16be(data) != 0x0101)
			continue;
		const size_t end = std::min<size_t>(size, 20 + get_u16be(data + 2));
		for (size_t a = 20; a + 4 <= end; a += 4 + ((get_u16be(data + a + 2) + 3) & ~3))
		{
			const u16 type = get_u16be(data + a);
			const u16 length = get_u16be(data + a + 2);
			if ((type != 0x0020 && type != 0x0001) || length < 8 || a + 4 + length > end || data[a + 5] != 0x01)
				continue;
			const u16 x = type == 0x0020 ? u16(STUN_COOKIE >> 16) : 0;
			const u32 y = type == 0x0020 ? STUN_COOKIE : 0;
			q.mapped = udp::endpoint(asio::ip::address_v4(get_u32be(data + a + 8) ^ y), get_u16be(data + a + 6) ^ x);
			q.done = true;
			if (type == 0x0020)
				break;
		}
		if (!q.done)
			continue;

		// the first answer gives the address; a second one that differs means
		// that the router changes port for every destination
		std::vector<udp::endpoint> seen;
		for (const stun_query &o : m_stun)
			if (o.done)
				seen.push_back(o.mapped);
		const std::string address = util::string_format("%s:%d", seen[0].address().to_string(), int(seen[0].port()));
		if (seen.size() == 1)
		{
			osd_printf_info("Netplay: your address: %s\n", address);
			if (m_host)
				osd_printf_info("Netplay: send it to the other players, then paste here the addresses they get, one per line\n");
			else
				osd_printf_info("Netplay: send it to the host\n");
			m_machine.popmessage("Netplay: your address is %s", address);
		}
		else if (seen.back() != seen[0])
		{
			osd_printf_info("Netplay: your router uses a different port for every destination (symmetric NAT): players outside your network will probably not reach you; use a VPN such as Tailscale\n");
			m_stun_reported = true;
		}
		if (seen.size() == m_stun.size())
			m_stun_reported = true;
		return true;
	}
	return true;
}

void netplay_session::read_console()
{
	std::vector<std::string> lines;
	{
		std::lock_guard<std::mutex> guard(m_console->lock);
		lines.swap(m_console->lines);
	}
	for (std::string &line : lines)
	{
		line = clean_address(line);
		const auto colon = line.rfind(':');
		asio::error_code ec;
		const auto address = asio::ip::make_address_v4(line.substr(0, colon), ec);
		const int port = colon == std::string::npos ? 0 : std::atoi(line.c_str() + colon + 1);
		if (line.empty())
			continue;
		if (ec || port <= 0 || port > 65535)
		{
			osd_printf_info("Netplay: \"%s\" is not an address like 203.0.113.5:40000\n", line);
			continue;
		}
		const udp::endpoint endpoint(address, u16(port));
		if (std::find(m_punch.begin(), m_punch.end(), endpoint) == m_punch.end())
		{
			m_punch.push_back(endpoint);
			osd_printf_info("Netplay: calling %s:%d\n", address.to_string(), port);
		}
	}
}

void netplay_session::punch_tick(u32 now)
{
	if (m_console)
		read_console();
	if (m_punch.empty() || now - m_punch_ms < PUNCH_MS)
		return;
	m_punch_ms = now;
	packet_writer out(P_PUNCH, 0, 0);
	for (const udp::endpoint &to : m_punch)
	{
		bool joined = false;
		for (int p = 1; p < m_players; p++)
			joined = joined || (m_peers[p].connected && m_peers[p].endpoint == to);
		if (!joined)
			send(to, out);
	}
}


//-------------------------------------------------
//  transport
//-------------------------------------------------

void netplay_session::send(const udp::endpoint &to, const packet_writer &packet)
{
	if (m_sim_delay || m_sim_jitter || m_sim_loss)
	{
		if (int(m_sim_rng() % 100) < m_sim_loss)
			return;
		const u32 extra = m_sim_jitter ? m_sim_rng() % (m_sim_jitter + 1) : 0;
		m_sim_queue.push_back({ now_ms() + m_sim_delay + extra, to, packet.data() });
		return;
	}
	asio::error_code ec;
	m_socket.send_to(asio::buffer(packet.data()), to, 0, ec);
	m_tx_bytes += packet.data().size();
}

void netplay_session::flush_simulated()
{
	// packets leave in time order, as out of order delivery is what jitter does to them anyway
	const u32 now = now_ms();
	for (auto it = m_sim_queue.begin(); it != m_sim_queue.end(); )
	{
		if (s32(now - it->time) >= 0)
		{
			asio::error_code ec;
			m_socket.send_to(asio::buffer(it->data), it->endpoint, 0, ec);
			m_tx_bytes += it->data.size();
			it = m_sim_queue.erase(it);
		}
		else
			++it;
	}
}

void netplay_session::send_all(const packet_writer &packet)
{
	for (int p = 0; p < m_players; p++)
		if (p != m_me && m_peers[p].connected)
			send(m_peers[p].endpoint, packet);
}

void netplay_session::poll()
{
	flush_simulated();
	u8 buffer[2048];
	for (;;)
	{
		udp::endpoint from;
		asio::error_code ec;
		const size_t size = m_socket.receive_from(asio::buffer(buffer), from, 0, ec);
		if (ec == asio::error::would_block || ec == asio::error::try_again)
			return;
		if (ec)
			continue; // e.g. ICMP port unreachable from a peer that left
		m_rx_bytes += size;
		if (!handle_stun(from, buffer, size))
			handle_packet(from, buffer, size);
		if (m_phase == phase::ENDED)
			return;
	}
}

void netplay_session::handle_packet(const udp::endpoint &from, const u8 *data, size_t size)
{
	packet_reader in(data, size);
	const u32 magic = in.get<u32>();
	const u32 session = in.get<u32>();
	const u8 type = in.get<u8>();
	const int sender = in.get<u8>();
	if (!in.ok() || magic != MAGIC)
		return;

	if (type == P_HELLO)
	{
		if (m_host)
			handle_hello(from, in);
		return;
	}
	if (type == P_PUNCH)
		return; // only there to open the sender's router

	if (type == P_WELCOME || type == P_REJECT)
	{
		if (m_host || from != m_host_endpoint)
			return;
		if (type == P_REJECT)
		{
			const std::string reason = in.get_string();
			if (in.ok())
				end_session(util::string_format("refused by the host: %s", reason).c_str());
		}
		else if (!m_welcomed)
		{
			m_session = session;
			handle_welcome(in);
		}
		return;
	}

	// from here on only members of this session
	if (session != m_session || !m_session || sender >= m_players || sender == m_me || m_slots.empty())
		return;
	if (type == P_START)
	{
		if (!m_host && from == m_host_endpoint)
			handle_start(in);
		return;
	}
	peer &pr = m_peers[sender];
	if (!pr.connected || pr.endpoint != from)
		return;
	pr.last_rx_ms = now_ms();

	switch (type)
	{
	case P_GO:
		if (!m_host && sender == 0)
			m_go = true;
		break;
	case P_INPUT:
		handle_input(sender, in);
		break;
	case P_HASH:
		if (!m_host && sender == 0)
			handle_hash(in);
		break;
	case P_RESYNC:
		if (m_host)
		{
			const s64 boundary = in.get<s32>();
			const s64 frame = in.get<s32>();
			if (in.ok() && boundary <= m_cur + INPUT_RING / 2)
			{
				pr.resync_wanted = true;
				pr.resync_frame = boundary;
				pr.resync_check = frame;
				m_desyncs++;
			}
		}
		break;
	case P_CHUNK:
		if (!m_host && sender == 0)
			handle_chunk(in);
		break;
	case P_CHUNK_ACK:
		if (m_host)
			handle_chunk_ack(sender, in);
		break;
	case P_BYE:
		if (m_phase == phase::RUNNING || m_phase == phase::LOBBY)
			end_session(util::string_format("player %d left", sender + 1).c_str());
		break;
	default:
		break;
	}
}

void netplay_session::handle_hello(const udp::endpoint &from, packet_reader &in)
{
	const u32 protocol = in.get<u32>();
	const std::string build = in.get_string();
	const std::string system = in.get_string();
	const u32 state_size = in.get<u32>();
	const u64 layout = in.get<u64>();
	const u64 rom = in.get<u64>();
	const u64 inputs = in.get<u64>();
	if (!in.ok() || m_phase != phase::LOBBY)
		return;

	std::string reason;
	if (protocol != PROTOCOL || build != emulator_info::get_build_version())
		reason = util::string_format("different MAME build: the host has %s, you have %s", emulator_info::get_build_version(), build);
	else if (system != m_machine.system().name)
		reason = "different game";
	else if (rom != m_rom_hash)
		reason = "different ROM contents";
	else if (state_size != m_state_size || layout != m_layout_hash)
		reason = "different save state layout";
	else if (inputs != m_input_hash)
		reason = "different DIP switch settings";

	int player = -1;
	for (int p = 1; p < m_players && player < 0; p++)
		if (m_peers[p].connected && m_peers[p].endpoint == from)
			player = p;
	if (reason.empty() && player < 0)
	{
		for (int p = 1; p < m_players && player < 0; p++)
			if (!m_peers[p].connected)
				player = p;
		if (player < 0)
			reason = "session full";
		else
		{
			m_peers[player].connected = true;
			m_peers[player].endpoint = from;
			m_peers[player].last_rx_ms = now_ms();
			osd_printf_info("Netplay: player %d joined from %s\n", player + 1, from.address().to_string());
		}
	}
	if (!reason.empty())
	{
		packet_writer out(P_REJECT, 0, 0);
		out.put_string(reason);
		send(from, out);
		osd_printf_info("Netplay: refused %s: %s\n", from.address().to_string(), reason);
		return;
	}

	send_welcome(player);
	if (m_all_joined)
	{
		send_start(player);
		return;
	}

	m_all_joined = true;
	for (int p = 1; p < m_players; p++)
		m_all_joined = m_all_joined && m_peers[p].connected;
	if (m_all_joined)
	{
		for (int p = 1; p < m_players; p++)
		{
			send_start(p);
			start_transfer(p, START_FRAME, slot_for(START_FRAME).data);
		}
	}
}

void netplay_session::send_welcome(int player)
{
	packet_writer out(P_WELCOME, m_session, 0);
	out.put<u8>(u8(player));
	out.put<u8>(u8(m_players));
	out.put<u8>(u8(m_delay));
	out.put<u8>(u8(m_window));
	out.put<u16>(u16(m_hash_every));
	out.put<u64>(m_base_time);
	out.put_string(m_exclude);
	send(m_peers[player].endpoint, out);
}

void netplay_session::handle_welcome(packet_reader &in)
{
	const int player = in.get<u8>();
	const int players = in.get<u8>();
	const int delay = in.get<u8>();
	const int window = in.get<u8>();
	const int hash_every = in.get<u16>();
	const u64 base_time = in.get<u64>();
	const std::string exclude = in.get_string();
	if (!in.ok() || players < 2 || players > MAX_PLAYERS || player < 1 || player >= players || delay > 8 || window < 1 || window > 60 || hash_every < 1)
		return;

	m_me = player;
	m_players = players;
	m_delay = delay;
	m_window = window;
	m_hash_every = hash_every;
	m_exclude = exclude;
	m_machine.set_base_time(time_t(base_time));
	m_peers[0].connected = true;
	m_peers[0].endpoint = m_host_endpoint;
	m_peers[0].last_rx_ms = now_ms();
	start_session_buffers();
	m_welcomed = true;
}

// the addresses of the other clients, as the host sees them
void netplay_session::send_start(int player)
{
	packet_writer out(P_START, m_session, 0);
	for (int p = 1; p < m_players; p++)
	{
		out.put<u32>(m_peers[p].endpoint.address().to_v4().to_uint());
		out.put<u16>(m_peers[p].endpoint.port());
	}
	send(m_peers[player].endpoint, out);
}

void netplay_session::handle_start(packet_reader &in)
{
	std::vector<udp::endpoint> endpoints;
	for (int p = 1; p < m_players; p++)
	{
		const u32 address = in.get<u32>();
		const u16 port = in.get<u16>();
		endpoints.emplace_back(asio::ip::address_v4(address), port);
	}
	if (!in.ok())
		return;
	for (int p = 1; p < m_players; p++)
	{
		if (p != m_me)
		{
			m_peers[p].connected = true;
			m_peers[p].endpoint = endpoints[p - 1];
			m_peers[p].last_rx_ms = now_ms();
		}
	}
	m_started = true;
}

void netplay_session::send_inputs()
{
	const s64 newest = m_known[m_me];
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		if (p == m_me || !pr.connected)
			continue;
		const s64 first = pr.acked + 1;
		const int count = int(std::clamp<s64>(newest - first + 1, 0, MAX_INPUTS_PER_PACKET));
		packet_writer out(P_INPUT, m_session, m_me);
		out.put<s32>(s32(m_cur));
		out.put<s32>(s32(m_known[p]));
		out.put<u32>(now);
		out.put<u32>(pr.echo_time);
		out.put<u16>(u16(std::min<u32>(now - pr.echo_rx_ms, 65535)));
		out.put<s16>(s16(std::clamp(pr.advantage * 16, -32000.0f, 32000.0f)));
		out.put<s32>(s32(first));
		out.put<u8>(u8(count));
		for (int i = 0; i < count; i++)
			out.put<u32>(m_input[m_me][(first + i) % INPUT_RING]);
		send(pr.endpoint, out);
	}
}

void netplay_session::handle_input(int sender, packet_reader &in)
{
	peer &pr = m_peers[sender];
	const s64 frame = in.get<s32>();
	const s64 acked = in.get<s32>();
	const u32 time = in.get<u32>();
	const u32 echo = in.get<u32>();
	const u32 hold = in.get<u16>();
	const float advantage = float(in.get<s16>()) / 16;
	const s64 first = in.get<s32>();
	const int count = in.get<u8>();
	if (!in.ok() || in.remaining_size() < size_t(count) * 4)
		return;

	const u32 now = now_ms();
	if (frame >= pr.frame)
	{
		pr.frame = frame;
		pr.frame_rx_ms = now;
		pr.their_advantage = advantage;
		pr.echo_time = time;
		pr.echo_rx_ms = now;
		if (echo)
		{
			const float sample = float(now - echo - hold);
			if (sample >= 0 && sample < 10000)
				pr.rtt = pr.rtt ? pr.rtt + (sample - pr.rtt) * 0.1f : sample;
		}
	}
	if (acked <= m_known[m_me])
		pr.acked = std::max(pr.acked, acked);

	for (int i = 0; i < count; i++)
	{
		const s64 f = first + i;
		const u32 word = in.get<u32>();
		if (f <= m_known[sender])
			continue;
		if (f != m_known[sender] + 1 || f > m_cur + INPUT_RING / 2)
			break;
		m_input[sender][f % INPUT_RING] = word;
		m_known[sender] = f;
		if (f <= m_cur && f > m_floor && m_used[sender][f % INPUT_RING] != word)
			m_rollback_to = std::min(m_rollback_to, f);
	}
	if (sender == 0)
		m_go = true; // the host is running, in case its "go" was lost
}

void netplay_session::handle_hash(packet_reader &in)
{
	const s64 frame = in.get<s32>();
	const u64 hash = in.get<u64>();
	if (!in.ok() || frame <= m_floor || frame % m_hash_every != 0)
		return;
	hash_entry &e = m_hashes[(frame / m_hash_every) % m_hashes.size()];
	if (e.frame != frame)
		e = hash_entry();
	e.frame = frame;
	e.host = hash;
	e.have_host = true;
	compare_hash(e);
}

void netplay_session::start_transfer(int player, s64 frame, const std::vector<u8> &state)
{
	auto data = std::make_shared<std::vector<u8>>(ZSTD_compressBound(state.size()));
	const size_t size = ZSTD_compress(data->data(), data->size(), state.data(), state.size(), 1);
	if (ZSTD_isError(size))
	{
		end_session("cannot compress the state");
		return;
	}
	data->resize(size);

	transfer_out &tx = m_peers[player].tx;
	tx.active = true;
	tx.data = std::move(data);
	tx.id = m_next_transfer_id++;
	tx.frame = frame;
	tx.total = u32((size + CHUNK_SIZE - 1) / CHUNK_SIZE);
	tx.next_missing = 0;
	tx.received_after.fill(0);
	tx.send_pos = 0;
	tx.last_tick_ms = tx.last_pass_ms = now_ms();
	osd_printf_verbose("Netplay: sending the state of frame %d to player %d, %u bytes\n", int(frame), player + 1, unsigned(size));
}

void netplay_session::tick_transfers()
{
	if (!m_host)
		return;
	for (int p = 1; p < m_players; p++)
	{
		peer &pr = m_peers[p];
		transfer_out &tx = pr.tx;
		if (!tx.active)
			continue;
		const u32 now = now_ms();
		u32 budget = std::min((now - tx.last_tick_ms) * CHUNKS_PER_MS, CHUNK_WINDOW);
		if (budget)
			tx.last_tick_ms = now;
		for (u32 tried = 0; budget && tried < 2 * CHUNK_WINDOW; tried++)
		{
			const u32 limit = std::min(tx.total, tx.next_missing + CHUNK_WINDOW);
			if (tx.send_pos >= limit)
			{
				if (now - tx.last_pass_ms < CHUNK_RESEND_MS)
					break;
				tx.last_pass_ms = now;
				tx.send_pos = tx.next_missing;
			}
			const u32 index = tx.send_pos++;
			const u32 after = index - tx.next_missing - 1;
			if (index > tx.next_missing && after < CHUNK_WINDOW && BIT(tx.received_after[after / 64], after % 64))
				continue;

			const size_t offset = size_t(index) * CHUNK_SIZE;
			const size_t size = std::min<size_t>(CHUNK_SIZE, tx.data->size() - offset);
			packet_writer out(P_CHUNK, m_session, m_me);
			out.put<u32>(tx.id);
			out.put<s32>(s32(tx.frame));
			out.put<u32>(u32(tx.data->size()));
			out.put<u32>(index);
			out.put_bytes(tx.data->data() + offset, size);
			send(pr.endpoint, out);
			budget--;
		}
	}
}

void netplay_session::handle_chunk(packet_reader &in)
{
	const u32 id = in.get<u32>();
	const s64 frame = in.get<s32>();
	const u32 compressed = in.get<u32>();
	const u32 index = in.get<u32>();
	if (!in.ok())
		return;
	m_send_ack = true;
	if (id == m_rx.done_id)
		return;

	if (!m_rx.active || id != m_rx.id)
	{
		if (compressed == 0 || compressed > ZSTD_compressBound(m_state_size))
			return;
		m_rx.active = true;
		m_rx.id = id;
		m_rx.frame = frame;
		m_rx.compressed = compressed;
		m_rx.total = (compressed + CHUNK_SIZE - 1) / CHUNK_SIZE;
		m_rx.next_missing = 0;
		m_rx.have.assign(m_rx.total, 0);
		m_rx.data.resize(compressed);
	}
	if (frame != m_rx.frame || compressed != m_rx.compressed || index >= m_rx.total)
		return;

	const size_t offset = size_t(index) * CHUNK_SIZE;
	const size_t size = std::min<size_t>(CHUNK_SIZE, compressed - offset);
	if (in.remaining_size() != size)
		return;
	memcpy(&m_rx.data[offset], in.remaining(), size);
	m_rx.have[index] = 1;
	while (m_rx.next_missing < m_rx.total && m_rx.have[m_rx.next_missing])
		m_rx.next_missing++;

	if (m_rx.next_missing == m_rx.total)
	{
		m_rx.active = false;
		m_rx.done_id = id;
		m_rx.raw.resize(m_state_size);
		const size_t raw = ZSTD_decompress(m_rx.raw.data(), m_rx.raw.size(), m_rx.data.data(), m_rx.data.size());
		if (ZSTD_isError(raw) || raw != m_state_size)
		{
			end_session("the host's state is corrupt");
			return;
		}
		m_rx.ready = true;
	}
}

void netplay_session::send_chunk_ack()
{
	if (!m_send_ack)
		return;
	m_send_ack = false;
	const bool done = m_rx.done_id && !m_rx.active;
	if (done && !m_started)
		return; // "done" lets the session start: first the addresses of the other clients are needed
	u64 after[CHUNK_WINDOW / 64] = { };
	if (!done)
		for (u32 i = 0; i < CHUNK_WINDOW && m_rx.next_missing + 1 + i < m_rx.total; i++)
			if (m_rx.have[m_rx.next_missing + 1 + i])
				after[i / 64] |= u64(1) << (i % 64);
	packet_writer out(P_CHUNK_ACK, m_session, m_me);
	out.put<u32>(done ? m_rx.done_id : m_rx.id);
	out.put<u8>(done ? 1 : 0);
	out.put<u32>(m_rx.next_missing);
	for (u64 bits : after)
		out.put<u64>(bits);
	send(m_peers[0].endpoint, out);
}

void netplay_session::handle_chunk_ack(int sender, packet_reader &in)
{
	peer &pr = m_peers[sender];
	const u32 id = in.get<u32>();
	const bool done = in.get<u8>() != 0;
	const u32 next_missing = in.get<u32>();
	std::array<u64, CHUNK_WINDOW / 64> after;
	for (u64 &bits : after)
		bits = in.get<u64>();
	if (!in.ok())
		return;

	// the starting state is the first transfer: finished means ready to go
	if (done && id == pr.tx.id)
	{
		pr.tx.active = false;
		pr.session_ready = true;
	}
	else if (done && pr.session_ready && m_phase == phase::RUNNING)
	{
		packet_writer out(P_GO, m_session, m_me);
		send(pr.endpoint, out);
	}
	else if (pr.tx.active && id == pr.tx.id && next_missing <= pr.tx.total && next_missing >= pr.tx.next_missing)
	{
		pr.tx.next_missing = next_missing;
		pr.tx.received_after = after;
	}
}

void netplay_session::check_timeouts()
{
	const u32 now = now_ms();
	for (int p = 0; p < m_players; p++)
		if (p != m_me && m_peers[p].connected && now - m_peers[p].last_rx_ms >= PEER_TIMEOUT_MS)
			end_session(util::string_format("no packets from player %d", p + 1).c_str());
}

void netplay_session::end_session(const char *reason)
{
	if (m_phase == phase::ENDED)
		return;
	osd_printf_info("Netplay: session over: %s\n", reason);
	if (m_phase == phase::RUNNING || m_phase == phase::LOBBY)
	{
		packet_writer out(P_BYE, m_session, m_me);
		for (int i = 0; i < 3; i++)
			send_all(out);
		flush_simulated();
	}
	m_end_reason = reason;
	m_phase = phase::ENDED;
	write_log();
	m_machine.schedule_exit();
}


void netplay_session::write_log()
{
	if (m_log_path.empty())
		return;
	FILE *f = std::fopen((m_log_path + ".json").c_str(), "w");
	if (!f)
		return;
	const double seconds = m_session_start ? double(osd_ticks() - m_session_start) / osd_ticks_per_second() : 0;
	float rtt = 0;
	for (int p = 0; p < m_players; p++)
		if (p != m_me)
			rtt = std::max(rtt, m_peers[p].rtt);
	std::fprintf(f, "{\"system\":\"%s\",\"player\":%d,\"players\":%d,\"frames\":%lld,\"seconds\":%.2f,\"end\":\"%s\",\"rtt_ms\":%.1f,"
			"\"rollbacks\":%llu,\"resim_frames\":%llu,\"rollback_ms_max\":%.2f,\"rollback_ms_mean\":%.2f,\"unrecoverable\":%llu,\"saves_failed\":%llu,"
			"\"hash_compared\":%llu,\"desyncs\":%llu,\"resyncs\":%llu,\"stall_ms\":%llu,\"slow_frames\":%llu,\"tx_bytes\":%llu,\"rx_bytes\":%llu,\"depth\":[",
			m_machine.system().name, m_me + 1, m_players, (long long)m_cur, seconds, m_end_reason.c_str(), rtt,
			(unsigned long long)m_rollbacks, (unsigned long long)m_resim_frames, m_rollback_ms_max, m_rollbacks ? m_rollback_ms_total / m_rollbacks : 0.0,
			(unsigned long long)m_unrecoverable, (unsigned long long)m_saves_failed,
			(unsigned long long)m_hash_compared, (unsigned long long)m_desyncs, (unsigned long long)m_resyncs,
			(unsigned long long)m_stall_ms, (unsigned long long)m_slow_frames, (unsigned long long)m_tx_bytes, (unsigned long long)m_rx_bytes);
	for (size_t i = 0; i < std::size(m_depth_histogram); i++)
		std::fprintf(f, "%s%llu", i ? "," : "", (unsigned long long)m_depth_histogram[i]);
	std::fprintf(f, "]}\n");
	std::fclose(f);
}

} // anonymous namespace


std::unique_ptr<netplay_manager> netplay_manager::create(running_machine &machine)
{
	const bool host = machine.options().netplay_host_port() != 0;
	const bool join = *machine.options().netplay_join() != 0;
	if (!host && !join)
		return nullptr;
	if (host && join)
		throw emu_fatalerror("Netplay: -netplay_host and -netplay_join exclude each other");
	return std::make_unique<netplay_session>(machine, host);
}

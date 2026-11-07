// license:BSD-3-Clause
/***************************************************************************

    netplay.h

    Rollback netplay session between 2 to 4 instances over UDP.

    Enabled by -netplay_host <port> or -netplay_join <host:port>. Every player
    sends its own inputs to all the others; missing inputs are predicted and
    corrected by rolling back. The session starts from the host's state, and
    the same transfer repairs a client whose state hash stops matching the
    host's.

***************************************************************************/

#ifndef MAME_EMU_NETPLAY_H
#define MAME_EMU_NETPLAY_H

#pragma once

#include <memory>


class netplay_manager
{
public:
	static std::unique_ptr<netplay_manager> create(running_machine &machine);
	virtual ~netplay_manager() = default;

	// true once the session runs: pausing, saving and resetting are then off
	virtual bool in_session() const = 0;

	// hooks
	virtual void frame_inputs_begin() = 0;                // ioport frame_update start (end of the current frame)
	virtual void frame_inputs_port(ioport_port &port) = 0; // per port, after the OS sample
	virtual void post_timeslice() = 0;                    // main loop, after every timeslice
};

#endif // MAME_EMU_NETPLAY_H

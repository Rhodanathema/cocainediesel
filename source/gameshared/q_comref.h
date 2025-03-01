/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#pragma once

// user command communications
constexpr unsigned int CMD_BACKUP = 64; // allow a lot of command backups for very fast systems

// pmove_state_t is the information necessary for client side movement
// prediction
enum pmtype_t {
	// can accelerate and turn
	PM_NORMAL,
	PM_SPECTATOR,

	// no acceleration or turning
	PM_FREEZE,
	PM_CHASECAM     // same as freeze, but so client knows it's in chasecam
};

// pmove->pm_flags
#define PMF_ON_GROUND       ( 1 << 0 )
#define PMF_NO_PREDICTION   ( 1 << 1 )  // temporarily disables prediction (used for grappling hook)
#define PMF_ABILITY1_HELD   ( 1 << 2 )  // Special held flag
#define PMF_ABILITY2_HELD   ( 1 << 3 )  // Jump held flag
#define PMF_CLIMBING        ( 1 << 4 )  // Jump held flag

// note that Q_rint was causing problems here
// (spawn looking straight up\down at delta_angles wrapping)

#define ANGLE2SHORT( x )    ( (int)( ( x ) * 65536 / 360 ) & 65535 )
#define SHORT2ANGLE( x )    ( ( x ) * ( 360.0 / 65536 ) )

//==============================================

constexpr const char * MASTER_SERVERS[] = { "dpmaster.deathmask.net", "excalibur.nvg.ntnu.no" };

// SyncEntityState is the information conveyed from the server
// in an update message about entities that the client will
// need to render in some way

// edict->svflags
#define SVF_NOCLIENT         ( 1 << 0 )      // don't send entity to clients, even if it has effects
#define SVF_SOUNDCULL        ( 1 << 1 )      // distance culling
#define SVF_FAKECLIENT       ( 1 << 2 )      // do not try to send anything to this client
#define SVF_BROADCAST        ( 1 << 3 )      // global sound
#define SVF_ONLYTEAM         ( 1 << 4 )      // this entity is only transmited to clients with the same ent->s.team value
#define SVF_FORCEOWNER       ( 1 << 5 )      // this entity forces the entity at s.ownerNum to be included in the snapshot
#define SVF_ONLYOWNER        ( 1 << 6 )      // this entity is only transmitted to its owner
#define SVF_OWNERANDCHASERS  ( 1 << 7 )      // this entity is only transmitted to its owner and people spectating them
#define SVF_FORCETEAM        ( 1 << 8 )      // this entity is always transmitted to clients with the same ent->s.team value
#define SVF_NEVEROWNER       ( 1 << 9 )      // this entity is tramitted to everyone but its owner

// SyncEntityState->event values
// entity events are for effects that take place relative
// to an existing entities origin. Very network efficient.

#define ISEVENTENTITY( x ) ( ( (SyncEntityState *)x )->type >= EVENT_ENTITIES_START )

//==============================================

enum connstate_t {
	CA_UNINITIALIZED,
	CA_DISCONNECTED,                    // not talking to a server
	CA_CONNECTING,                      // sending request packets to the server
	CA_HANDSHAKE,                       // netchan_t established, waiting for svc_serverdata
	CA_CONNECTED,                       // connection established, game module not loaded
	CA_ACTIVE,                          // game views should be displayed
};

enum server_state_t {
	ss_dead,        // no map loaded
	ss_loading,     // spawning level edicts
	ss_game         // actively running
};

#define DROP_FLAG_AUTORECONNECT 1       // it's okay try reconnectting automatically

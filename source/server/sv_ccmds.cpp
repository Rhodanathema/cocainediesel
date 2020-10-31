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

#include "server.h"


//===============================================================================
//
//OPERATOR CONSOLE ONLY COMMANDS
//
//These commands can only be entered from stdin or by a remote operator datagram
//===============================================================================

/*
* SV_FindPlayer
* Helper for the functions below. It finds the client_t for the given name or id
*/
static client_t *SV_FindPlayer( const char *s ) {
	client_t *cl;
	client_t *player;
	int i;
	int idnum = 0;

	if( !s ) {
		return NULL;
	}

	// numeric values are just slot numbers
	if( s[0] >= '0' && s[0] <= '9' ) {
		idnum = atoi( s );
		if( idnum < 0 || idnum >= sv_maxclients->integer ) {
			Com_Printf( "Bad client slot: %i\n", idnum );
			return NULL;
		}

		player = &svs.clients[idnum];
		goto found_player;
	}

	// check for a name match
	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}
		if( !Q_stricmp( cl->name, s ) ) {
			player = cl;
			goto found_player;
		}
	}

	Com_Printf( "Userid %s is not on the server\n", s );
	return NULL;

found_player:
	if( !player->state || !player->edict ) {
		Com_Printf( "Client %s is not active\n", s );
		return NULL;
	}

	return player;
}

/*
* SV_Map_f
*
* User command to change the map
* map: restart game, and start map
* devmap: restart game, enable cheats, and start map
* gamemap: just start the map
*/
static void SV_Map_f( void ) {
	if( Cmd_Argc() < 2 ) {
		Com_Printf( "Usage: %s <map>\n", Cmd_Argv( 0 ) );
		return;
	}

	const char * map;

	// if map "<map>" is used Cmd_Args() will return the "" as well.
	if( Cmd_Argc() == 2 ) {
		map = Cmd_Argv( 1 );
	} else {
		map = Cmd_Args();
	}

	Com_DPrintf( "SV_GameMap(%s)\n", map );

	if( !MapExists( map ) ) {
		RefreshMapList();
		if( !MapExists( map ) ) {
			Com_Printf( "Couldn't find map: %s\n", map );
			return;
		}
	}

	if( !Q_stricmp( Cmd_Argv( 0 ), "map" ) || !Q_stricmp( Cmd_Argv( 0 ), "devmap" ) ) {
		sv.state = ss_dead; // don't save current level when changing
	}

	SV_UpdateMaster();

	// start up the next map
	SV_Map( map, !Q_stricmp( Cmd_Argv( 0 ), "devmap" ) );
}

//===============================================================

/*
* SV_Status_f
*/
void SV_Status_f( void ) {
	int i, j, l;
	client_t *cl;
	const char *s;
	int ping;
	if( !svs.clients ) {
		Com_Printf( "No server running.\n" );
		return;
	}
	Com_Printf( "map              : %s\n", sv.mapname );

	Com_Printf( "num score ping name                            lastmsg address               port  \n" );
	Com_Printf( "--- ----- ---- ------------------------------- ------- --------------------- ------\n" );
	for( i = 0, cl = svs.clients; i < sv_maxclients->integer; i++, cl++ ) {
		if( !cl->state ) {
			continue;
		}
		Com_Printf( "%3i ", i );
		Com_Printf( "%5i ", cl->edict->r.client->r.frags );

		if( cl->state == CS_CONNECTED ) {
			Com_Printf( "CNCT " );
		} else if( cl->state == CS_ZOMBIE ) {
			Com_Printf( "ZMBI " );
		} else if( cl->state == CS_CONNECTING ) {
			Com_Printf( "AWAI " );
		} else {
			ping = cl->ping < 9999 ? cl->ping : 9999;
			Com_Printf( "%4i ", ping );
		}

		Com_Printf( "%s", cl->name );
		l = MAX_NAME_CHARS - (int)strlen( cl->name );
		for( j = 0; j < l; j++ )
			Com_Printf( " " );

		Com_Printf( "%7i ", (int)(svs.realtime - cl->lastPacketReceivedTime) );

		s = NET_AddressToString( &cl->netchan.remoteAddress );
		Com_Printf( "%s", s );
		l = 21 - (int)strlen( s );
		for( j = 0; j < l; j++ )
			Com_Printf( " " );
		Com_Printf( " " ); // always add at least one space between the columns because IPv6 addresses are long

		Com_Printf( "%5i", cl->netchan.game_port );
		Com_Printf( "\n" );
	}
	Com_Printf( "\n" );
}

/*
* SV_Heartbeat_f
*/
static void SV_Heartbeat_f( void ) {
	svc.nextHeartbeat = Sys_Milliseconds();
}

/*
* SV_Serverinfo_f
* Examine or change the serverinfo string
*/
static void SV_Serverinfo_f( void ) {
	Com_Printf( "Server info settings:\n" );
	Info_Print( Cvar_Serverinfo() );
}

/*
* SV_DumpUser_f
* Examine all a users info strings
*/
static void SV_DumpUser_f( void ) {
	client_t *client;
	if( Cmd_Argc() != 2 ) {
		Com_Printf( "Usage: info <userid>\n" );
		return;
	}

	client = SV_FindPlayer( Cmd_Argv( 1 ) );
	if( !client ) {
		return;
	}

	Com_Printf( "userinfo\n" );
	Com_Printf( "--------\n" );
	Info_Print( client->userinfo );
}

/*
* SV_KillServer_f
* Kick everyone off, possibly in preparation for a new game
*/
static void SV_KillServer_f( void ) {
	if( !svs.initialized ) {
		return;
	}

	SV_ShutdownGame( "Server was killed", false );
}

//===========================================================

/*
* SV_InitOperatorCommands
*/
void SV_InitOperatorCommands( void ) {
	Cmd_AddCommand( "heartbeat", SV_Heartbeat_f );
	Cmd_AddCommand( "status", SV_Status_f );
	Cmd_AddCommand( "serverinfo", SV_Serverinfo_f );
	Cmd_AddCommand( "dumpuser", SV_DumpUser_f );

	Cmd_AddCommand( "map", SV_Map_f );
	Cmd_AddCommand( "devmap", SV_Map_f );
	Cmd_AddCommand( "gamemap", SV_Map_f );
	Cmd_AddCommand( "killserver", SV_KillServer_f );

	Cmd_AddCommand( "serverrecord", SV_Demo_Start_f );
	Cmd_AddCommand( "serverrecordstop", SV_Demo_Stop_f );
	Cmd_AddCommand( "serverrecordcancel", SV_Demo_Cancel_f );
	Cmd_AddCommand( "serverrecordpurge", SV_Demo_Purge_f );

	Cmd_SetCompletionFunc( "map", CompleteMapName );
	Cmd_SetCompletionFunc( "devmap", CompleteMapName );
	Cmd_SetCompletionFunc( "gamemap", CompleteMapName );
}

/*
* SV_ShutdownOperatorCommands
*/
void SV_ShutdownOperatorCommands( void ) {
	Cmd_RemoveCommand( "heartbeat" );
	Cmd_RemoveCommand( "status" );
	Cmd_RemoveCommand( "serverinfo" );
	Cmd_RemoveCommand( "dumpuser" );

	Cmd_RemoveCommand( "map" );
	Cmd_RemoveCommand( "devmap" );
	Cmd_RemoveCommand( "gamemap" );
	Cmd_RemoveCommand( "killserver" );

	Cmd_RemoveCommand( "serverrecord" );
	Cmd_RemoveCommand( "serverrecordstop" );
	Cmd_RemoveCommand( "serverrecordcancel" );
	Cmd_RemoveCommand( "serverrecordpurge" );
}

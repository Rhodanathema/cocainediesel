#include "qcommon/base.h"
#include "qcommon/array.h"
#include "qcommon/fs.h"
#include "qcommon/string.h"

#include "game/g_local.h"

#include "qcommon/cmodel.h"

struct TeamQueue {
	int players[ MAX_CLIENTS ];
};

static struct GladiatorState {
	s64 round_state_start;
	s64 round_state_end;
	s32 countdown;

	TeamQueue teams[ Team_Count ];

	bool randomize_arena;

	s64 bomb_action_time;
	bool bomb_exploded;
} gladiator_state;

static constexpr int countdown_seconds = 2;
static constexpr u32 bomb_explosion_effect_radius = 1024;
static Cvar * g_glad_bombtimer;

static void RemovePlayerFromQueue( TeamQueue * team, size_t idx ) {
	team->players[ idx ] = -1; // in case this is the last element
	memcpy( &team->players[ idx ], &team->players[ idx + 1 ], ARRAY_COUNT( team->players ) - ( idx + 1 ) );
}

static void RemovePlayerFromAllQueues( int player ) {
	for( TeamQueue & team : gladiator_state.teams ) {
		for( size_t i = 0; i < ARRAY_COUNT( team.players ); i++ ) {
			if( team.players[ i ] == player ) {
				RemovePlayerFromQueue( &team, i );
			}
		}
	}
}

static void Enqueue( TeamQueue * team, int player ) {
	for( int & slot : team->players ) {
		if( slot == -1 ) {
			slot = player;
			return;
		}
	}

	Assert( false );
}

static int Rotate( TeamQueue * team ) {
	int player = team->players[ 0 ];
	RemovePlayerFromQueue( team, 0 );
	Enqueue( team, player );
	return player;
}

static void GhostEveryone() {
	for( int i = 0; i < server_gs.maxclients; i++ ) {
		edict_t * ent = PLAYERENT( i );
		if( PF_GetClientState( i ) >= CS_SPAWNED ) {
			GClip_UnlinkEntity( ent );
			G_ClientRespawn( ent, true );
		}
	}
}

static void BombExplode() {
	server_gs.gameState.exploding = true;
	server_gs.gameState.exploded_at = svs.gametime;

	Vec3 zero( 0.0f );
	G_SpawnEvent( EV_BOMB_EXPLOSION, bomb_explosion_effect_radius, &zero );
}

static void BombKill() {
	Vec3 zero( 0.0f );
	gladiator_state.bomb_exploded = true;

	for( int i = 0; i < server_gs.maxclients; i++ ) {
		G_Damage( PLAYERENT( i ), world, world, zero, zero, zero, 100.0f, 0.0f, 0, WorldDamage_Explosion );
	}
}

void G_Aasdf(); // TODO
static void PickRandomArena() {
	if( !gladiator_state.randomize_arena )
		return;

	TempAllocator temp = svs.frame_arena.temp();

	const char * maps_dir = temp( "{}/base/maps/gladiator", RootDirPath() );
	DynamicArray< const char * > maps( &temp );

	ListDirHandle scan = BeginListDir( sys_allocator, maps_dir );

	const char * name;
	bool dir;
	while( ListDirNext( &scan, &name, &dir ) ) {
		// skip ., .., .git, etc
		if( name[ 0 ] == '.' || dir )
			continue;

		if( FileExtension( name ) != ".bsp" && FileExtension( StripExtension( name ) ) != ".bsp" )
			continue;

		maps.add( temp( "gladiator/{}", StripExtension( StripExtension( name ) ) ) );
	}

	G_LoadMap( RandomElement( &svs.rng, maps.begin(), maps.size() ) );
	G_Aasdf();
}

static void NewRound() {
	server_gs.gameState.round_num++;
	server_gs.gameState.exploding = false;

	PickRandomArena();

	gladiator_state.round_state_end = level.time + countdown_seconds * 1000;
	gladiator_state.countdown = 4;
	gladiator_state.bomb_exploded = false;

	// set up teams
	GhostEveryone();

	u8 players_per_team = U8_MAX;
	u8 top_score = 0;
	for( int i = 0; i < level.gametype.numTeams; i++ ) {
		const SyncTeamState * team = &server_gs.gameState.teams[ Team_One + i ];
		top_score = Max2( top_score, team->score );

		if( team->num_players > 0 ) {
			players_per_team = Min2( players_per_team, team->num_players );
		}
	}

	for( int i = 0; i < level.gametype.numTeams; i++ ) {
		for( u8 j = 0; j < players_per_team; j++ ) {
			int player = Rotate( &gladiator_state.teams[ Team_One + i ] );
			if( player == -1 )
				continue;

			edict_t * ent = PLAYERENT( player );

			G_ClientRespawn( ent, false );
			GiveInventory( ent );
			ent->r.client->ps.can_change_loadout = false;
			ent->r.client->ps.pmove.no_shooting_time = countdown_seconds * 1000;

			if( server_gs.gameState.teams[ Team_One + i ].score == top_score ) {
				ent->s.model2 = "models/crown/crown";
				ent->s.effects |= EF_HAT;
			}
			else {
				ent->s.model2 = EMPTY_HASH;
				ent->s.effects &= ~EF_HAT;
			}
		}
	}

	// check for match point
	server_gs.gameState.round_type = RoundType_Normal;

	if( g_scorelimit->integer > 0 ) {
		for( int i = 0; i < level.gametype.numTeams; i++ ) {
			if( server_gs.gameState.teams[ Team_One + i ].score == g_scorelimit->integer - 1 ) {
				server_gs.gameState.round_type = RoundType_MatchPoint;
				break;
			}
		}
	}

	G_SpawnEvent( EV_FLASH_WINDOW, 0, NULL );
}

static void NewRoundState( RoundState newState ) {
	if( newState > RoundState_Post ) {
		newState = RoundState_Countdown;
	}

	server_gs.gameState.round_state = newState;
	gladiator_state.round_state_start = level.time;

	switch( newState ) {
		case RoundState_None: {
			gladiator_state.round_state_end = 0;
			gladiator_state.countdown = 0;
		} break;

		case RoundState_Countdown: {
			NewRound();
		} break;

		case RoundState_Round: {
			gladiator_state.countdown = 0;
			gladiator_state.round_state_start = level.time;
			gladiator_state.round_state_end = 0;
			G_AnnouncerSound( NULL, "sounds/gladiator/fight", Team_Count, false, NULL );
			G_ClearCenterPrint( NULL );
		} break;

		case RoundState_Finished: {
			gladiator_state.round_state_end = level.time + 1000;
			gladiator_state.countdown = 0;
		} break;

		case RoundState_Post: {
			gladiator_state.round_state_end = level.time + (gladiator_state.bomb_exploded ? 1500 : 1000); //it's too short when the bomb explodes

			Team winner = Team_None;
			for( int i = 0; i < server_gs.maxclients; i++ ) {
				const edict_t * ent = PLAYERENT( i );
				if( !G_ISGHOSTING( ent ) && ent->health > 0 ) {
					winner = ent->s.team;
					break;
				}
			}

			if( winner == Team_None && !gladiator_state.bomb_exploded ) {
				G_AnnouncerSound( NULL, "sounds/gladiator/wowyourterrible", Team_Count, false, NULL );
			}
			else {
				G_AnnouncerSound( NULL, "sounds/gladiator/score", winner, true, NULL );
				server_gs.gameState.teams[ winner ].score++;
			}

			if( G_Match_ScorelimitHit() ) {
				G_Match_LaunchState( MatchState_PostMatch );
			}
		} break;
	}
}

static void NewGame() {
	level.gametype.autoRespawn = false;
	server_gs.gameState.round_num = 0;
	NewRoundState( RoundState_Countdown );
}

static void EndGame() {
	NewRoundState( RoundState_None );
	GhostEveryone();
	G_AnnouncerSound( NULL, "sounds/announcer/game_over", Team_Count, true, NULL );
}

static void Gladiator_Think() {
	if( server_gs.gameState.match_state >= MatchState_PostMatch ) {
		return;
	}

	if( server_gs.gameState.round_state == RoundState_None ) {
		return;
	}

	if( server_gs.gameState.match_state != MatchState_Playing ) {
		EndGame();
		return;
	}

	if( gladiator_state.round_state_end != 0 ) {
		if( gladiator_state.round_state_end < level.time ) {
			NewRoundState( RoundState( server_gs.gameState.round_state + 1 ) );
			return;
		}

		if( gladiator_state.countdown > 0 ) {
			int remainingCounts = int( ( gladiator_state.round_state_end - level.time ) * 0.002f ) + 1;
			remainingCounts = Max2( 0, remainingCounts );

			if( remainingCounts < gladiator_state.countdown ) {
				gladiator_state.countdown = remainingCounts;

				TempAllocator temp = svs.frame_arena.temp();
				if( gladiator_state.countdown <= 3 ) {
					StringHash sound = StringHash( temp( "sounds/gladiator/countdown_{}", gladiator_state.countdown ) );
					G_AnnouncerSound( NULL, sound, Team_Count, false, NULL );
				}
				G_CenterPrintMsg( NULL, "%d", gladiator_state.countdown );
			}
		}
	}

	if( server_gs.gameState.round_state == RoundState_Round ) {
		s64 round_end = gladiator_state.round_state_start + int( g_glad_bombtimer->number * 1000.0f );
		s64 round_time = round_end - level.time;

		if( round_end >= level.time ) {
			server_gs.gameState.clock_override = round_time;
		} else {
			server_gs.gameState.clock_override = -1;
			
			if( !server_gs.gameState.exploding ) {
				BombExplode();
			} else if( server_gs.gameState.exploding && !gladiator_state.bomb_exploded ) {
				BombKill();
			}
		}
		// drop disconnected players
		int disconnected[ MAX_CLIENTS ] = { };
		size_t num_disconnected = 0;

		for( const TeamQueue & team : gladiator_state.teams ) {
			for( int player : team.players ) {
				if( !PLAYERENT( player )->r.inuse ) {
					disconnected[ num_disconnected ] = player;
					num_disconnected++;
				}
			}
		}

		for( size_t i = 0; i < num_disconnected; i++ ) {
			RemovePlayerFromAllQueues( disconnected[ i ] );
		}

		// check if anyone won
		int teams_with_players = 0;
		for( int i = 0; i < level.gametype.numTeams; i++ ) {
			if( PlayersAliveOnTeam( Team( Team_One + i ) ) > 0 ) {
				teams_with_players++;
			}
		}

		if( teams_with_players <= 1 ) {
			NewRoundState( RoundState_Finished );
		}
	} else {
		server_gs.gameState.clock_override = -1;
	}
}

static const edict_t * Gladiator_SelectSpawnPoint( const edict_t * ent ) {
	const edict_t * spawn = NULL;
	edict_t * cursor = NULL;
	float max_dist = 0.0f;

	while( ( cursor = G_Find( cursor, &edict_t::classname, "spawn_gladiator" ) ) != NULL ) {
		float min_dist = -1.0f;
		for( edict_t * player = game.edicts + 1; PLAYERNUM( player ) < server_gs.maxclients; player++ ) {
			if( player == ent || G_IsDead( player ) || player->s.team <= Team_None || PF_GetClientState( PLAYERNUM( player ) ) < CS_SPAWNED ) {
				continue;
			}

			float dist = Length( player->s.origin - cursor->s.origin );
			if( min_dist == -1.0f || dist < min_dist ) {
				min_dist = dist;
			}
		}

		if( min_dist == -1.0f ) { //If no player is spawned, pick a random spawn
			spawn = G_PickRandomEnt( &edict_t::classname, "spawn_gladiator" );
			break;
		}

		if( spawn == NULL || max_dist < min_dist ) {
			max_dist = min_dist;
			spawn = cursor;
		}
	}

	if( spawn == NULL ) {
		Com_GGPrint( "null spawn btw" );
	}

	return spawn;
}

static void Gladiator_PlayerConnected( edict_t * ent ) {
	SetLoadout( ent, Info_ValueForKey( ent->r.client->userinfo, "cg_loadout" ), true );
}

static void Gladiator_PlayerRespawned( edict_t * ent, Team old_team, Team new_team ) {
	if( old_team != new_team ) {
		RemovePlayerFromAllQueues( PLAYERNUM( ent ) );

		if( new_team != Team_None ) {
			Enqueue( &gladiator_state.teams[ new_team ], PLAYERNUM( ent ) );
		}
	}

	if( G_ISGHOSTING( ent ) ) {
		return;
	}


	if( server_gs.gameState.match_state != MatchState_Playing ) {
		ent->r.client->ps.can_change_loadout = new_team >= Team_One;
		GiveInventory( ent );
		G_RespawnEffect( ent );
	}
}

static void Gladiator_PlayerKilled( edict_t * victim, edict_t * attacker, edict_t * inflictor ) {
	if( server_gs.gameState.round_state > RoundState_None && server_gs.gameState.round_state < RoundState_Post ) {
		if( victim->velocity.z < -1600.0f && victim->health < victim->max_health ) {
			G_GlobalSound( "sounds/gladiator/smackdown" );
		}
	}

	if( server_gs.gameState.round_state == RoundState_Countdown ) {
		G_LocalSound( victim, "sounds/gladiator/ouch" );
	}
}

static void Gladiator_MatchStateStarted() {
	switch( server_gs.gameState.match_state ) {
		case MatchState_Warmup:
			break;
		case MatchState_Countdown:
			G_AnnouncerSound( NULL, "sounds/gladiator/let_the_games_begin", Team_Count, false, NULL );
			break;
		case MatchState_Playing:
			NewGame();
			break;
		case MatchState_PostMatch:
			EndGame();
			break;
		case MatchState_WaitExit:
			break;
	}
}

static void Gladiator_Init() {
	server_gs.gameState.gametype = Gametype_Gladiator;

	gladiator_state = { };
	gladiator_state.randomize_arena = G_GetWorldspawnKey( "randomize_arena" ) != "";

	for( TeamQueue & team : gladiator_state.teams ) {
		for( int & slot : team.players ) {
			slot = -1;
		}
	}

	G_AddCommand( ClientCommand_LoadoutMenu, ShowShop );

	G_AddCommand( ClientCommand_SetLoadout, []( edict_t * ent, msg_t args ) {
		SetLoadout( ent, MSG_ReadString( &args ), false );
	} );

	g_glad_bombtimer = NewCvar( "g_glad_bombtimer", "40", CvarFlag_Archive );

	PickRandomArena();
}

static void Gladiator_Shutdown() {
	if( gladiator_state.randomize_arena ) {
		G_LoadMap( "gladiator" );
		G_Aasdf();
	}
}

static bool Gladiator_SpawnEntity( StringHash classname, edict_t * ent ) {
	if( classname == "spawn_gladiator" ) {
		DropSpawnToFloor( ent );
		return true;
	}

	return false;
}

GametypeDef GetGladiatorGametype() {
	GametypeDef gt = { };

	gt.Init = Gladiator_Init;
	gt.MatchStateStarted = Gladiator_MatchStateStarted;
	gt.Think = Gladiator_Think;
	gt.PlayerConnected = Gladiator_PlayerConnected;
	gt.PlayerRespawned = Gladiator_PlayerRespawned;
	gt.PlayerKilled = Gladiator_PlayerKilled;
	gt.SelectSpawnPoint = Gladiator_SelectSpawnPoint;
	gt.Shutdown = Gladiator_Shutdown;
	gt.SpawnEntity = Gladiator_SpawnEntity;

	gt.numTeams = 4;
	gt.removeInactivePlayers = true;
	gt.selfDamage = true;
	gt.autoRespawn = true;

	return gt;
}

#include "game/g_local.h"
#include "gameshared/gs_public.h"

static const char * bot_names[] = {
	"vic",
	"crizis",
	"jal",

	"MWAGA",

	"Triangel",

	"Perrina",

	"__mute__",
	"Slice*>",

	// twitch subs
	"ne0ns0up",
	"hvaholic",
	"catman1900",
};

static void CreateUserInfo( char * buffer, size_t bufferSize ) {
	memset( buffer, 0, bufferSize );

	Info_SetValueForKey( buffer, "name", random_select( &svs.rng, bot_names ) );
	Info_SetValueForKey( buffer, "hand", va( "%i", random_uniform( &svs.rng, 0, 3 ) ) );
}

static edict_t * ConnectFakeClient() {
	char userInfo[MAX_INFO_STRING];
	static char fakeSocketType[] = "loopback";
	static char fakeIP[] = "127.0.0.1";
	CreateUserInfo( userInfo, sizeof( userInfo ) );
	int entNum = SVC_FakeConnect( userInfo, fakeSocketType, fakeIP );
	if( entNum < 1 ) {
		Com_Printf( "AI: Can't spawn the fake client\n" );
		return NULL;
	}
	return game.edicts + entNum;
}

void AI_SpawnBot() {
	if( level.spawnedTimeStamp + 5000 > svs.realtime || !level.canSpawnEntities ) {
		return;
	}

	edict_t * ent = ConnectFakeClient();
	if( ent == NULL )
		return;

	ent->think = NULL;
	ent->nextThink = level.time + 500 + random_uniform( &svs.rng, 0, 2000 );
	ent->classname = "bot";
	ent->die = player_die;

	AI_Respawn( ent );

	game.numBots++;
}

void AI_Respawn( edict_t * ent ) {
	ent->r.client->ps.pmove.delta_angles[ 0 ] = 0;
	ent->r.client->ps.pmove.delta_angles[ 1 ] = 0;
	ent->r.client->ps.pmove.delta_angles[ 2 ] = 0;
	ent->r.client->level.last_activity = level.time;
}

static void AI_SpecThink( edict_t * self ) {
	self->nextThink = level.time + 100;

	if( !level.canSpawnEntities )
		return;

	if( self->r.client->team == TEAM_SPECTATOR ) {
		// try to join a team
		if( !self->r.client->queueTimeStamp ) {
			G_Teams_JoinAnyTeam( self, false );
		}

		if( self->r.client->team == TEAM_SPECTATOR ) { // couldn't join, delay the next think
			self->nextThink = level.time + 100;
		} else {
			self->nextThink = level.time + 1;
		}
		return;
	}

	usercmd_t ucmd;
	memset( &ucmd, 0, sizeof( usercmd_t ) );

	// set approximate ping and show values
	ucmd.serverTimeStamp = svs.gametime;
	ucmd.msec = (uint8_t)game.frametime;

	ClientThink( self, &ucmd, 0 );
}

static void AI_GameThink( edict_t * self ) {
	if( GS_MatchState( &server_gs ) <= MATCH_STATE_WARMUP ) {
		G_Match_Ready( self );
	}

	usercmd_t ucmd;
	memset( &ucmd, 0, sizeof( usercmd_t ) );

	// set up for pmove
	ucmd.angles[ 0 ] = (short)ANGLE2SHORT( self->s.angles.x ) - self->r.client->ps.pmove.delta_angles[ 0 ];
	ucmd.angles[ 1 ] = (short)ANGLE2SHORT( self->s.angles.y ) - self->r.client->ps.pmove.delta_angles[ 1 ];
	ucmd.angles[ 2 ] = (short)ANGLE2SHORT( self->s.angles.z ) - self->r.client->ps.pmove.delta_angles[ 2 ];

	self->r.client->ps.pmove.delta_angles[ 0 ] = 0;
	self->r.client->ps.pmove.delta_angles[ 1 ] = 0;
	self->r.client->ps.pmove.delta_angles[ 2 ] = 0;

	// set approximate ping and show values
	ucmd.msec = (uint8_t)game.frametime;
	ucmd.serverTimeStamp = svs.gametime;

	ClientThink( self, &ucmd, 0 );
	self->nextThink = level.time + 1;
}

void AI_Think( edict_t * self ) {
	if( G_ISGHOSTING( self ) ) {
		AI_SpecThink( self );
	}
	else {
		AI_GameThink( self );
	}
}

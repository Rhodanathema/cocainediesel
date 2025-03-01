#pragma once

#include "qcommon/types.h"

enum SolidBits : u16 {
	Solid_NotSolid = 0,

	// useful to stop the bomb etc falling through the floor without also blocking movement/shots
	Solid_World = ( 1 << 0 ),

	Solid_PlayerClip = ( 1 << 1 ),
	Solid_WeaponClip = ( 1 << 2 ),
	Solid_Wallbangable = ( 1 << 3 ),
	Solid_Ladder = ( 1 << 4 ),
	Solid_Trigger = ( 1 << 5 ),

	Solid_PlayerTeamOne = ( 1 << 6 ),
	Solid_PlayerTeamTwo = ( 1 << 7 ),
	Solid_PlayerTeamThree = ( 1 << 8 ),
	Solid_PlayerTeamFour = ( 1 << 9 ),

	Solid_MaskGenerator
};

constexpr SolidBits SolidMask_Player = SolidBits( Solid_PlayerTeamOne | Solid_PlayerTeamTwo | Solid_PlayerTeamThree | Solid_PlayerTeamFour );
constexpr SolidBits SolidMask_AnySolid = SolidBits( Solid_World | Solid_PlayerClip | Solid_WeaponClip | Solid_Wallbangable | SolidMask_Player );
constexpr SolidBits SolidMask_Opaque = SolidBits( Solid_World | Solid_WeaponClip | Solid_Wallbangable );
constexpr SolidBits SolidMask_WallbangShot = SolidBits( Solid_WeaponClip | SolidMask_Player );
constexpr SolidBits SolidMask_Shot = SolidBits( SolidMask_WallbangShot | Solid_Wallbangable );

constexpr SolidBits SolidMask_Everything = SolidBits( ( Solid_MaskGenerator - 1 ) * 2 - 1 );

struct trace_t {
	float fraction;
	Vec3 endpos;
	Vec3 contact;
	Vec3 normal;
	SolidBits solidity;
	int ent;

	bool HitSomething() const { return ent > -1; }
	bool HitNothing() const { return ent == -1; }
	bool GotSomewhere() const { return fraction > 0.0f; }
	bool GotNowhere() const { return fraction == 0.0f; }
};

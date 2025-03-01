#include "gameshared/movement.h"

static constexpr float dashupspeed = 160.0f;
static constexpr float dashspeed = 550.0f;

static constexpr float wjupspeed = 371.875f;
static constexpr float wjbouncefactor = 0.4f;



static constexpr float wallclimbspeed = 450.0f;

static constexpr float jumpupspeed = 280.0f;

static constexpr float climbfriction = 5.0f;

static constexpr float stamina_use = 0.15f;
static constexpr float stamina_use_moving = 0.3f;
static constexpr float stamina_usewj = 0.5f;
static constexpr float stamina_recover = 1.0f;

static bool CanClimb( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps ) {
	if( !StaminaAvailable( ps, pml, stamina_use ) ) {
		return false;
	}

	Vec3 spots[ 2 ];
	spots[ 0 ] = pml->origin + pml->forward - pml->right * 2;
	spots[ 1 ] = pml->origin + pml->forward + pml->right * 2;

	for( Vec3 spot : spots ) {
		trace_t trace = pmove_gs->api.Trace( pml->origin, pm->bounds, spot, pm->playerState->POVnum, pm->solid_mask, 0 );
		if( trace.HitSomething() && !ISWALKABLEPLANE( trace.normal ) && trace.GotSomewhere() ) {
			return true;
		}
	}

	return false;
}

static void PM_MidgetJump( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {

	ps->pmove.stamina_state = Stamina_Normal;

	if( !pressed ) {
		return;
	}

	if( pm->groundentity == -1 ) {
		if ( ps->pmove.pm_flags & PMF_CLIMBING ) {
			Walljump( pm, pml, pmove_gs, ps, jumpupspeed, dashupspeed, dashspeed, wjupspeed, wjbouncefactor, stamina_usewj, stamina_recover );
			ps->pmove.pm_flags &= ~PMF_ABILITY2_HELD;
		}
		return;
	}

	Jump( pm, pml, pmove_gs, ps, jumpupspeed, true );
}

static void PM_MidgetSpecial( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {
	if( ps->pmove.stamina_state == Stamina_Normal && !( ps->pmove.pm_flags & PMF_ABILITY2_HELD ) ) {
		StaminaRecover( ps, pml, stamina_recover );
	}

	if( ps->pmove.knockback_time > 0 ) { // can not start a new dash during knockback time
		return;
	}

	if( pml->ladder ) {
		return;
	}

	if( pressed && CanClimb( pm, pml, pmove_gs, ps ) ) {
		pml->ladder = Ladder_Fake;
		pml->groundFriction = climbfriction;

		Vec3 wishvel = pml->forward * pml->forwardPush + pml->right * pml->sidePush;
		wishvel.z = 0.0;

		ps->pmove.stamina_state = Stamina_UsingAbility;
		ps->pmove.pm_flags |= PMF_ABILITY2_HELD;
		ps->pmove.pm_flags |= PMF_CLIMBING;

		if( wishvel == Vec3( 0.0f ) ) {
			StaminaUse( ps, pml, stamina_use );
			return;
		}

		wishvel = Normalize( wishvel );

		if( pml->forwardPush > 0 ) {
			wishvel.z = Lerp( -1.0f, Unlerp01( 15.0f, ps->viewangles[ PITCH ], -15.0f ), 1.0f );
		}

		StaminaUse( ps, pml, Length( wishvel ) * stamina_use_moving );
		pml->velocity = wishvel * wallclimbspeed;

		pm->groundentity = -1;
	} else {
		ps->pmove.pm_flags &= ~PMF_ABILITY2_HELD;
	}
}

void PM_MidgetInit( pmove_t * pm, pml_t * pml ) {
	PM_InitPerk( pm, pml, Perk_Midget, PM_MidgetJump, PM_MidgetSpecial );
}

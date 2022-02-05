#include "gameshared/movement.h"
#include "gameshared/gs_weapons.h"

static constexpr float jump_upspeed = 270.0f;
static constexpr float jump_detection = 0.06f;


static constexpr float charge_groundAccel = 2.0f;
static constexpr float charge_friction = 0.75f;
static constexpr float charge_speed = 900.0f;
static constexpr float charge_sidespeed = 400.0f;

static constexpr float stamina_limit = 0.5f;
static constexpr float stamina_use = 0.5f;
static constexpr float stamina_recover = 0.25f;


static void PM_BoomerJump( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {
	if( ps->pmove.pm_flags & PMF_ABILITY2_HELD ) {
		return;
	}

	if( pressed ) {
		if( !( ps->pmove.pm_flags & PMF_ABILITY1_HELD ) ) {
			ps->pmove.stamina_stored = jump_detection;
		}

		ps->pmove.pm_flags |= PMF_ABILITY1_HELD;
		ps->pmove.stamina_stored = Max2( 0.0f, ps->pmove.stamina_stored - pml->frametime );

		if( ( ps->pmove.pm_flags & PMF_ABILITY1_HELD ) && ps->pmove.stamina_stored == 0.0f ) {
			return;
		}

		if( pm->groundentity == -1 ) {
			return;
		}

		ps->pmove.stamina_stored = 0.0f;
		Jump( pm, pml, pmove_gs, ps, jump_upspeed, JumpType_Normal, true );
	} else {
		ps->pmove.pm_flags &= ~PMF_ABILITY1_HELD;
	}
}



static void PM_BoomerSpecial( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {
	if( !pressed ) {
		ps->pmove.pm_flags &= ~PMF_ABILITY2_HELD;
	}

	if( ( ps->pmove.pm_flags & PMF_ABILITY2_HELD ) && !pml->ladder ) {
		if( StaminaAvailable( ps, pml, stamina_use ) ) {
			StaminaUse( ps, pml, stamina_use );
			pml->maxSpeed = charge_speed;
			
			pml->groundAccel = charge_groundAccel;
			pml->friction = charge_friction;

			pml->forwardPush = charge_speed;
			pml->sidePush = pm->cmd.sidemove * charge_sidespeed;
		}
	} else if( pressed && ps->pmove.stamina >= stamina_limit ) {
		ps->pmove.pm_flags |= PMF_ABILITY2_HELD;
	} else {
		StaminaRecover( ps, pml, stamina_recover );
	}
}


void PM_BoomerInit( pmove_t * pm, pml_t * pml ) {
	PM_InitPerk( pm, pml, Perk_Boomer, PM_BoomerJump, PM_BoomerSpecial );
}

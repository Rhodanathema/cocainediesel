#include "gameshared/movement.h"
#include "qcommon/qfiles.h"

static constexpr float dash_jump_speed = 750.0f;

static constexpr float charge_jump_speed = 600.0f;
static constexpr float charge_min_speed = 200.0f;
static constexpr float charge_jump_range = charge_jump_speed - charge_min_speed;

static constexpr float charge_slide_time = 1.0f;
static constexpr float charge_buffering = 0.1f;

static constexpr float min_bounce_speed = 250.0f;
static constexpr float bounce_factor = 0.5f;


static constexpr float stamina_use = 5.0f;
static constexpr float stamina_recover = 10.0f;

static constexpr float floor_distance = STEPSIZE;

static void PM_WheelCharge( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {
	if( pm->groundentity != -1 || pml->ladder ) {
		ps->pmove.stamina_state = Stamina_Normal;
		ps->pmove.stamina_stored = charge_buffering;
	}

	if( pressed && ps->pmove.stamina_state != Stamina_UsedAbility && pm->groundentity == -1 && ps->pmove.stamina_stored == 0.0f ) {
		Vec3 fwd;
		AngleVectors( pm->playerState->viewangles, &fwd, NULL, NULL );
		Vec3 dashdir = fwd;

		dashdir = Normalize( dashdir );
		dashdir *= dash_jump_speed;

		pml->velocity = dashdir;

		ps->pmove.stamina_state = Stamina_UsedAbility;
		pmove_gs->api.PredictedEvent( ps->POVnum, EV_JUMP, JumpType_WheelCharge );
	}
}


//in this one we don't care about pressing special
static void PM_WheelJump( pmove_t * pm, pml_t * pml, const gs_state_t * pmove_gs, SyncPlayerState * ps, bool pressed ) {
	if( pressed ) {
		StaminaRecover( ps, pml, stamina_use );
		ps->pmove.pm_flags |= PMF_ABILITY1_HELD;

		pmove_gs->api.PredictedEvent( ps->POVnum, EV_CHARGEJUMP, 0 );
	} else {
		float jumpspeed = charge_min_speed + charge_jump_range * ps->pmove.stamina;
		if( ps->pmove.stamina_stored > 0.0 && ( ps->pmove.pm_flags & PMF_ABILITY1_HELD ) ) {
			Jump( pm, pml, pmove_gs, ps, jumpspeed, true );
			ps->pmove.stamina_stored = 0.0;
		}

		ps->pmove.pm_flags &= ~PMF_ABILITY1_HELD;
		StaminaUse( ps, pml, stamina_recover );
	}
	
	ps->pmove.stamina_stored = Max2( 0.0f, ps->pmove.stamina_stored - pml->frametime );

	//bounce
	trace_t trace;
	Vec3 point = pml->origin;
	point.z -= floor_distance;

	pmove_gs->api.Trace( &trace, pml->origin, pm->mins, pm->maxs, point, ps->POVnum, pm->contentmask, 0 );

	if( pm->groundentity == -1 && ps->pmove.stamina_state == Stamina_UsedAbility &&
		!ISWALKABLEPLANE( &trace.plane ) && !pml->ladder &&
		(trace.fraction == 1 || !trace.startsolid) )
	{
		Vec3 normal( 0.0f );
		PlayerTouchWall( pm, pml, pmove_gs, 12, 0.3f, &normal, true, SURF_NOWALLJUMP | SURF_LADDER );
		if( !Length( normal ) )
			return;

		//don't want to bounce everywhere while falling imo
		Vec3 velocity2 = pml->velocity;
		if( velocity2.z < 0 ) {
			velocity2.z = 0;
		}

		float speed = Length( velocity2 );
		pml->velocity = GS_ClipVelocity( pml->velocity, normal, 1.0005f );
		if( speed > min_bounce_speed ) {
			pml->velocity = pml->velocity + normal * speed * bounce_factor;
		}
	}
}


void PM_WheelInit( pmove_t * pm, pml_t * pml ) {
	PM_InitPerk( pm, pml, Perk_Wheel, PM_WheelJump, PM_WheelCharge );
}

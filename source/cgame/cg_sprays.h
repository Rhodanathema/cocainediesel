#pragma once

#include "qcommon/types.h"
#include "qcommon/hash.h"

void InitSprays();
void AddSpray( Vec3 origin, Vec3 normal, Vec3 angles, StringHash material );
void DrawSprays();

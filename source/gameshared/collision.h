#pragma once

#include "qcommon/types.h"
#include "qcommon/hash.h"
#include "qcommon/hashtable.h"
#include "gameshared/cdmap.h"
#include "gameshared/intersection_tests.h"
#include "gameshared/gs_synctypes.h"
#include "gameshared/q_collision.h"

struct GLTFCollisionBrush {
	u32 first_plane;
	u32 num_planes;
	u32 first_vertex;
	u32 num_vertices;
};

struct GLTFCollisionData {
	MinMax3 bounds;
	Span< Vec3 > vertices;
	Span< Plane > planes;
	Span< GLTFCollisionBrush > brushes;
};

struct MapSubModelCollisionData {
	StringHash base_hash;
	u32 sub_model;
};

struct MapSharedCollisionData {
	StringHash base_hash;
	MapData data;
};

struct CollisionModelStorage {
	static constexpr size_t MAX_GLTF_COLLISION_MODELS = 4096;

	GLTFCollisionData gltfs[ MAX_GLTF_COLLISION_MODELS ];
	Hashtable< MAX_GLTF_COLLISION_MODELS * 2 > gltfs_hashtable;

	static constexpr size_t MAX_MAPS = 128;
	static constexpr size_t MAX_MAP_MODELS = 4096;

	MapSharedCollisionData maps[ MAX_MAPS ];
	Hashtable< MAX_MAPS * 2 > maps_hashtable;

	MapSubModelCollisionData map_models[ MAX_MAP_MODELS ];
	Hashtable< MAX_MAP_MODELS * 2 > map_models_hashtable;
};

CollisionModel CollisionModelAABB( const MinMax3 & aabb );

void InitCollisionModelStorage( CollisionModelStorage * storage );
void ShutdownCollisionModelStorage( CollisionModelStorage * storage );

struct cgltf_data;
bool LoadGLTFCollisionData( CollisionModelStorage * storage, const cgltf_data * gltf, const char * path, StringHash name );

void LoadMapCollisionData( CollisionModelStorage * storage, const MapData * map, StringHash base_hash );

const MapSharedCollisionData * FindMapSharedCollisionData( const CollisionModelStorage * storage, StringHash name );
const MapSubModelCollisionData * FindMapSubModelCollisionData( const CollisionModelStorage * storage, StringHash name );

CollisionModel EntityCollisionModel( const SyncEntityState * ent );
MinMax3 EntityBounds( const CollisionModelStorage * storage, const SyncEntityState * ent );

trace_t TraceVsEnt( const CollisionModelStorage * storage, const Ray & ray, const Shape & shape, const SyncEntityState * ent );

#include "qcommon/base.h"
#include "qcommon/array.h"
#include "qcommon/hash.h"
#include "qcommon/string.h"
#include "gameshared/cdmap.h"
#include "gameshared/collision.h"
#include "gameshared/editor_materials.h"
#include "gameshared/intersection_tests.h"
#include "gameshared/gs_synctypes.h"
#include "gameshared/q_collision.h"
#include "gameshared/q_math.h"

#include "cgltf/cgltf.h"

CollisionModel CollisionModelAABB( const MinMax3 & aabb ) {
	CollisionModel model = { };
	model.type = CollisionModelType_AABB;
	model.aabb = aabb;
	return model;
}

static void DeleteGLTFCollisionData( GLTFCollisionData data ) {
	FREE( sys_allocator, data.vertices.ptr );
	FREE( sys_allocator, data.planes.ptr );
	FREE( sys_allocator, data.brushes.ptr );
}

void InitCollisionModelStorage( CollisionModelStorage * storage ) {
	storage->gltfs_hashtable.clear();
	storage->maps_hashtable.clear();
	storage->map_models_hashtable.clear();
}

void ShutdownCollisionModelStorage( CollisionModelStorage * storage ) {
	for( size_t i = 0; i < storage->gltfs_hashtable.size(); i++ ) {
		DeleteGLTFCollisionData( storage->gltfs[ i ] );
	}
}

static bool IsConvex( GLTFCollisionData data, GLTFCollisionBrush brush ) {
	for( u32 i = 0; i < brush.num_planes; i++ ) {
		Plane plane = data.planes[ brush.first_plane + i ];
		for( u32 j = 0; j < brush.num_vertices; j++ ) {
			Vec3 v = data.vertices[ brush.first_vertex + j ];
			if( Dot( plane.normal, v ) - plane.distance > 0.001f ) {
				return false;
			}
		}
	}

	return true;
}

template< typename T >
Span< T > DedupeSorted( Allocator * a, Span< T > sorted ) {
	NonRAIIDynamicArray< T > deduped( a );

	for( size_t i = 1; i < sorted.n; i++ ) {
		if( sorted[ i ] != sorted[ i - 1 ] ) {
			deduped.add( sorted );
		}
	}

	return deduped.span();
}

static Span< const u8 > AccessorToSpan( const cgltf_accessor * accessor ) {
	cgltf_size offset = accessor->offset + accessor->buffer_view->offset;
	return Span< const u8 >( ( const u8 * ) accessor->buffer_view->buffer->data + offset, accessor->count * accessor->stride );
}

static bool PlaneFrom3Points( Plane * plane, Vec3 a, Vec3 b, Vec3 c ) {
	Vec3 ab = b - a;
	Vec3 ac = c - a;

	Vec3 normal = SafeNormalize( Cross( ac, ab ) );
	if( normal == Vec3( 0.0f ) )
		return false;

	plane->normal = normal;
	plane->distance = Dot( a, normal );

	return true;
}

bool LoadGLTFCollisionData( CollisionModelStorage * storage, const cgltf_data * gltf, const char * path, StringHash name ) {
	// Com_Printf( "load %s\n", path );

	NonRAIIDynamicArray< Vec3 > vertices( sys_allocator );
	NonRAIIDynamicArray< Plane > planes( sys_allocator );
	NonRAIIDynamicArray< GLTFCollisionBrush > brushes( sys_allocator );
	GLTFCollisionData data;

	for( size_t i = 0; i < gltf->nodes_count; i++ ) {
		const cgltf_node * node = &gltf->nodes[ i ];
		if( node->mesh == NULL )
			continue;

		const cgltf_primitive & prim = node->mesh->primitives[ 0 ];
		if( prim.material == NULL )
			continue;

		const EditorMaterial * material = FindEditorMaterial( StringHash( prim.material->name ) );
		if( material == NULL )
			continue;

		Mat4 transform;
		cgltf_node_transform_world( node, transform.ptr() );

		GLTFCollisionBrush brush = { };
		brush.first_plane = planes.size();
		brush.first_vertex = vertices.size();
		brush.solidity = material->solidity;

		Span< const Vec3 > gltf_verts;
		for( size_t j = 0; j < prim.attributes_count; j++ ) {
			const cgltf_attribute & attr = prim.attributes[ j ];
			if( attr.type == cgltf_attribute_type_position ) {
				gltf_verts = AccessorToSpan( attr.data ).cast< const Vec3 >();

				Vec3 min, max;
				for( int k = 0; k < 3; k++ ) {
					min[ k ] = attr.data->min[ k ];
					max[ k ] = attr.data->max[ k ];
				}
				data.bounds = Union( data.bounds, ( transform * Vec4( min, 1.0f ) ).xyz() );
				data.bounds = Union( data.bounds, ( transform * Vec4( max, 1.0f ) ).xyz() );
			}
		}

		for( size_t j = 0; j < gltf_verts.n; j++ ) {
			bool found = false;
			for( Vec3 & vert : vertices ) {
				if( Length( gltf_verts[ j ] - vert ) < 0.01f ) {
					found = true;
					break;
				}
			}
			if( found ) {
				continue;
			}
			vertices.add( gltf_verts[ j ] );
		}

		Span< const u8 > indices_data = AccessorToSpan( prim.indices );
		Assert( prim.indices->count % 3 == 0 );
		for( size_t j = 0; j < prim.indices->count; j += 3 ) {
			u32 a, b, c;
			if( prim.indices->component_type == cgltf_component_type_r_16u ) {
				a = indices_data.cast< const u16 >()[ j + 0 ];
				b = indices_data.cast< const u16 >()[ j + 1 ];
				c = indices_data.cast< const u16 >()[ j + 2 ];
			}
			else {
				a = indices_data.cast< const u32 >()[ j + 0 ];
				b = indices_data.cast< const u32 >()[ j + 1 ];
				c = indices_data.cast< const u32 >()[ j + 2 ];
			}

			Plane plane;
			if( !PlaneFrom3Points( &plane, gltf_verts[ a ], gltf_verts[ c ], gltf_verts[ b ] ) ) {
				return false;
			}

			bool found = false;
			for( Plane & other_plane : planes ) {
				if( Abs( plane.distance - other_plane.distance ) < 0.01f && Dot( plane.normal, other_plane.normal ) >= 0.99999f ) {
					found = true;
					break;
				}
			}
			if( found ) {
				continue;
			}

			planes.add( plane );
		}
		brush.num_planes = planes.size() - brush.first_plane;
		brush.num_vertices = vertices.size() - brush.first_vertex;

		brushes.add( brush );
	}

	// TODO: remove verts that are not on the boundary of the complete hull?

	if( brushes.size() == 0 ) {
		return true;
	}

	data.vertices = vertices.span();
	data.planes = planes.span();
	data.brushes = brushes.span();

	for( GLTFCollisionBrush brush : data.brushes ) {
		if( !IsConvex( data, brush ) ) {
			Fatal( "failed convexity check" );
			DeleteGLTFCollisionData( data );
			return false;
		}
	}

	u64 idx = storage->gltfs_hashtable.size();
	if( !storage->gltfs_hashtable.get( name.hash, &idx ) ) {
		storage->gltfs_hashtable.add( name.hash, storage->gltfs_hashtable.size() );
	}
	else {
		DeleteGLTFCollisionData( storage->gltfs[ idx ] );
	}

	storage->gltfs[ idx ] = data;

	return true;
}

const GLTFCollisionData * FindGLTFSharedCollisionData( const CollisionModelStorage * storage, StringHash name ) {
	u64 idx;
	if( !storage->gltfs_hashtable.get( name.hash, &idx ) )
		return NULL;
	return &storage->gltfs[ idx ];
}

static void FillMapModelsHashtable( CollisionModelStorage * storage ) {
	TracyZoneScoped;

	storage->map_models_hashtable.clear();

	for( u32 i = 0; i < storage->maps_hashtable.size(); i++ ) {
		const MapSharedCollisionData * map = &storage->maps[ i ];
		for( size_t j = 0; j < map->data.models.n; j++ ) {
			String< 16 > suffix( "*{}", j );
			u64 hash = Hash64( suffix.c_str(), suffix.length(), map->base_hash.hash );

			if( storage->map_models_hashtable.size() == ARRAY_COUNT( storage->map_models ) ) {
				Fatal( "Too many map submodels" );
			}

			storage->map_models[ storage->map_models_hashtable.size() ] = {
				StringHash( map->base_hash ),
				checked_cast< u32 >( j )
			};
			storage->map_models_hashtable.add( hash, storage->map_models_hashtable.size() );
		}
	}
}

void LoadMapCollisionData( CollisionModelStorage * storage, const MapData * data, StringHash base_hash ) {
	TracyZoneScoped;

	u64 idx = storage->maps_hashtable.size();
	if( !storage->maps_hashtable.get( base_hash.hash, &idx ) ) {
		storage->maps_hashtable.add( base_hash.hash, storage->maps_hashtable.size() );
	}

	if( idx == ARRAY_COUNT( storage->maps ) ) {
		Fatal( "Too many maps" );
	}

	MapSharedCollisionData map;
	map.base_hash = base_hash;
	map.data = *data;
	storage->maps[ idx ] = map;

	FillMapModelsHashtable( storage );
}

const MapSharedCollisionData * FindMapSharedCollisionData( const CollisionModelStorage * storage, StringHash name ) {
	u64 idx;
	if( !storage->maps_hashtable.get( name.hash, &idx ) )
		return NULL;
	return &storage->maps[ idx ];
}

const MapSubModelCollisionData * FindMapSubModelCollisionData( const CollisionModelStorage * storage, StringHash name ) {
	u64 idx;
	if( !storage->map_models_hashtable.get( name.hash, &idx ) )
		return NULL;
	return &storage->map_models[ idx ];
}

CollisionModel EntityCollisionModel( const SyncEntityState * ent ) {
	if( ent->override_collision_model.exists ) {
		return ent->override_collision_model.value;
	}

	CollisionModel model = { };
	model.type = CollisionModelType_MapModel;
	model.map_model = ent->model;
	return model;
}

MinMax3 EntityBounds( const CollisionModelStorage * storage, const SyncEntityState * ent ) {
	CollisionModel model = EntityCollisionModel( ent );

	if( model.type == CollisionModelType_MapModel ) {
		const MapSubModelCollisionData * map_model = FindMapSubModelCollisionData( storage, model.map_model );
		if( map_model == NULL )
			return MinMax3::Empty();
		const MapSharedCollisionData * map = FindMapSharedCollisionData( storage, map_model->base_hash );
		return map->data.models[ map_model->sub_model ].bounds;
	}

	CollisionModelType type = model.type;
	Assert( type == CollisionModelType_Point || type == CollisionModelType_AABB );

	if( type == CollisionModelType_Point ) {
		return MinMax3( Vec3( 0.0f ), Vec3( 0.0f ) );
	}

	return model.aabb;
}

trace_t MakeMissedTrace( const Ray & ray ) {
	trace_t trace = { };
	trace.fraction = 1.0f;
	trace.endpos = ray.origin + ray.direction * ray.length;
	trace.ent = -1;
	trace.solidity = Solid_NotSolid;
	return trace;
}

static trace_t FUCKING_HELL( const Ray & ray, const Shape & shape, const Intersection & intersection, int ent ) {
	trace_t trace = { };
	trace.fraction = ray.length == 0.0f ? 1.0f : intersection.t / ray.length;
	trace.normal = intersection.normal;
	trace.ent = ent;
	trace.solidity = intersection.solidity;
	trace.contact = ray.origin + ray.direction * intersection.t - trace.normal * Support( shape, -trace.normal );

	// step back endpos slightly so objects don't get stuck inside each other
	constexpr float epsilon = 1.0f / 32.0f;
	float stepped_back_t = intersection.t;
	if( intersection.normal != Vec3( 0.0f ) ) {
		stepped_back_t += epsilon / Dot( ray.direction, intersection.normal );
		stepped_back_t = Max2( stepped_back_t, 0.0f );
	}

	trace.endpos = ray.origin + ray.direction * stepped_back_t;

	return trace;
}

trace_t TraceVsEnt( const CollisionModelStorage * storage, const Ray & ray, const Shape & shape, const SyncEntityState * ent, SolidBits solid_mask ) {
	trace_t trace = MakeMissedTrace( ray );

	CollisionModel collision_model = EntityCollisionModel( ent );

	if( ent->type != ET_PLAYER ) {
		for( int i = 0; i < 3; i++ ) {
			Assert( PositiveMod( ent->angles[ i ], 90.0f ) == 0.0f );
		}
	}

	// TODO: accomodate angles!!!!!!!!!
	Vec3 object_space_origin = ( ray.origin - ent->origin ) / ent->scale;
	Vec3 object_space_translation = ( ray.direction * ray.length ) / ent->scale;
	Ray object_space_ray = MakeRayOriginDirection( object_space_origin, SafeNormalize( object_space_translation ), Length( object_space_translation ) );

	if( collision_model.type == CollisionModelType_MapModel ) {
		const MapSubModelCollisionData * map_model = FindMapSubModelCollisionData( storage, collision_model.map_model );
		if( map_model == NULL )
			return trace;
		const MapSharedCollisionData * map = FindMapSharedCollisionData( storage, map_model->base_hash );

		Intersection intersection;
		if( SweptShapeVsMapModel( &map->data, &map->data.models[ map_model->sub_model ], object_space_ray, shape, solid_mask, &intersection ) ) {
			trace = FUCKING_HELL( ray, shape, intersection, ent->number );
		}
	}
	else if( shape.type == ShapeType_AABB ) {
		Assert( collision_model.type == CollisionModelType_AABB );

		MinMax3 object_space_aabb = ToMinMax( shape.aabb );
		object_space_aabb.mins += object_space_origin;
		object_space_aabb.maxs += object_space_origin;

		Intersection intersection;
		if( SweptAABBVsAABB( object_space_aabb, ray.direction * ray.length, collision_model.aabb, Vec3( 0.0f ), &intersection ) ) {
			intersection.t *= ray.length; // TODO: make this consistent with the rest...
			trace = FUCKING_HELL( ray, shape, intersection, ent->number );
		}
	}
	else {
		Assert( shape.type == ShapeType_Ray );

		switch( collision_model.type ) {
			case CollisionModelType_Point:
				break;

			case CollisionModelType_AABB: {
				Intersection enter, leave;
				if( RayVsAABB( object_space_ray, collision_model.aabb, &enter, &leave ) ) {
					enter.solidity = ent->solidity;
					trace = FUCKING_HELL( ray, shape, enter, ent->number );
				}
			} break;

			case CollisionModelType_Sphere: {
				float t;
				if( RayVsSphere( object_space_ray, collision_model.sphere, &t ) ) {
					trace = FUCKING_HELL( ray, shape, { t }, ent->number );
				}
			} break;

			case CollisionModelType_Capsule: {
				float t;
				if( RayVsCapsule( object_space_ray, collision_model.capsule, &t ) ) {
					trace = FUCKING_HELL( ray, shape, { t }, ent->number );
				}
			} break;
		}
	}

	return trace;
}

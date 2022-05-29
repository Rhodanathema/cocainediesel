#include <algorithm>
#include <math.h>

#include <vector>

#include "parsing.h"
#include "materials.h"

#include "qcommon/base.h"
#include "qcommon/array.h"
#include "qcommon/fs.h"
#include "qcommon/qfiles.h"
#include "qcommon/string.h"
#include "qcommon/hash.h"
#include "gameshared/cdmap.h"
#include "gameshared/q_math.h"
#include "gameshared/q_shared.h"

#include "zstd/zstd.h"

void ShowErrorMessage( const char * msg, const char * file, int line ) {
	printf( "%s (%s:%d)\n", msg, file, line );
}

template< typename T >
Span< const T > VectorToSpan( const std::vector< T > & v ) {
	return Span< const T >( &v[ 0 ], v.size() );
}

struct CompiledMesh {
	u64 material = 0;
	std::vector< Vec3 > vertices;
	std::vector< u32 > indices;
};

struct CompiledKDTree {
	MinMax3 bounds;
	std::vector< MapBrush > brushes;
	std::vector< Plane > planes;

	std::vector< MapKDTreeNode > nodes;
	std::vector< u16 > brush_indices;
};

struct CompiledEntity {
	std::vector< CompiledMesh > render_geometry;
	CompiledKDTree collision_geometry;
	std::vector< ParsedKeyValue > key_values;
};

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

static bool Intersect3PlanesPoint( Vec3 * p, Plane plane1, Plane plane2, Plane plane3 ) {
	constexpr float epsilon = 0.001f;

	Vec3 n2xn3 = Cross( plane2.normal, plane3.normal );
	float n1_n2xn3 = Dot( plane1.normal, n2xn3 );

	if( Abs( n1_n2xn3 ) < epsilon )
		return false;

	Vec3 n3xn1 = Cross( plane3.normal, plane1.normal );
	Vec3 n1xn2 = Cross( plane1.normal, plane2.normal );

	*p = ( plane1.distance * n2xn3 + plane2.distance * n3xn1 + plane3.distance * n1xn2 ) / n1_n2xn3;
	return true;
}

static bool PointInsideBrush( Span< const Plane > planes, Vec3 p ) {
	constexpr float epsilon = 0.001f;
	for( Plane plane : planes ) {
		if( Dot( p, plane.normal ) - plane.distance > epsilon ) {
			return false;
		}
	}

	return true;
}

static Vec2 ProjectFaceVert( Vec3 centroid, Vec3 tangent, Vec3 bitangent, Vec3 p ) {
	Vec3 d = p - centroid;
	return Vec2( Dot( d, tangent ), Dot( d, bitangent ) );
}

static std::vector< Vec3 > TriangulateFace( Span< const Plane > brush, size_t face ) {
	// generate a set of candidate points
	std::vector< Vec3 > points;
	for( size_t i = 0; i < brush.n; i++ ) {
		if( i == face )
			continue;

		for( size_t j = i + 1; j < brush.n; j++ ) {
			if( j == face )
				continue;

			Vec3 p;
			if( !Intersect3PlanesPoint( &p, brush[ face ], brush[ i ], brush[ j ] ) )
				continue;

			if( !PointInsideBrush( brush, p ) )
				continue;

			points.push_back( p );
		}
	}

	// figure out some ordering
	Vec3 centroid = Vec3( 0.0f );
	for( Vec3 p : points ) {
		centroid += p;
	}
	centroid /= points.size();

	Vec3 tangent, bitangent;
	OrthonormalBasis( brush[ face ].normal, &tangent, &bitangent );

	struct SortedPoint {
		Vec3 p;
		float theta;
	};

	std::vector< SortedPoint > projected_points;
	for( Vec3 p : points ) {
		Vec2 projected = ProjectFaceVert( centroid, tangent, bitangent, p );

		SortedPoint sorted;
		sorted.p = p;
		sorted.theta = atan2f( projected.y, projected.x );
	}

	std::sort( projected_points.begin(), projected_points.end(), []( const SortedPoint & a, const SortedPoint & b ) {
		return a.theta < b.theta;
	} );

	std::vector< Vec3 > sorted_points;
	for( SortedPoint p : projected_points ) {
		sorted_points.push_back( p.p );
	}

	return sorted_points;
}

static std::vector< CompiledMesh > BrushToCompiledMeshes( int entity_id, const ParsedBrush & brush ) {
	// convert 3 verts to planes
	std::vector< Plane > planes;
	for( size_t i = 0; i < brush.faces.n; i++ ) {
		Plane plane;
		const ParsedBrushFace & face = brush.faces.elems[ i ];
		if( !PlaneFrom3Points( &plane, face.plane[ 0 ], face.plane[ 1 ], face.plane[ 2 ] ) ) {
			Fatal( "[entity %d brush %d/line %d] has a non-planar face", entity_id, brush.id, brush.line_number );
		}
		planes.push_back( plane );
	}

	// triangulate faces
	std::vector< CompiledMesh > face_meshes;
	for( size_t i = 0; i < planes.size(); i++ ) {
		CompiledMesh material_mesh;
		material_mesh.material = brush.faces.span()[ i ].material_hash;
		material_mesh.vertices = TriangulateFace( VectorToSpan( planes ), i );

		// TODO: this is not triangulating anything...
		for( u32 j = 0; j < checked_cast< u32 >( material_mesh.vertices.size() ); j++ ) {
			material_mesh.indices.push_back( j );
		}

		face_meshes.push_back( material_mesh );
	}

	return face_meshes;
}

struct KDTreeNode {
	bool leaf;

	// node stuff
	u8 axis;
	float distance;
	u32 above_child;

	// leaf stuff
	u32 first_brush;
	u32 num_brushes;
};

struct CandidatePlane {
	float distance;
	u32 brush_id;
	bool start_edge;
};

struct CandidatePlanes {
	std::vector< CandidatePlane > axes[ 3 ];
};

static int MaxAxis( MinMax3 bounds ) {
	Vec3 dims = bounds.maxs - bounds.mins;
	if( dims.x > dims.y && dims.x > dims.z )
		return 0;
	return dims.y > dims.z ? 1 : 2;
}

static float SurfaceArea( MinMax3 bounds ) {
	Vec3 dims = bounds.maxs - bounds.mins;
	return 2.0f * ( dims.x * dims.y + dims.x * dims.z + dims.y * dims.z );
}

static CandidatePlanes BuildCandidatePlanes( Span< const u32 > brush_ids, Span< const MinMax3 > brush_bounds ) {
	TracyZoneScoped;

	CandidatePlanes planes;

	const char * zone_labels[ 3 ] = { "X", "Y", "Z" };

	for( int i = 0; i < 3; i++ ) {
		TracyZoneScopedN( zone_labels[ i ] );

		std::vector< CandidatePlane > & axis = planes.axes[ i ];
		axis.reserve( brush_ids.n * 2 );

		for( u32 brush_id : brush_ids ) {
			axis.push_back( { brush_bounds[ brush_id ].mins[ i ], brush_id, true } );
			axis.push_back( { brush_bounds[ brush_id ].maxs[ i ], brush_id, false } );
		}

		{
			TracyZoneScopedN( "Sort" );
			std::sort( axis.begin(), axis.end(), []( const CandidatePlane & a, const CandidatePlane & b ) {
				if( a.distance == b.distance )
					return a.start_edge < b.start_edge;
				return a.distance < b.distance;
			} );
		}
	}

	return planes;
}

static void SplitBounds( MinMax3 bounds, int axis, float distance, MinMax3 * below, MinMax3 * above ) {
	*below = bounds;
	*above = bounds;

	below->maxs[ axis ] = distance;
	above->mins[ axis ] = distance;
}

static u32 MakeLeaf( CompiledKDTree * tree, Span< const u32 > brush_ids ) {
	TracyZoneScoped;

	MapKDTreeNode leaf;
	leaf.leaf.is_leaf = 3;
	leaf.leaf.first_brush = tree->brush_indices.size();
	leaf.leaf.num_brushes = checked_cast< u16 >( brush_ids.n );

	for( u32 brush_id : brush_ids ) {
		tree->brush_indices.push_back( brush_id );
	}

	tree->nodes.push_back( leaf );
	return tree->nodes.size() - 1;
}

static u32 BuildKDTreeRecursive( CompiledKDTree * tree, Span< const u32 > brush_ids, Span< const MinMax3 > brush_bounds, CandidatePlanes candidate_planes, MinMax3 node_bounds, u32 max_depth ) {
	TracyZoneScoped;

	if( brush_ids.n <= 1 || max_depth == 0 ) {
		return MakeLeaf( tree, brush_ids );
	}

	float best_cost = INFINITY;
	int best_axis = 0;
	size_t best_plane = 0;

	float node_surface_area = SurfaceArea( node_bounds );

	{
		TracyZoneScopedN( "Find best split" );

		for( int i = 0; i < 3; i++ ) {
			int axis = ( MaxAxis( node_bounds ) + i ) % 3;

			size_t num_below = 0;
			size_t num_above = brush_ids.n;

			for( size_t j = 0; j < candidate_planes.axes[ axis ].size(); j++ ) {
				const CandidatePlane & plane = candidate_planes.axes[ axis ][ j ];

				if( !plane.start_edge ) {
					num_above--;
				}

				if( plane.distance > node_bounds.mins[ axis ] && plane.distance < node_bounds.maxs[ axis ] ) {
					MinMax3 below_bounds, above_bounds;
					SplitBounds( node_bounds, axis, plane.distance, &below_bounds, &above_bounds );

					float frac_below = SurfaceArea( below_bounds ) / node_surface_area;
					float frac_above = SurfaceArea( above_bounds ) / node_surface_area;

					float empty_bonus = num_below == 0 || num_above == 0 ? 0.5f : 1.0f;

					constexpr float traversal_cost = 1.0f;
					constexpr float intersect_cost = 80.0f;
					float cost = traversal_cost + intersect_cost * empty_bonus * ( frac_below * num_below + frac_above * num_above );

					if( cost < best_cost ) {
						best_cost = cost;
						best_axis = axis;
						best_plane = j;
					}
				}

				if( plane.start_edge ) {
					num_below++;
				}
			}

			if( best_cost != INFINITY ) {
				break;
			}
		}
	}

	if( best_cost == INFINITY ) {
		return MakeLeaf( tree, brush_ids );
	}

	// make node
	float distance = candidate_planes.axes[ best_axis ][ best_plane ].distance;

	MapKDTreeNode node;
	node.node.is_leaf_and_splitting_plane_axis = best_axis;
	node.node.splitting_plane_distance = distance;

	std::vector< u32 > below_brush_ids;
	std::vector< u32 > above_brush_ids;

	{
		TracyZoneScopedN( "Classify above/below" );

		for( size_t i = 0; i < best_plane; i++ ) {
			if( candidate_planes.axes[ best_axis ][ i ].start_edge ) {
				below_brush_ids.push_back( candidate_planes.axes[ best_axis ][ i ].brush_id );
			}
		}

		for( size_t i = best_plane + 1; i < candidate_planes.axes[ best_axis ].size(); i++ ) {
			if( !candidate_planes.axes[ best_axis ][ i ].start_edge ) {
				above_brush_ids.push_back( candidate_planes.axes[ best_axis ][ i ].brush_id );
			}
		}
	}

	CandidatePlanes below_planes;
	CandidatePlanes above_planes;
	{
		TracyZoneScopedN( "Split candidate planes" );

		for( int i = 0; i < 3; i++ ) {
			std::vector< CandidatePlane > & axis_below = below_planes.axes[ i ];
			std::vector< CandidatePlane > & axis_above = above_planes.axes[ i ];

			for( size_t j = 0; j < candidate_planes.axes[ i ].size(); j++ ) {
				CandidatePlane & plane = candidate_planes.axes[ i ][ j ];
				const MinMax3 & curr_brush_bounds = brush_bounds[ plane.brush_id ];

				if( curr_brush_bounds.mins[ best_axis ] < distance )
					axis_below.push_back( plane );
				if( curr_brush_bounds.maxs[ best_axis ] > distance )
					axis_above.push_back( plane );
			}

			below_planes.axes[ i ] = axis_below;
			above_planes.axes[ i ] = axis_above;
		}
	}

	MinMax3 below_bounds, above_bounds;
	SplitBounds( node_bounds, best_axis, distance, &below_bounds, &above_bounds );

	u16 node_id = checked_cast< u16 >( tree->nodes.size() );
	tree->nodes.push_back( MapKDTreeNode() );

	BuildKDTreeRecursive( tree, VectorToSpan( below_brush_ids ), brush_bounds, below_planes, below_bounds, max_depth - 1 );
	node.node.front_child = BuildKDTreeRecursive( tree, VectorToSpan( above_brush_ids ), brush_bounds, above_planes, above_bounds, max_depth - 1 );

	tree->nodes[ node_id ] = node;

	return node_id;
}

static void BuildKDTree( CompiledKDTree * tree, Span< const MinMax3 > brush_bounds ) {
	TracyZoneScoped;

	MinMax3 tree_bounds = MinMax3::Empty();
	for( const MinMax3 & bounds : brush_bounds ) {
		tree_bounds = Union( bounds, tree_bounds );
	}

	u32 max_depth = roundf( 8.0f + 1.3f * Log2( brush_bounds.n ) );

	std::vector< u32 > brush_ids;
	for( u32 i = 0; i < checked_cast< u32 >( brush_bounds.n ); i++ ) {
		brush_ids.push_back( i );
	}

	CandidatePlanes candidate_planes = BuildCandidatePlanes( VectorToSpan( brush_ids ), brush_bounds );

	BuildKDTreeRecursive( tree, VectorToSpan( brush_ids ), brush_bounds, candidate_planes, tree_bounds, max_depth );
}

static CompiledMesh MergeMeshes( Span< const CompiledMesh > meshes ) {
	CompiledMesh merged;

	for( const CompiledMesh & mesh : meshes ) {
		size_t base_vertex = merged.vertices.size();
		for( Vec3 p : mesh.vertices ) {
			merged.vertices.push_back( p );
		}
		for( u32 idx : mesh.indices ) {
			merged.indices.push_back( idx + base_vertex );
		}
	}

	return merged;
}

static std::vector< CompiledMesh > GenerateRenderGeometry( const ParsedEntity & entity ) {
	TracyZoneScoped;

	std::vector< CompiledMesh > face_meshes;

	for( const ParsedBrush & brush : entity.brushes ) {
		std::vector< CompiledMesh > brush_meshes = BrushToCompiledMeshes( entity.id, brush );
		for( const CompiledMesh & mesh : brush_meshes ) {
			face_meshes.push_back( mesh );
		}
	}

	// TODO
	// for( const ParsedPatch & patch : entity.patches ) {
	// 	face_meshes.push_back( PatchToCompiledMesh( patch ) );
	// }

	std::sort( face_meshes.begin(), face_meshes.end(), []( const CompiledMesh & a, const CompiledMesh & b ) {
		return a.material < b.material;
	} );

	CompiledMesh dummy_mesh = { };
	face_meshes.push_back( dummy_mesh );

	std::vector< CompiledMesh > merged_meshes;
	size_t material_start_index = 0;

	for( size_t i = 1; i < face_meshes.size(); i++ ) {
		if( face_meshes[ i ].material == material_start_index )
			continue;

		Span< const CompiledMesh > meshes_with_the_same_material( &face_meshes[ material_start_index ], i - material_start_index );
		CompiledMesh merged = MergeMeshes( meshes_with_the_same_material );
		if( merged.indices.size() > 0 ) {
			merged_meshes.push_back( merged );
		}

		material_start_index = i + 1;
	}

	// TODO: meshopt

	return merged_meshes;
}

static CompiledKDTree GenerateCollisionGeometry( const ParsedEntity & entity ) {
	TracyZoneScoped;

	CompiledKDTree kd_tree;
	std::vector< MinMax3 > brush_bounds;

	for( const ParsedBrush & brush : entity.brushes ) {
		// convert triple-vert planes to normal-distance planes
		std::vector< Plane > planes;
		for( size_t i = 0; i < brush.faces.n; i++ ) {
			Plane plane;
			const ParsedBrushFace & face = brush.faces.elems[ i ];
			if( !PlaneFrom3Points( &plane, face.plane[ 0 ], face.plane[ 1 ], face.plane[ 2 ] ) ) {
				Fatal( "[entity %d brush %d/line %d] has a non-planar face", entity.id, brush.id, brush.line_number );
			}
			planes.push_back( plane );
		}

		// compute brush bounds
		MinMax3 bounds = MinMax3::Empty();
		for( size_t i = 0; i < planes.size(); i++ ) {
			std::vector< Vec3 > points = TriangulateFace( VectorToSpan( planes ), i );
			for( Vec3 p : points ) {
				bounds = Union( bounds, p );
			}
		}
		brush_bounds.push_back( bounds );

		// make MapBrush
		MapBrush map_brush = { };
		map_brush.first_plane = checked_cast< u16 >( kd_tree.planes.size() );
		map_brush.num_planes = checked_cast< u8 >( planes.size() );
		map_brush.solidity = 0; // TODO

		for( Plane plane : planes ) {
			kd_tree.planes.push_back( plane );
		}

		kd_tree.brushes.push_back( map_brush );
		kd_tree.bounds = Union( kd_tree.bounds, bounds );
	}

	BuildKDTree( &kd_tree, VectorToSpan( brush_bounds ) );

	return kd_tree;
}

static void WriteFileOrComplain( ArenaAllocator * arena, const char * path, const void * data, size_t len ) {
	if( WriteFile( arena, path, data, len ) ) {
		printf( "Wrote %s\n", path );
	}
	else {
		char * msg = ( *arena )( "Can't write {}", path );
		perror( msg );
	}
}

static Span< const char > GetKey( Span< const ParsedKeyValue > kvs, const char * key ) {
	for( ParsedKeyValue kv : kvs ) {
		if( StrCaseEqual( kv.key, key ) ) {
			return kv.value;
		}
	}

	return Span< const char >( NULL, 0 );
}

static constexpr const char * map_section_names[] = {
	"EntityData",
	"EntityKeyValues",
	"Entities",

	"Models",

	"Nodes",
	"Brushes",
	"BrushIndices",
	"BrushPlanes",
	"BrushPlaneIndices",

	"Meshes",
	"Vertices",
	"VertexIndices",
};

template< typename T >
void PackCDMap( DynamicArray< u8 > & packed, MapHeader * header, MapSectionType section, Span< const T > data ) {
	size_t padding = packed.num_bytes() % alignof( T );
	if( padding != 0 )
		padding = alignof( T ) - padding;
	size_t before_padding = packed.extend( padding );
	memset( &packed[ before_padding ], 0, packed.size() - before_padding ); // zero out alignment and struct padding bytes

	size_t offset = packed.extend( data.num_bytes() );
	memcpy( &packed[ offset ], data.ptr, data.num_bytes() );

	header->sections[ section ].offset = checked_cast< u32 >( offset );
	header->sections[ section ].size = checked_cast< u32 >( data.num_bytes() );

	ggprint( "Section {-20} is size {.2}MB\n", map_section_names[ section ], data.num_bytes() / 1000.0f / 1000.0f );
}

static void WriteCDMap( ArenaAllocator * arena, const char * path, const MapData * map ) {
	TracyZoneScoped;

	DynamicArray< u8 > packed( arena );

	MapHeader header = { };
	strcpy( header.magic, "cdmap" );
	header.format_version = CDMAP_FORMAT_VERSION;

	packed.extend( sizeof( header ) );
	memset( packed.ptr(), 0, packed.size() ); // zero out padding bytes

	PackCDMap( packed, &header, MapSection_EntityData, map->entity_data );
	PackCDMap( packed, &header, MapSection_Entities, map->entities );
	PackCDMap( packed, &header, MapSection_Models, map->models );
	PackCDMap( packed, &header, MapSection_Nodes, map->nodes );
	PackCDMap( packed, &header, MapSection_Brushes, map->brushes );
	PackCDMap( packed, &header, MapSection_BrushIndices, map->brush_indices );
	PackCDMap( packed, &header, MapSection_BrushPlanes, map->brush_planes );
	PackCDMap( packed, &header, MapSection_BrushPlaneIndices, map->brush_plane_indices );
	PackCDMap( packed, &header, MapSection_Meshes, map->meshes );
	PackCDMap( packed, &header, MapSection_Vertices, map->vertices );
	PackCDMap( packed, &header, MapSection_VertexIndices, map->vertex_indices );

	memcpy( &packed[ 0 ], &header, sizeof( header ) );

	WriteFileOrComplain( arena, path, packed.ptr(), packed.num_bytes() );
}

int main( int argc, char ** argv ) {
	if( argc != 2 && !( argc == 3 && StrEqual( argv[ 1 ], "--compress" ) ) ) {
		printf( "Usage: %s [--compress] <file.map>\n", argv[ 0 ] );
		return 1;
	}

	const char * src_path = argc == 2 ? argv[ 1 ] : argv[ 2 ];
	bool compress = argc == 3;

	size_t src_len;
	char * src = ReadFileString( sys_allocator, src_path, &src_len );
	if( src == NULL ) {
		char * msg = ( *sys_allocator )( "Can't read {}", src_path );
		perror( msg );
		FREE( sys_allocator, msg );
		return 1;
	}
	defer { FREE( sys_allocator, src ); };

	constexpr size_t arena_size = 1024 * 1024 * 1024; // 1GB
	ArenaAllocator arena( ALLOC_SIZE( sys_allocator, arena_size, 16 ), arena_size );

	InitFS();
	InitMaterials();

	// parse the .map
	std::vector< ParsedEntity > entities = ParseEntities( Span< char >( src, src_len ) );

	// flatten func_groups into entity 0
	{
		TracyZoneScopedN( "Flatten func_groups" );

		for( ParsedEntity & entity : entities ) {
			if( GetKey( entity.kvs.span(), "classname" ) != "func_group" )
				continue;

			entities[ 0 ].brushes.add_many( entity.brushes.span() );
			entities[ 0 ].patches.add_many( entity.patches.span() );

			entity.brushes.clear();
			entity.patches.clear();
		}
	}

	// generate geometries
	std::vector< CompiledEntity > compiled_entities;

	{
		TracyZoneScopedN( "Compile entities" );

		for( ParsedEntity & entity : entities ) {
			CompiledEntity compiled;
			compiled.render_geometry = GenerateRenderGeometry( entity );
			compiled.collision_geometry = GenerateCollisionGeometry( entity ); // TODO: patches

			// TODO: keyvalues
			// TODO: assign model IDs
		}
	}

	// fix up entity models
	// for( const ParsedEntity & entity : entities ) {
	// 	if( GetKey( entity.kvs.span(), "classname" ) == "func_group" )
	// 		continue;
        //
	// 	bsp.entities->append( "{{\n" );
	// 	for( ParsedKeyValue kv : entity.kvs.span() ) {
	// 		if( kv.key == "model" && FileExtension( kv.value ) == ".glb" ) {
	// 			bsp.entities->append( "\t\"{}\" \"{}\"\n", kv.key, StripExtension( kv.value ) );
	// 		}
	// 		else {
	// 			bsp.entities->append( "\t\"{}\" \"{}\"\n", kv.key, kv.value );
	// 		}
	// 	}
	// 	if( entity.brushes.size() > 0 ) {
	// 		bsp.entities->append( "\t\"model\" \"*{}\"\n", entity.model_id );
	// 	}
	// 	bsp.entities->append( "}}\n" );
	// }

	// flatten everything into linear arrays
	DynamicArray< char > flat_entity_data( &arena );
	DynamicArray< MapEntityKeyValue > flat_entity_key_values( &arena );
	DynamicArray< MapEntity > flat_entities( &arena );
	DynamicArray< MapModel > flat_models( &arena );
	DynamicArray< MapKDTreeNode > flat_nodes( &arena );
	DynamicArray< MapBrush > flat_brushes( &arena );
	DynamicArray< u32 > flat_brush_indices( &arena );
	DynamicArray< Plane > flat_brush_planes( &arena );
	DynamicArray< u32 > flat_brush_plane_indices( &arena );
	DynamicArray< MapMesh > flat_meshes( &arena );
	DynamicArray< MapVertex > flat_vertices( &arena );
	DynamicArray< u32 > flat_vertex_indices( &arena );

	{
		TracyZoneScopedN( "Flatten" );

		for( const CompiledEntity & entity : compiled_entities ) {
			MapEntity map_entity;
			map_entity.first_key_value = checked_cast< u32 >( flat_entity_key_values.size() );
			map_entity.num_key_values = entity.key_values.size();

			for( const ParsedKeyValue kv : entity.key_values ) {
				MapEntityKeyValue map_kv;
				map_kv.offset = flat_entity_data.size();
				map_kv.key_size = kv.key.n;
				map_kv.value_size = kv.value.n;
				flat_entity_key_values.add( map_kv );

				flat_entity_data.add_many( kv.key );
				flat_entity_data.add_many( kv.value );
			}

			size_t base_node = flat_nodes.size();
			size_t base_brush = flat_brushes.size();
			size_t base_brush_plane = flat_brush_planes.size();
			flat_nodes.add_many( VectorToSpan( entity.collision_geometry.nodes ) );
			flat_brushes.add_many( VectorToSpan( entity.collision_geometry.brushes ) );
			flat_brush_planes.add_many( VectorToSpan( entity.collision_geometry.planes ) );
			// TODO: indices


			size_t base_mesh = flat_meshes.size();

			for( const CompiledMesh & mesh : entity.render_geometry ) {
				MapMesh map_mesh;
				map_mesh.material = mesh.material;
				map_mesh.first_vertex_index = flat_vertex_indices.size();
				// TODO: do this right so glDrawElements w/ offset works
			}
			size_t base_vertex = flat_vertices.size();

			// TODO: models
		}
	}

	// write to disk
	MapData flattened;
	flattened.entity_data = flat_entity_data.span();
	flattened.entities = flat_entities.span();
	flattened.models = flat_models.span();
	flattened.nodes = flat_nodes.span();
	flattened.brushes = flat_brushes.span();
	flattened.brush_indices = flat_brush_indices.span();
	flattened.brush_planes = flat_brush_planes.span();
	flattened.brush_plane_indices = flat_brush_plane_indices.span();
	flattened.meshes = flat_meshes.span();
	flattened.vertices = flat_vertices.span();
	flattened.vertex_indices = flat_vertex_indices.span();

	const char * cdmap_path = arena( "{}.cdmap", StripExtension( src_path ) );
	WriteCDMap( &arena, cdmap_path, &flattened );

	// TODO: generate render geometry
	// - figure out what postprocessing we need e.g. welding
	// - meshopt, before patch brushes
	// - extend void render geometry
	// - extend void brushes
	//
	// TODO: generate all models
	// - kdtree per model after new format
	//
	// TODO: new map format
	// - see bsp2.cpp
	// - flip CW to CCW winding. q3 bsp was CW lol

	FREE( sys_allocator, arena.get_memory() );

	ShutdownMaterials();
	ShutdownFS();

	return 0;
}

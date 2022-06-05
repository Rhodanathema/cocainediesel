#include "qcommon/base.h"
#include "qcommon/qcommon.h"
#include "qcommon/hash.h"
#include "client/client.h"
#include "client/renderer/renderer.h"
#include "client/assets.h"
#include "cgame/ref.h"

#include "cgltf/cgltf.h"

#define JSMN_HEADER
#include "jsmn/jsmn.h"

// like cgltf_load_buffers, but doesn't try to load URIs
static bool LoadBinaryBuffers( cgltf_data * data ) {
	if( data->buffers_count && data->buffers[0].data == NULL && data->buffers[0].uri == NULL && data->bin ) {
		if( data->bin_size < data->buffers[0].size )
			return false;
		data->buffers[0].data = const_cast< void * >( data->bin );
	}

	for( cgltf_size i = 0; i < data->buffers_count; i++ ) {
		if( data->buffers[i].data == NULL )
			return false;
	}

	return true;
}

static u8 GetNodeIdx( const cgltf_node * node ) {
	return u8( uintptr_t( node->light ) - 1 );
}

static void SetNodeIdx( cgltf_node * node, u8 idx ) {
	node->light = ( cgltf_light * ) uintptr_t( idx + 1 );
}

static Span< const u8 > AccessorToSpan( const cgltf_accessor * accessor ) {
	cgltf_size offset = accessor->offset + accessor->buffer_view->offset;
	return Span< const u8 >( ( const u8 * ) accessor->buffer_view->buffer->data + offset, accessor->count * accessor->stride );
}

static VertexFormat VertexFormatFromGLTF( cgltf_type dim, cgltf_component_type component, bool normalized ) {
	// TODO: support signed types
	if( dim == cgltf_type_vec2 ) {
		if( component == cgltf_component_type_r_8u )
			return normalized ? VertexFormat_U8x2_Norm : VertexFormat_U8x2;
		if( component == cgltf_component_type_r_16u )
			return normalized ? VertexFormat_U16x2_Norm : VertexFormat_U16x2;
		if( component == cgltf_component_type_r_32f )
			return VertexFormat_Floatx2;
	}

	if( dim == cgltf_type_vec3 ) {
		if( component == cgltf_component_type_r_8u )
			return normalized ? VertexFormat_U8x3_Norm : VertexFormat_U8x3;
		if( component == cgltf_component_type_r_16u )
			return normalized ? VertexFormat_U16x3_Norm : VertexFormat_U16x3;
		if( component == cgltf_component_type_r_32f )
			return VertexFormat_Floatx3;
	}

	if( dim == cgltf_type_vec4 ) {
		if( component == cgltf_component_type_r_8u )
			return normalized ? VertexFormat_U8x4_Norm : VertexFormat_U8x4;
		if( component == cgltf_component_type_r_16u )
			return normalized ? VertexFormat_U16x4_Norm : VertexFormat_U16x4;
		if( component == cgltf_component_type_r_32f )
			return VertexFormat_Floatx4;
	}

	assert( false );
	return VertexFormat_Floatx4;
}

static void LoadGeometry( const char * filename, GLTFRenderData * model, const cgltf_node * node, const Mat4 & transform ) {
	TempAllocator temp = cls.frame_arena.temp();

	const cgltf_primitive & prim = node->mesh->primitives[ 0 ];

	MeshConfig mesh_config;
	mesh_config.name = temp( "{} nodes[{}]", filename, node->name );

	for( size_t i = 0; i < prim.attributes_count; i++ ) {
		const cgltf_attribute & attr = prim.attributes[ i ];

		if( attr.type == cgltf_attribute_type_position ) {
			mesh_config.num_vertices = attr.data->count;
			mesh_config.positions = NewGPUBuffer( AccessorToSpan( attr.data ) );

			Vec3 min, max;
			for( int j = 0; j < 3; j++ ) {
				min[ j ] = attr.data->min[ j ];
				max[ j ] = attr.data->max[ j ];
			}

			model->bounds = Union( model->bounds, ( transform * Vec4( min, 1.0f ) ).xyz() );
			model->bounds = Union( model->bounds, ( transform * Vec4( max, 1.0f ) ).xyz() );
		}

		if( attr.type == cgltf_attribute_type_normal ) {
			mesh_config.normals = NewGPUBuffer( AccessorToSpan( attr.data ) );
		}

		if( attr.type == cgltf_attribute_type_texcoord ) {
			if( mesh_config.tex_coords.buffer != 0 ) {
				Com_Printf( S_COLOR_YELLOW "%s has multiple sets of uvs\n", filename );
			}
			else {
				mesh_config.tex_coords = NewGPUBuffer( AccessorToSpan( attr.data ) );
				mesh_config.tex_coords_format = VertexFormatFromGLTF( attr.data->type, attr.data->component_type, attr.data->normalized );
			}
		}

		if( attr.type == cgltf_attribute_type_color ) {
			mesh_config.colors = NewGPUBuffer( AccessorToSpan( attr.data ) );
			mesh_config.colors_format = VertexFormatFromGLTF( attr.data->type, attr.data->component_type, attr.data->normalized );
		}

		if( attr.type == cgltf_attribute_type_joints ) {
			mesh_config.joints = NewGPUBuffer( AccessorToSpan( attr.data ) );
			mesh_config.joints_format = VertexFormatFromGLTF( attr.data->type, attr.data->component_type, attr.data->normalized );
		}

		if( attr.type == cgltf_attribute_type_weights ) {
			mesh_config.weights = NewGPUBuffer( AccessorToSpan( attr.data ) );
			mesh_config.weights_format = VertexFormatFromGLTF( attr.data->type, attr.data->component_type, attr.data->normalized );
		}
	}

	mesh_config.indices = NewGPUBuffer( AccessorToSpan( prim.indices ) );
	mesh_config.indices_format = prim.indices->component_type == cgltf_component_type_r_16u ? IndexFormat_U16 : IndexFormat_U32;
	mesh_config.num_vertices = prim.indices->count;
	mesh_config.ccw_winding = true;

	GLTFRenderData::Primitive * primitive = &model->primitives[ model->num_primitives ];
	model->num_primitives++;

	primitive->mesh = NewMesh( mesh_config );
	primitive->first_index = 0;
	primitive->num_vertices = 0;

	const char * material_name = prim.material != NULL ? prim.material->name : "";
	primitive->material = FindMaterial( material_name );
}

constexpr u32 MAX_EXTRAS = 16;
struct GltfExtras {
	Span< const char > keys[ MAX_EXTRAS ];
	Span< const char > values[ MAX_EXTRAS ];
	u32 num_extras = 0;
};

static GltfExtras LoadExtras( cgltf_data * gltf, cgltf_node * gltf_node ) {
	char json_data[ 1024 ];
	size_t json_size = 1024;
	cgltf_copy_extras_json( gltf, &gltf_node->extras, json_data, &json_size );
	Span< const char > json( json_data, json_size );

	GltfExtras extras = { };

	jsmn_parser p;
	constexpr u32 max_tokens = MAX_EXTRAS * 2 + 1;
	jsmntok_t tokens[ max_tokens ];
	jsmn_init( &p );
	int res = jsmn_parse( &p, json.ptr, json.n, tokens, max_tokens );
	if( res < 0 ) {
		return extras;
	}
	if( res < 1 || tokens[ 0 ].type != JSMN_OBJECT ) {
		return extras;
	}

	for( s32 i = 0; i < tokens[ 0 ].size; i++ ) {
		u32 idx = 1 + i * 2;
		extras.keys[ extras.num_extras ] = json.slice( tokens[ idx ].start, tokens[ idx ].end );
		// TODO: value could be object or array...
		extras.values[ extras.num_extras ] = json.slice( tokens[ idx + 1 ].start, tokens[ idx + 1 ].end );
		extras.num_extras++;
	}

	return extras;
}

static Span< const char > GetExtrasKey( const char * key, GltfExtras * extras ) {
	for( u32 i = 0; i < extras->num_extras; i++ ) {
		if( StrEqual( extras->keys[ i ], key ) ) {
			return extras->values[ i ];
		}
	}
	return MakeSpan( "" );
}

static void LoadNode( const char * filename, GLTFRenderData * model, cgltf_data * gltf, cgltf_node * gltf_node, u8 * node_idx ) {
	u8 idx = *node_idx;
	*node_idx += 1;
	SetNodeIdx( gltf_node, idx );

	GLTFRenderData::Node * node = &model->nodes[ idx ];
	node->parent = gltf_node->parent != NULL ? GetNodeIdx( gltf_node->parent ) : U8_MAX;
	node->primitive = U8_MAX;
	node->name = gltf_node->name == NULL ? 0 : Hash32( gltf_node->name );

	cgltf_node_transform_world( gltf_node, node->global_transform.ptr() );

	node->local_transform.rotation = Quaternion::Identity();
	node->local_transform.translation = Vec3( 0.0f );
	node->local_transform.scale = 1.0f;

	if( gltf_node->has_rotation ) {
		node->local_transform.rotation = Quaternion(
			gltf_node->rotation[ 0 ],
			gltf_node->rotation[ 1 ],
			gltf_node->rotation[ 2 ],
			gltf_node->rotation[ 3 ]
		);
	}

	if( gltf_node->has_translation ) {
		node->local_transform.translation = Vec3(
			gltf_node->translation[ 0 ],
			gltf_node->translation[ 1 ],
			gltf_node->translation[ 2 ]
		);
	}

	if( gltf_node->has_scale ) {
		// TODO
		// assert( Abs( gltf_node->scale[ 0 ] / gltf_node->scale[ 1 ] - 1.0f ) < 0.001f );
		// assert( Abs( gltf_node->scale[ 0 ] / gltf_node->scale[ 2 ] - 1.0f ) < 0.001f );
		node->local_transform.scale = gltf_node->scale[ 0 ];
	}

	{
		GltfExtras extras = LoadExtras( gltf, gltf_node );
		Span< const char > type = GetExtrasKey( "type", &extras );
		Span< const char > color_value = GetExtrasKey( "color", &extras );
		Vec4 color;
		for( u32 i = 0; i < 4; i++ ) {
			color[ i ] = ParseFloat( &color_value, 1.0f, Parse_StopOnNewLine );
		}
		if( type == "vfx" ) {
			node->vfx_type = ModelVfxType_Vfx;
			node->vfx_node.name = StringHash( GetExtrasKey( "name", &extras ) );
			node->vfx_node.color = color;
		}
		else if( type == "dlight" ) {
			node->vfx_type = ModelVfxType_DynamicLight;
			node->dlight_node.color = color;
			Span< const char > intensity = GetExtrasKey( "intensity", &extras );
			node->dlight_node.intensity = ParseFloat( &intensity, 0.0f, Parse_DontStopOnNewLine );
		}
		else if( type == "decal" ) {
			node->vfx_type = ModelVfxType_Decal;
			node->decal_node.color = color;
			Span< const char > angle = GetExtrasKey( "angle", &extras );
			node->decal_node.angle = ParseFloat( &angle, 0.0f, Parse_DontStopOnNewLine );
			Span< const char > radius = GetExtrasKey( "radius", &extras );
			node->decal_node.radius = ParseFloat( &radius, 0.0f, Parse_DontStopOnNewLine );
			node->decal_node.name = StringHash( GetExtrasKey( "name", &extras ) );
		}
	}

	node->skinned = gltf_node->skin != NULL;

	// TODO: this will break if multiple nodes share a mesh
	if( gltf_node->mesh != NULL ) {
		node->primitive = model->num_primitives;
		LoadGeometry( filename, model, gltf_node, node->global_transform );
	}

	for( size_t i = 0; i < gltf_node->children_count; i++ ) {
		LoadNode( filename, model, gltf, gltf_node->children[ i ], node_idx );
	}

	if( gltf_node->children_count == 0 ) {
		node->first_child = U8_MAX;
	}
	else {
		node->first_child = GetNodeIdx( gltf_node->children[ 0 ] );

		for( size_t i = 0; i < gltf_node->children_count - 1; i++ ) {
			model->nodes[ GetNodeIdx( gltf_node->children[ i ] ) ].sibling = GetNodeIdx( gltf_node->children[ i + 1 ] );
		}

		model->nodes[ GetNodeIdx( gltf_node->children[ gltf_node->children_count - 1 ] ) ].sibling = U8_MAX;
	}
}

static InterpolationMode InterpolationModeFromGLTF( cgltf_interpolation_type interpolation ) {
	// TODO: cubic
	return interpolation == cgltf_interpolation_type_step ? InterpolationMode_Step : InterpolationMode_Linear;
}

template< typename T >
static float LoadChannel( const cgltf_animation_channel * chan, GLTFRenderData::AnimationChannel< T > * out_channel ) {
	constexpr size_t lanes = sizeof( T ) / sizeof( float );
	size_t n = chan->sampler->input->count;

	float * memory = ALLOC_MANY( sys_allocator, float, n * ( lanes + 1 ) );
	out_channel->times = memory;
	out_channel->samples = ( T * ) ( memory + n );
	out_channel->num_samples = n;
	out_channel->interpolation = InterpolationModeFromGLTF( chan->sampler->interpolation );

	for( size_t i = 0; i < n; i++ ) {
		cgltf_bool ok = cgltf_accessor_read_float( chan->sampler->input, i, &out_channel->times[ i ], 1 );
		ok = ok && cgltf_accessor_read_float( chan->sampler->output, i, out_channel->samples[ i ].ptr(), lanes );
		assert( ok != 0 );
	}

	float duration = chan->sampler->input->max[ 0 ] - chan->sampler->input->min[ 0 ];
	return duration;
}

static float LoadScaleChannel( const cgltf_animation_channel * chan, GLTFRenderData::AnimationChannel< float > * out_channel ) {
	size_t n = chan->sampler->input->count;

	float * memory = ALLOC_MANY( sys_allocator, float, n * 2 );
	out_channel->times = memory;
	out_channel->samples = memory + n;
	out_channel->num_samples = n;
	out_channel->interpolation = InterpolationModeFromGLTF( chan->sampler->interpolation );

	for( size_t i = 0; i < n; i++ ) {
		cgltf_accessor_read_float( chan->sampler->input, i, &out_channel->times[ i ], 1 );

		float scale[ 3 ];
		cgltf_accessor_read_float( chan->sampler->output, i, scale, 3 );

		assert( Abs( scale[ 0 ] - scale[ 1 ] ) < 0.001f );
		assert( Abs( scale[ 0 ] - scale[ 2 ] ) < 0.001f );

		out_channel->samples[ i ] = scale[ 0 ];
	}

	float duration = chan->sampler->input->max[ 0 ] - chan->sampler->input->min[ 0 ];
	return duration;
}

template< typename T >
static void CreateSingleSampleChannel( GLTFRenderData::AnimationChannel< T > * out_channel, T sample ) {
	constexpr size_t lanes = sizeof( T ) / sizeof( float );

	float * memory = ALLOC_MANY( sys_allocator, float, lanes + 1 );
	out_channel->times = memory;
	out_channel->samples = ( T * ) ( memory + 1 );
	out_channel->num_samples = 1;

	out_channel->times[ 0 ] = 0.0f;
	out_channel->samples[ 0 ] = sample;
}

static void LoadAnimation( GLTFRenderData * model, const cgltf_animation * animation, u8 index = 0 ) {
	float duration = 0.0f;
	for( size_t i = 0; i < animation->channels_count; i++ ) {
		const cgltf_animation_channel * chan = &animation->channels[ i ];

		u8 node_idx = GetNodeIdx( chan->target_node );
		assert( node_idx != U8_MAX );

		float channel_duration = 0.0f;
		if( chan->target_path == cgltf_animation_path_type_translation ) {
			channel_duration = LoadChannel( chan, &model->nodes[ node_idx ].animations[ index ].translations );
		}
		else if( chan->target_path == cgltf_animation_path_type_rotation ) {
			channel_duration = LoadChannel( chan, &model->nodes[ node_idx ].animations[ index ].rotations );
		}
		else if( chan->target_path == cgltf_animation_path_type_scale ) {
			channel_duration = LoadScaleChannel( chan, &model->nodes[ node_idx ].animations[ index ].scales );
		}
		duration = Max2( channel_duration, duration );
	}
	model->animations[ index ].name = StringHash( animation->name );
	model->animations[ index ].duration = duration;
}

static void LoadSkin( GLTFRenderData * model, const cgltf_skin * skin ) {
	model->skin = ALLOC_MANY( sys_allocator, GLTFRenderData::Joint, skin->joints_count );
	model->num_joints = skin->joints_count;

	for( size_t i = 0; i < skin->joints_count; i++ ) {
		GLTFRenderData::Joint * joint = &model->skin[ i ];
		joint->node_idx = GetNodeIdx( skin->joints[ i ] );

		cgltf_bool ok = cgltf_accessor_read_float( skin->inverse_bind_matrices, i, joint->joint_to_bind.ptr(), 16 );
		assert( ok != 0 );
	}
}

static bool NewGLTFRenderData( GLTFRenderData * render_data, const cgltf_data * gltf, const char * path ) {
	TracyZoneScoped;

	*render_data = { };
	render_data->bounds = MinMax3::Empty();

	bool ok = false;
	defer {
		if( !ok ) {
			DeleteGLTFRenderData( render_data );
		}
	};

	constexpr Mat4 y_up_to_z_up(
		1, 0, 0, 0,
		0, 0, -1, 0,
		0, 1, 0, 0,
		0, 0, 0, 1
	);
	render_data->transform = y_up_to_z_up;

	render_data->primitives = ALLOC_MANY( sys_allocator, render_data::Primitive, gltf->meshes_count );

	render_data->nodes = ALLOC_MANY( sys_allocator, render_data::Node, gltf->nodes_count );
	memset( render_data->nodes, 0, sizeof( render_data::Node ) * gltf->nodes_count );
	render_data->num_nodes = gltf->nodes_count;

	u8 node_idx = 0;
	for( size_t i = 0; i < gltf->scene->nodes_count; i++ ) {
		LoadNode( path, render_data, gltf, gltf->scene->nodes[ i ], &node_idx );
		render_data->nodes[ GetNodeIdx( gltf->scene->nodes[ i ] ) ].sibling = U8_MAX;
	}

	if( gltf->animations_count > 0 ) {
		render_data->num_animations = gltf->animations_count;
		if( gltf->skins_count > 0 ) {
			LoadSkin( render_data, &gltf->skins[ 0 ] );
		}

		render_data->animations = ALLOC_MANY( sys_allocator, render_data::Animation, gltf->animations_count );
		for( size_t i = 0; i < render_data->num_nodes; i++ ) {
			render_data->nodes[ i ].animations = ALLOC_SPAN( sys_allocator, render_data::NodeAnimation, gltf->animations_count );
			memset( render_data->nodes[ i ].animations.ptr, 0, sizeof( render_data::NodeAnimation ) * gltf->animations_count );
		}
		for( size_t i = 0; i < gltf->animations_count; i++ ) {
			LoadAnimation( render_data, &gltf->animations[ i ], i );
		}
	}

	render_data->camera = U8_MAX;
	for( size_t i = 0; i < gltf->nodes_count; i++ ) {
		if( gltf->nodes[ i ].camera != NULL ) {
			render_data->camera = GetNodeIdx( &gltf->nodes[ i ] );
		}
	}

	ok = true;

	return true;
}

static void DeleteGLTFRenderData( GLTFRenderData * render_data ) {
	for( GLTFRenderData::Primitive primitive : render_data->primitives ) {
		DeleteMesh( primtive.mesh );
	}

	for( GLTFRenderData::Node node : render_data->nodes ) {
		for( size_t i = 0; i < render_data->animations.n; i++ ) {
			FREE( sys_allocator, node.animations[ i ].rotations.samples );
			FREE( sys_allocator, node.animations[ i ].translations.samples );
			FREE( sys_allocator, node.animations[ i ].scales.samples );
		}
		FREE( sys_allocator, node.animations.ptr );
	}

	FREE( sys_allocator, model->primitives );
	FREE( sys_allocator, model->nodes );
	FREE( sys_allocator, model->skin );
	FREE( sys_allocator, model->animations );
}

bool LoadGLTF( GLTFRenderData * render_data, const char * path ) {
	TracyZoneScoped;
	TracyZoneText( path, strlen( path ) );

	Span< const u8 > data = AssetBinary( path );

	cgltf_options options = { };
	options.type = cgltf_file_type_glb;

	cgltf_data * gltf;
	if( cgltf_parse( &options, data.ptr, data.num_bytes(), &gltf ) != cgltf_result_success ) {
		Com_Printf( S_COLOR_YELLOW "%s isn't a GLTF file\n", path );
		return false;
	}

	defer { cgltf_free( gltf ); };

	if( !LoadBinaryBuffers( gltf ) ) {
		Com_Printf( S_COLOR_YELLOW "Couldn't load buffers in %s\n", path );
		return false;
	}

	if( cgltf_validate( gltf ) != cgltf_result_success ) {
		Com_Printf( S_COLOR_YELLOW "%s is invalid GLTF\n", path );
		return false;
	}

	if( gltf->scenes_count != 1 || gltf->skins_count > 1 || gltf->cameras_count > 1 ) {
		Com_Printf( S_COLOR_YELLOW "Trivial models only please (%s)\n", path );
		return false;
	}

	if( gltf->lights_count != 0 ) {
		Com_Printf( S_COLOR_YELLOW "We can't load models that have lights in them (%s)\n", path );
		return false;
	}

	for( size_t i = 0; i < gltf->meshes_count; i++ ) {
		if( gltf->meshes[ i ].primitives_count != 1 ) {
			Com_Printf( S_COLOR_YELLOW "Meshes with multiple primitives are unsupported (%s)\n", path );
			return false;
		}
	}

	return NewGLTFRenderData( render_data, gltf, path );
}

template< typename T, typename F >
static T SampleAnimationChannel( const Model::AnimationChannel< T > & channel, float t, T def, F lerp ) {
	if( channel.samples == NULL )
		return def;
	if( channel.num_samples == 1 )
		return channel.samples[ 0 ];

	t = Clamp( channel.times[ 0 ], t, channel.times[ channel.num_samples - 1 ] );

	u32 sample = 0;
	for( u32 i = 1; i < channel.num_samples; i++ ) {
		if( channel.times[ i ] >= t ) {
			sample = i - 1;
			break;
		}
	}

	// TODO: cubic
	if( channel.interpolation == InterpolationMode_Step ) {
		return channel.samples[ sample ];
	}

	float lerp_frac = ( t - channel.times[ sample ] ) / ( channel.times[ sample + 1 ] - channel.times[ sample ] );
	return lerp( channel.samples[ sample ], lerp_frac, channel.samples[ sample + 1 ] );
}

// can't use overloaded function as a template parameter
static Vec3 LerpVec3( Vec3 a, float t, Vec3 b ) { return Lerp( a, t, b ); }
static float LerpFloat( float a, float t, float b ) { return Lerp( a, t, b ); }

Span< TRS > SampleAnimation( Allocator * a, const Model * model, float t, u8 animation ) {
	TracyZoneScoped;

	Span< TRS > local_poses = ALLOC_SPAN( a, TRS, model->num_nodes );

	for( u8 i = 0; i < model->num_nodes; i++ ) {
		const Model::Node * node = &model->nodes[ i ];
		local_poses[ i ].rotation = SampleAnimationChannel( node->animations[ animation ].rotations, t, node->local_transform.rotation, NLerp );
		local_poses[ i ].translation = SampleAnimationChannel( node->animations[ animation ].translations, t, node->local_transform.translation, LerpVec3 );
		local_poses[ i ].scale = SampleAnimationChannel( node->animations[ animation ].scales, t, node->local_transform.scale, LerpFloat );
	}

	return local_poses;
}

static Mat4 TRSToMat4( const TRS & trs ) {
	Quaternion q = trs.rotation;
	Vec3 t = trs.translation;
	float s = trs.scale;

	// return t * q * s;
	return Mat4(
		( 1.0f - 2 * q.y * q.y - 2.0f * q.z * q.z ) * s,
		( 2.0f * q.x * q.y - 2.0f * q.z * q.w ) * s,
		( 2.0f * q.x * q.z + 2.0f * q.y * q.w ) * s,
		t.x,

		( 2.0f * q.x * q.y + 2.0f * q.z * q.w ) * s,
		( 1.0f - 2.0f * q.x * q.x - 2.0f * q.z * q.z ) * s,
		( 2.0f * q.y * q.z - 2.0f * q.x * q.w ) * s,
		t.y,

		( 2.0f * q.x * q.z - 2.0f * q.y * q.w ) * s,
		( 2.0f * q.y * q.z + 2.0f * q.x * q.w ) * s,
		( 1.0f - 2.0f * q.x * q.x - 2.0f * q.y * q.y ) * s,
		t.z,

		0.0f, 0.0f, 0.0f, 1.0f
	);
}

MatrixPalettes ComputeMatrixPalettes( Allocator * a, const Model * model, Span< const TRS > local_poses ) {
	TracyZoneScoped;

	assert( local_poses.n == model->num_nodes );

	MatrixPalettes palettes = { };
	palettes.node_transforms = ALLOC_SPAN( a, Mat4, model->num_nodes );
	if( model->num_joints != 0 ) {
		palettes.skinning_matrices = ALLOC_SPAN( a, Mat4, model->num_joints );
	}

	for( u8 i = 0; i < model->num_nodes; i++ ) {
		u8 parent = model->nodes[ i ].parent;
		if( parent == U8_MAX ) {
			palettes.node_transforms[ i ] = TRSToMat4( local_poses[ i ] );
		}
		else {
			palettes.node_transforms[ i ] = palettes.node_transforms[ parent ] * TRSToMat4( local_poses[ i ] );
		}
	}

	for( u8 i = 0; i < model->num_joints; i++ ) {
		u8 node_idx = model->skin[ i ].node_idx;
		palettes.skinning_matrices[ i ] = palettes.node_transforms[ node_idx ] * model->skin[ i ].joint_to_bind;
	}

	return palettes;
}

bool FindNodeByName( const Model * model, u32 name, u8 * idx ) {
	for( u8 i = 0; i < model->num_nodes; i++ ) {
		if( model->nodes[ i ].name == name ) {
			*idx = i;
			return true;
		}
	}

	return false;
}

static void MergePosesRecursive( Span< TRS > lower, Span< const TRS > upper, const Model * model, u8 i ) {
	lower[ i ] = upper[ i ];

	const Model::Node * node = &model->nodes[ i ];
	if( node->sibling != U8_MAX )
		MergePosesRecursive( lower, upper, model, node->sibling );
	if( node->first_child != U8_MAX )
		MergePosesRecursive( lower, upper, model, node->first_child );
}

void MergeLowerUpperPoses( Span< TRS > lower, Span< const TRS > upper, const Model * model, u8 upper_root_node ) {
	lower[ upper_root_node ] = upper[ upper_root_node ];

	const Model::Node * node = &model->nodes[ upper_root_node ];
	if( node->first_child != U8_MAX )
		MergePosesRecursive( lower, upper, model, node->first_child );
}

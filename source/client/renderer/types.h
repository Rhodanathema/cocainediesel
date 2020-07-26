#pragma once

#include "qcommon/types.h"

enum BlendFunc : u8 {
	BlendFunc_Disabled,
	BlendFunc_Blend,
	BlendFunc_Add,
};

enum PrimitiveType : u8 {
	PrimitiveType_Triangles,
	PrimitiveType_TriangleStrip,
	PrimitiveType_Points,
	PrimitiveType_Lines,
};

enum IndexFormat : u8 {
	IndexFormat_U16,
	IndexFormat_U32,
};

struct Shader {
	u32 program;
	u64 uniforms[ 8 ];
	u64 textures[ 4 ];
	u64 texture_buffers[ 4 ];
	u64 texture_array;
};

struct VertexBuffer {
	u32 vbo;
};

struct IndexBuffer {
	u32 ebo;
};

struct TextureBuffer {
	u32 tbo;
	u32 texture;
};

struct UniformBlock {
	u32 ubo;
	u32 offset;
	u32 size;
};

struct Mesh {
	u32 num_vertices;
	PrimitiveType primitive_type;
	u32 vao;
	VertexBuffer positions;
	VertexBuffer normals;
	VertexBuffer tex_coords;
	VertexBuffer colors;
	VertexBuffer joints;
	VertexBuffer weights;
	IndexBuffer indices;
	IndexFormat indices_format;
	bool ccw_winding;
};

struct GPUParticle {
	Vec3 position;
	float scale;
	float t;
	RGBA8 color;
};

struct Font;
struct Material;
struct Model;
struct PipelineState;

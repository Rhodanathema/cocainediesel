#include "include/uniforms.glsl"
#include "include/common.glsl"
#include "include/skinning.glsl"

layout( std140 ) uniform u_Silhouette {
	vec4 u_SilhouetteColor;
};

#if VERTEX_SHADER

in vec4 a_Position;

#if INSTANCED
struct Instance {
	mat3x4 transform;
	vec4 color;
};

layout( std430 ) readonly buffer b_Instances {
	Instance instances[];
};
#endif

void main() {
#if INSTANCED
	mat4 u_M = AffineToMat4( instances[ gl_InstanceID ].transform );
#endif
	vec4 Position = a_Position;
	vec3 NormalDontCare = vec3( 0 );

#if SKINNED
	Skin( Position, NormalDontCare );
#endif

	gl_Position = u_P * u_V * u_M * Position;
}

#else

out vec4 f_Albedo;

void main() {
#if INSTANCED
	f_Albedo = instances[ gl_InstanceID ].color;
#else
	f_Albedo = u_SilhouetteColor;
#endif
}

#endif

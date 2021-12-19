struct PSInput {
	float4 position : POSITION;
	float2 uv : TEXCOORD0;
};

cbuffer View : register( b0 ) {
	float4x4 u_V;
	float4x4 u_InverseV;
	float4x4 u_P;
	float4x4 u_InverseP;
	float3 u_CameraPos;
	float2 u_ViewportSize;
	float u_NearClip;
	int u_Samples;
	float3 u_LightDir;
};

cbuffer Text : register( b1 ) {
	float4 text_color;
	float4 border_color;
	float2 atlas_size;
	float dSDFdTexel;
	bool has_border;
};

Texture2D atlas : register( t0 );
SamplerState ss : register( s0 );

float sRGBToLinear( float srgb ) {
	if( srgb <= 0.04045f )
		return srgb * ( 1.0f / 12.92f );
	return pow( ( srgb + 0.055f ) * ( 1.0f / 1.055f ), 2.4f );
}

float3 sRGBToLinear( float3 srgb ) {
	return float3( sRGBToLinear( srgb.r ), sRGBToLinear( srgb.g ), sRGBToLinear( srgb.b ) );
}

float4 sRGBToLinear( float4 srgb ) {
	return float4( sRGBToLinear( srgb.r ), sRGBToLinear( srgb.g ), sRGBToLinear( srgb.b ), srgb.a );
}

float LinearTosRGB( float lin ) {
	if( lin <= 0.0031308f )
		return lin * 12.92f;
	return 1.055f * pow( lin, 1.0 / 2.4 ) - 0.055f;
}

float3 LinearTosRGB( float3 lin ) {
	return float3( LinearTosRGB( lin.r ), LinearTosRGB( lin.g ), LinearTosRGB( lin.b ) );
}

float4 LinearTosRGB( float4 lin ) {
	return float4( LinearTosRGB( lin.r ), LinearTosRGB( lin.g ), LinearTosRGB( lin.b ), lin.a );
}


PSInput VSMain( float3 position : POSITION, float2 uv : TEXCOORD0 ) {
	PSInput result;

	result.position = mul( u_V, float4( position, 1.0f ) );
	result.uv = uv;

	return result;
}

float Median( float3 v ) {
	return max( min( v.x, v.y ), min( max( v.x, v.y ), v.z ) );
}

float LinearStep( float lo, float hi, float x ) {
	return ( clamp( x, lo, hi ) - lo ) / ( hi - lo );
}

float4 SampleMSDF( float2 uv, float half_pixel_size ) {
	float3 sample = atlas.Sample( ss, uv ).rgb;
	float d = 2.0f * Median( sample ) - 1.0f; // rescale to [-1,1], positive being inside

	if( has_border != 0 ) {
		float border_amount = LinearStep( -half_pixel_size, half_pixel_size, d );
		float4 color = lerp( border_color, text_color, border_amount );

		float alpha = LinearStep( -3.0f * half_pixel_size, -half_pixel_size, d );
		return float4( color.rgb, color.a * alpha );
	}

	float alpha = LinearStep( -half_pixel_size, half_pixel_size, d );
	return float4( text_color.rgb, text_color.a * alpha );
}

float4 PSMain( PSInput input ) : SV_TARGET {
	float2 fw = fwidth( input.uv );
	float2 atlas_dims;
	atlas.GetDimensions( atlas_dims.x, atlas_dims.y );
	float half_pixel_size = 0.5f * dSDFdTexel * dot( fw, atlas_dims );

	float supersample_offset = 0.354f; // rsqrt( 2 ) / 2
	float2 ssx = float2( supersample_offset * fw.x, 0.0f );
	float2 ssy = float2( 0.0f, supersample_offset * fw.y );

	float4 color = SampleMSDF( input.uv, half_pixel_size );
	color += 0.5f * SampleMSDF( input.uv - ssx, half_pixel_size );
	color += 0.5f * SampleMSDF( input.uv + ssx, half_pixel_size );
	color += 0.5f * SampleMSDF( input.uv - ssy, half_pixel_size );
	color += 0.5f * SampleMSDF( input.uv + ssy, half_pixel_size );

	return LinearTosRGB( color * ( 1.0f / 3.0f ) );
}

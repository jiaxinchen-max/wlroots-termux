#version 450

layout (input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput in_color;

// Matches enum wlr_vk_output_transform
#define OUTPUT_TRANSFORM_INVERSE_SRGB 0
#define OUTPUT_TRANSFORM_LUT_3D 1

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 out_color;

#if OUTPUT_TRANSFORM == OUTPUT_TRANSFORM_LUT_3D
	layout(set = 1, binding = 0) uniform sampler3D lut_3d;

	/* struct wlr_vk_frag_output_pcr_data */
	layout(push_constant) uniform UBO {
		layout(offset = 80) float lut_3d_offset;
		float lut_3d_scale;
	} data;
#endif

float linear_channel_to_srgb(float x) {
	return max(min(x * 12.92, 0.04045), 1.055 * pow(x, 1. / 2.4) - 0.055);
}

void main() {
	vec4 val = subpassLoad(in_color).rgba;
	if (val.a == 0) {
		out_color = vec4(0);
		return;
	}

	// Convert from pre-multiplied alpha to straight alpha
	vec3 rgb = val.rgb / val.a;

#if OUTPUT_TRANSFORM == OUTPUT_TRANSFORM_LUT_3D
	// Apply 3D LUT
	vec3 pos = data.lut_3d_offset + rgb * data.lut_3d_scale;
	rgb = texture(lut_3d, pos).rgb;
#endif

#if OUTPUT_TRANSFORM == OUTPUT_TRANSFORM_INVERSE_SRGB
	rgb = vec3(
		linear_channel_to_srgb(rgb.r),
		linear_channel_to_srgb(rgb.g),
		linear_channel_to_srgb(rgb.b)
	);
#endif

	// Back to pre-multiplied alpha
	out_color = vec4(rgb * val.a, val.a);
}

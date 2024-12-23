vulkan_shaders_src = [
	'common.vert',
	'texture.frag',
	'quad.frag',
]

vulkan_shaders = []
foreach shader : vulkan_shaders_src
	name = shader.underscorify() + '_data'
	args = [glslang, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name]
	if glslang_version.version_compare('>=11.0.0')
		args += '--quiet'
	endif
	header = custom_target(
		shader + '_spv',
		output: shader + '.h',
		input: shader,
		command: args)

	vulkan_shaders += [header]
endforeach

vulkan_shader_output_color_transforms = {
	'inverse_rgb': '0',
	'lut_3d': '1',
}

foreach name, ident : vulkan_shader_output_color_transforms
	name = 'output_' + name
	args = [glslang, '-DOUTPUT_TRANSFORM=' + ident, '-V', '@INPUT@', '-o', '@OUTPUT@', '--vn', name + '_data']
	if glslang_version.version_compare('>=11.0.0')
		args += '--quiet'
	endif
	header = custom_target(
		name + '.frag_spv',
		output: name + '.frag.h',
		input: 'output.frag',
		command: args)

	vulkan_shaders += [header]
endforeach

wlr_files += vulkan_shaders

termuxdc = cc.find_library('termux-display-client')

if not termuxdc.found()
	subdir_done()
endif

wlr_files += files(
	'backend.c',
	'output.c',
	'input.c',
	'allocator.c',
)

wlr_deps += termuxdc
features += { 'termuxdc-backend': true }

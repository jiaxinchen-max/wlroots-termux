termuxdc = cc.find_library('termux-display-client')

if not termuxdc-display-client.found()
	subdir_done()
endif

wlr_files += files(
	'backend.c',
	'output.c',
	'input.c',
	'allocator.c',
)

wlr_deps += termux-termuxdc-client
features += { 'termuxdc-backend': true }

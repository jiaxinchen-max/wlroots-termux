termux-display-client = cc.find_library('termux-display-client')

if not termux-display-client.found()
	subdir_done()
endif

wlr_files += files(
	'backend.c',
	'output.c',
	'input.c',
	'allocator.c',
)

wlr_deps += termux-display-client
features += { 'termux-display-client-backend': true }

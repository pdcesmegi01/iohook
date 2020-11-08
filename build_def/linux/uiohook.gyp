{
	"targets": [{
		"target_name": "uiohook",
		"type": "shared_library",
		"sources": [
			"libuiohook/include/uiohook.h",
			"libuiohook/src/logger.c",
			"libuiohook/src/logger.h",
			"libuiohook/src/x11/input_helper.h",
			"libuiohook/src/x11/input_helper.c",
			"libuiohook/src/x11/input_hook.c",
			"libuiohook/src/x11/post_event.c",
			"libuiohook/src/x11/system_properties.c"
		],
		"cflags": [
			"-std=c99",
			"-fPIC"
		],
		"link_settings": {
				"libraries": [
						"-Wl,-rpath,<!(node -e \"console.log('builds/' + (process.versions['electron'] ? 'electron' : 'node') + '-v' + process.versions.modules + '-' + process.platform + '-' + process.arch + '/build/Release')\")",
						"-Wl,-rpath,<!(pwd)/build/Release/",
						"-lX11"
				]
		},
		"defines": [
			"USE_XKBCOMMON"
		],
		"include_dirs": [
			"<!(node -e \"require('nan')\")",
			'libuiohook/include',
			'libuiohook/src'
		]
	}]
}

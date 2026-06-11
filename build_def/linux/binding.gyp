{
	"targets": [{
		"target_name": "iohook",
		"win_delay_load_hook": "true",
		"type": "loadable_module",
		"sources": [
			"src/iohook.cc",
			"src/iohook.h"
		],
		"dependencies": [
			"<!(node -p \"require('node-addon-api').gyp\")",
			"./uiohook.gyp:uiohook"
		],
		"cflags_cc": [
			"-std=c++17",
			"-fPIC"
		],
		"defines": [
			"USE_XKBCOMMON",
			"NAPI_VERSION=9"
		],
		"link_settings": {
				"libraries": [
						"-Wl,-rpath,<!(node -e \"console.log('builds/' + process.env.gyp_iohook_runtime + '-v' + process.env.gyp_iohook_abi + '-' + process.env.gyp_iohook_platform + '-' + process.env.gyp_iohook_arch + '/build/Release')\")",
						"-Wl,-rpath,<!(pwd)/build/Release/"
				]
		},
		"include_dirs": [
			"<!@(node -p \"require('node-addon-api').include\")",
			"libuiohook/include"
		],
		"configurations": {
			"Release": {
			}
		}
	}]
}

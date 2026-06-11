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
			"-std=c++17"
		],
		"defines": [
			"NAPI_VERSION=9"
		],
		"link_settings": {
				"libraries": [
						"-Wl,-rpath,@executable_path/.",
						"-Wl,-rpath,@loader_path/.",
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

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
		"include_dirs": [
			"<!@(node -p \"require('node-addon-api').include\")",
			"libuiohook/include"
		],
		"defines": [
			"NAPI_VERSION=9"
		],
		"configurations": {
			"Release": {
				"msvs_settings": {
					"VCCLCompilerTool": {
						'ExceptionHandling': 1,
						"AdditionalOptions": [
							"/std:c++17"
						]
					}
				}
			}
		}
	}]
}

// SPDX-License-Identifier: Apache-2.0
CONTAINER Ocyclesproxy
{
	NAME Ocyclesproxy;
	INCLUDE Obase;

	// One tab: the file, what a Blender read from it, what of it is shown. The menu's entries are the
	// file's scenes and collections (objects/proxy_object.cpp, GetDDescription). Kept plain: the layout
	// is to be redesigned (ids never change with it).
	GROUP CYCLESPROXY_GROUP_PROXY
	{
		DEFAULT 1;

		FILENAME CYCLESPROXY_FILE { ANIM OFF; }
		GROUP
		{
			COLUMNS 2;

			BUTTON CYCLESPROXY_MAKE_RELATIVE { }
			BUTTON CYCLESPROXY_MAKE_ABSOLUTE { }
		}
		SEPARATOR { LINE; }
		BUTTON CYCLESPROXY_READ_FILE { }
		STATICTEXT CYCLESPROXY_STATUS { ANIM OFF; }
		LONG CYCLESPROXY_TAKE { ANIM OFF; CYCLE { CYCLESPROXY_TAKE_FILE_SCENE; } }
		SEPARATOR { LINE; }
		BOOL CYCLESPROXY_LIGHTS { ANIM OFF; }
		LONG CYCLESPROXY_FRAME_OFFSET { STEP 1; }
	}
}

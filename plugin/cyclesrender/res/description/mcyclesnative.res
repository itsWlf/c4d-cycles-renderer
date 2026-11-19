// SPDX-License-Identifier: Apache-2.0
CONTAINER Mcyclesnative
{
	NAME Mcyclesnative;
	INCLUDE Mpreview;
	INCLUDE Mbase;

	GROUP CYCLESNATIVE_GROUP
	{
		DEFAULT 1;

		BUTTON CYCLESNATIVE_OPEN { }
		STATICTEXT CYCLESNATIVE_INFO { }
	}
}

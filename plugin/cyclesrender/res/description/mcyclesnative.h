#ifndef MCYCLESNATIVE_H__
#define MCYCLESNATIVE_H__

// Parameter IDs of the Cycles Native Material. The material itself (a Blender material as bytes)
// is not a parameter: it is read and written with the scene by materials/native_material.cpp.
enum
{
	CYCLESNATIVE_GROUP = 1000,
	CYCLESNATIVE_OPEN = 1001,   // button: open Blender's Shader Editor
	CYCLESNATIVE_INFO = 1002,   // a line saying what is stored

	CYCLESNATIVE_END_
};

#endif // MCYCLESNATIVE_H__

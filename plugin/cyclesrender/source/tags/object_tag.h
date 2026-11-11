// SPDX-License-Identifier: Apache-2.0
// The "Cycles Object" tag: settings of one object that Cinema 4D has no place for.

#ifndef CYCLESRENDER_OBJECT_TAG_H__
#define CYCLESRENDER_OBJECT_TAG_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the tag with Cinema 4D. Called once from PluginStart(). The exporters read the
/// tag's parameters straight from its container (ids in res/description/tcyclesobject.h),
/// with the same defaults as Init() sets here.
cinema::Bool RegisterObjectTag();

/// The Cycles Object tag that counts for @p op in one respect: the nearest one, on the object
/// or on a parent, whose tab is enabled.
/// @param[in] enableId  The tab's "enable" checkbox, e.g. CYCLESOBJECT_VOLUME_ENABLE.
const cinema::BaseTag* FindObjectTag(const cinema::BaseObject* op, maxon::Int32 enableId);

} // namespace cyclesrender

#endif // CYCLESRENDER_OBJECT_TAG_H__

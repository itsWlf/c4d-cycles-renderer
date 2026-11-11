// SPDX-License-Identifier: Apache-2.0
// The "Cycles Render" page in Cinema 4D's Preferences.

#ifndef CYCLESRENDER_PREFS_PAGE_H__
#define CYCLESRENDER_PREFS_PAGE_H__

#include "c4d.h"

namespace cyclesrender
{

/// Registers the Preferences page with Cinema 4D. Called once from PluginStart().
cinema::Bool RegisterPreferencesPage();

} // namespace cyclesrender

#endif // CYCLESRENDER_PREFS_PAGE_H__

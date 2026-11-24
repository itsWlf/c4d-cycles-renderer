// SPDX-License-Identifier: Apache-2.0
// The Imager tab of the Cycles Camera tag (imager_tab.h). Reading order: the description (the
// menus, a layer's settings), then what is greyed out, then the menu and the buttons.

#include "c4d.h"

#include "tcyclescamera.h"

#include "../sdk_compat.h"
#include "imager_effects.h"
#include "imager_tab.h"

#if CYCLESRENDER_ITEM_TREE
#include "customgui_itemtree.h"
#include "customgui_itemtree_utils.h"
#include "customgui_listview.h"
#endif

namespace cyclesrender
{

// Blender's limits beyond this are none (tools/make_effects.py, NO_LIMIT).
static const maxon::Float64 NO_LIMIT = 1.0e29;
// The widest range a number is shown as a slider over (as tools/make_nodes.py's SLIDER_SPAN): wider ones keep a field.
static const maxon::Float64 IMAGER_SLIDER_SPAN = 100.0;

// The first Blender the table knows: an effect, setting or option that came later says so in its name.
static maxon::Int32 OldestBlender()
{
	maxon::Int32 oldest = maxon::LIMIT<maxon::Int32>::MAX;
	for (const ImagerEffect& effect : GetImagerEffects())
		oldest = maxon::Min(oldest, effect.since);
	return oldest;
}

// " (Blender 5.3)" for what Blender has from version @p since on; nothing for what every supported Blender has.
static maxon::String SinceNote(maxon::Int32 since)
{
	if (since <= OldestBlender())
		return maxon::String();
	return FormatString(" (Blender @.@)", since / 100, since % 100);
}

static cinema::DescID ParameterId(maxon::Int32 id, maxon::Int32 type)
{
	return CreateDescID(cinema::DescLevel(id, type, 0));
}

void InitImagerTab(cinema::BaseContainer& data)
{
	data.SetBool(CYCLESCAMERA_IMAGER_ENABLE, true);
	data.SetContainer(CYCLESCAMERA_IMAGER_LAYERS, cinema::BaseContainer());
	data.SetInt32(CYCLESCAMERA_IMAGER_SELECTED, 0);
	data.SetInt32(CYCLESCAMERA_IMAGER_ADD, 0);
	data.SetInt32(CYCLESCAMERA_IMAGER_NEXT, 1);
}

#if CYCLESRENDER_ITEM_TREE

// The list's column beside the name: a layer's opacity as a slider, 0 to 1, shown in percent. (A
// layer's switch is the list's own checkbox, in front of the name.)
static const maxon::Int32 OPACITY_COLUMN = ITEMTREE_USER_COL;

// What a list entry of a layer is: no link to an object, dropped before or after another.
static const maxon::UInt32 ENTRY_FLAGS = ITEMTREE_FLAG_NOLINK | ITEMTREE_FLAG_INSERT_BEFORE | ITEMTREE_FLAG_INSERT_AFTER;

// The list the Attribute Manager shows, kept in the tag's container; made when it is not there.
static cinema::ItemTreeData* ListOf(cinema::BaseList2D* tag)
{
	cinema::BaseContainer& data = tag->GetDataInstanceRef();
	cinema::ItemTreeData* tree = data.GetCustomDataTypeWritableObsolete<cinema::ItemTreeData>(CYCLESCAMERA_IMAGER_LIST);
	if (!tree)
	{
		cinema::GeData made(CUSTOMDATA_ITEMTREE, cinema::DEFAULTVALUE);
		data.SetData(CYCLESCAMERA_IMAGER_LIST, made);
		tree = data.GetCustomDataTypeWritableObsolete<cinema::ItemTreeData>(CYCLESCAMERA_IMAGER_LIST);
	}
	if (tree)
		tree->SetOwner(tag);
	return tree;
}

static maxon::Float ListOpacity(const cinema::ItemTreeNodeData& entry)
{
	const cinema::BaseContainer* const values = entry.m_Data.GetContainer();
	return values ? values->GetFloat(OPACITY_COLUMN, 1.0) : 1.0;
}

// The entry of a layer among the list's entries, or nullptr.
static cinema::ItemTreeNode* EntryOf(cinema::ItemTreeData* tree, maxon::Int32 layer)
{
	for (cinema::ItemTreeNode* node = tree->GetFirst(); node; node = tree->GetNext(node))
	{
		const cinema::ItemTreeNodeData* const entry = tree->GetNodeData(node);
		if (entry && entry->m_ID == layer)
			return node;
	}
	return nullptr;
}

// The layer of the list's selected entry, or 0.
static maxon::Int32 SelectedEntry(cinema::ItemTreeData* tree)
{
	const cinema::ItemTreeNodeData* const entry = tree->GetNodeData(tree->GetFirstSelected());
	return entry ? entry->m_ID : 0;
}

// The layers into the list: the list is a view, the layers are what counts. An entry stays the entry
// it is (the Attribute Manager knows an entry by a number the list gave it: a list made anew loses
// the click that is under way, measured 2026-11-24); only what differs is set, moved, added or freed.
// @param[in] selection  True: the list's selected entry becomes the selected layer's. False (while
//                       the tab is only described): only a list with no entry selected gets one. A
//                       click selects in the list first and tells the tag afterwards
//                       (HandleImagerList): between the two the list is ahead and must stay so.
static void PushImagerList(cinema::BaseList2D* tag, maxon::Bool selection)
{
	cinema::ItemTreeData* const tree = ListOf(tag);
	if (!tree)
		return;
	const cinema::BaseContainer& data = tag->GetDataInstanceRef();
	maxon::BaseArray<maxon::Int32> layers, kept;
	iferr (GetImagerLayers(data, layers))
		return;
	const maxon::Int32 selected = GetSelectedImagerLayer(data);

	// Entries that are no layer, or the second entry of one, go.
	for (cinema::ItemTreeNode* node = tree->GetFirst(); node;)
	{
		cinema::ItemTreeNode* const next = tree->GetNext(node);
		const cinema::ItemTreeNodeData* const entry = tree->GetNodeData(node);
		const maxon::Bool keep = entry && layers.Contains(entry->m_ID) && !kept.Contains(entry->m_ID);
		if (keep)
		{
			iferr (kept.Append(entry->m_ID))
				return;
		}
		if (!keep)
			tree->FreeNode(node);
		node = next;
	}

	cinema::ItemTreeNode* previous = nullptr;
	for (const maxon::Int32 layer : layers)
	{
		const maxon::String name = data.GetString(ImagerLayerId(layer, IMAGERLAYER_NAME));
		const maxon::Float opacity = data.GetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), 1.0);
		const maxon::Bool enabled = data.GetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), true);
		cinema::ItemTreeNode* node = EntryOf(tree, layer);
		if (!node)
		{
			cinema::BaseContainer values;
			values.SetFloat(OPACITY_COLUMN, opacity);
			node = tree->AddNode(layer, name, nullptr, cinema::GeData(values), ENTRY_FLAGS | (enabled ? ITEMTREE_FLAG_ENABLE : 0), nullptr, previous, false);
			if (!node)
				return;
		}
		else if (tree->GetPrev(node) != previous)
		{
			tree->Remove(node);
			tree->Insert(node, nullptr, previous, false);
		}
		cinema::ItemTreeNodeData* const entry = tree->GetNodeData(node);
		if (entry)
		{
			if (entry->m_Text != name)
				entry->m_Text = name;
			if (entry->IsEnabled() != enabled)
				entry->SetEnabled(enabled);
			if (maxon::Abs(ListOpacity(*entry) - opacity) > 1e-6)
			{
				cinema::BaseContainer values;
				values.SetFloat(OPACITY_COLUMN, opacity);
				entry->m_Data = cinema::GeData(values);
			}
		}
		previous = node;
	}
	if (SelectedEntry(tree) != selected && (selection || SelectedEntry(tree) == 0))
		tree->SetSelected(selected != 0 ? EntryOf(tree, selected) : nullptr);
}

// What a pull takes from the list. Only what the user's action was about: the list the Attribute
// Manager holds can be behind the layers in everything else.
enum class PULL
{
	NONE = 0,
	SWITCHES = 1 << 0,
	ORDER = 1 << 1
} MAXON_ENUM_FLAGS(PULL);

// What the user did in the list, into the layers: the switches, and the order (when the list holds
// exactly the tag's layers), as far as @p what asks. The selected entry, a typed name, a moved
// opacity slider and a removal each arrive as a message of their own (HandleImagerList): the list
// does not hold them yet when it is written or when its message comes (measured 2026-11-24).
// @return  True when a layer changed.
static maxon::Bool PullImagerList(cinema::BaseList2D* tag, cinema::ItemTreeData* tree, PULL what)
{
	cinema::BaseContainer& data = tag->GetDataInstanceRef();
	maxon::BaseArray<maxon::Int32> layers, listed;
	iferr (GetImagerLayers(data, layers))
		return false;
	maxon::Bool changed = false;
	for (cinema::ItemTreeNode* node = tree->GetFirst(); node; node = tree->GetNext(node))
	{
		const cinema::ItemTreeNodeData* const entry = tree->GetNodeData(node);
		if (!entry || !layers.Contains(entry->m_ID) || listed.Contains(entry->m_ID))
			continue;
		const maxon::Int32 layer = entry->m_ID;
		iferr (listed.Append(layer))
			return changed;
		if ((what & PULL::SWITCHES) && entry->IsEnabled() != data.GetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), true))
		{
			data.SetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), entry->IsEnabled());
			changed = true;
		}
	}
	if ((what & PULL::ORDER) && listed.GetCount() == layers.GetCount() && !listed.IsEqual(layers))
	{
		SetImagerLayers(data, listed);
		changed = true;
	}
	return changed;
}

// The list's selected entry becomes the selected layer, when it is another one: the settings below the
// list follow. A list that is behind (written before a click has selected) has the entry of the
// selected layer selected, and changes nothing here.
static void TakeSelection(cinema::BaseList2D* tag, cinema::ItemTreeData* tree, maxon::Int32 clicked = 0)
{
	cinema::BaseContainer& data = tag->GetDataInstanceRef();
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return;
	const maxon::Int32 selected = clicked != 0 ? clicked : SelectedEntry(tree);
	if (!layers.Contains(selected) || selected == data.GetInt32(CYCLESCAMERA_IMAGER_SELECTED, 0))
		return;
	data.SetInt32(CYCLESCAMERA_IMAGER_SELECTED, selected);
	tag->SetDirty(cinema::DIRTYFLAGS::DESCRIPTION);
	cinema::EventAdd();
}

// The settings of the list as the Attribute Manager shows it: a plain list with a checkbox in front
// of each entry (the layer's switch), entries dragged into another order, and a column with the
// opacity as a slider.
static cinema::BaseContainer ListDescription()
{
	cinema::BaseContainer bc = cinema::GetCustomDataTypeDefault(CUSTOMDATA_ITEMTREE);
	bc.SetString(cinema::DESC_NAME, "Effects"_s);
	bc.SetString(cinema::DESC_SHORT_NAME, "Effects"_s);
	bc.SetInt32(cinema::DESC_CUSTOMGUI, CUSTOMGUI_ITEMTREE);
	bc.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
	bc.SetBool(cinema::DESC_SCALEH, true);
	bc.SetInt32(cinema::DESC_COLUMNS, 3);
	bc.SetBool(ITEMTREE_ID_LIST_FLAG, true);
	bc.SetBool(ITEMTREE_ID_DRAG_FLAG, true);
	bc.SetBool(ITEMTREE_ID_HEADERS_FLAG, true);
	bc.SetBool(ITEMTREE_ID_CHECKBOX_FLAG, true);
	bc.SetBool(ITEMTREE_CHECKBOX_FRONT, true);
	bc.SetBool(ITEMTREE_ID_NO_SELECT_IN_OM_FLAG, true);
	bc.SetBool(ITEMTREE_ID_NO_COPYPASTEREPLACE_FLAG, true);
	bc.SetBool(ITEMTREE_ID_ALTERNATE_BG, true);
	bc.SetInt32(ITEMTREE_ID_ROWS, 5);
	cinema::BaseContainer types, active, headers, minimum, maximum, formats;
	types.SetInt32(OPACITY_COLUMN, LV_SLIDER);
	active.SetBool(OPACITY_COLUMN, true);
	headers.SetString(ITEMTREE_OBJECT_COL, "Effect"_s);
	headers.SetString(OPACITY_COLUMN, "Opacity"_s);
	bc.SetContainer(ITEMTREE_USER_COL_TYPES, types);
	bc.SetContainer(ITEMTREE_USER_COL_ACTIVE, active);
	bc.SetContainer(ITEMTREE_ID_HEADERS_DATA, headers);
	minimum.SetFloat(OPACITY_COLUMN, 0.0);
	maximum.SetFloat(OPACITY_COLUMN, 1.0);
	formats.SetInt32(OPACITY_COLUMN, cinema::FORMAT_PERCENT);
	bc.SetContainer(ITEMTREE_USER_COL_MIN, minimum);
	bc.SetContainer(ITEMTREE_USER_COL_MAX, maximum);
	bc.SetContainer(ITEMTREE_USER_COL_FORMAT, formats);
	return bc;
}

#endif // CYCLESRENDER_ITEM_TREE

// One line of the layer menu: the layer's name, and whether it is switched off.
static maxon::String LayerLine(const cinema::BaseContainer& data, maxon::Int32 layer)
{
	maxon::String line = data.GetString(ImagerLayerId(layer, IMAGERLAYER_NAME));
	if (line.IsEmpty())
		line = data.GetString(ImagerLayerId(layer, IMAGERLAYER_EFFECT));
	if (!data.GetBool(ImagerLayerId(layer, IMAGERLAYER_ENABLED), true))
		line += " (off)"_s;
	return line;
}

// The description of one setting of an effect, as Blender shows it.
static cinema::BaseContainer SettingDescription(const ImagerSetting& setting)
{
	maxon::Int32 type = cinema::DTYPE_REAL;
	switch (setting.kind)
	{
		case 'i': case 'm': type = cinema::DTYPE_LONG; break;
		case 'b': type = cinema::DTYPE_BOOL; break;
		case 'c': type = cinema::DTYPE_COLOR; break;
		case 'v': type = cinema::DTYPE_VECTOR; break;
		default: break;
	}
	cinema::BaseContainer bc = cinema::GetCustomDataTypeDefault(type);
	const maxon::String label = maxon::String(setting.label) + SinceNote(setting.since);
	bc.SetString(cinema::DESC_NAME, label);
	bc.SetString(cinema::DESC_SHORT_NAME, label);
	bc.SetData(cinema::DESC_DEFAULT, GetImagerDefault(setting));
	const maxon::Bool hasMinimum = setting.minimum > -NO_LIMIT, hasMaximum = setting.maximum < NO_LIMIT;
	if (setting.kind == 'f' || setting.kind == 'v')
	{
		if (hasMinimum)
			bc.SetFloat(cinema::DESC_MIN, setting.minimum);
		if (hasMaximum)
			bc.SetFloat(cinema::DESC_MAX, setting.maximum);
		bc.SetInt32(cinema::DESC_UNIT, setting.subtype == 'A' ? cinema::DESC_UNIT_DEGREE : cinema::DESC_UNIT_FLOAT);
		bc.SetFloat(cinema::DESC_STEP, setting.subtype == 'A' ? 0.017453292519943295 : (setting.subtype == 'F' ? 0.01 : 0.1));
		// A slider between Blender's limits wherever they make a usable range: a factor, or any number bounded on
		// both sides within IMAGER_SLIDER_SPAN (0.3.0, F11 step 2, the rule of tools/make_nodes.py, number_plan).
		if (setting.kind == 'f' && hasMinimum && hasMaximum && (setting.subtype == 'F' || setting.maximum - setting.minimum <= IMAGER_SLIDER_SPAN))
		{
			bc.SetInt32(cinema::DESC_CUSTOMGUI, CUSTOMGUI_REALSLIDER);
			bc.SetFloat(cinema::DESC_MINSLIDER, setting.minimum);
			bc.SetFloat(cinema::DESC_MAXSLIDER, setting.maximum);
		}
	}
	else if (setting.kind == 'i')
	{
		if (hasMinimum)
			bc.SetInt32(cinema::DESC_MIN, maxon::Int32(setting.minimum));
		if (hasMaximum)
			bc.SetInt32(cinema::DESC_MAX, maxon::Int32(setting.maximum));
		bc.SetInt32(cinema::DESC_STEP, 1);
		if (hasMinimum && hasMaximum && setting.maximum - setting.minimum <= IMAGER_SLIDER_SPAN)
		{
			bc.SetInt32(cinema::DESC_CUSTOMGUI, CUSTOMGUI_LONGSLIDER);
			bc.SetInt32(cinema::DESC_MINSLIDER, maxon::Int32(setting.minimum));
			bc.SetInt32(cinema::DESC_MAXSLIDER, maxon::Int32(setting.maximum));
		}
	}
	else if (setting.kind == 'm')
	{
		cinema::BaseContainer cycle;
		const maxon::Int32 count = GetImagerOptionCount(setting);
		for (maxon::Int32 i = 0; i < count; ++i)
			cycle.SetString(GetImagerOptionValue(setting, i), GetImagerOption(setting, i) + SinceNote(GetImagerOptionSince(setting, i)));
		bc.SetContainer(cinema::DESC_CYCLE, cycle);
		bc.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
	}
	return bc;
}

// Adds the group of one layer's settings below the tab; hidden unless the layer is the selected one.
static maxon::Bool AddLayerDescription(const cinema::BaseContainer& data, cinema::Description* description, maxon::Int32 layer, maxon::Bool shown)
{
	const cinema::DescID tab = ParameterId(CYCLESCAMERA_GROUP_IMAGER, cinema::DTYPE_GROUP);
	const cinema::DescID group = ParameterId(ImagerLayerId(layer, IMAGERLAYER_GROUP), cinema::DTYPE_GROUP);
	const ImagerEffect* const effect = GetImagerLayerEffect(data, layer);
	const maxon::String effectName = data.GetString(ImagerLayerId(layer, IMAGERLAYER_EFFECT));

	cinema::BaseContainer bc = cinema::GetCustomDataTypeDefault(cinema::DTYPE_GROUP);
	// The group is named by the effect, as Blender's node is; the layer's own name is the field below.
	bc.SetString(cinema::DESC_NAME, effect ? effectName + SinceNote(effect->since) : effectName + " (unknown to this version of the plugin)"_s);
	bc.SetInt32(cinema::DESC_COLUMNS, 1);
	bc.SetBool(cinema::DESC_DEFAULT, true);
	bc.SetBool(cinema::DESC_HIDE, !shown);
	if (!description->SetParameter(group, bc, tab))
		return false;

	bc = cinema::GetCustomDataTypeDefault(cinema::DTYPE_STRING);
	bc.SetString(cinema::DESC_NAME, "Name"_s);
	bc.SetString(cinema::DESC_SHORT_NAME, "Name"_s);
	bc.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
	if (!description->SetParameter(ParameterId(ImagerLayerId(layer, IMAGERLAYER_NAME), cinema::DTYPE_STRING), bc, group))
		return false;

	bc = cinema::GetCustomDataTypeDefault(cinema::DTYPE_BOOL);
	bc.SetString(cinema::DESC_NAME, "Enabled"_s);
	bc.SetString(cinema::DESC_SHORT_NAME, "Enabled"_s);
	bc.SetBool(cinema::DESC_DEFAULT, true);
	if (!description->SetParameter(ParameterId(ImagerLayerId(layer, IMAGERLAYER_ENABLED), cinema::DTYPE_BOOL), bc, group))
		return false;

	bc = cinema::GetCustomDataTypeDefault(cinema::DTYPE_REAL);
	bc.SetString(cinema::DESC_NAME, "Opacity"_s);
	bc.SetString(cinema::DESC_SHORT_NAME, "Opacity"_s);
	bc.SetInt32(cinema::DESC_UNIT, cinema::DESC_UNIT_PERCENT);
	bc.SetFloat(cinema::DESC_MIN, 0.0);
	bc.SetFloat(cinema::DESC_MAX, 1.0);
	bc.SetFloat(cinema::DESC_MINSLIDER, 0.0);
	bc.SetFloat(cinema::DESC_MAXSLIDER, 1.0);
	bc.SetFloat(cinema::DESC_STEP, 0.01);
	bc.SetFloat(cinema::DESC_DEFAULT, 1.0);
	bc.SetInt32(cinema::DESC_CUSTOMGUI, CUSTOMGUI_REALSLIDER);
	if (!description->SetParameter(ParameterId(ImagerLayerId(layer, IMAGERLAYER_OPACITY), cinema::DTYPE_REAL), bc, group))
		return false;
	if (!effect)
		return true;

	// The effect's settings in Blender's order; a panel of Blender's is a group, made when its first setting comes.
	const maxon::Char* panels[100];
	maxon::Int32 panelCount = 0;
	for (maxon::Int32 i = 0; i < effect->settingCount; ++i)
	{
		const ImagerSetting& setting = effect->settings[i];
		cinema::DescID parent = group;
		if (setting.panel[0])
		{
			maxon::Int32 panel = 0;
			while (panel < panelCount && maxon::String(panels[panel]) != maxon::String(setting.panel))
				++panel;
			if (panel >= 100)
				continue;
			parent = ParameterId(ImagerLayerId(layer, IMAGERLAYER_PANELS + panel), cinema::DTYPE_GROUP);
			if (panel == panelCount)
			{
				panels[panelCount++] = setting.panel;
				cinema::BaseContainer panelGroup = cinema::GetCustomDataTypeDefault(cinema::DTYPE_GROUP);
				panelGroup.SetString(cinema::DESC_NAME, maxon::String(setting.panel));
				panelGroup.SetInt32(cinema::DESC_COLUMNS, 1);
				panelGroup.SetBool(cinema::DESC_DEFAULT, true);
				if (!description->SetParameter(parent, panelGroup, group))
					return false;
			}
		}
		const cinema::BaseContainer settingDescription = SettingDescription(setting);
		const maxon::Int32 type = setting.kind == 'f' ? cinema::DTYPE_REAL : (setting.kind == 'b' ? cinema::DTYPE_BOOL : (setting.kind == 'c' ? cinema::DTYPE_COLOR :
			(setting.kind == 'v' ? cinema::DTYPE_VECTOR : cinema::DTYPE_LONG)));
		if (!description->SetParameter(ParameterId(ImagerLayerId(layer, setting.number), type), settingDescription, parent))
			return false;
	}
	return true;
}

maxon::Bool BuildImagerDescription(const cinema::BaseList2D* tag, cinema::Description* description)
{
#if CYCLESRENDER_ITEM_TREE
	// The list shows the layers as they are now. It is a view kept in the tag's container, so this
	// writes into the tag while it is only being described: nothing that counts changes by it.
	PushImagerList(const_cast<cinema::BaseList2D*>(tag), false);
#endif
	const cinema::BaseContainer& data = tag->GetDataInstanceRef();
	maxon::BaseArray<maxon::Int32> layers;
	iferr (GetImagerLayers(data, layers))
		return false;
	const maxon::Int32 selected = GetSelectedImagerLayer(data);

	const cinema::DescID tab = ParameterId(CYCLESCAMERA_GROUP_IMAGER, cinema::DTYPE_GROUP);
	// "Add Effect": the effects of the table, by Blender's names.
	{
		cinema::BaseContainer menu = cinema::GetCustomDataTypeDefault(cinema::DTYPE_LONG);
		menu.SetString(cinema::DESC_NAME, "Add Effect"_s);
		menu.SetString(cinema::DESC_SHORT_NAME, "Add Effect"_s);
		menu.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
		cinema::BaseContainer cycle;
		cycle.SetString(0, "Choose an Effect..."_s);
		maxon::Int32 choice = 1;
		for (const ImagerEffect& effect : GetImagerEffects())
			cycle.SetString(choice++, maxon::String(effect.name) + SinceNote(effect.since));
		menu.SetContainer(cinema::DESC_CYCLE, cycle);
		if (!description->SetParameter(ParameterId(CYCLESCAMERA_IMAGER_ADD, cinema::DTYPE_LONG), menu, tab))
			return false;
	}
#if CYCLESRENDER_ITEM_TREE
	// The list of layers, as a field list shows its fields.
	if (!description->SetParameter(ParameterId(CYCLESCAMERA_IMAGER_LIST, CUSTOMDATA_ITEMTREE), ListDescription(), tab))
		return false;
#endif
	// "Layer": the layers, the top one first. Hidden where the list is, which selects by a click; the
	// parameter stays, for scripts.
	{
		cinema::BaseContainer menu = cinema::GetCustomDataTypeDefault(cinema::DTYPE_LONG);
		menu.SetString(cinema::DESC_NAME, "Layer"_s);
		menu.SetString(cinema::DESC_SHORT_NAME, "Layer"_s);
		menu.SetInt32(cinema::DESC_ANIMATE, cinema::DESC_ANIMATE_OFF);
		cinema::BaseContainer cycle;
		if (layers.IsEmpty())
			cycle.SetString(0, "No Effects"_s);
		for (const maxon::Int32 layer : layers)
			cycle.SetString(layer, LayerLine(data, layer));
		menu.SetContainer(cinema::DESC_CYCLE, cycle);
		menu.SetBool(cinema::DESC_HIDE, CYCLESRENDER_ITEM_TREE != 0);
		if (!description->SetParameter(ParameterId(CYCLESCAMERA_IMAGER_SELECTED, cinema::DTYPE_LONG), menu, tab))
			return false;
	}
	// The buttons, in one row. Hidden where the list is (drag an entry, press Delete); they still answer a script.
	{
		cinema::BaseContainer row = cinema::GetCustomDataTypeDefault(cinema::DTYPE_GROUP);
		row.SetInt32(cinema::DESC_COLUMNS, 3);
		row.SetBool(cinema::DESC_HIDE, CYCLESRENDER_ITEM_TREE != 0);
		const cinema::DescID rowId = ParameterId(CYCLESCAMERA_GROUP_IMAGER_BUTTONS, cinema::DTYPE_GROUP);
		if (!description->SetParameter(rowId, row, tab))
			return false;
		const maxon::Int32 ids[3] = { CYCLESCAMERA_IMAGER_REMOVE, CYCLESCAMERA_IMAGER_UP, CYCLESCAMERA_IMAGER_DOWN };
		const maxon::Char* const names[3] = { "Remove", "Move Up", "Move Down" };
		for (maxon::Int32 i = 0; i < 3; ++i)
		{
			cinema::BaseContainer button = cinema::GetCustomDataTypeDefault(cinema::DTYPE_BUTTON);
			button.SetString(cinema::DESC_NAME, maxon::String(names[i]));
			button.SetString(cinema::DESC_SHORT_NAME, maxon::String(names[i]));
			button.SetInt32(cinema::DESC_CUSTOMGUI, cinema::DTYPE_BUTTON);
			if (!description->SetParameter(ParameterId(ids[i], cinema::DTYPE_BUTTON), button, rowId))
				return false;
		}
	}
	for (const maxon::Int32 layer : layers)
	{
		if (!AddLayerDescription(data, description, layer, layer == selected))
			return false;
	}
	return true;
}

maxon::Bool IsImagerParameterEnabled(const cinema::BaseContainer& data, maxon::Int32 id)
{
	maxon::Int32 layer = 0, field = 0;
	const maxon::Bool ofLayer = ImagerLayerOfId(id, layer, field);
	if (!ofLayer && (id < CYCLESCAMERA_IMAGER_LAYERS || id > CYCLESCAMERA_IMAGER_NEXT))
		return true;
	if (!data.GetBool(CYCLESCAMERA_IMAGER_ENABLE, true))
		return false;
	if (id == CYCLESCAMERA_IMAGER_REMOVE || id == CYCLESCAMERA_IMAGER_UP || id == CYCLESCAMERA_IMAGER_DOWN || id == CYCLESCAMERA_IMAGER_SELECTED)
	{
		maxon::BaseArray<maxon::Int32> layers;
		iferr (GetImagerLayers(data, layers))
			return false;
		const maxon::Int index = layers.FindIndex(GetSelectedImagerLayer(data));
		if (index == NOTOK)
			return false;
		if (id == CYCLESCAMERA_IMAGER_UP)
			return index > 0;
		if (id == CYCLESCAMERA_IMAGER_DOWN)
			return index + 1 < layers.GetCount();
	}
	return true;
}

// The tab's layers changed: the tag's data did, and the Attribute Manager has to build the tab again.
static void LayersChanged(cinema::BaseList2D* tag)
{
	tag->SetDirty(cinema::DIRTYFLAGS::DATA | cinema::DIRTYFLAGS::DESCRIPTION);
	cinema::EventAdd();
}

// After the menu or a button chose the selected layer: the list selects its entry.
static void ShowSelectedLayer(cinema::BaseList2D* tag)
{
#if CYCLESRENDER_ITEM_TREE
	PushImagerList(tag, true);
#endif
}

maxon::Bool SetImagerMenu(cinema::BaseList2D* tag, maxon::Int32 id, const cinema::GeData& value)
{
	if (id != CYCLESCAMERA_IMAGER_ADD)
		return false;
	const maxon::Int32 choice = value.GetInt32();
	const maxon::Block<const ImagerEffect> effects = GetImagerEffects();
	if (choice >= 1 && choice <= effects.GetCount())
	{
		AddImagerLayer(tag->GetDataInstanceRef(), effects[choice - 1]);
		LayersChanged(tag);
		ShowSelectedLayer(tag);
	}
	// The menu itself goes back to its first line.
	tag->GetDataInstanceRef().SetInt32(CYCLESCAMERA_IMAGER_ADD, 0);
	return true;
}

maxon::Bool RunImagerCommand(cinema::BaseList2D* tag, maxon::Int32 id)
{
	if (id != CYCLESCAMERA_IMAGER_REMOVE && id != CYCLESCAMERA_IMAGER_UP && id != CYCLESCAMERA_IMAGER_DOWN)
		return false;
	cinema::BaseContainer& data = tag->GetDataInstanceRef();
	const maxon::Int32 selected = GetSelectedImagerLayer(data);
	if (selected == 0 || !IsImagerParameterEnabled(data, id))
		return true;
	// A button's press is no parameter change: the undo step is made here.
	cinema::BaseDocument* const doc = tag->GetDocument();
	if (doc)
	{
		doc->StartUndo();
		doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, tag);
	}
	if (id == CYCLESCAMERA_IMAGER_REMOVE)
		RemoveImagerLayer(data, selected);
	else
		MoveImagerLayer(data, selected, id == CYCLESCAMERA_IMAGER_UP ? -1 : 1);
	if (doc)
		doc->EndUndo();
	LayersChanged(tag);
	ShowSelectedLayer(tag);
	return true;
}

maxon::Bool HandleImagerList(cinema::BaseList2D* tag, void* message)
{
#if CYCLESRENDER_ITEM_TREE
	cinema::ItemTreeCallbackData* const callback = static_cast<cinema::ItemTreeCallbackData*>(message);
	if (!callback || !callback->item)
		return false;
	cinema::BaseContainer& data = tag->GetDataInstanceRef();
	cinema::BaseDocument* const doc = tag->GetDocument();
	switch (callback->type)
	{
		// The Delete key, or "Remove" of the list's menu: the selected entries' layers go, as by the
		// Remove button. The list removes its entries itself, after this.
		case ITEMTREE_CALLBACK_DELETE:
		{
			maxon::BaseArray<maxon::Int32> layers, going;
			iferr (GetImagerLayers(data, layers))
				return false;
			for (cinema::ItemTreeNode* node = callback->item->GetFirst(); node; node = callback->item->GetNext(node))
			{
				const cinema::ItemTreeNodeData* const entry = callback->item->GetNodeData(node);
				if (entry && entry->IsSelected() && layers.Contains(entry->m_ID))
				{
					iferr (going.Append(entry->m_ID))
						return false;
				}
			}
			if (going.IsEmpty())
				return false;
			if (doc)
			{
				doc->StartUndo();
				doc->AddUndo(cinema::UNDOTYPE::CHANGE_SMALL, tag);
			}
			for (const maxon::Int32 layer : going)
				RemoveImagerLayer(data, layer);
			if (doc)
				doc->EndUndo();
			LayersChanged(tag);
			return false;
		}
		// The slider of the opacity column was moved.
		case ITEMTREE_CALLBACK_VALUECHANGED:
		{
			if (!callback->nodedata || !callback->data || callback->id != OPACITY_COLUMN)
				return false;
			const maxon::Int32 layer = callback->nodedata->m_ID;
			const maxon::Float opacity = maxon::Clamp01(callback->data->GetFloat(ITEMTREE_VALUECHANGED_VALUE, 1.0));
			if (maxon::Abs(opacity - data.GetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), 1.0)) > 1e-6)
			{
				data.SetFloat(ImagerLayerId(layer, IMAGERLAYER_OPACITY), opacity);
				tag->SetDirty(cinema::DIRTYFLAGS::DATA);
				cinema::EventAdd();
			}
			callback->ret = 1;
			return true;
		}
		// Another entry selected (the one clicked comes with the message; the list may not have it
		// selected yet): its layer's settings show below the list.
		case ITEMTREE_CALLBACK_SELECTIONCHG:
		{
			TakeSelection(tag, callback->item, callback->nodedata ? callback->nodedata->m_ID : 0);
			return false;
		}
		// A name typed over an entry (double click). The new name comes with the message; the list
		// itself still has the old one.
		case ITEMTREE_CALLBACK_RENAME:
		{
			maxon::BaseArray<maxon::Int32> layers;
			iferr (GetImagerLayers(data, layers))
				return false;
			if (!callback->nodedata || callback->nodedata->m_Text.IsEmpty() || !layers.Contains(callback->nodedata->m_ID))
				return false;
			const maxon::Int32 name = ImagerLayerId(callback->nodedata->m_ID, IMAGERLAYER_NAME);
			if (callback->nodedata->m_Text != data.GetString(name))
			{
				data.SetString(name, callback->nodedata->m_Text);
				LayersChanged(tag);
			}
			return false;
		}
		// A checkbox clicked, an entry dragged: the layers take from the list what that was about.
		case ITEMTREE_CALLBACK_CHECKBOX:
		case ITEMTREE_CALLBACK_MOVENODE:
		{
			if (PullImagerList(tag, callback->item, callback->type == ITEMTREE_CALLBACK_CHECKBOX ? PULL::SWITCHES : PULL::ORDER))
				LayersChanged(tag);
			// A drag selects its entry without a message of its own.
			if (callback->type == ITEMTREE_CALLBACK_MOVENODE)
				TakeSelection(tag, callback->item);
			return false;
		}
		default:
			return false;
	}
#else
	return false;
#endif
}

maxon::Bool SetImagerList(cinema::BaseList2D* tag, maxon::Int32 id)
{
#if CYCLESRENDER_ITEM_TREE
	if (id != CYCLESCAMERA_IMAGER_LIST)
		return false;
	cinema::ItemTreeData* const tree = tag->GetDataInstanceRef().GetCustomDataTypeWritableObsolete<cinema::ItemTreeData>(CYCLESCAMERA_IMAGER_LIST);
	// (Not the opacities: the column's slider says so itself, and the list may be behind the slider below.
	// Not the names: the list is written before a typed name is in it, and the name says so itself,
	// HandleImagerList. The selected entry is taken where the list is ahead: an entry a drag began on.)
	if (tree && PullImagerList(tag, tree, PULL::SWITCHES | PULL::ORDER))
		LayersChanged(tag);
	if (tree)
		TakeSelection(tag, tree);
	return true;
#else
	return false;
#endif
}

void ImagerParameterWasSet(cinema::BaseList2D* tag, maxon::Int32 id)
{
	maxon::Int32 layer = 0, field = 0;
	if (id == CYCLESCAMERA_IMAGER_SELECTED || id == CYCLESCAMERA_IMAGER_LAYERS ||
		(ImagerLayerOfId(id, layer, field) && (field == IMAGERLAYER_NAME || field == IMAGERLAYER_ENABLED)))
		tag->SetDirty(cinema::DIRTYFLAGS::DESCRIPTION);
#if CYCLESRENDER_ITEM_TREE
	// The list shows a layer's name, switch and opacity, and the selected layer: it follows the value at once.
	if (id == CYCLESCAMERA_IMAGER_SELECTED || id == CYCLESCAMERA_IMAGER_LAYERS || ImagerLayerOfId(id, layer, field))
		PushImagerList(tag, id == CYCLESCAMERA_IMAGER_SELECTED || id == CYCLESCAMERA_IMAGER_LAYERS);
#endif
}

} // namespace cyclesrender

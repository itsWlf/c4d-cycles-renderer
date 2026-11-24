// SPDX-License-Identifier: Apache-2.0
// Between a node port that links an object of the scene and the object (object_link.h).

#include "c4d.h"
#include "c4d_basematerial.h"

#include "cycles_nodes.h"
#include "object_link.h"

namespace cyclesrender
{

maxon::Result<cinema::BaseObject*> FindLinkedObject(const cinema::BaseMaterial* material, const maxon::Uuid& link)
{
	iferr_scope;
	if (!material || link.IsEmpty() || !material->IsInstanceOf(Mmaterial))
		return nullptr;
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	// A key the material does not hold (a Uuid typed in, a node pasted into another material) is no object either.
	iferr (cinema::BaseList2D* const found = nodeMaterial->UuidToBaseList2D(maxon::Id(CYCLES_NODESPACE_ID), link))
		return nullptr;
	if (!found || !found->IsInstanceOf(Obase))
		return nullptr;
	return static_cast<cinema::BaseObject*>(found);
}

maxon::Result<maxon::Uuid> MakeObjectLink(cinema::BaseMaterial* material, cinema::BaseObject* op)
{
	iferr_scope;
	if (!op)
		return maxon::Uuid();
	if (!material || !material->IsInstanceOf(Mmaterial))
		return maxon::IllegalArgumentError(MAXON_SOURCE_LOCATION, "no node material"_s);
	const cinema::NodeMaterial* const nodeMaterial = static_cast<const cinema::NodeMaterial*>(material);
	return nodeMaterial->BaseList2DToUuid(maxon::Id(CYCLES_NODESPACE_ID), op);
}

} // namespace cyclesrender

// SPDX-License-Identifier: Apache-2.0
// A setting of a Cycles node that links an object of the scene (kind 'o' of the node table, the
// Texture Coordinate node's "Object"): between the port's value and the object.

#ifndef CYCLESRENDER_OBJECT_LINK_H__
#define CYCLESRENDER_OBJECT_LINK_H__

#include "c4d.h"
#include "maxon/uuid.h"

namespace cyclesrender
{

//----------------------------------------------------------------------------------------
/// The port holds a maxon::Uuid, which Cinema 4D's link field in the node's attributes writes (the
/// description's gui net.maxon.ui.baselink, as Redshift's own nodes link a scene object). The Uuid
/// is no property of the object: it is the key of a link the node material keeps for its node space
/// (NodeMaterial::BaseList2DToUuid and UuidToBaseList2D), a BaseLink of Cinema 4D's own, which a copy
/// of the document, saving and loading and a paste carry along as they carry any link. What was
/// measured: docs/reference/c4d-sdk.md, "A node port that links an object of the scene".
//----------------------------------------------------------------------------------------

/// The object that @p link names in @p material's graph of the Cycles node space, in the document the
/// material is in, or nullptr: an empty link, a link to something that is no object, or one whose
/// object is not in that document (deleted, or the material was copied into another one without it).
maxon::Result<cinema::BaseObject*> FindLinkedObject(const cinema::BaseMaterial* material, const maxon::Uuid& link);

/// The value a link field of @p material's Cycles graph holds once @p op is dropped into it (the
/// material keeps the link); an empty Uuid for nullptr. For tests (ipr/ipr_testhook.h, "objectlink"):
/// a script cannot drop an object into the field, and Python has no such conversion.
maxon::Result<maxon::Uuid> MakeObjectLink(cinema::BaseMaterial* material, cinema::BaseObject* op);

} // namespace cyclesrender

#endif // CYCLESRENDER_OBJECT_LINK_H__

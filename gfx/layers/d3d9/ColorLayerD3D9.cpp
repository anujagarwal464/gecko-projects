/* -*- Mode: C++; tab-width: 20; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MPL 1.1/GPL 2.0/LGPL 2.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is Mozilla Corporation code.
 *
 * The Initial Developer of the Original Code is Mozilla Foundation.
 * Portions created by the Initial Developer are Copyright (C) 2009
 * the Initial Developer. All Rights Reserved.
 *
 * Contributor(s):
 *   Robert O'Callahan <robert@ocallahan.org>
 *   Bas Schouten <bschouten@mozilla.com>
 *
 * Alternatively, the contents of this file may be used under the terms of
 * either the GNU General Public License Version 2 or later (the "GPL"), or
 * the GNU Lesser General Public License Version 2.1 or later (the "LGPL"),
 * in which case the provisions of the GPL or the LGPL are applicable instead
 * of those above. If you wish to allow use of your version of this file only
 * under the terms of either the GPL or the LGPL, and not to allow others to
 * use your version of this file under the terms of the MPL, indicate your
 * decision by deleting the provisions above and replace them with the notice
 * and other provisions required by the GPL or the LGPL. If you do not delete
 * the provisions above, a recipient may use your version of this file under
 * the terms of any one of the MPL, the GPL or the LGPL.
 *
 * ***** END LICENSE BLOCK ***** */

#include "ColorLayerD3D9.h"

namespace mozilla {
namespace layers {

Layer*
ColorLayerD3D9::GetLayer()
{
  return this;
}

static void
RenderColorLayerD3D9(ColorLayer* aLayer, LayerManagerD3D9 *aManager)
{
  // XXX we might be able to improve performance by using
  // IDirect3DDevice9::Clear

  nsIntRect visibleRect = aLayer->GetEffectiveVisibleRegion().GetBounds();

  aManager->device()->SetVertexShaderConstantF(
    CBvLayerQuad,
    ShaderConstantRect(visibleRect.x,
                       visibleRect.y,
                       visibleRect.width,
                       visibleRect.height),
    1);

  const gfx3DMatrix& transform = aLayer->GetEffectiveTransform();
  aManager->device()->SetVertexShaderConstantF(CBmLayerTransform, &transform._11, 4);

  gfxRGBA layerColor(aLayer->GetColor());
  float color[4];
  float opacity = aLayer->GetEffectiveOpacity() * layerColor.a;
  // output color is premultiplied, so we need to adjust all channels.
  // mColor is not premultiplied.
  color[0] = (float)(layerColor.r * opacity);
  color[1] = (float)(layerColor.g * opacity);
  color[2] = (float)(layerColor.b * opacity);
  color[3] = (float)(opacity);

  aManager->device()->SetPixelShaderConstantF(0, color, 1);

  aManager->SetShaderMode(DeviceManagerD3D9::SOLIDCOLORLAYER,
                          aLayer->GetMaskLayer());

  aManager->device()->DrawPrimitive(D3DPT_TRIANGLESTRIP, 0, 2);
}

void
ColorLayerD3D9::RenderLayer()
{
  return RenderColorLayerD3D9(this, mD3DManager);
}

void
ShadowColorLayerD3D9::RenderLayer()
{
  return RenderColorLayerD3D9(this, mD3DManager);
}

} /* layers */
} /* mozilla */

/*
 * Copyright © 2013 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authored by: Kevin DuBois <kevin.dubois@canonical.com>
 */

#include "mir/graphics/android/sync_fence.h"
#include "mir/graphics/android/native_buffer.h"
#include "hwc_layerlist.h"

#include <algorithm>
#include <cstring>

namespace mg=mir::graphics;
namespace mga=mir::graphics::android;
namespace geom=mir::geometry;

mga::HWCLayer::HWCLayer(HWCLayerType type, mg::NativeBuffer const& buffer, bool must_use_gl)
{
    int skip_flag = 0;
    if (must_use_gl)
    {
        skip_flag = HWC_SKIP_LAYER;
    }

    compositionType = type;
    hints = 0;
    flags = skip_flag;
    transform = 0;
    blending = HWC_BLENDING_NONE;
    //TODO: acquireFenceFd should be buffer.fence()
    acquireFenceFd = -1;
    releaseFenceFd = -1;

    visible_rect.top = 0;
    visible_rect.left = 0;
    visible_rect.bottom = buffer.anwb()->height;
    visible_rect.right = buffer.anwb()->width; 
    sourceCrop = visible_rect;
    displayFrame = visible_rect;
    visibleRegionScreen.numRects=1;
    visibleRegionScreen.rects= &visible_rect;
    handle = buffer.handle();
}

mga::HWCLayerType mga::HWCLayer::type() const
{
    if (compositionType == HWC_FRAMEBUFFER)
        return mga::HWCLayerType::gles;
    if (compositionType == HWC_FRAMEBUFFER_TARGET)
        return mga::HWCLayerType::framebuffer;
    if (compositionType == HWC_OVERLAY)
        return mga::HWCLayerType::overlay;
    return mga::HWCLayerType::overlay;
}

mga::FramebufferLayer::FramebufferLayer(mg::NativeBuffer const& buffer)
    : HWCLayer(mga::HWCLayerType::framebuffer, buffer, false)
{
}

mga::CompositionLayer::CompositionLayer(mg::NativeBuffer const& buffer, bool must_use_gl)
    : HWCLayer(mga::HWCLayerType::gles, buffer, must_use_gl)
{
}

mga::LayerList::LayerList(std::initializer_list<HWCLayer> const& layer_list)
{
    auto struct_size = sizeof(hwc_display_contents_1_t) + sizeof(hwc_layer_1_t)*(layer_list.size());
    hwc_representation = std::shared_ptr<hwc_display_contents_1_t>(
        static_cast<hwc_display_contents_1_t*>( ::operator new(struct_size)));

    auto i = 0u;
    for(auto& layer : layer_list)
    {
        hwc_representation->hwLayers[i++] = layer;        
    }
    hwc_representation->numHwLayers = layer_list.size();
    hwc_representation->retireFenceFd = -1;
    hwc_representation->flags = HWC_GEOMETRY_CHANGED;
    hwc_representation->dpy = reinterpret_cast<void*>(0xDECAF);
    hwc_representation->sur = reinterpret_cast<void*>(0xC0FFEE);
    
}

hwc_display_contents_1_t* mga::LayerList::native_list() const
{
    return hwc_representation.get();
}

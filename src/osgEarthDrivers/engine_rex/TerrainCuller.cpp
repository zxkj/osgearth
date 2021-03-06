/* -*-c++-*- */
/* osgEarth - Dynamic map generation toolkit for OpenSceneGraph
 * Copyright 2008-2014 Pelican Mapping
 * http://osgearth.org
 *
 * osgEarth is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include "TerrainCuller"
#include "TileNode"
#include "SurfaceNode"

#define LC "[TerrainCuller] "

using namespace osgEarth::Drivers::RexTerrainEngine;


TerrainCuller::TerrainCuller() :
_frame(0L),
_context(0L),
_camera(0L),
_currentTileNode(0L),
_orphanedPassesDetected(0u)
{
    setVisitorType(CULL_VISITOR);
    setTraversalMode(TRAVERSE_ALL_CHILDREN);
}

void
TerrainCuller::setup(const MapFrame& frame, const RenderBindings& bindings, osg::StateSet* defaultStateSet)
{
    unsigned frameNum = getFrameStamp() ? getFrameStamp()->getFrameNumber() : 0u;
    _terrain.setup(frame, bindings, defaultStateSet, frameNum);
}

float
TerrainCuller::getDistanceToViewPoint(const osg::Vec3& pos, bool withLODScale) const
{
    if (withLODScale) return (pos-getViewPointLocal()).length()*getLODScale();
    else return (pos-getViewPointLocal()).length();
}

DrawTileCommand*
TerrainCuller::addDrawCommand(UID uid, const RenderingPass& pass, TileNode* tileNode)
{
    SurfaceNode* surface = tileNode->getSurfaceNode();

    const RenderBindings&  bindings = _context->getRenderBindings();

    // skip layers that are not visible:
    if (pass._imageLayer.valid() && !pass._imageLayer->getVisible())
        return 0L;

    // add a new Draw command to the appropriate layer
    osg::ref_ptr<LayerDrawable> layer = _terrain.layer(uid);
    if (layer.valid())
    {
        layer->_tiles.push_back(DrawTileCommand());
        DrawTileCommand& tile = layer->_tiles.back();

        // install everything we need in the Draw Command:
        tile._pass = &pass;
        tile._matrix = surface->getMatrix();
        tile._modelViewMatrix = *this->getModelViewMatrix();
        tile._keyValue = tileNode->getTileKeyValue();
        tile._geom = surface->getDrawable()->_geom.get();
        tile._morphConstants = tileNode->getMorphConstants();
        tile._key = tileNode->getKey();

#if 1
        osg::Vec3 c = surface->getBound().center() * surface->getInverseMatrix();
        tile._range = getDistanceToViewPoint(c, true);
#else
        osg::Vec3f eyeWorld = getViewPointLocal() * surface->getMatrix();
        tile._range = (eyeWorld - surface->getBound().center()).length();
#endif

        const osg::Image* elevRaster = tileNode->getElevationRaster();
        if (elevRaster)
        {
            float bias = _context->getUseTextureBorder() ? 1.5 : 0.5;

            // Compute an elevation texture sampling scale/bias so we sample elevation data on center
            // instead of on edge (as we do with color, etc.)
            //
            // This starts out as:
            //   scale = (size-1)/size : this shrinks the sample area by one texel since we're sampling on center
            //   bias = 0.5/size : this shifts the sample area over 1/2 texel to the center.
            //
            // But, since we also have a 1-texel border, we need to further reduce the scale by 2 texels to
            // remove the border, and shift an extra texel over as well. Giving us this:
            float size = (float)elevRaster->s();
            tile._elevTexelCoeff.set((size - (2.0*bias)) / size, bias / size);
        }

        return &tile;
    }
    else
    {
        ++_orphanedPassesDetected;
        //OE_WARN << LC << "Internal error - _terrain.layer(uid=" << uid << ") returned NULL\n";
        return 0L;
    }
}

void
TerrainCuller::apply(osg::Node& node)
{
    // push the node's state.
    osg::StateSet* node_state = node.getStateSet();

    TileNode* tileNode = dynamic_cast<TileNode*>(&node);
    if (tileNode)
    {
        _currentTileNode = tileNode;
        
        if (!_terrain.patchLayers().empty())
        {
            // todo: check for patch/virtual
            const RenderBindings&  bindings    = _context->getRenderBindings();
            TileRenderModel& renderModel = _currentTileNode->renderModel();

            bool pushedMatrix = false;

            // Patch layers will use the default (empty) pass for now.
            // TODO: allow access to other passes.
            const RenderingPass* defaultPass = renderModel.getPass(-1);
            
            for (PatchLayerVector::const_iterator i = _terrain.patchLayers().begin(); i != _terrain.patchLayers().end(); ++i)
            {
                PatchLayer* layer = i->get();
                if (layer->getAcceptCallback() == 0L ||
                    layer->getAcceptCallback()->accept(_currentTileNode->getKey()))
                {
                    // Push this tile's matrix if we haven't already done so:
                    if (!pushedMatrix)
                    {
                        SurfaceNode* surface = tileNode->getSurfaceNode();

                        // push the surface matrix:
                        osg::Matrix mvm = *getModelViewMatrix();
                        surface->computeLocalToWorldMatrix(mvm, this);
                        pushModelViewMatrix(createOrReuseMatrix(mvm), surface->getReferenceFrame());
                        pushedMatrix = true;
                    }

                    // Add the draw command:
                    DrawTileCommand* cmd = addDrawCommand(layer->getUID(), *defaultPass, tileNode);
                    if (cmd)
                    {
                        cmd->_drawPatch = true;
                        cmd->_drawCallback = layer->getDrawCallback();
                    }
                }
            }

            if (pushedMatrix)
            {
                popModelViewMatrix();
            }
        }
    }

    else
    {
        SurfaceNode* surface = dynamic_cast<SurfaceNode*>(&node);
        if (surface)
        {            
            TileRenderModel& renderModel = _currentTileNode->renderModel();

            // push the surface matrix:
            osg::Matrix mvm = *getModelViewMatrix();
            surface->computeLocalToWorldMatrix(mvm, this);
            pushModelViewMatrix(createOrReuseMatrix(mvm), surface->getReferenceFrame());

            unsigned count = 0;
            
            // First go through any legit rendering pass data in the Tile and
            // and add a DrawCommand for each.
            for (unsigned p = 0; p < renderModel._passes.size(); ++p)
            {
                const RenderingPass& pass = renderModel._passes[p];
                
                if (pass._layer.valid() && pass._layer->getRenderType() == Layer::RENDERTYPE_TILE)
                {
                    if (addDrawCommand(pass._sourceUID, pass, _currentTileNode))
                        ++count;
                }
            }

            // Next, add a DrawCommand for each "global" tile layer (i.e. layers that are
            // not represented in the TerrainTileModel for whatever reason).
            // These use the "default" rendering pass samplers (UID -1) for rendering.
            // (TODO: make this configuration, so one of these layers can use a specific
            // color layer's samplers)
            for (LayerVector::const_iterator i = _terrain.tileLayers().begin(); i != _terrain.tileLayers().end(); ++i)
            {
                Layer* layer = i->get();
                addDrawCommand(layer->getUID(), *renderModel.getPass(-1), _currentTileNode);
            }

            popModelViewMatrix();

            _terrain._drawState->_bs.expandBy(surface->getBound());
            _terrain._drawState->_box.expandBy(_terrain._drawState->_bs);
        }
    }

    traverse(node);
}

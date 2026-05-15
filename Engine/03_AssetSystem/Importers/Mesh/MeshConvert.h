
// 没啃

#pragma once

#include "03_AssetSystem/Importers/glTF.h"
#include "00_Core/Math/BoundingSphere.h"
#include "00_Core/Math/BoundingBox.h"   
#include <cstdint>
#include <string>

namespace Renderer
{
    struct Primitive
    {
        Math::BoundingSphere m_BoundsLS;  // local space bounds
        Math::BoundingSphere m_BoundsOS; // object space bounds
        Math::AxisAlignedBox m_BBoxLS;       // local space AABB
        Math::AxisAlignedBox m_BBoxOS;       // object space AABB
        Utility::ByteArray VB;
        Utility::ByteArray IB;
        Utility::ByteArray DepthVB;
        uint32_t primCount;
        union MyUnion
        {

        };
        {
            uint32_t hash;
            struct {
                uint32_t psoFlags : 16;
                uint32_t index32 : 1;
                uint32_t materialIdx : 15;
            };
        };
        uint16_t vertexStride;
    };
} // namespace Renderer

void OptimizeMesh(Renderer::Primitive& outPrim, const glTF::Primitive& inPrim, const Math::Matrix4& localToObject);

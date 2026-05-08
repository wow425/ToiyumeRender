
// 没啃

#pragma once

#include "05_ResourceSystem/03_Model/glTF.h"
#include "13_Math/BoundingSphere.h"
#include "13_Math/BoundingBox.h"   
#include <cstdint>
#include <string>

namespace Renderer
{
    using namespace Math;

    struct Primitive
    {
        BoundingSphere m_BoundsLS;  // local space bounds
        BoundingSphere m_BoundsOS;  // object space bounds
        AxisAlignedBox m_BBoxLS;       // local space AABB
        AxisAlignedBox m_BBoxOS;       // object space AABB
        Utility::ByteArray VB;
        Utility::ByteArray IB;
        Utility::ByteArray DepthVB;
        uint32_t primCount;
        union
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
}

void OptimizeMesh(Renderer::Primitive& outPrim, const glTF::Primitive& inPrim, const Math::Matrix4& localToObject);

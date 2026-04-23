
// 没啃

#pragma once

#include "glTF.h"

#include <cstdint>
#include <string>

namespace Renderer
{
    using namespace Math;

    struct Primitive
    {
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

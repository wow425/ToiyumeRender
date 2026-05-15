#pragma once

//材质复杂也能自动绑定
//同时未来还能自由切换 PSO 和渲染策略
//
//那就必须把系统拆成：
//
//资产导入层：保存材质信息与几何信息
//材质实例层：保存参数与纹理绑定
//渲染策略层：在运行时决定 PSO


#include <cstdint>

namespace Renderer
{
    enum VertexLayoutFlags : uint32_t
    {
        kVertex_Position = 1,   // 00001
        kVertex_Normal = 2,   // 00010
        kVertex_Tangent = 4,   // 00100
        kVertex_UV0 = 8,   // 01000
        kVertex_UV1 = 16,  // 10000
    };

    enum class RenderPassType
    {
        Depth,
        Forward, 
        Deferred,
        Shadow,
        Transparent,
    };

    struct PipelineDesc
    {
        uint32_t VertexFlags = 0;
        uint32_t MaterialFlags = 0;
        RenderPassType PassType = RenderPassType::Forward; // 

        bool operator==(const PipelineDesc& rhs) const
        {
            return     VertexFlags == rhs.VertexFlags &&
                MaterialFlags == rhs.MaterialFlags &&
                PassType == rhs.PassType;
        }

        // 提供哈希以便作为 Pipeline Manager (PSO Cache) 的 Key
        struct Hash
        {
            size_t operator()(const PipelineDesc& desc) const
            {
                return std::hash<uint32_t>()(desc.VertexFlags) ^
                    (std::hash<uint32_t>()(desc.MaterialFlags) << 1) ^
                    (std::hash<uint8_t>()(static_cast<uint8_t>(desc.PassType)) << 2);
            }
        };
    };
}

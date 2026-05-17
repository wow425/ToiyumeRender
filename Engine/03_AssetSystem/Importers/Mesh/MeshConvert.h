


#pragma once

#include "03_AssetSystem/Importers/glTF.h"
#include "00_Core/Math/BoundingSphere.h"
#include "00_Core/Math/BoundingBox.h"   
#include "04_Renderer/Pipeline/PipelineDesc.h"
#include <cstdint>
#include <string>

namespace Renderer
{
	struct Primitive
	{
		Math::BoundingSphere m_BoundsLS;  //0 local space bounds
		Math::BoundingSphere m_BoundsOS; // object space bounds
		Math::AxisAlignedBox m_BBoxLS;       // local space AABB
		Math::AxisAlignedBox m_BBoxOS;       // object space AABB
		Utility::ByteArray VB;
		Utility::ByteArray IB;
		Utility::ByteArray DepthVB;
		uint32_t primCount;
		// 资源导入层的 Primitive/Mesh 调整为只记录几何与资源事实：顶点布局、索引宽度、材质槽位、VB/IB/Draw 信息，
		// 不再把材质透明状态或 PSO 选择结果写进模型资源结构
		union
		{
			uint32_t hash;
			struct {
				uint32_t vertexFlags : 16; // NOT psoFlags
				uint32_t index32 : 1;
				uint32_t materialIdx : 15;
			};
		};
		uint16_t vertexStride;
		uint16_t depthVertexStride;
	};
} // namespace Renderer

void OptimizeMesh(Renderer::Primitive& outPrim, const glTF::Primitive& inPrim, const Math::Matrix4& localToObject);

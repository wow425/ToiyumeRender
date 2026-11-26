


#pragma once

#include "03_AssetSystem/Assets/Importers/glTF.h"
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
		// 璧勬簮瀵煎叆灞傜殑 Primitive/Mesh 璋冩暣涓哄彧璁板綍鍑犱綍涓庤祫婧愪簨瀹烇細椤剁偣甯冨眬銆佺储寮曞搴︺€佹潗璐ㄦЫ浣嶃€乂B/IB/Draw 淇℃伅锛?
		// 涓嶅啀鎶婃潗璐ㄩ€忔槑鐘舵€佹垨 PSO 閫夋嫨缁撴灉鍐欒繘妯″瀷璧勬簮缁撴瀯
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


//  



#pragma once

#include <cstdint>

class StructuredBuffer;
class GraphicsContext;

namespace Math
{
	class Vector3;
	class Matrix4;

}

namespace Scene
{
	namespace Camera
	{
		class Camera;
	}
}

namespace Scene::LightingSystem
{
	// 必须与HLSL中LightData结构体保持一致
	struct alignas(16) LightData
	{
		// xyz = Direction
		// w   = Intensity
		DirectX::XMFLOAT4 DirectionIntensity;

		// rgb = Directional Color
		// a   = unused
		DirectX::XMFLOAT4 DirectionalColor;

		// xyz = Point Light Position
		// w   = Range
		DirectX::XMFLOAT4 PointLightPositionRange;

		// rgb = Point Light Color
		// a   = Point Light Intensity
		DirectX::XMFLOAT4 PointLightColorIntensity;
	};

	enum { MaxLights = 1 };

	enum
	{
		point,
		direction,

		Lights
	};

	void InitializeResources(void);
	void Shutdown(void);
	void CreateLights(void);
	void UpdataState(GraphicsContext& gfxContext);


	extern LightData m_LightCPUBuffer[MaxLights]; // 管理CPU侧灯光数据
	extern StructuredBuffer m_LightGPUBuffer; // 管理GPU Light Buffer

};

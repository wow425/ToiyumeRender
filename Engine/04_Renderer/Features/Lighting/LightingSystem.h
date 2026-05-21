
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
	struct LightData
	{
		DirectX::XMFLOAT3 DirectionalLightDir;		// 光方向
		float DirectionalLightIntensity;	// 光强度

		DirectX::XMFLOAT3 DirectionalLightColor[3];		// 光颜色
		float _pad;
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


//  




#pragma once

#include <cstdint>

class StructuredBuffer;
class GraphicsContext;

namespace Math
{
	class Vector3;
	class Matrix4;
	class Camera;
}

// 必须与HLSL中LightData结构体保持一致
struct LightData
{
	float pos[3]; // 灯位置
	float radiusSq; // 影响范围的平方

	float color[3]; // 灯颜色
	uint32_t type; // 灯类型

	float coneDir[3]; // 灯方向
	float coneAngles[2]; // Spot Light角度参数

};




namespace LightingSystem
{

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

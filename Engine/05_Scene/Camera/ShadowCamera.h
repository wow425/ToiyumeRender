#pragma once

// 没完全啃明白

#include "05_Scene/Camera/Camera.h"
#include "00_Core/Math/VectorMath.h"

class ShadowCamera : public Scene::BaseCamera
{
public:

	ShadowCamera() {}

	void UpdateMatrix(
		Math::Vector3 LightDirection,		// Direction parallel to light, in direction of travel
		Math::Vector3 ShadowCenter,		// Center location on far bounding plane of shadowed region
		Math::Vector3 ShadowBounds,		// Width, height, and depth in world space represented by the shadow buffer
		uint32_t BufferWidth,		// Shadow buffer width
		uint32_t BufferHeight,		// Shadow buffer height--usually same as width
		uint32_t BufferPrecision	// Bit depth of shadow buffer--usually 16 or 24
	);

	// Used to transform world space to texture space for shadow sampling
	const Math::Matrix4& GetShadowMatrix() const { return m_ShadowMatrix; }

private:
	Math::Matrix4 m_ShadowMatrix;
};

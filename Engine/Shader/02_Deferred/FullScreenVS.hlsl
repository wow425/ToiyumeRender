struct VSOutput
{
	float4 Position : SV_POSITION;
};

VSOutput MainVS(uint vertexID : SV_VertexID)
{
	VSOutput o;
	
	float2 uv = float2((vertexID << 1) & 2, vertexID & 2);
	// DX12 中的 NDC 规定：X 轴向右为正 $[-1, 1]$，Y 轴向上为正 $[-1, 1]$。（注意：纹理 UV 的 V 轴是向下的）。
	o.Position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
	
	return o;
}

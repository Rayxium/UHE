#include "UHInputs.hlsli"
#include "UHCommon.hlsli"

ByteAddressBuffer OcclusionVisible : register(t3);
StructuredBuffer<float2> UV0Buffer : register(t4);

MotionVertexOutput MotionObjectVS(float3 Position : POSITION, uint Vid : SV_VertexID)
{
	MotionVertexOutput Vout = (MotionVertexOutput)0;

#if WITH_OCCLUSION_TEST
	uint IsVisible = OcclusionVisible.Load(UHInstanceIndex * 4);
	UHBRANCH
		if (IsVisible == 0)
		{
			Vout.Position = -10;
			return Vout;
		}
#endif

	float3 WorldPos = mul(float4(Position, 1.0f), UHWorld).xyz;
	float3 PrevWorldPos = mul(float4(Position, 1.0f), UHPrevWorld).xyz;

	// calculate jitter
	float4x4 JitterMatrix = GetDistanceScaledJitterMatrix(length(WorldPos - UHCameraPos));

	// pass through the vertex data
	Vout.Position = mul(float4(WorldPos, 1.0f), UHViewProj_NonJittered);
	Vout.Position = mul(Vout.Position, JitterMatrix);
	Vout.UV0 = UV0Buffer[Vid];
	Vout.WorldPos = WorldPos;
	Vout.PrevWorldPos = PrevWorldPos;

	return Vout;
}
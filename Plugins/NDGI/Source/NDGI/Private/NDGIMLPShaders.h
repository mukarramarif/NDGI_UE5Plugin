// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RenderGraphResources.h"
#include "ShaderParameterStruct.h"

namespace NDGIMLPShader
{
	inline constexpr uint32 ThreadGroupSize = 64;

	inline bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsPCPlatform(Parameters.Platform)
			&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}
}

/** Per-sample forward pass; outputs follow the layout in NDGIMLP.ush. */
class FNDGIMLPForwardCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIMLPForwardCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIMLPForwardCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, MLPParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleInputs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, ForwardResults)
		SHADER_PARAMETER(uint32, NumSamples)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGIMLPShader::ShouldCompile(Parameters);
	}
};

/** Per-sample backward pass; per-sample gradients follow the layout in NDGIMLP.ush. */
class FNDGIMLPBackwardCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIMLPBackwardCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIMLPBackwardCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, MLPParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleInputs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleTargets)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, SampleGradients)
		SHADER_PARAMETER(uint32, NumSamples)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGIMLPShader::ShouldCompile(Parameters);
	}
};

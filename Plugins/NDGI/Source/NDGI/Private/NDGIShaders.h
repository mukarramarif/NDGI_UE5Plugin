// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/**
 * Phase 0 smoke-test compute shader.
 * Writes OutputBuffer[i] = i * 2 + Offset, so the dispatch path (shader mapping,
 * RDG pass, GPU readback) can be validated end-to-end before real kernels exist.
 */
class FNDGISmokeTestCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGISmokeTestCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGISmokeTestCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, OutputBuffer)
		SHADER_PARAMETER(uint32, Count)
		SHADER_PARAMETER(uint32, Offset)
	END_SHADER_PARAMETER_STRUCT()

	static constexpr uint32 ThreadGroupSize = 64;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsPCPlatform(Parameters.Platform)
			&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}
};

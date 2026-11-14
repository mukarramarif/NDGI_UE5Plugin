// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "RenderGraphResources.h"
#include "ShaderParameterStruct.h"

/**
 * Flat per-tile parameter layout shared with Shaders/Private/NDGITile.ush and
 * NDGI::Reference::FTileReference (see NDGITile.ush for the layout contract).
 */
BEGIN_SHADER_PARAMETER_STRUCT(NDGITileParameters, )
	SHADER_PARAMETER(uint32, UVSize)
	SHADER_PARAMETER(uint32, UVTSize)
	SHADER_PARAMETER(uint32, UTSize)
	SHADER_PARAMETER(uint32, VTSize)
	SHADER_PARAMETER(uint32, TimeSlices)
	SHADER_PARAMETER(uint32, UVBlocksX)
	SHADER_PARAMETER(uint32, UVTBlocksX)
	SHADER_PARAMETER(uint32, MLPOffset)
	SHADER_PARAMETER(uint32, UVE1Offset)
	SHADER_PARAMETER(uint32, UVE2Offset)
	SHADER_PARAMETER(uint32, UVWOffset)
	SHADER_PARAMETER(uint32, UVTE1Offset)
	SHADER_PARAMETER(uint32, UVTE2Offset)
	SHADER_PARAMETER(uint32, UVTWOffset)
	SHADER_PARAMETER(uint32, UTOffset)
	SHADER_PARAMETER(uint32, VTOffset)
	SHADER_PARAMETER(uint32, TotalParams)
END_SHADER_PARAMETER_STRUCT()

namespace NDGITileShader
{
	inline constexpr uint32 ThreadGroupSize = 64;

	inline bool ShouldCompile(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsPCPlatform(Parameters.Platform)
			&& EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}
}

/** One training step for a batch of samples across all tiles (one group per tile). */
class FNDGITrainTileCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGITrainTileCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGITrainTileCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TileParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleCoords)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleTargets)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleNoise)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, GradientInt)
		SHADER_PARAMETER(uint32, NumSteps)
		SHADER_PARAMETER(uint32, BatchSize)
		SHADER_PARAMETER(uint32, StepIndex)
		SHADER_PARAMETER_STRUCT_INCLUDE(NDGITileParameters, TileParams)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGITileShader::ShouldCompile(Parameters);
	}
};

/** Adam update + fixed-point gradient consumption over the flat parameter layout. */
class FNDGIAdamCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIAdamCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIAdamCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, TileParameters)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, MomentsM)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, MomentsV)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<int>, GradientInt)
		SHADER_PARAMETER(uint32, NumElements)
		SHADER_PARAMETER(uint32, TotalParams)
		SHADER_PARAMETER(uint32, NumMLPParameters)
		SHADER_PARAMETER(float, LearningRate)
		SHADER_PARAMETER(float, Beta1)
		SHADER_PARAMETER(float, Beta2)
		SHADER_PARAMETER(float, BetaCorrection1)
		SHADER_PARAMETER(float, BetaCorrection2)
		SHADER_PARAMETER(float, Epsilon)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGITileShader::ShouldCompile(Parameters);
	}
};

/** Forward-only decode kernel (same sampling + MLP as training, no gradients). */
class FNDGIDecodeCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIDecodeCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIDecodeCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TileParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, SampleCoords)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<float>, DecodedOutput)
		SHADER_PARAMETER(uint32, NumSamples)
		SHADER_PARAMETER(uint32, TileIndex)
		SHADER_PARAMETER_STRUCT_INCLUDE(NDGITileParameters, TileParams)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGITileShader::ShouldCompile(Parameters);
	}
};

/** Decode into a 2D render target (editor preview / in-level display). */
class FNDGIDecodeTextureCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIDecodeTextureCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIDecodeTextureCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TileParameters)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)
		SHADER_PARAMETER(uint32, OutputWidth)
		SHADER_PARAMETER(uint32, OutputHeight)
		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER(FVector3f, MeanRGB)
		SHADER_PARAMETER(float, Gamma)
		SHADER_PARAMETER(uint32, TileIndex)
		SHADER_PARAMETER_STRUCT_INCLUDE(NDGITileParameters, TileParams)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return NDGITileShader::ShouldCompile(Parameters);
	}
};

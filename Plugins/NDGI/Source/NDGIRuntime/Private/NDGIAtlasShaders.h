// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GlobalShader.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphResources.h"

/** Decodes one NDGI lightmap atlas and writes the log-encoded HQ coefficients. */
class FNDGIDecodeAtlasCS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FNDGIDecodeAtlasCS);
	SHADER_USE_PARAMETER_STRUCT(FNDGIDecodeAtlasCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float>, TileParameters)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, TileModelIndices)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<float4>, EncodingRanges)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, EncodingIndices)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float4>, SourceTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, FeatureUVTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2DArray<float4>, FeatureUVTTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, OutputTexture)

		SHADER_PARAMETER(uint32, AtlasWidth)
		SHADER_PARAMETER(uint32, AtlasHeight)
		SHADER_PARAMETER(uint32, TileSize)
		SHADER_PARAMETER(uint32, OverlapTexels)
		SHADER_PARAMETER(uint32, GridX)
		SHADER_PARAMETER(uint32, GridY)
		SHADER_PARAMETER(uint32, TimeSlices)
		SHADER_PARAMETER(uint32, TotalParams)
		SHADER_PARAMETER(uint32, bHighQuality)

		SHADER_PARAMETER(float, Time)
		SHADER_PARAMETER(FVector3f, MeanRGB)
		SHADER_PARAMETER(FVector3f, MeanSH)
		SHADER_PARAMETER(uint32, bDirectionalSH)
		SHADER_PARAMETER(float, Gamma)

		SHADER_PARAMETER(uint32, bHasEncodingRanges)

		SHADER_PARAMETER(uint32, UVSize)
		SHADER_PARAMETER(uint32, UVTSize)
		SHADER_PARAMETER(uint32, UTSize)
		SHADER_PARAMETER(uint32, VTSize)
		SHADER_PARAMETER(uint32, LayoutTimeSlices)
		SHADER_PARAMETER(uint32, MLPOffset)
		SHADER_PARAMETER(uint32, UTOffset)
		SHADER_PARAMETER(uint32, VTOffset)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		// This is the production runtime decode shader, so it must be cooked for
		// game targets too. The editor-only training/preview shaders stay behind
		// HasEditorOnlyData, but excluding the decode shader here makes packaged
		// builds fail to find it at runtime.
		return IsPCPlatform(Parameters.Platform);
	}
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Runtime-safe NDGI tile layout definitions shared by training (NDGI) and the
 * runtime decoder (NDGIRuntime). The flat parameter layout matches NDGITile.ush:
 *
 *   MLP (MLPParameterCount) | F_uv E1 | F_uv E2 | F_uv W | F_uvt E1 | F_uvt E2 | F_uvt W | F_ut | F_vt
 */
namespace NDGI
{
	namespace Train
	{
		constexpr int32 MLPInputDim = 16;
		constexpr int32 MLPHiddenDim = 16;
		constexpr int32 MLPOutputDim = 6;
		constexpr int32 MLPParameterCount =
			MLPInputDim * MLPHiddenDim + MLPHiddenDim
			+ MLPHiddenDim * MLPHiddenDim + MLPHiddenDim
			+ MLPOutputDim * MLPHiddenDim + MLPOutputDim;

		struct FTileTrainConfig
		{
			int32 UVSize = 16;
			int32 UVTSize = 8;
			int32 UTSize = 8;
			int32 VTSize = 8;
			int32 TimeSlices = 4;
			int32 NumTiles = 1;
			int32 BatchSize = 64;
		};

		/** Flat layout offsets for one tile. */
		struct FTileLayout
		{
			uint32 UVBlocksX = 0;
			uint32 UVTBlocksX = 0;
			uint32 MLPOffset = 0;
			uint32 UVE1Offset = 0;
			uint32 UVE2Offset = 0;
			uint32 UVWOffset = 0;
			uint32 UVTE1Offset = 0;
			uint32 UVTE2Offset = 0;
			uint32 UVTWOffset = 0;
			uint32 UTOffset = 0;
			uint32 VTOffset = 0;
			uint32 TotalParams = 0;
		};

		NDGIRUNTIME_API FTileLayout ComputeTileLayout(const FTileTrainConfig& Config);

		/** Total flattened parameters per tile for the given dimensions. */
		NDGIRUNTIME_API int32 ComputeTotalParameters(const FTileTrainConfig& Config);
	}
}

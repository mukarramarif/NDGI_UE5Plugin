// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGITileLayout.h"

/**
 * GPU training entry points for NDGI tiles.
 *
 * The flat per-tile parameter layout is defined in NDGITileLayout.h and shared with
 * the runtime decoder (NDGIRuntime) and NDGITile.ush.
 */
namespace NDGI
{
	namespace Train
	{
		/**
		 * Runs NumSteps full training steps for all tiles in one RDG graph:
		 * per step, a train kernel (one threadgroup per tile, one thread per sample)
		 * followed by an Adam kernel over the flat parameter buffer.
		 *
		 * InitialParameters: [NumTiles * TotalParams], float.
		 * SampleCoords:      [NumTiles * NumSteps * BatchSize * 3] (u, v, t).
		 * SampleTargets:     [NumTiles * NumSteps * BatchSize * MLPOutputDim] (normalized RGB + SH).
		 * SampleNoise:       [NumTiles * NumSteps * BatchSize * 12] (already scaled).
		 *
		 * OnComplete receives the updated parameter buffer on the game thread.
		 * FirstAdamStep is the Adam step count already applied (for bias correction).
		 */
		NDGI_API void RunTrainingSteps(
			const FTileTrainConfig& Config,
			const TArray<float>& InitialParameters,
			const TArray<float>& SampleCoords,
			const TArray<float>& SampleTargets,
			const TArray<float>& SampleNoise,
			int32 NumSteps,
			float LearningRate,
			float Beta1,
			float Beta2,
			float Epsilon,
			int32 FirstAdamStep,
			TFunction<void(bool bSuccess, TArray<float>&& OutParameters)> OnComplete);
	}
}

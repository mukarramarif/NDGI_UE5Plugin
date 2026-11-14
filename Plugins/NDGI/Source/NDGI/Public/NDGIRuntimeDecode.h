// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGITileTrain.h"

namespace NDGI
{
	namespace Decode
	{
		/**
		 * Forward-only decode of one tile for a batch of (u, v, t) coordinates.
		 * Uses the same sampling + MLP device code as training.
		 *
		 * Parameters: [NumTiles * TotalParams] flat tile parameters.
		 * SampleCoords: [NumSamples * 3].
		 * OnComplete receives [NumSamples * MLPOutputDim] normalized RGB + SH outputs on the game thread.
		 */
		NDGI_API void DecodeTile(
			const Train::FTileTrainConfig& Config,
			const TArray<float>& Parameters,
			const TArray<float>& SampleCoords,
			int32 TileIndex,
			TFunction<void(bool bSuccess, TArray<float>&& OutOutputs)> OnComplete);
	}
}

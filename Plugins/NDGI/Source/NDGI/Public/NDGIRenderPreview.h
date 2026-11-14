// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGITileTrain.h"

class UTextureRenderTarget2D;

namespace NDGI
{
	namespace Decode
	{
		/**
		 * Decodes a tile into a render target for the editor preview / in-level display.
		 * The output is linear RGB restored through the per-step means and gamma.
		 */
		NDGI_API void DecodeTileToRenderTarget(
			const Train::FTileTrainConfig& Config,
			const TArray<float>& Parameters,
			int32 TileIndex,
			float Time,
			const FVector3f& MeanRGB,
			float Gamma,
			UTextureRenderTarget2D* Target);
	}
}

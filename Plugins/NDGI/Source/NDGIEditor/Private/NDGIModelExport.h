// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"

namespace NDGI
{
	namespace EditorTools
	{
		/**
		 * Finalizes a trained tile (8-bit quantization of the BC-simulated representation),
		 * reconstructs its feature maps, and writes a UNDGIModelAsset package which is then
		 * reloaded and verified.
		 */
		bool WriteModelAsset(
			const FString& PackageName,
			const FString& AssetName,
			Reference::FTileReference& Tile,
			const Reference::FTileConfig& Config,
			const TArray<float>& StepMeans,
			const TArray<float>& SHMeans,
			float Gamma,
			FString& OutError,
			int32 OverlapTexels = 0);
	}
}

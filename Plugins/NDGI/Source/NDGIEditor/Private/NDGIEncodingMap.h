// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGILevelAsset.h"

class UWorld;
class UTexture2D;

namespace NDGI
{
	namespace Editor
	{
		/**
		 * Per-texel lightmap HQ encoding ranges for one atlas. CPU Lightmass gives
		 * every component sharing an atlas its own scale/add, so one atlas-wide
		 * range is not enough to decode or re-encode the texture.
		 */
		struct FEncodingRangeMap
		{
			TArray<FNDGIEncodingRange> Ranges;
			/** Width * Height entries for the coefficient-0 half. */
			TArray<uint8> Indices;
			/** 1 when a triangle covers the texel center (real lit surface). */
			TArray<uint8> Coverage;
			/** Streaming-level packages that reference this atlas. */
			TArray<FString> SourceCells;
			int32 Conflicts = 0;
			int32 Covered = 0;
			int32 RangeCovered = 0;

			bool IsValid() const
			{
				return Ranges.Num() > 0 && Indices.Num() > 0;
			}

			const FNDGIEncodingRange& GetRange(int32 X, int32 Y, int32 Width) const
			{
				check(Ranges.Num() > 0);
				const int32 Index = Indices.IsValidIndex(Y * Width + X) ? Indices[Y * Width + X] : 0;
				return Ranges[FMath::Clamp<int32>(Index, 0, Ranges.Num() - 1)];
			}
		};

		/**
		 * Builds the range map from the static mesh components that reference the
		 * atlas. Ownership is approximated by each component's lightmap-UV
		 * bounding rectangle; packed components normally do not overlap.
		 */
		bool BuildEncodingRangeMap(
			UWorld* World,
			UTexture2D* AtlasTexture,
			int32 Width,
			int32 Height,
			FEncodingRangeMap& OutMap,
			FString* OutError = nullptr);
	}
}

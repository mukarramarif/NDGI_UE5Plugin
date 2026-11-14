// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGILevelAsset.h"

class UTexture2D;
class UWorld;

namespace NDGI
{
	namespace Editor
	{
		namespace LightmapSources
		{
			/** One unique HQ/LQ lightmap texture plus the per-atlas scale/add needed to decode it. */
			struct FAtlasSource
			{
				UTexture2D* Texture = nullptr;
				bool bHighQuality = true;
				FVector4f Scale = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				FVector4f Add = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
				/** Coefficient 1 (directional SH) range for HQ lightmaps. */
				FVector4f ScaleSH = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
				FVector4f AddSH = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			};

			struct FReadback
			{
				TArray<FLinearColor> Pixels;
				int32 Width = 0;
				int32 Height = 0;
				bool bSuccess = false;
			};

			/**
			 * Renders the texture into a transient float target so compressed formats are
			 * hardware-decoded, then reads it back as linear colors. The returned RGBA
			 * values are the stored lightmap texels, not the decoded linear lighting.
			 */
			bool ReadbackTexture(UTexture2D* Texture, FReadback& OutResult);

			/**
			 * Collects the unique lightmap atlases of all static primitives in the world,
			 * reading map build data directly so CPU Lightmass is always used instead of
			 * the GPULightmass static-lighting interface.
			 */
			void CollectLightmapTextures(
				UWorld* World,
				TArray<FAtlasSource>& OutSources,
				int32& OutVirtualTextureCount);

			/** Decodes an HQ coefficient-0/coefficient-1 texel pair. */
			void DecodeHQTexel(
				const FLinearColor& Top,
				const FLinearColor& Bottom,
				const FVector4f& Scale0,
				const FVector4f& Add0,
				const FVector4f& Scale1,
				const FVector4f& Add1,
				FVector3f& OutRGB,
				FVector3f& OutSH);

			/** Decodes an LQ coefficient-0 texel. LQ directional SH is not available. */
			void DecodeLQTexel(
				const FLinearColor& Top,
				const FVector4f& Scale0,
				const FVector4f& Add0,
				FVector3f& OutRGB,
				FVector3f& OutSH);
		}
	}
}

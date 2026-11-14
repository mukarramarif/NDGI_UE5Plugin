// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NDGIModelAsset.generated.h"

/**
 * Final NDGI tile model (paper Section 4 output): reconstructed feature maps,
 * MLP weights, raw F_ut/F_vt, and the preprocessing means needed to restore
 * linear lightmap values at runtime.
 */
UCLASS()
class NDGIRUNTIME_API UNDGIModelAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	/**
	 * F_uv spatial resolution. This is the model's full inference extent: the
	 * tile core plus OverlapTexels on every side.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FIntPoint TileSize = FIntPoint(0, 0);

	/**
	 * Per-side halo in atlas texels. Models trained with overlap cover
	 * TileSize + 2*OverlapTexels texels and contribute to a cross-fade with
	 * their neighbours. 0 = legacy model that covers exactly one tile.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 OverlapTexels = 0;

	/** F_uvt spatial resolution per time slice. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FIntPoint FeatureUVTSize = FIntPoint(0, 0);

	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 TimeSlices = 0;

	/** F_ut spatial (u) resolution. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 UTSize = 0;

	/** F_vt spatial (v) resolution. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 VTSize = 0;

	/**
	 * F_uv texels, RGBA8, row-major (TileSize.x * TileSize.y * 4).
	 *
	 * Editor-only: cooked builds use FeatureUVBC7. Kept so the editor preview,
	 * validation and reference tools can reconstruct a tile without a BC7
	 * decoder. New models always have both representations; legacy cooked models
	 * must be retrained and re-cooked.
	 */
#if WITH_EDITORONLY_DATA
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<uint8> FeatureUV;

	/** F_uvt texels, RGBA8, slice-major (TimeSlices * FeatureUVTSize.x * FeatureUVTSize.y * 4). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<uint8> FeatureUVT;
#endif

	/**
	 * BC7-compressed F_uv, one 2D layer. Byte layout is standard BC7 block
	 * order: 16 bytes per 4x4 block, row-major. Empty on legacy assets.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<uint8> FeatureUVBC7;

	/**
	 * BC7-compressed F_uvt, slice-major. Layer L is the feature slice
	 * L % TimeSlices of model slot L / TimeSlices.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<uint8> FeatureUVTBC7;

	/** Bytes per BC7 layer in FeatureUVBC7 (0 on legacy assets). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 FeatureUVBC7BytesPerLayer = 0;

	/** Bytes per BC7 layer in FeatureUVTBC7 (0 on legacy assets). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 FeatureUVTBC7BytesPerLayer = 0;

	bool HasBC7Features() const
	{
		return FeatureUVBC7.Num() > 0
			&& FeatureUVTBC7.Num() > 0
			&& FeatureUVBC7BytesPerLayer > 0
			&& FeatureUVTBC7BytesPerLayer > 0;
	}

	/** MLP weights in the canonical flat layout (RGB + SH outputs; see NDGITileLayout.h). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> MLPParameters;

	/** Raw F_ut texels. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> UT;

	/** Raw F_vt texels. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> VT;

	/** Per time step, per channel RGB means from preprocessing (used to restore linear values). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> StepMeans;

	/** Per time step, per component SH means (signed; no gamma). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> SHMeans;

	UPROPERTY(EditAnywhere, Category = "NDGI")
	float Gamma = 2.2f;
};

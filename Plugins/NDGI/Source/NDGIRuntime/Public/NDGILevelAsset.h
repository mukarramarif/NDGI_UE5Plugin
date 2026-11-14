// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NDGIModelAsset.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/SoftObjectPtr.h"
#include "NDGILevelAsset.generated.h"

/** One component's lightmap HQ encoding range (per-atlas regions can differ). */
USTRUCT()
struct FNDGIEncodingRange
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Scale0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Add0 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Scale1 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Add1 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
};

/** One lightmap atlas covered by NDGI. */
USTRUCT()
struct FNDGIAtlasEntry
{
	GENERATED_BODY()

	/**
	 * Path of the uncompressed HQ lightmap atlas texture that will be updated at runtime.
	 * Stored as a soft path because lightmap textures live in the level's private built-data package.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI", meta = (AllowedClasses = "/Script/Engine.Texture2D"))
	FSoftObjectPath LightmapTexturePath;

	/** Full texture size (includes both coefficient halves). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FIntPoint AtlasSize = FIntPoint(0, 0);

	/** NDGI tile core size in texels (multiple of 16). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 TileSize = 128;

	/**
	 * Per-side halo in atlas texels used by every model in this atlas. Each
	 * model covers TileSize + 2*OverlapTexels texels; adjacent models are
	 * cross-faded over this band at runtime. 0 = legacy non-overlapping models.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 OverlapTexels = 0;

	/** Tile grid over the coefficient-0 half of the atlas. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FIntPoint TileGrid = FIntPoint(0, 0);

	/**
	 * Grid-indexed soft references to the trained models. The runtime loads
	 * only the pages that are resident, so these are deliberately not hard
	 * references.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<TSoftObjectPtr<UNDGIModelAsset>> TileModelPaths;

#if WITH_EDITORONLY_DATA
	/**
	 * Deprecated hard references from pre-streaming level assets. PostLoad
	 * migrates them into TileModelPaths and clears this array so editor PIE
	 * sessions also stream model packages.
	 */
	UPROPERTY()
	TArray<TObjectPtr<UNDGIModelAsset>> TileModels;
#endif

	/** Resolves the runtime model path for one grid cell (empty = no model). */
	FSoftObjectPath GetTileModelPath(int32 Cell) const
	{
		if (TileModelPaths.IsValidIndex(Cell) && !TileModelPaths[Cell].IsNull())
		{
			return TileModelPaths[Cell].ToSoftObjectPath();
		}
#if WITH_EDITORONLY_DATA
		if (TileModels.IsValidIndex(Cell) && TileModels[Cell])
		{
			return FSoftObjectPath(TileModels[Cell].Get());
		}
#endif
		return FSoftObjectPath();
	}

	/**
	 * World-space bounds of each tile's owning geometry. Used by the runtime
	 * virtual-texture page cache to prioritize resident models by camera
	 * distance. Invalid bounds mean "no geometry" (skipped tiles).
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<FBox> TileBounds;

	/** Fallback atlas-wide lightmap encoding parameters (used when no range map exists). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Scale0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Add0 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Scale1 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
	UPROPERTY(EditAnywhere, Category = "NDGI")
	FVector4f Add1 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

	/**
	 * Distinct lightmap HQ encoding ranges used by components that share this
	 * atlas. CPU Lightmass gives each component its own scale/add even though the
	 * texture is shared, so re-encoding with a single range is wrong.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<FNDGIEncodingRange> EncodingRanges;

	/**
	 * Per-texel range index for the coefficient-0 half (AtlasSize.X *
	 * AtlasSize.Y / 2 entries, row-major). Empty = legacy single-range asset.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<uint8> EncodingRangeIndices;

	/**
	 * Streaming-level packages that own lightmap components in this atlas.
	 * Empty = non-partitioned atlas (always active). Used to gate decoding and
	 * release GPU resources when World Partition cells unload.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<FString> SourceCells;

	UPROPERTY(EditAnywhere, Category = "NDGI")
	bool bHighQuality = true;

	UPROPERTY(EditAnywhere, Category = "NDGI")
	int32 TimeSlices = 0;

	UPROPERTY(EditAnywhere, Category = "NDGI")
	float Gamma = 2.2f;

	/** Per time step, per channel RGB means (same layout as UNDGIModelAsset::StepMeans). */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> StepMeans;

	/** Per time step, per component SH means (signed; no gamma). Empty = SH disabled. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<float> StepMeansSH;
};

/**
 * Runtime NDGI data for one persistent level: every covered lightmap atlas, its
 * tile models, and the encoding parameters required to write decoded lighting
 * back into the engine lightmaps.
 */
UCLASS()
class NDGIRUNTIME_API UNDGILevelAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TArray<FNDGIAtlasEntry> Atlases;

#if WITH_EDITORONLY_DATA
	virtual void PostLoad() override;
#endif
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGILevelAsset.h"

namespace NDGI
{
	/** One time step of a temporal lightmap set: linear RGB + directional SH + validity mask. */
	struct FLightmapStep
	{
		TArray<float> RGB;  // Width * Height * 3, row-major, linear HDR
		TArray<float> SH;   // Width * Height * 3, row-major, decoded Lightmap1.xyz
		TArray<uint8> Mask; // Width * Height, nonzero = valid lightmap texel
	};

	/**
	 * A temporal lightmap set in atlas space plus the training-time preprocessing
	 * (gamma + per-channel per-time mean normalization) described in the paper.
	 * SH is signed, so it is mean-normalized only (no gamma).
	 *
	 * On-disk format (little-endian, engine archive):
	 *   magic/version/width/height/stepCount, then per step:
	 *     v1: float32 RGB, uint8 mask
	 *     v2: float32 RGB, uint8 mask, float32 SH.xyz
	 *     v3: + per-texel lightmap encoding range table (component scale/add)
	 */
	struct FLightmapSet
	{
		static constexpr uint32 MagicValue = 0x4947444eu; // 'NDGI'
		static constexpr uint32 FormatVersion = 3u;

		int32 Width = 0;
		int32 Height = 0;
		TArray<FLightmapStep> Steps;

		/** Per-component HQ ranges used to decode this atlas (v3+). */
		TArray<FNDGIEncodingRange> EncodingRanges;
		/** Width * Height range index per coefficient-0 texel (v3+). */
		TArray<uint8> EncodingRangeIndices;

		/** Filled by Preprocess: per-step per-channel means in gamma space. */
		float Gamma = 2.2f;
		TArray<float> StepMeans;   // [Steps * 3] RGB (gamma space)
		TArray<float> StepMeansSH; // [Steps * 3] SH (linear, signed)

		bool Save(const FString& FilePath) const;
		bool Load(const FString& FilePath);

		bool HasSH() const;

		/** Gamma-corrects RGB and mean-normalizes RGB + SH in place (training space). */
		void Preprocess(float InGamma);

		/** Restores a normalized value to linear space for a given step. */
		void DenormalizeTexel(int32 StepIndex, float& InOutR, float& InOutG, float& InOutB) const;

		/** Restores normalized SH to the engine's signed coefficient space. */
		void DenormalizeTexelSH(int32 StepIndex, float& InOutX, float& InOutY, float& InOutZ) const;

		/**
		 * Generates training/eval samples for one tile: texel-center coordinates in
		 * tile-local [0, 1]^2 plus the time coordinate s / (NumSteps - 1), with the
		 * normalized target. Only valid (masked) texels are sampled.
		 */
		void GenerateTileSamples(
			int32 TileX, int32 TileY, int32 TileSize,
			int32 NumSamples, int32 Seed,
			TArray<FVector>& OutCoords, // (u, v, time)
			TArray<float>& OutTargets, // [NumSamples * 6] (RGB then SH)
			int32 BorderTexels = 0) const;

		/**
		 * Samples an arbitrary atlas-space rectangle. (RegionX, RegionY) is the
		 * rectangle to draw texels from; (CoordOriginX, CoordOriginY) is the
		 * atlas texel that the model maps to u=v=0, and CoordinateSize is the
		 * model extent. They differ when evaluating just the core of a model
		 * that covers a halo. Region coordinates may be negative at atlas
		 * borders; out-of-atlas texels are skipped.
		 *
		 * StepFilter limits sampling to the listed time-slice indices (for
		 * held-out-time validation). An empty filter samples every slice. The
		 * time coordinate always uses the original slice index, so an excluded
		 * slice stays a genuine interpolation target.
		 */
		void GenerateRegionSamples(
			int32 RegionX, int32 RegionY, int32 RegionSizeX, int32 RegionSizeY,
			int32 CoordOriginX, int32 CoordOriginY, int32 CoordinateSize,
			int32 NumSamples, int32 Seed,
			TArray<FVector>& OutCoords,
			TArray<float>& OutTargets,
			const TArray<int32>& StepFilter = TArray<int32>(),
			const TArray<float>& StepTimes = TArray<float>()) const;

		/**
		 * Samples one arbitrary atlas-space rectangle from a fixed baked step,
		 * using an explicit normalized time coordinate. Used by held-out-time
		 * validation to evaluate the excluded slice through a model whose
		 * feature maps have one fewer temporal slice.
		 */
		void GenerateRegionSamplesAtStep(
			int32 RegionX, int32 RegionY, int32 RegionSizeX, int32 RegionSizeY,
			int32 CoordOriginX, int32 CoordOriginY, int32 CoordinateSize,
			int32 StepIndex, float Time,
			int32 NumSamples, int32 Seed,
			TArray<FVector>& OutCoords,
			TArray<float>& OutTargets) const;
	};

	/** Fills OutSet with a deterministic synthetic set for tests (smooth field, masked corner). */
	void GenerateSyntheticLightmapSet(int32 Width, int32 Height, int32 NumSteps, int32 Seed, FLightmapSet& OutSet);
}

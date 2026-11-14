// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGILightmapSet.h"

#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace NDGI
{
	namespace
	{
		void SerializeSet(FArchive& Ar, FLightmapSet& Set)
		{
			uint32 Magic = FLightmapSet::MagicValue;
			uint32 Version = FLightmapSet::FormatVersion;
			int32 StepCount = Set.Steps.Num();
			float Gamma = Set.Gamma;

			Ar << Magic;
			Ar << Version;
			Ar << Set.Width;
			Ar << Set.Height;
			Ar << StepCount;
			Ar << Gamma;

			if (Ar.IsLoading())
			{
				Set.Steps.SetNum(StepCount);
			}

			for (FLightmapStep& Step : Set.Steps)
			{
				int32 RgbCount = Step.RGB.Num();
				int32 MaskCount = Step.Mask.Num();
				int32 ShCount = Step.SH.Num();
				Ar << RgbCount;
				Ar << MaskCount;
				if (Version >= 2)
				{
					Ar << ShCount;
				}
				if (Ar.IsLoading())
				{
					Step.RGB.SetNumUninitialized(RgbCount);
					Step.Mask.SetNumUninitialized(MaskCount);
					if (Version >= 2)
					{
						Step.SH.SetNumUninitialized(ShCount);
					}
				}
				Ar.Serialize(Step.RGB.GetData(), RgbCount * sizeof(float));
				Ar.Serialize(Step.Mask.GetData(), MaskCount * sizeof(uint8));
				if (Version >= 2)
				{
					Ar.Serialize(Step.SH.GetData(), ShCount * sizeof(float));
				}
			}

			if (Version >= 3)
			{
				int32 RangeCount = Set.EncodingRanges.Num();
				Ar << RangeCount;
				if (Ar.IsLoading())
				{
					Set.EncodingRanges.SetNum(RangeCount);
				}
				for (FNDGIEncodingRange& Range : Set.EncodingRanges)
				{
					Ar << Range.Scale0.X << Range.Scale0.Y << Range.Scale0.Z << Range.Scale0.W;
					Ar << Range.Add0.X << Range.Add0.Y << Range.Add0.Z << Range.Add0.W;
					Ar << Range.Scale1.X << Range.Scale1.Y << Range.Scale1.Z << Range.Scale1.W;
					Ar << Range.Add1.X << Range.Add1.Y << Range.Add1.Z << Range.Add1.W;
				}

				int32 IndexCount = Set.EncodingRangeIndices.Num();
				Ar << IndexCount;
				if (Ar.IsLoading())
				{
					Set.EncodingRangeIndices.SetNumUninitialized(IndexCount);
				}
				if (IndexCount > 0)
				{
					Ar.Serialize(Set.EncodingRangeIndices.GetData(), IndexCount * sizeof(uint8));
				}
			}
		}
	}

	bool FLightmapSet::Save(const FString& FilePath) const
	{
		TArray<uint8> Bytes;
		FMemoryWriter Writer(Bytes);
		// Serialize needs a mutable view; const_cast is safe because the writer only reads.
		SerializeSet(Writer, const_cast<FLightmapSet&>(*this));
		return FFileHelper::SaveArrayToFile(Bytes, *FilePath);
	}

	bool FLightmapSet::Load(const FString& FilePath)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *FilePath))
		{
			return false;
		}
		FMemoryReader Reader(Bytes);
		uint32 Magic = 0;
		Reader << Magic;
		if (Magic != MagicValue)
		{
			return false;
		}
		// Rewind and deserialize fully (including the magic).
		Reader.Seek(0);
		SerializeSet(Reader, *this);
		const bool bValid = Width > 0 && Height > 0 && Steps.Num() > 0
			&& Steps[0].RGB.Num() == Width * Height * 3
			&& Steps[0].Mask.Num() == Width * Height;
		if (!bValid)
		{
			Steps.Reset();
			return false;
		}
		// v1 sets have no SH; keep a zeroed array so training/runtime code has a
		// uniform layout.
		for (FLightmapStep& Step : Steps)
		{
			if (Step.SH.Num() != Width * Height * 3)
			{
				Step.SH.SetNumZeroed(Width * Height * 3);
			}
		}
		return true;
	}

	bool FLightmapSet::HasSH() const
	{
		bool bAnyNonZero = false;
		for (const FLightmapStep& Step : Steps)
		{
			if (Step.SH.Num() != Width * Height * 3)
			{
				return false;
			}
			if (!bAnyNonZero)
			{
				for (float Value : Step.SH)
				{
					if (FMath::Abs(Value) > 1e-8f)
					{
						bAnyNonZero = true;
						break;
					}
				}
			}
		}
		return Steps.Num() > 0 && bAnyNonZero;
	}

	void FLightmapSet::Preprocess(float InGamma)
	{
		Gamma = FMath::Max(InGamma, 0.01f);
		const float InvGamma = 1.0f / Gamma;
		StepMeans.SetNumZeroed(Steps.Num() * 3);
		StepMeansSH.SetNumZeroed(Steps.Num() * 3);

		for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
		{
			FLightmapStep& Step = Steps[StepIndex];
			if (Step.SH.Num() != Width * Height * 3)
			{
				Step.SH.SetNumZeroed(Width * Height * 3);
			}

			// Gamma-correct first.
			for (int32 Texel = 0; Texel < Width * Height; ++Texel)
			{
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					float& Value = Step.RGB[Texel * 3 + Channel];
					Value = FMath::Pow(FMath::Max(Value, 0.0f), InvGamma);
				}
			}

			// Per-channel means over valid texels: RGB and signed SH are normalized
			// independently (no gamma on SH, which can be negative).
			double Sum[3] = { 0.0, 0.0, 0.0 };
			double SumSH[3] = { 0.0, 0.0, 0.0 };
			int64 ValidCount = 0;
			for (int32 Texel = 0; Texel < Width * Height; ++Texel)
			{
				if (Step.Mask[Texel] == 0)
				{
					continue;
				}
				++ValidCount;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Sum[Channel] += Step.RGB[Texel * 3 + Channel];
					SumSH[Channel] += Step.SH[Texel * 3 + Channel];
				}
			}
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				const float Mean = (ValidCount > 0) ? static_cast<float>(Sum[Channel] / static_cast<double>(ValidCount)) : 0.0f;
				const float MeanSH = (ValidCount > 0) ? static_cast<float>(SumSH[Channel] / static_cast<double>(ValidCount)) : 0.0f;
				StepMeans[StepIndex * 3 + Channel] = Mean;
				StepMeansSH[StepIndex * 3 + Channel] = MeanSH;
			}

			// Subtract means.
			for (int32 Texel = 0; Texel < Width * Height; ++Texel)
			{
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Step.RGB[Texel * 3 + Channel] -= StepMeans[StepIndex * 3 + Channel];
					Step.SH[Texel * 3 + Channel] -= StepMeansSH[StepIndex * 3 + Channel];
				}
			}
		}
	}

	void FLightmapSet::DenormalizeTexel(int32 StepIndex, float& InOutR, float& InOutG, float& InOutB) const
	{
		const float MeanR = StepMeans[StepIndex * 3 + 0];
		const float MeanG = StepMeans[StepIndex * 3 + 1];
		const float MeanB = StepMeans[StepIndex * 3 + 2];
		InOutR = FMath::Pow(FMath::Max(InOutR + MeanR, 0.0f), Gamma);
		InOutG = FMath::Pow(FMath::Max(InOutG + MeanG, 0.0f), Gamma);
		InOutB = FMath::Pow(FMath::Max(InOutB + MeanB, 0.0f), Gamma);
	}

	void FLightmapSet::DenormalizeTexelSH(int32 StepIndex, float& InOutX, float& InOutY, float& InOutZ) const
	{
		const int32 Base = StepIndex * 3;
		if (StepMeansSH.Num() < Base + 3)
		{
			return;
		}
		InOutX += StepMeansSH[Base + 0];
		InOutY += StepMeansSH[Base + 1];
		InOutZ += StepMeansSH[Base + 2];
	}

	void FLightmapSet::GenerateTileSamples(
		int32 TileX, int32 TileY, int32 TileSize,
		int32 NumSamples, int32 Seed,
		TArray<FVector>& OutCoords, TArray<float>& OutTargets,
		int32 BorderTexels) const
	{
		const int32 Border = FMath::Max(0, BorderTexels);
		const int32 RegionX = TileX * TileSize - Border;
		const int32 RegionY = TileY * TileSize - Border;
		GenerateRegionSamples(
			RegionX, RegionY, TileSize + 2 * Border, TileSize + 2 * Border,
			RegionX, RegionY, TileSize,
			NumSamples, Seed, OutCoords, OutTargets);
	}

	void FLightmapSet::GenerateRegionSamples(
		int32 RegionX, int32 RegionY, int32 RegionSizeX, int32 RegionSizeY,
		int32 CoordOriginX, int32 CoordOriginY, int32 CoordinateSize,
		int32 NumSamples, int32 Seed,
		TArray<FVector>& OutCoords, TArray<float>& OutTargets,
		const TArray<int32>& StepFilter, const TArray<float>& StepTimes) const
	{
		check(Steps.Num() > 0);

		// Resolve the allowed slice list once. Invalid indices are ignored; an
		// empty or fully-invalid filter falls back to every slice. StepTimes is
		// parallel to StepFilter and supplies non-uniform normalized times for
		// held-out validation (a model trained with one fewer temporal slice).
		const float DefaultTimeDenominator = static_cast<float>(FMath::Max(1, Steps.Num() - 1));
		TArray<int32> AllowedSteps;
		TArray<float> AllowedTimes;
		if (StepFilter.Num() > 0)
		{
			AllowedSteps.Reserve(StepFilter.Num());
			AllowedTimes.Reserve(StepFilter.Num());
			for (int32 FilterIndex = 0; FilterIndex < StepFilter.Num(); ++FilterIndex)
			{
				const int32 StepIndex = StepFilter[FilterIndex];
				if (!Steps.IsValidIndex(StepIndex) || AllowedSteps.Contains(StepIndex))
				{
					continue;
				}
				AllowedSteps.Add(StepIndex);
				AllowedTimes.Add(StepTimes.IsValidIndex(FilterIndex)
					? StepTimes[FilterIndex]
					: static_cast<float>(StepIndex) / DefaultTimeDenominator);
			}
		}
		if (AllowedSteps.Num() == 0)
		{
			AllowedSteps.Reserve(Steps.Num());
			AllowedTimes.Reserve(Steps.Num());
			for (int32 StepIndex = 0; StepIndex < Steps.Num(); ++StepIndex)
			{
				AllowedSteps.Add(StepIndex);
				AllowedTimes.Add(StepTimes.IsValidIndex(StepIndex)
					? StepTimes[StepIndex]
					: static_cast<float>(StepIndex) / DefaultTimeDenominator);
			}
		}

		const int32 SizeX = FMath::Max(1, RegionSizeX);
		const int32 SizeY = FMath::Max(1, RegionSizeY);
		const double CoordScale = 1.0 / static_cast<double>(FMath::Max(1, CoordinateSize));
		TArray<FIntPoint> ValidTexels;
		ValidTexels.Reserve(SizeX * SizeY);
		for (int32 Y = 0; Y < SizeY; ++Y)
		{
			for (int32 X = 0; X < SizeX; ++X)
			{
				const int32 AtlasX = RegionX + X;
				const int32 AtlasY = RegionY + Y;
				if (AtlasX >= 0 && AtlasX < Width && AtlasY >= 0 && AtlasY < Height
					&& Steps[0].Mask[AtlasY * Width + AtlasX] != 0)
				{
					ValidTexels.Add(FIntPoint(X, Y));
				}
			}
		}

		OutCoords.Reset();
		OutTargets.Reset();
		if (ValidTexels.Num() == 0)
		{
			return;
		}

		FRandomStream Stream(Seed);
		OutCoords.SetNumUninitialized(NumSamples);
		OutTargets.SetNumUninitialized(NumSamples * 6);
		for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
		{
			const FIntPoint Texel = ValidTexels[Stream.RandRange(0, ValidTexels.Num() - 1)];
			const int32 Choice = Stream.RandRange(0, AllowedSteps.Num() - 1);
			const int32 StepIndex = AllowedSteps[Choice];
			const int32 AtlasX = RegionX + Texel.X;
			const int32 AtlasY = RegionY + Texel.Y;

			OutCoords[SampleIndex] = FVector(
				(static_cast<double>(AtlasX - CoordOriginX) + 0.5) * CoordScale,
				(static_cast<double>(AtlasY - CoordOriginY) + 0.5) * CoordScale,
				static_cast<double>(AllowedTimes[Choice]));

			const int32 AtlasIndex = AtlasY * Width + AtlasX;
			check(AtlasIndex >= 0 && AtlasIndex < Width * Height);
			OutTargets[SampleIndex * 6 + 0] = Steps[StepIndex].RGB[AtlasIndex * 3 + 0];
			OutTargets[SampleIndex * 6 + 1] = Steps[StepIndex].RGB[AtlasIndex * 3 + 1];
			OutTargets[SampleIndex * 6 + 2] = Steps[StepIndex].RGB[AtlasIndex * 3 + 2];
			const bool bHaveSH = Steps[StepIndex].SH.Num() == Width * Height * 3;
			OutTargets[SampleIndex * 6 + 3] = bHaveSH ? Steps[StepIndex].SH[AtlasIndex * 3 + 0] : 0.0f;
			OutTargets[SampleIndex * 6 + 4] = bHaveSH ? Steps[StepIndex].SH[AtlasIndex * 3 + 1] : 0.0f;
			OutTargets[SampleIndex * 6 + 5] = bHaveSH ? Steps[StepIndex].SH[AtlasIndex * 3 + 2] : 0.0f;
		}
	}

	void FLightmapSet::GenerateRegionSamplesAtStep(
		int32 RegionX, int32 RegionY, int32 RegionSizeX, int32 RegionSizeY,
		int32 CoordOriginX, int32 CoordOriginY, int32 CoordinateSize,
		int32 StepIndex, float Time,
		int32 NumSamples, int32 Seed,
		TArray<FVector>& OutCoords, TArray<float>& OutTargets) const
	{
		TArray<int32> StepFilter;
		StepFilter.Add(StepIndex);
		TArray<float> StepTimes;
		StepTimes.Add(Time);
		GenerateRegionSamples(
			RegionX, RegionY, RegionSizeX, RegionSizeY,
			CoordOriginX, CoordOriginY, CoordinateSize,
			NumSamples, Seed, OutCoords, OutTargets, StepFilter, StepTimes);
	}

	void GenerateSyntheticLightmapSet(int32 Width, int32 Height, int32 NumSteps, int32 Seed, FLightmapSet& OutSet)
	{
		FRandomStream Stream(Seed);
		const double FreqU[3] = { 1.0 + Stream.FRand(), 0.5 + 1.5 * Stream.FRand(), 2.0 + Stream.FRand() };
		const double FreqV[3] = { 1.5 + Stream.FRand(), 2.0 + Stream.FRand(), 0.5 + 1.0 * Stream.FRand() };
		const double FreqT[3] = { 0.5 + 0.5 * Stream.FRand(), 1.0, 0.25 + 0.75 * Stream.FRand() };
		const double Phase[3] = { Stream.FRand(), Stream.FRand(), Stream.FRand() };

		OutSet.Width = Width;
		OutSet.Height = Height;
		OutSet.Steps.SetNum(NumSteps);

		const float TimeDenominator = static_cast<float>(FMath::Max(1, NumSteps - 1));
		for (int32 StepIndex = 0; StepIndex < NumSteps; ++StepIndex)
		{
			FLightmapStep& Step = OutSet.Steps[StepIndex];
			Step.RGB.SetNumUninitialized(Width * Height * 3);
			Step.SH.SetNumUninitialized(Width * Height * 3);
			Step.Mask.SetNumUninitialized(Width * Height);

			const double Time = static_cast<double>(StepIndex) / TimeDenominator;
			for (int32 Y = 0; Y < Height; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const double U = (X + 0.5) / Width;
					const double V = (Y + 0.5) / Height;
					const int32 Texel = Y * Width + X;
					// Masked corner to exercise validity handling.
					Step.Mask[Texel] = (X < 8 && Y < 8) ? 0 : 1;
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						const double Value = 0.3 + 0.35
							* FMath::Sin(2.0 * PI * (FreqU[Channel] * U + Phase[Channel]))
							* FMath::Cos(2.0 * PI * FreqV[Channel] * V)
							* FMath::Cos(2.0 * PI * FreqT[Channel] * Time);
						Step.RGB[Texel * 3 + Channel] = static_cast<float>(FMath::Clamp(Value, 0.0, 2.0));

						// Signed SH-like field so the 6-output path is exercised.
						const double SHValue = 0.45
							* FMath::Sin(2.0 * PI * (FreqU[Channel] * U + 0.25 * Channel))
							* FMath::Cos(2.0 * PI * FreqV[Channel] * V)
							* FMath::Cos(2.0 * PI * (FreqT[Channel] * Time + 0.5 * Channel));
						Step.SH[Texel * 3 + Channel] = static_cast<float>(FMath::Clamp(SHValue, -1.0, 1.0));
					}
				}
			}
		}
	}
}

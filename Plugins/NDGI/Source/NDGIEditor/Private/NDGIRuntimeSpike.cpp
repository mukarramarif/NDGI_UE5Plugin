// Copyright Epic Games, Inc. All Rights Reserved.

#include "Algo/Count.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "LightMap.h"
#include "StaticMeshComponentLODInfo.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Paths.h"
#include "NDGI.h"
#include "NDGIAtlasDecoder.h"
#include "NDGIEncodingMap.h"
#include "NDGILevelAsset.h"
#include "NDGILightmapSet.h"
#include "NDGIModelAsset.h"
#include "NDGIPipelineStatus.h"
#include "NDGIReference.h"
#include "NDGIRuntime.h"
#include "NDGITileLayout.h"
#include "NDGITileReference.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "UObject/UObjectIterator.h"

namespace
{
	using namespace NDGI;

	bool ReadbackTexturePixels(UTexture2D* Texture, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Resource || !PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		OutWidth = PlatformData->Mips[0].SizeX;
		OutHeight = PlatformData->Mips[0].SizeY;

		TSharedRef<TArray<FLinearColor>> Pixels = MakeShared<TArray<FLinearColor>>();
		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.RuntimeSpikeReadback"));
		FTextureRHIRef TextureRHI = Resource->GetTextureRHI();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueueSpikeCopy)(
			[Readback, TextureRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TextureRHI);
			});
		FlushRenderingCommands();

		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			ENQUEUE_RENDER_COMMAND(NDGIDeleteSpikeReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGILockSpikeReadback)(
			[Readback, Pixels, Width, Height](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
				if (Data)
				{
					Pixels->SetNumUninitialized(Width * Height);
					for (int32 Y = 0; Y < FMath::Min(BufferHeight, Height); ++Y)
					{
						const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
							static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FFloat16Color));
						for (int32 X = 0; X < Width; ++X)
						{
							(*Pixels)[Y * Width + X] = FLinearColor(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
						}
					}
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();

		OutPixels = MoveTemp(*Pixels);
		return OutPixels.Num() == Width * Height;
	}

	bool ReadbackColorPixels(UTexture2D* Texture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Resource || !PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		OutWidth = PlatformData->Mips[0].SizeX;
		OutHeight = PlatformData->Mips[0].SizeY;

		TSharedRef<TArray<FColor>> Pixels = MakeShared<TArray<FColor>>();
		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.RuntimeSpikeColorReadback"));
		FTextureRHIRef TextureRHI = Resource->GetTextureRHI();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueueSpikeColorCopy)(
			[Readback, TextureRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TextureRHI);
			});
		FlushRenderingCommands();

		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			ENQUEUE_RENDER_COMMAND(NDGIDeleteSpikeColorReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGILockSpikeColorReadback)(
			[Readback, Pixels, Width, Height](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
				if (Data)
				{
					Pixels->SetNumUninitialized(Width * Height);
					for (int32 Y = 0; Y < FMath::Min(BufferHeight, Height); ++Y)
					{
						const FColor* Row = reinterpret_cast<const FColor*>(
							static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FColor));
						for (int32 X = 0; X < Width; ++X)
						{
							(*Pixels)[Y * Width + X] = Row[X];
						}
					}
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();

		OutPixels = MoveTemp(*Pixels);
		return OutPixels.Num() == Width * Height;
	}

	/** Reads raw lightmap texels as [0,1] floats for both RGBA16F and BGRA8 atlases. */
	bool ReadbackRawPixels(UTexture2D* Texture, TArray<FVector4f>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Resource || !PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		OutWidth = PlatformData->Mips[0].SizeX;
		OutHeight = PlatformData->Mips[0].SizeY;
		const EPixelFormat Format = PlatformData->PixelFormat;
		const bool bFloat = Format == PF_FloatRGBA;
		const bool bByte = Format == PF_B8G8R8A8;
		if ((!bFloat && !bByte) || OutWidth <= 0 || OutHeight <= 0)
		{
			return false;
		}

		TSharedRef<TArray<FVector4f>> Pixels = MakeShared<TArray<FVector4f>>();
		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.ValidateSHReadback"));
		FTextureRHIRef TextureRHI = Resource->GetTextureRHI();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueueValidateSHCopy)(
			[Readback, TextureRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TextureRHI);
			});
		FlushRenderingCommands();

		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			ENQUEUE_RENDER_COMMAND(NDGIDeleteValidateSHReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGILockValidateSHReadback)(
			[Readback, Pixels, Width, Height, bFloat](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
				if (Data)
				{
					Pixels->SetNumUninitialized(Width * Height);
					for (int32 Y = 0; Y < FMath::Min(BufferHeight, Height); ++Y)
					{
						if (bFloat)
						{
							const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
								static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FFloat16Color));
							for (int32 X = 0; X < Width; ++X)
							{
								(*Pixels)[Y * Width + X] = FVector4f(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
							}
						}
						else
						{
							const FColor* Row = reinterpret_cast<const FColor*>(
								static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FColor));
							for (int32 X = 0; X < Width; ++X)
							{
								(*Pixels)[Y * Width + X] = FVector4f(
									Row[X].R / 255.0f, Row[X].G / 255.0f, Row[X].B / 255.0f, Row[X].A / 255.0f);
							}
						}
					}
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();

		OutPixels = MoveTemp(*Pixels);
		return OutPixels.Num() == Width * Height;
	}

	bool PrepareForReadback(UTexture2D* Texture, const FIntPoint& AtlasSize)
	{
		if (!Texture)
		{
			return false;
		}
		FTextureResource* Resource = Texture->GetResource();
		const FRHITexture* RHI = Resource ? Resource->GetTextureRHI() : nullptr;
		const bool bFullRes = RHI
			&& static_cast<int32>(RHI->GetSizeX()) >= AtlasSize.X
			&& static_cast<int32>(RHI->GetSizeY()) >= AtlasSize.Y;
		if (!bFullRes)
		{
			Texture->NeverStream = true;
			Texture->UpdateResource();
			FlushRenderingCommands();
		}
		return true;
	}

	/** Extracts coefficient 1 (SH.xyz) for every texel in the coefficient-0 half. */
	void ExtractSH(const TArray<FVector4f>& Pixels, int32 Width, int32 FullHeight,
		const TArray<FNDGIEncodingRange>& Ranges, const TArray<uint8>& RangeIndices,
		const FVector4f& GlobalScale1, const FVector4f& GlobalAdd1,
		TArray<FVector3f>& OutSH, TArray<uint8>* OutValid = nullptr)
	{
		const int32 HalfHeight = FullHeight / 2;
		OutSH.SetNumUninitialized(Width * HalfHeight);
		if (OutValid)
		{
			OutValid->SetNumUninitialized(Width * HalfHeight);
		}
		for (int32 Y = 0; Y < HalfHeight; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Texel = Y * Width + X;
				FVector4f Scale1 = GlobalScale1;
				FVector4f Add1 = GlobalAdd1;
				bool bOwned = true;
				if (RangeIndices.IsValidIndex(Texel))
				{
					const int32 RangeIndex = RangeIndices[Texel];
					if (RangeIndex == 255)
					{
						bOwned = false;
					}
					else if (Ranges.IsValidIndex(RangeIndex))
					{
						Scale1 = Ranges[RangeIndex].Scale1;
						Add1 = Ranges[RangeIndex].Add1;
					}
				}
				if (OutValid)
				{
					(*OutValid)[Texel] = bOwned ? 1 : 0;
				}

				const FVector4f Raw = Pixels[(Y + HalfHeight) * Width + X];
				OutSH[Texel] = FVector3f(
					Raw.X * Scale1.X + Add1.X,
					Raw.Y * Scale1.Y + Add1.Y,
					Raw.Z * Scale1.Z + Add1.Z);
			}
		}
	}

	void RunValidateSH(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float Time = 1.0f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { Time = FCString::Atof(*Arg); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Validating,
			TEXT("Validating SH"), 1, TEXT("Building cache and decoding..."));

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateSH: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			NDGI::Editor::Pipeline::Complete(bPassed,
				bPassed ? TEXT("ValidateSH passed.") : TEXT("ValidateSH failed — see the log."));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString AssetPath = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSH: %s not found - train the level first."), *AssetPath);
			Finish(false);
			return;
		}

		TSharedPtr<Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSH: cache build failed."));
			Finish(false);
			return;
		}

		struct FAtlasSnapshot
		{
			TArray<FVector3f> SH;
			TArray<FVector3f> Predicted;
			TArray<FVector4f> Pixels;
			TArray<uint8> Valid;
			TArray<uint8> Coverage;
			int32 Width = 0;
			int32 Height = 0;
		};

		TArray<FAtlasSnapshot> Snapshots;
		Snapshots.SetNum(LevelAsset->Atlases.Num());
		bool bCaptured = true;
		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			if (!Texture || !PrepareForReadback(Texture, Entry.AtlasSize))
			{
				bCaptured = false;
				continue;
			}
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			if (!ReadbackRawPixels(Texture, Snapshot.Pixels, Snapshot.Width, Snapshot.Height))
			{
				bCaptured = false;
				continue;
			}
			ExtractSH(Snapshot.Pixels, Snapshot.Width, Snapshot.Height,
				Entry.EncodingRanges, Entry.EncodingRangeIndices, Entry.Scale1, Entry.Add1,
				Snapshot.SH, &Snapshot.Valid);

			NDGI::Editor::FEncodingRangeMap CoverageMap;
			FString CoverageError;
			if (NDGI::Editor::BuildEncodingRangeMap(World, Texture, Snapshot.Width, Snapshot.Height / 2, CoverageMap, &CoverageError))
			{
				Snapshot.Coverage = MoveTemp(CoverageMap.Coverage);
			}
		}
		if (!bCaptured)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSH: could not read back every atlas (unsupported format or not resident)."));
			Finish(false);
			return;
		}

		const bool bDecoded = Runtime::DecodeCachedLevel(Cache, Time);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();
		if (!bDecoded)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSH: cached decode failed."));
			Finish(false);
			return;
		}
		UE_LOG(LogNDGI, Display, TEXT("ValidateSH: cache %d atlas(es), %d model(s), %.1f ms, %d skipped."),
			Cache->Entries.Num(), Cache->Tiles, BuildMs, SkippedModels);

		double TotalSquared = 0.0;
		double TotalMax = 0.0;
		double GlobalRange = 0.0;
		int64 TotalValues = 0;
		int64 TotalOutOfRange = 0;
		double WorstPsnr = MAX_dbl;
		bool bAllAtlasCaptured = true;

		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			TArray<FVector4f> AfterPixels;
			int32 AfterWidth = 0;
			int32 AfterHeight = 0;
			if (!Texture || !ReadbackRawPixels(Texture, AfterPixels, AfterWidth, AfterHeight)
				|| AfterWidth != Snapshot.Width || AfterHeight != Snapshot.Height)
			{
				UE_LOG(LogNDGI, Warning, TEXT("ValidateSH: atlas %d readback failed after decode."), Index);
				bAllAtlasCaptured = false;
				continue;
			}
			ExtractSH(AfterPixels, AfterWidth, AfterHeight,
				Entry.EncodingRanges, Entry.EncodingRangeIndices, Entry.Scale1, Entry.Add1,
				Snapshot.Predicted);

			const int32 HalfHeight = Snapshot.Height / 2;
			double AtlasSquared = 0.0;
			double AtlasMax = 0.0;
			double AtlasRange = 0.0;
			int64 AtlasValues = 0;
			int64 AtlasOutOfRange = 0;
			for (int32 Y = 0; Y < HalfHeight; ++Y)
			{
				const int32 TileY = (Y * Entry.TileGrid.Y) / FMath::Max(1, HalfHeight);
				for (int32 X = 0; X < Snapshot.Width; ++X)
				{
					const int32 TileX = (X * Entry.TileGrid.X) / FMath::Max(1, Snapshot.Width);
					const int32 Cell = TileY * Entry.TileGrid.X + TileX;
					if (!Entry.GetTileModelPath(Cell).IsValid())
					{
						continue;
					}
					const int32 Texel = Y * Snapshot.Width + X;
					if (!Snapshot.Valid.IsValidIndex(Texel) || Snapshot.Valid[Texel] == 0)
					{
						continue;
					}
					if (Snapshot.Coverage.Num() == Snapshot.Width * HalfHeight && Snapshot.Coverage[Texel] == 0)
					{
						continue;
					}
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						const float Before = Snapshot.SH[Texel][Channel];
						const float After = Snapshot.Predicted[Texel][Channel];
						const double Diff = static_cast<double>(After) - static_cast<double>(Before);
						AtlasSquared += Diff * Diff;
						AtlasMax = FMath::Max(AtlasMax, FMath::Abs(Diff));
						AtlasRange = FMath::Max(AtlasRange, FMath::Abs(static_cast<double>(Before)));
						const FVector4f Raw = AfterPixels[(Y + HalfHeight) * Snapshot.Width + X];
						const float RawChannel = Channel == 0 ? Raw.X : (Channel == 1 ? Raw.Y : Raw.Z);
						AtlasOutOfRange += (RawChannel < -1e-4f || RawChannel > 1.0f + 1e-4f) ? 1 : 0;
						++AtlasValues;
					}
				}
			}

			const double AtlasMse = AtlasValues > 0 ? AtlasSquared / static_cast<double>(AtlasValues) : 0.0;
			const double AtlasRangeSafe = FMath::Max(AtlasRange, 1e-6);
			const double AtlasPsnr = AtlasMse > 0.0 ? 10.0 * FMath::LogX(10.0, AtlasRangeSafe * AtlasRangeSafe / AtlasMse) : 99.0;
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateSH: atlas %d (%dx%d): %lld values, PSNR %.2f dB, max |err| %.4f, out-of-range %lld."),
				Index, Snapshot.Width, Snapshot.Height, AtlasValues, AtlasPsnr, AtlasMax, AtlasOutOfRange);

			TotalSquared += AtlasSquared;
			TotalMax = FMath::Max(TotalMax, AtlasMax);
			GlobalRange = FMath::Max(GlobalRange, AtlasRange);
			TotalValues += AtlasValues;
			TotalOutOfRange += AtlasOutOfRange;
			if (AtlasValues > 0)
			{
				WorstPsnr = FMath::Min(WorstPsnr, AtlasPsnr);
			}
		}

		const double OverallMse = TotalValues > 0 ? TotalSquared / static_cast<double>(TotalValues) : 0.0;
		const double OverallRange = FMath::Max(GlobalRange, 1e-6);
		const double OverallPsnr = OverallMse > 0.0 ? 10.0 * FMath::LogX(10.0, OverallRange * OverallRange / OverallMse) : 99.0;
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSH: time %.3f, %d atlas(es), %lld values, overall PSNR %.2f dB, worst atlas %.2f dB, max |err| %.4f, out-of-range %lld."),
			Time, LevelAsset->Atlases.Num(), TotalValues, OverallPsnr, WorstPsnr == MAX_dbl ? 0.0 : WorstPsnr, TotalMax, TotalOutOfRange);

		Finish(bAllAtlasCaptured && TotalValues > 0 && OverallPsnr >= 20.0);
	}

	void RunTestCachedDecodeMatch(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float Time = 0.5f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { Time = FCString::Atof(*Arg); }
				++NumericArgIndex;
			}
		}

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("TestCachedDecodeMatch: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString AssetPath = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestCachedDecodeMatch: %s not found."), *AssetPath);
			Finish(false);
			return;
		}

		TSharedPtr<Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 Skipped = 0;
		if (!Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &Skipped))
		{
			UE_LOG(LogNDGI, Error, TEXT("TestCachedDecodeMatch: cache build failed."));
			Finish(false);
			return;
		}
		UE_LOG(LogNDGI, Display, TEXT("TestCachedDecodeMatch: cache %d atlas(es), %d model(s), %.1f ms, %d skipped."),
			Cache->Entries.Num(), Cache->Tiles, BuildMs, Skipped);
		for (const Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			const int32 Step = FMath::Clamp(
				FMath::RoundToInt(Time * FMath::Max(1, Entry.Binding.TimeSlices - 1)),
				0, FMath::Max(0, Entry.Binding.TimeSlices - 1));
			UE_LOG(LogNDGI, Display,
				TEXT("TestCachedDecodeMatch: atlas %dx%d slices %d, step %d, SH means %d, directional %d, meanSH (%.4f %.4f %.4f)."),
				Entry.Binding.AtlasWidth, Entry.Binding.AtlasHeight, Entry.Binding.TimeSlices, Step,
				Entry.StepMeansSH.Num(), Entry.bDirectionalSH ? 1 : 0,
				Entry.StepMeansSH.IsValidIndex(Step) ? Entry.StepMeansSH[Step].X : 0.0f,
				Entry.StepMeansSH.IsValidIndex(Step) ? Entry.StepMeansSH[Step].Y : 0.0f,
				Entry.StepMeansSH.IsValidIndex(Step) ? Entry.StepMeansSH[Step].Z : 0.0f);
		}

		TArray<UTexture2D*> Targets;
		Targets.Reserve(Cache->Entries.Num());
		for (const Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			Targets.Add(Entry.Binding.TargetTexture);
		}

		TArray<TArray<FVector4f>> BeforePixels;
		BeforePixels.SetNum(Cache->Entries.Num());
		bool bCaptured = true;
		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadbackRawPixels(Targets[Index], BeforePixels[Index], Width, Height))
			{
				bCaptured = false;
			}
		}
		if (!bCaptured)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestCachedDecodeMatch: pre-decode readback failed."));
			Finish(false);
			return;
		}

		TArray<TArray<FVector4f>> CachedPixels;
		CachedPixels.SetNum(Cache->Entries.Num());
		Runtime::DecodeCachedLevel(Cache, Time, Targets);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		int64 DecodedChangedValues = 0;
		int64 EmptyTileChangedValues = 0;
		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadbackRawPixels(Targets[Index], CachedPixels[Index], Width, Height))
			{
				bCaptured = false;
				continue;
			}
			if (BeforePixels[Index].Num() == CachedPixels[Index].Num() && Width > 0)
			{
				const Runtime::FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
				const int32 HalfHeight = Height / 2;
				for (int32 Value = 0; Value < CachedPixels[Index].Num(); ++Value)
				{
					const int32 X = Value % Width;
					const int32 Y = Value / Width;
					const int32 LocalY = Y % FMath::Max(1, HalfHeight);
					const int32 TileX = X / FMath::Max(1, Entry.Binding.TileSize);
					const int32 TileY = LocalY / FMath::Max(1, Entry.Binding.TileSize);
					const int32 Cell = TileY * FMath::Max(1, Entry.Binding.AtlasWidth / FMath::Max(1, Entry.Binding.TileSize)) + TileX;
					const bool bHasModel = Entry.Binding.TileModelIndices.IsValidIndex(Cell)
						&& Entry.Binding.TileModelIndices[Cell] >= 0;
					for (int32 Channel = 0; Channel < 4; ++Channel)
					{
						const bool bChanged = FMath::Abs(CachedPixels[Index][Value][Channel] - BeforePixels[Index][Value][Channel]) > 1e-6;
						DecodedChangedValues += bChanged ? 1 : 0;
						EmptyTileChangedValues += (bChanged && !bHasModel) ? 1 : 0;
					}
				}
			}
		}
		if (!bCaptured)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestCachedDecodeMatch: cached readback failed."));
			Finish(false);
			return;
		}

		UE_LOG(LogNDGI, Display,
			TEXT("TestCachedDecodeMatch: decode changed %lld value(s); empty tiles changed %lld value(s)."),
			DecodedChangedValues, EmptyTileChangedValues);
		Finish(DecodedChangedValues > 0 && EmptyTileChangedValues == 0);
	}

	void RunValidateDirectionality(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float Time = 1.0f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { Time = FCString::Atof(*Arg); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Validating,
			TEXT("Validating directionality"), 1, TEXT("Comparing engine and decoded SH..."));

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateDirectionality: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			NDGI::Editor::Pipeline::Complete(bPassed,
				bPassed ? TEXT("Directionality passed.") : TEXT("Directionality failed — see the log."));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString AssetPath = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateDirectionality: %s not found."), *AssetPath);
			Finish(false);
			return;
		}

		TSharedPtr<Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateDirectionality: cache build failed."));
			Finish(false);
			return;
		}

		const TArray<FVector3f> Normals =
		{
			FVector3f(0, 0, 1), FVector3f(0, 1, 0), FVector3f(1, 0, 0),
			FVector3f(-1, 0, 0), FVector3f(0, -1, 0), FVector3f(0, 0, -1),
			FVector3f(1, 1, 1).GetSafeNormal(), FVector3f(-1, 1, 1).GetSafeNormal(),
			FVector3f(1, -1, 1).GetSafeNormal(), FVector3f(-1, -1, 1).GetSafeNormal()
		};

		struct FAtlasSnapshot
		{
			TArray<FVector4f> Pixels;
			int32 Width = 0;
			int32 Height = 0;
			FVector4f Scale1 = FVector4f(1, 1, 1, 1);
			FVector4f Add1 = FVector4f(0, 0, 0, 0);
			TArray<FNDGIEncodingRange> Ranges;
			TArray<uint8> RangeIndices;
			TArray<uint8> Coverage;
			FNDGIEncodingRange GlobalRange;
			TArray<int32> ModelIndices;
			FIntPoint Grid = FIntPoint(0, 0);
			int32 TileSize = 0;
		};

		TArray<FAtlasSnapshot> Snapshots;
		Snapshots.SetNum(LevelAsset->Atlases.Num());
		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			if (!Texture || !PrepareForReadback(Texture, Entry.AtlasSize))
			{
				UE_LOG(LogNDGI, Error, TEXT("ValidateDirectionality: atlas %d not readable."), Index);
				Finish(false);
				return;
			}
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			Snapshot.Scale1 = Entry.Scale1;
			Snapshot.Add1 = Entry.Add1;
			Snapshot.Ranges = Entry.EncodingRanges;
			Snapshot.RangeIndices = Entry.EncodingRangeIndices;
			Snapshot.GlobalRange.Scale0 = Entry.Scale0;
			Snapshot.GlobalRange.Add0 = Entry.Add0;
			Snapshot.GlobalRange.Scale1 = Entry.Scale1;
			Snapshot.GlobalRange.Add1 = Entry.Add1;
			Snapshot.TileSize = Entry.TileSize;
			Snapshot.Grid = Entry.TileGrid;
			Snapshot.ModelIndices.SetNumUninitialized(Entry.TileGrid.X * Entry.TileGrid.Y);
			for (int32 Cell = 0; Cell < Snapshot.ModelIndices.Num(); ++Cell)
			{
				Snapshot.ModelIndices[Cell] = Entry.GetTileModelPath(Cell).IsValid() ? Cell : -1;
			}
			if (!ReadbackRawPixels(Texture, Snapshot.Pixels, Snapshot.Width, Snapshot.Height))
			{
				UE_LOG(LogNDGI, Error, TEXT("ValidateDirectionality: atlas %d readback failed."), Index);
				Finish(false);
				return;
			}

			NDGI::Editor::FEncodingRangeMap CoverageMap;
			FString CoverageError;
			if (NDGI::Editor::BuildEncodingRangeMap(World, Texture, Snapshot.Width, Snapshot.Height / 2, CoverageMap, &CoverageError))
			{
				Snapshot.Coverage = MoveTemp(CoverageMap.Coverage);
			}
		}

		if (!Runtime::DecodeCachedLevel(Cache, Time))
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateDirectionality: cached decode failed."));
			Finish(false);
			return;
		}
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		double SHwMin = MAX_dbl;
		double SHwMax = -MAX_dbl;
		double SHwSum = 0.0;
		double SHwSquared = 0.0;
		double SHwDecodedMin = MAX_dbl;
		double SHwDecodedMax = -MAX_dbl;
		double SHwDiffSum = 0.0;
		double SHwDiffMax = 0.0;
		double DirSum = 0.0;
		double DirMax = 0.0;
		double DirP99 = 0.0;
		double DirP999 = 0.0;
		int64 SHwCount = 0;
		int64 DirCount = 0;
		int64 OutOfRange = 0;

		struct FDirectionalityOutlier
		{
			double Error = 0.0;
			int32 X = 0;
			int32 Y = 0;
			int32 Cell = 0;
			FVector4f Engine = FVector4f::Zero();
			FVector4f Decoded = FVector4f::Zero();
		};
		TArray<FDirectionalityOutlier> DirectionalityOutliers;
		TArray<float> DirectionalityErrors;
		DirectionalityErrors.Reserve(1 << 16);

		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			TArray<FVector4f> AfterPixels;
			int32 AfterWidth = 0;
			int32 AfterHeight = 0;
			if (!Texture || !ReadbackRawPixels(Texture, AfterPixels, AfterWidth, AfterHeight)
				|| AfterWidth != Snapshot.Width || AfterHeight != Snapshot.Height)
			{
				continue;
			}

			auto GetRange = [&Snapshot](int32 X, int32 Y)
			{
				if (Snapshot.Ranges.Num() > 0 && Snapshot.RangeIndices.IsValidIndex(Y * Snapshot.Width + X))
				{
					const int32 RangeIndex = Snapshot.RangeIndices[Y * Snapshot.Width + X];
					if (Snapshot.Ranges.IsValidIndex(RangeIndex))
					{
						return Snapshot.Ranges[RangeIndex];
					}
				}
				return Snapshot.GlobalRange;
			};

			const int32 HalfHeight = Snapshot.Height / 2;
			for (int32 Y = 0; Y < HalfHeight; ++Y)
			{
				const int32 TileY = (Y * Snapshot.Grid.Y) / FMath::Max(1, HalfHeight);
				for (int32 X = 0; X < Snapshot.Width; ++X)
				{
					const int32 TileX = (X * Snapshot.Grid.X) / FMath::Max(1, Snapshot.Width);
					const int32 Cell = TileY * Snapshot.Grid.X + TileX;
					if (!Snapshot.ModelIndices.IsValidIndex(Cell) || Snapshot.ModelIndices[Cell] < 0)
					{
						continue;
					}

					// Atlas padding (no owning component) is preserved by the decoder
					// and has no meaningful decoded SH range.
					if (Snapshot.Ranges.Num() > 0
						&& Snapshot.RangeIndices.IsValidIndex(Y * Snapshot.Width + X)
						&& Snapshot.RangeIndices[Y * Snapshot.Width + X] == 255)
					{
						continue;
					}

					// Only validate texels that a lightmap triangle actually
					// covers; dilation gutter texels have no real lighting.
					if (Snapshot.Coverage.Num() == Snapshot.Width * HalfHeight
						&& Snapshot.Coverage[Y * Snapshot.Width + X] == 0)
					{
						continue;
					}

					const FNDGIEncodingRange Range = GetRange(X, Y);
					const FVector4f Raw = Snapshot.Pixels[(Y + HalfHeight) * Snapshot.Width + X];
					const FVector4f RawAfter = AfterPixels[(Y + HalfHeight) * Snapshot.Width + X];
					const FVector3f SH(
						Raw.X * Range.Scale1.X + Range.Add1.X,
						Raw.Y * Range.Scale1.Y + Range.Add1.Y,
						Raw.Z * Range.Scale1.Z + Range.Add1.Z);
					const float SHw = Raw.W * Range.Scale1.W + Range.Add1.W;
					const FVector3f SHAfter(
						RawAfter.X * Range.Scale1.X + Range.Add1.X,
						RawAfter.Y * Range.Scale1.Y + Range.Add1.Y,
						RawAfter.Z * Range.Scale1.Z + Range.Add1.Z);
					const float SHwAfter = RawAfter.W * Range.Scale1.W + Range.Add1.W;
					SHwMin = FMath::Min(SHwMin, static_cast<double>(SHw));
					SHwMax = FMath::Max(SHwMax, static_cast<double>(SHw));
					SHwSum += SHw;
					SHwSquared += static_cast<double>(SHw) * SHw;
					SHwDecodedMin = FMath::Min(SHwDecodedMin, static_cast<double>(SHwAfter));
					SHwDecodedMax = FMath::Max(SHwDecodedMax, static_cast<double>(SHwAfter));
					const double SHwDiff = FMath::Abs(static_cast<double>(SHwAfter) - static_cast<double>(SHw));
					SHwDiffSum += SHwDiff;
					SHwDiffMax = FMath::Max(SHwDiffMax, SHwDiff);
					++SHwCount;
					OutOfRange += (RawAfter.X < -1e-4f || RawAfter.X > 1.0f + 1e-4f
						|| RawAfter.Y < -1e-4f || RawAfter.Y > 1.0f + 1e-4f
						|| RawAfter.Z < -1e-4f || RawAfter.Z > 1.0f + 1e-4f) ? 1 : 0;

					const FVector4f EngineSH(SH.X, SH.Y, SH.Z, SHw);
					const FVector4f NGDISH(SHAfter.X, SHAfter.Y, SHAfter.Z, SHwAfter);
					for (const FVector3f& Normal : Normals)
					{
						const FVector4f N(Normal.Y, Normal.Z, Normal.X, 1.0f);
						const double EngineDirectionality = FMath::Max(0.0,
							static_cast<double>(EngineSH.X * N.X + EngineSH.Y * N.Y + EngineSH.Z * N.Z + EngineSH.W * N.W));
						const double NGDIirectionality = FMath::Max(0.0,
							static_cast<double>(NGDISH.X * N.X + NGDISH.Y * N.Y + NGDISH.Z * N.Z + NGDISH.W * N.W));
						const double Diff = FMath::Abs(EngineDirectionality - NGDIirectionality);
						DirSum += Diff;
						DirectionalityErrors.Add(static_cast<float>(Diff));
						if (Diff > DirMax)
						{
							DirMax = Diff;
						}
						if (Diff >= 0.15 && DirectionalityOutliers.Num() < 16)
						{
							FDirectionalityOutlier Outlier;
							Outlier.Error = Diff;
							Outlier.X = X;
							Outlier.Y = Y;
							Outlier.Cell = Cell;
							Outlier.Engine = EngineSH;
							Outlier.Decoded = NGDISH;
							DirectionalityOutliers.Add(Outlier);
						}
						++DirCount;
					}
				}
			}
		}

		const double SHwMean = SHwCount > 0 ? SHwSum / static_cast<double>(SHwCount) : 0.0;
		const double SHwRms = SHwCount > 0
			? FMath::Sqrt(FMath::Max(0.0, SHwSquared / static_cast<double>(SHwCount) - SHwMean * SHwMean))
			: 0.0;
		const double SHwDiffMean = SHwCount > 0 ? SHwDiffSum / static_cast<double>(SHwCount) : 1.0;
		const double DirMean = DirCount > 0 ? DirSum / static_cast<double>(DirCount) : 0.0;
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateDirectionality: engine SHw [%.4f .. %.4f] mean %.4f, stddev %.4f; decoded SHw [%.4f .. %.4f] mean |err| %.5f max |err| %.5f; directionality mean |err| %.4f, max |err| %.4f; out-of-range %lld."),
			SHwMin == MAX_dbl ? 0.0 : SHwMin, SHwMax == -MAX_dbl ? 0.0 : SHwMax, SHwMean, SHwRms,
			SHwDecodedMin == MAX_dbl ? 0.0 : SHwDecodedMin, SHwDecodedMax == -MAX_dbl ? 0.0 : SHwDecodedMax,
			SHwDiffMean, SHwDiffMax, DirMean, DirMax, OutOfRange);

		if (DirectionalityErrors.Num() > 0)
		{
			DirectionalityErrors.Sort();
			auto Percentile = [&DirectionalityErrors](double Fraction)
			{
				const int32 Index = FMath::Clamp(
					FMath::FloorToInt(Fraction * (DirectionalityErrors.Num() - 1)), 0, DirectionalityErrors.Num() - 1);
				return DirectionalityErrors[Index];
			};
			const int32 Over015 = Algo::CountIf(DirectionalityErrors, [](float Value) { return Value > 0.15f; });
			const int32 Over020 = Algo::CountIf(DirectionalityErrors, [](float Value) { return Value > 0.20f; });
			const int32 Over030 = Algo::CountIf(DirectionalityErrors, [](float Value) { return Value > 0.30f; });
			DirP99 = Percentile(0.99);
			DirP999 = Percentile(0.999);
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateDirectionality: percentiles p50 %.4f p95 %.4f p99 %.4f p99.9 %.4f; over 0.15: %d, over 0.20: %d, over 0.30: %d of %d."),
				Percentile(0.50), Percentile(0.95), DirP99, DirP999,
				Over015, Over020, Over030, DirectionalityErrors.Num());
		}

		for (const FDirectionalityOutlier& Outlier : DirectionalityOutliers)
		{
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateDirectionality outlier %.4f at (%d,%d) cell %d: engine (%.4f %.4f %.4f %.4f) decoded (%.4f %.4f %.4f %.4f)."),
				Outlier.Error, Outlier.X, Outlier.Y, Outlier.Cell,
				Outlier.Engine.X, Outlier.Engine.Y, Outlier.Engine.Z, Outlier.Engine.W,
				Outlier.Decoded.X, Outlier.Decoded.Y, Outlier.Decoded.Z, Outlier.Decoded.W);
		}

		const bool bSHwMatches = SHwDiffMean < 0.005 && SHwDiffMax < 0.02;
		// Isolated sub-texel UV islands dominate the absolute max, so the gate
		// tracks the distribution plus a sanity bound on the worst value.
		const bool bDirectionalityPassed = bSHwMatches && DirMean < 0.02 && DirP99 < 0.10
			&& DirP999 < 0.20 && DirMax < 0.60 && DirCount > 0;
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateDirectionality gate: mean %.4f (<0.02) %s, p99 %.4f (<0.10) %s, p99.9 %.4f (<0.20) %s, max %.4f (<0.60) %s."),
			DirMean, DirMean < 0.02 ? TEXT("ok") : TEXT("FAIL"),
			DirP99, DirP99 < 0.10 ? TEXT("ok") : TEXT("FAIL"),
			DirP999, DirP999 < 0.20 ? TEXT("ok") : TEXT("FAIL"),
			DirMax, DirMax < 0.60 ? TEXT("ok") : TEXT("FAIL"));
		Finish(bDirectionalityPassed);
	}

	void RunValidateSeams(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float Time = 1.0f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { Time = FCString::Atof(*Arg); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Validating,
			TEXT("Validating seams"), 1, TEXT("Decoding and comparing tile boundaries..."));

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateSeams: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			NDGI::Editor::Pipeline::Complete(bPassed,
				bPassed ? TEXT("Seams passed.") : TEXT("Seams failed — see the log for the residual values."));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString AssetPath = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSeams: %s not found."), *AssetPath);
			Finish(false);
			return;
		}

		TSharedPtr<Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSeams: cache build failed."));
			Finish(false);
			return;
		}

		struct FAtlasSnapshot
		{
			TArray<FVector4f> Pixels;
			int32 Width = 0;
			int32 Height = 0;
			TArray<int32> ModelIndices;
			FIntPoint Grid = FIntPoint(0, 0);
			int32 TileSize = 0;
			/** Bake validity mask for the coefficient-0 half, if available. */
			TArray<uint8> Mask;
			TArray<FNDGIEncodingRange> Ranges;
			TArray<uint8> RangeIndices;
			FNDGIEncodingRange GlobalRange;
		};

		TArray<FAtlasSnapshot> Snapshots;
		Snapshots.SetNum(LevelAsset->Atlases.Num());
		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			if (!Texture || !PrepareForReadback(Texture, Entry.AtlasSize))
			{
				UE_LOG(LogNDGI, Error, TEXT("ValidateSeams: atlas %d not readable."), Index);
				Finish(false);
				return;
			}
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			Snapshot.TileSize = Entry.TileSize;
			Snapshot.Grid = Entry.TileGrid;
			Snapshot.Ranges = Entry.EncodingRanges;
			Snapshot.RangeIndices = Entry.EncodingRangeIndices;
			Snapshot.GlobalRange.Scale0 = Entry.Scale0;
			Snapshot.GlobalRange.Add0 = Entry.Add0;
			Snapshot.GlobalRange.Scale1 = Entry.Scale1;
			Snapshot.GlobalRange.Add1 = Entry.Add1;
			Snapshot.ModelIndices.SetNumUninitialized(Entry.TileGrid.X * Entry.TileGrid.Y);
			for (int32 Cell = 0; Cell < Snapshot.ModelIndices.Num(); ++Cell)
			{
				Snapshot.ModelIndices[Cell] = Entry.GetTileModelPath(Cell).IsValid() ? Cell : -1;
			}
			// The bake's validity mask lets the diagnostic ignore atlas padding,
			// which has no meaningful decoded value and would otherwise dominate.
			// Recompute per-texel coverage from the current world instead of
			// reading the shared bake folder, which may belong to another level.
			NDGI::Editor::FEncodingRangeMap CoverageMap;
			FString CoverageError;
			if (NDGI::Editor::BuildEncodingRangeMap(World, Texture, Snapshot.Width, Snapshot.Height / 2, CoverageMap, &CoverageError)
				&& CoverageMap.Coverage.Num() == Snapshot.Width * (Snapshot.Height / 2))
			{
				Snapshot.Mask = MoveTemp(CoverageMap.Coverage);
			}
			else
			{
				UE_LOG(LogNDGI, Warning, TEXT("ValidateSeams: no validity mask for atlas %d (%s); including padding texels."),
					Index, CoverageError.IsEmpty() ? TEXT("no covered texels") : *CoverageError);
			}
			if (!ReadbackRawPixels(Texture, Snapshot.Pixels, Snapshot.Width, Snapshot.Height))
			{
				UE_LOG(LogNDGI, Error, TEXT("ValidateSeams: atlas %d readback failed."), Index);
				Finish(false);
				return;
			}
		}

		if (!Runtime::DecodeCachedLevel(Cache, Time))
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateSeams: cached decode failed."));
			Finish(false);
			return;
		}
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		double RgbSum = 0.0;
		double RgbMax = 0.0;
		double SHSum = 0.0;
		double SHMax = 0.0;
		int64 RgbCount = 0;
		int64 SHCount = 0;
		int32 Violations = 0;
		int32 SamplesLogged = 0;
		// Boundaries whose baked value is already continuous but whose decoded
		// value is not are the ones that create visible tile seams.
		int64 RgbIntroduced = 0;
		int64 SHIntroduced = 0;
		double RgbIntroducedSum = 0.0;
		double SHIntroducedSum = 0.0;
		// Decoded-space metrics use each texel's own HQ range, so boundaries
		// between components with different ranges are compared fairly.
		double LumSum = 0.0;
		double LumMax = 0.0;
		int64 LumCount = 0;
		int64 LumIntroduced = 0;
		double LumIntroducedSum = 0.0;
		double DecSHSum = 0.0;
		double DecSHMax = 0.0;
		int64 DecSHCount = 0;
		int64 DecSHIntroduced = 0;
		double DecSHIntroducedSum = 0.0;
		double DecSHIntroducedMax = 0.0;
		int64 LumContinuousCount = 0;
		double LumContinuousSum = 0.0;
		int64 DecSHContinuousCount = 0;
		double DecSHContinuousSum = 0.0;

		struct FDecodedSeamSample
		{
			double BeforeDelta = 0.0;
			double AfterDelta = 0.0;
			int32 AX = 0;
			int32 AY = 0;
			int32 BX = 0;
			int32 BY = 0;
			int32 RangeA = -1;
			int32 RangeB = -1;
		};
		TArray<FDecodedSeamSample> IntroducedLum;
		TArray<FDecodedSeamSample> IntroducedSH;

		struct FSeamSample
		{
			double Error = 0.0;
			double BeforeDelta = 0.0;
			double AfterDelta = 0.0;
			int32 AX = 0;
			int32 AY = 0;
			int32 BX = 0;
			int32 BY = 0;
			int32 Channel = 0;
			bool bSH = false;
			FVector4f BeforeA;
			FVector4f BeforeB;
			FVector4f AfterA;
			FVector4f AfterB;
		};
		TArray<FSeamSample> WorstRgb;
		TArray<FSeamSample> WorstSH;

		auto RecordWorst = [](TArray<FSeamSample>& List, const FSeamSample& Sample)
		{
			constexpr int32 MaxSamples = 8;
			if (List.Num() < MaxSamples)
			{
				List.Add(Sample);
				return;
			}
			int32 MinIndex = 0;
			for (int32 SampleIndex = 1; SampleIndex < List.Num(); ++SampleIndex)
			{
				if (List[SampleIndex].Error < List[MinIndex].Error)
				{
					MinIndex = SampleIndex;
				}
			}
			if (Sample.Error > List[MinIndex].Error)
			{
				List[MinIndex] = Sample;
			}
		};

		for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
		{
			const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
			FAtlasSnapshot& Snapshot = Snapshots[Index];
			UTexture2D* Texture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			TArray<FVector4f> AfterPixels;
			int32 AfterWidth = 0;
			int32 AfterHeight = 0;
			if (!Texture || !ReadbackRawPixels(Texture, AfterPixels, AfterWidth, AfterHeight)
				|| AfterWidth != Snapshot.Width || AfterHeight != Snapshot.Height)
			{
				continue;
			}

			const int32 TileSize = Snapshot.TileSize;
			const int32 CoefHeight = Snapshot.Height / 2;
			if (TileSize <= 0 || Snapshot.Grid.X <= 0 || Snapshot.Grid.Y <= 0)
			{
				continue;
			}

			auto GetRange = [&Snapshot](int32 X, int32 MaskY)
			{
				if (Snapshot.Ranges.Num() > 0
					&& Snapshot.RangeIndices.IsValidIndex(MaskY * Snapshot.Width + X))
				{
					const int32 RangeIndex = Snapshot.RangeIndices[MaskY * Snapshot.Width + X];
					if (Snapshot.Ranges.IsValidIndex(RangeIndex))
					{
						return Snapshot.Ranges[RangeIndex];
					}
				}
				return Snapshot.GlobalRange;
			};

			auto DecodeLuminance = [](const FVector4f& Top, const FVector4f& Bottom, const FNDGIEncodingRange& Range)
			{
				double LogL = static_cast<double>(Top.W) + static_cast<double>(Bottom.W) / 255.0 - 0.5 / 255.0;
				LogL = LogL * Range.Scale0.W + Range.Add0.W;
				return FMath::Max(FMath::Exp2(LogL) - 0.01858136, 0.0);
			};

			auto DecodeSH = [](const FVector4f& Bottom, const FNDGIEncodingRange& Range)
			{
				return FVector3f(
					Bottom.X * Range.Scale1.X + Range.Add1.X,
					Bottom.Y * Range.Scale1.Y + Range.Add1.Y,
					Bottom.Z * Range.Scale1.Z + Range.Add1.Z);
			};

			auto CompareBoundary = [&](int32 AX, int32 AY, int32 BX, int32 BY)
			{
				const int32 CoefHeightLocal = Snapshot.Height / 2;
				// Padding texels are not lit; their decoded values are undefined, so
				// only compare boundaries where both sides are valid bake texels.
				if (Snapshot.Mask.Num() == Snapshot.Width * CoefHeightLocal)
				{
					const int32 MaskYA = (AY >= CoefHeightLocal) ? AY - CoefHeightLocal : AY;
					const int32 MaskYB = (BY >= CoefHeightLocal) ? BY - CoefHeightLocal : BY;
					if (Snapshot.Mask[MaskYA * Snapshot.Width + AX] == 0
						|| Snapshot.Mask[MaskYB * Snapshot.Width + BX] == 0)
					{
						return;
					}
				}

				const int32 TexelA = AY * Snapshot.Width + AX;
				const int32 TexelB = BY * Snapshot.Width + BX;
				const FVector4f& BeforeA = Snapshot.Pixels[TexelA];
				const FVector4f& BeforeB = Snapshot.Pixels[TexelB];
				const FVector4f& AfterA = AfterPixels[TexelA];
				const FVector4f& AfterB = AfterPixels[TexelB];
				if (SamplesLogged < 8)
				{
					UE_LOG(LogNDGI, Display,
						TEXT("ValidateSeams sample (%d,%d)-(%d,%d) before (%.4f %.4f %.4f %.4f)/(%.4f %.4f %.4f %.4f) after (%.4f %.4f %.4f %.4f)/(%.4f %.4f %.4f %.4f)."),
						AX, AY, BX, BY,
						BeforeA.X, BeforeA.Y, BeforeA.Z, BeforeA.W, BeforeB.X, BeforeB.Y, BeforeB.Z, BeforeB.W,
						AfterA.X, AfterA.Y, AfterA.Z, AfterA.W, AfterB.X, AfterB.Y, AfterB.Z, AfterB.W);
					++SamplesLogged;
				}
				// Coefficient 0 RGB is the visible color; coefficient 1 RGB is SH.xyz.
				// Alpha (channel 3) is the HQ luminance/constant-basis encoding, not a
				// lighting seam signal, so it is excluded.
				const bool bSHHalf = AY >= CoefHeightLocal;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					const double BeforeDelta = FMath::Abs(static_cast<double>(BeforeA[Channel]) - BeforeB[Channel]);
					const double AfterDelta = FMath::Abs(static_cast<double>(AfterA[Channel]) - AfterB[Channel]);
					const double SeamError = FMath::Abs(AfterDelta - BeforeDelta);
					const bool bIntroduced = BeforeDelta <= (2.0 / 255.0) && AfterDelta >= (4.0 / 255.0);
					if (bSHHalf)
					{
						SHSum += SeamError;
						SHMax = FMath::Max(SHMax, SeamError);
						++SHCount;
						if (bIntroduced)
						{
							++SHIntroduced;
							SHIntroducedSum += AfterDelta;
						}
					}
					else
					{
						RgbSum += SeamError;
						RgbMax = FMath::Max(RgbMax, SeamError);
						++RgbCount;
						if (bIntroduced)
						{
							++RgbIntroduced;
							RgbIntroducedSum += AfterDelta;
						}
					}
					Violations += SeamError > (4.0 / 255.0) ? 1 : 0;

					FSeamSample SeamSample;
					SeamSample.Error = SeamError;
					SeamSample.BeforeDelta = BeforeDelta;
					SeamSample.AfterDelta = AfterDelta;
					SeamSample.AX = AX;
					SeamSample.AY = AY;
					SeamSample.BX = BX;
					SeamSample.BY = BY;
					SeamSample.Channel = Channel;
					SeamSample.bSH = bSHHalf;
					SeamSample.BeforeA = BeforeA;
					SeamSample.BeforeB = BeforeB;
					SeamSample.AfterA = AfterA;
					SeamSample.AfterB = AfterB;
					RecordWorst(bSHHalf ? WorstSH : WorstRgb, SeamSample);
				}

				// Decoded-space comparison using each texel's own range.
				const int32 MaskYA = (AY >= CoefHeightLocal) ? AY - CoefHeightLocal : AY;
				const int32 MaskYB = (BY >= CoefHeightLocal) ? BY - CoefHeightLocal : BY;
				const FNDGIEncodingRange RangeA = GetRange(AX, MaskYA);
				const FNDGIEncodingRange RangeB = GetRange(BX, MaskYB);
				const int32 TopAIndex = MaskYA * Snapshot.Width + AX;
				const int32 BottomAIndex = (MaskYA + CoefHeightLocal) * Snapshot.Width + AX;
				const int32 TopBIndex = MaskYB * Snapshot.Width + BX;
				const int32 BottomBIndex = (MaskYB + CoefHeightLocal) * Snapshot.Width + BX;

				const double LB_A = DecodeLuminance(Snapshot.Pixels[TopAIndex], Snapshot.Pixels[BottomAIndex], RangeA);
				const double LB_B = DecodeLuminance(Snapshot.Pixels[TopBIndex], Snapshot.Pixels[BottomBIndex], RangeB);
				const double LA_A = DecodeLuminance(AfterPixels[TopAIndex], AfterPixels[BottomAIndex], RangeA);
				const double LA_B = DecodeLuminance(AfterPixels[TopBIndex], AfterPixels[BottomBIndex], RangeB);
				const double LogLumBA = FMath::Log2(FMath::Max(LB_A, 1e-5));
				const double LogLumBB = FMath::Log2(FMath::Max(LB_B, 1e-5));
				const double LogLumAA = FMath::Log2(FMath::Max(LA_A, 1e-5));
				const double LogLumAB = FMath::Log2(FMath::Max(LA_B, 1e-5));
				const double LumBeforeDelta = FMath::Abs(LogLumBA - LogLumBB);
				const double LumAfterDelta = FMath::Abs(LogLumAA - LogLumAB);
				const double LumSeamError = FMath::Abs(LumAfterDelta - LumBeforeDelta);
				LumSum += LumSeamError;
				LumMax = FMath::Max(LumMax, LumSeamError);
				++LumCount;
				if (LumBeforeDelta <= 0.005)
				{
					++LumContinuousCount;
					LumContinuousSum += LumAfterDelta;
					if (LumAfterDelta >= 0.02)
					{
						++LumIntroduced;
						LumIntroducedSum += LumAfterDelta;
						FDecodedSeamSample Sample;
						Sample.BeforeDelta = LumBeforeDelta;
						Sample.AfterDelta = LumAfterDelta;
						Sample.AX = AX; Sample.AY = AY; Sample.BX = BX; Sample.BY = BY;
						IntroducedLum.Add(Sample);
					}
				}

				const FVector3f SHB_A = DecodeSH(Snapshot.Pixels[BottomAIndex], RangeA);
				const FVector3f SHB_B = DecodeSH(Snapshot.Pixels[BottomBIndex], RangeB);
				const FVector3f SHA_A = DecodeSH(AfterPixels[BottomAIndex], RangeA);
				const FVector3f SHA_B = DecodeSH(AfterPixels[BottomBIndex], RangeB);
				auto MaxComponentDiff = [](const FVector3f& A, const FVector3f& B)
				{
					return FMath::Max(FMath::Abs(A.X - B.X), FMath::Max(FMath::Abs(A.Y - B.Y), FMath::Abs(A.Z - B.Z)));
				};
				const double DecSHBeforeDelta = MaxComponentDiff(SHB_A, SHB_B);
				const double DecSHAfterDelta = MaxComponentDiff(SHA_A, SHA_B);
				const double DecSHSeamError = FMath::Abs(DecSHAfterDelta - DecSHBeforeDelta);
				DecSHSum += DecSHSeamError;
				DecSHMax = FMath::Max(DecSHMax, DecSHSeamError);
				++DecSHCount;
				if (DecSHBeforeDelta <= (2.0 / 255.0))
				{
					++DecSHContinuousCount;
					DecSHContinuousSum += DecSHAfterDelta;
					if (DecSHAfterDelta >= (4.0 / 255.0))
					{
						++DecSHIntroduced;
						DecSHIntroducedSum += DecSHAfterDelta;
						DecSHIntroducedMax = FMath::Max(DecSHIntroducedMax, DecSHAfterDelta);
						FDecodedSeamSample Sample;
						Sample.BeforeDelta = DecSHBeforeDelta;
						Sample.AfterDelta = DecSHAfterDelta;
						Sample.AX = AX; Sample.AY = AY; Sample.BX = BX; Sample.BY = BY;
						Sample.RangeA = Snapshot.RangeIndices.IsValidIndex(MaskYA * Snapshot.Width + AX)
							? Snapshot.RangeIndices[MaskYA * Snapshot.Width + AX] : -1;
						Sample.RangeB = Snapshot.RangeIndices.IsValidIndex(MaskYB * Snapshot.Width + BX)
							? Snapshot.RangeIndices[MaskYB * Snapshot.Width + BX] : -1;
						IntroducedSH.Add(Sample);
					}
				}
			};

			for (int32 TileY = 0; TileY < Snapshot.Grid.Y; ++TileY)
			{
				for (int32 TileX = 0; TileX < Snapshot.Grid.X; ++TileX)
				{
					const int32 Cell = TileY * Snapshot.Grid.X + TileX;
					if (!Snapshot.ModelIndices.IsValidIndex(Cell) || Snapshot.ModelIndices[Cell] < 0)
					{
						continue;
					}

					// Right neighbour.
					if (TileX + 1 < Snapshot.Grid.X
						&& Snapshot.ModelIndices[Cell + 1] >= 0)
					{
						const int32 XLeft = (TileX + 1) * TileSize - 1;
						// Ignore the outer 4 texels: they are atlas padding, not seams.
						for (int32 LocalY = 4; LocalY < TileSize - 4; ++LocalY)
						{
							const int32 Y = TileY * TileSize + LocalY;
							CompareBoundary(XLeft, Y, XLeft + 1, Y);
							CompareBoundary(XLeft, Y + CoefHeight, XLeft + 1, Y + CoefHeight);
						}
					}

					// Bottom neighbour.
					if (TileY + 1 < Snapshot.Grid.Y
						&& Snapshot.ModelIndices[Cell + Snapshot.Grid.X] >= 0)
					{
						const int32 YTop = (TileY + 1) * TileSize - 1;
						for (int32 LocalX = 4; LocalX < TileSize - 4; ++LocalX)
						{
							const int32 X = TileX * TileSize + LocalX;
							CompareBoundary(X, YTop, X, YTop + 1);
							CompareBoundary(X, YTop + CoefHeight, X, YTop + 1 + CoefHeight);
						}
					}
				}
			}
		}

		const double RgbMean = RgbCount > 0 ? RgbSum / static_cast<double>(RgbCount) : 1.0;
		const double SHMean = SHCount > 0 ? SHSum / static_cast<double>(SHCount) : 1.0;
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSeams: raw RGB mean |seam err| %.5f max %.5f; raw SH mean %.5f max %.5f; %d value(s) over 4/255."),
			RgbMean, RgbMax, SHMean, SHMax, Violations);
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSeams: raw (range-dependent) introduced discontinuity (|before| <= 2/255, |after| >= 4/255): RGB %lld value(s), mean |after| %.5f; SH %lld value(s), mean |after| %.5f."),
			static_cast<int64>(RgbIntroduced), RgbIntroduced > 0 ? RgbIntroducedSum / static_cast<double>(RgbIntroduced) : 0.0,
			static_cast<int64>(SHIntroduced), SHIntroduced > 0 ? SHIntroducedSum / static_cast<double>(SHIntroduced) : 0.0);

		const double LumMean = LumCount > 0 ? LumSum / static_cast<double>(LumCount) : 1.0;
		const double DecSHMean = DecSHCount > 0 ? DecSHSum / static_cast<double>(DecSHCount) : 1.0;
		const double LumContinuousMean = LumContinuousCount > 0 ? LumContinuousSum / static_cast<double>(LumContinuousCount) : 0.0;
		const double DecSHContinuousMean = DecSHContinuousCount > 0 ? DecSHContinuousSum / static_cast<double>(DecSHContinuousCount) : 0.0;
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSeams: decoded luminance mean |seam err| %.5f max %.5f; SH mean %.5f max %.5f; introduced luminance %lld (mean |after| %.5f), SH %lld (mean |after| %.5f)."),
			LumMean, LumMax, DecSHMean, DecSHMax,
			static_cast<int64>(LumIntroduced), LumIntroduced > 0 ? LumIntroducedSum / static_cast<double>(LumIntroduced) : 0.0,
			static_cast<int64>(DecSHIntroduced), DecSHIntroduced > 0 ? DecSHIntroducedSum / static_cast<double>(DecSHIntroduced) : 0.0);
		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSeams: continuous boundaries (n=%lld lum, %lld SH): mean |after| luminance %.5f, SH %.5f."),
			static_cast<int64>(LumContinuousCount), static_cast<int64>(DecSHContinuousCount),
			LumContinuousMean, DecSHContinuousMean);
		for (int32 SampleIndex = 0; SampleIndex < IntroducedSH.Num(); ++SampleIndex)
		{
			const FDecodedSeamSample& Sample = IntroducedSH[SampleIndex];
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateSeams introduced SH (%d,%d)-(%d,%d) ranges %d/%d before %.5f after %.5f."),
				Sample.AX, Sample.AY, Sample.BX, Sample.BY, Sample.RangeA, Sample.RangeB,
				Sample.BeforeDelta, Sample.AfterDelta);
		}
		for (int32 SampleIndex = 0; SampleIndex < IntroducedLum.Num(); ++SampleIndex)
		{
			const FDecodedSeamSample& Sample = IntroducedLum[SampleIndex];
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateSeams introduced luminance (%d,%d)-(%d,%d) before %.5f after %.5f."),
				Sample.AX, Sample.AY, Sample.BX, Sample.BY, Sample.BeforeDelta, Sample.AfterDelta);
		}

		auto GetChannel = [](const FVector4f& Value, int32 Channel)
		{
			return Channel == 0 ? Value.X : (Channel == 1 ? Value.Y : (Channel == 2 ? Value.Z : Value.W));
		};
		auto LogWorst = [&GetChannel](const TCHAR* Label, TArray<FSeamSample>& List)
		{
			List.Sort([](const FSeamSample& A, const FSeamSample& B) { return A.Error > B.Error; });
			for (int32 SampleIndex = 0; SampleIndex < FMath::Min(4, List.Num()); ++SampleIndex)
			{
				const FSeamSample& Sample = List[SampleIndex];
				UE_LOG(LogNDGI, Display,
					TEXT("ValidateSeams %s worst (%.4f) (%d,%d)-(%d,%d) ch%d before %.4f/%.4f after %.4f/%.4f (before delta %.4f, after delta %.4f)."),
					Label, Sample.Error, Sample.AX, Sample.AY, Sample.BX, Sample.BY, Sample.Channel,
					GetChannel(Sample.BeforeA, Sample.Channel), GetChannel(Sample.BeforeB, Sample.Channel),
					GetChannel(Sample.AfterA, Sample.Channel), GetChannel(Sample.AfterB, Sample.Channel),
					Sample.BeforeDelta, Sample.AfterDelta);
			}
		};
		LogWorst(TEXT("RGB"), WorstRgb);
		LogWorst(TEXT("SH"), WorstSH);

		UE_LOG(LogNDGI, Display,
			TEXT("ValidateSeams gate: luminance introduced %lld (==0), SH introduced %lld/%lld (<=0.5%%), SH max introduced %.5f (<=0.0625), continuous SH mean %.5f (<=0.02)."),
			static_cast<int64>(LumIntroduced), static_cast<int64>(DecSHIntroduced), static_cast<int64>(DecSHContinuousCount),
			DecSHIntroducedMax, DecSHContinuousMean);
		Finish(LumCount > 0 && DecSHCount > 0
			&& LumContinuousMean <= 0.01 && DecSHContinuousMean <= 0.02
			&& LumIntroduced == 0
			&& DecSHIntroduced <= FMath::Max<int64>(1, static_cast<int64>(DecSHContinuousCount * 0.005))
			&& DecSHIntroducedMax <= 0.0625);
	}

	/** CPU mirror of the overlapping decode for one atlas texel; useful when a seam looks wrong. */
	void RunDebugAtlasPixel(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 TexIndex = 0;
		int32 PixelX = 0;
		int32 PixelY = 0;
		float Time = 1.0f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				const float Value = FCString::Atof(*Arg);
				if (NumericArgIndex == 0) { TexIndex = FMath::Max(0, static_cast<int32>(Value)); }
				else if (NumericArgIndex == 1) { PixelX = static_cast<int32>(Value); }
				else if (NumericArgIndex == 2) { PixelY = static_cast<int32>(Value); }
				else if (NumericArgIndex == 3) { Time = Value; }
				++NumericArgIndex;
			}
		}

		auto Finish = [bQuitWhenDone]()
		{
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString AssetPath = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset || !LevelAsset->Atlases.IsValidIndex(TexIndex))
		{
			UE_LOG(LogNDGI, Error, TEXT("DebugAtlasPixel: atlas %d not found in %s."), TexIndex, *AssetPath);
			Finish();
			return;
		}

		const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[TexIndex];
		const int32 CoefHeight = Entry.AtlasSize.Y / 2;
		if (PixelX < 0 || PixelX >= Entry.AtlasSize.X || PixelY < 0 || PixelY >= CoefHeight)
		{
			UE_LOG(LogNDGI, Error, TEXT("DebugAtlasPixel: (%d,%d) outside atlas %dx%d coefficient 0."),
				PixelX, PixelY, Entry.AtlasSize.X, CoefHeight);
			Finish();
			return;
		}

		const int32 TileSize = Entry.TileSize;
		const int32 Overlap = FMath::Max(0, Entry.OverlapTexels);
		const int32 Extent = TileSize + 2 * Overlap;
		const int32 CellX = PixelX / FMath::Max(1, TileSize);
		const int32 CellY = PixelY / FMath::Max(1, TileSize);
		const int32 GridX = Entry.TileGrid.X;

		Reference::FTileConfig RefConfig;
		RefConfig.UVSize = Extent;
		RefConfig.UVTSize = Extent / 4;
		RefConfig.UTSize = Extent / 2;
		RefConfig.VTSize = Extent / 2;
		RefConfig.TimeSlices = Entry.TimeSlices;
		RefConfig.bSimulateBC = true;

		Train::FTileTrainConfig Config;
		Config.UVSize = Extent;
		Config.UVTSize = Extent / 4;
		Config.UTSize = Extent / 2;
		Config.VTSize = Extent / 2;
		Config.TimeSlices = Entry.TimeSlices;
		Config.NumTiles = 1;
		Config.BatchSize = 1;

		UE_LOG(LogNDGI, Display,
			TEXT("DebugAtlasPixel: Tex%02d pixel (%d,%d), tile (%d,%d), tile %d, overlap %d, extent %d, time %.3f."),
			TexIndex, PixelX, PixelY, CellX, CellY, TileSize, Overlap, Extent, Time);
		const int32 RangeIndex = Entry.EncodingRangeIndices.IsValidIndex(PixelY * Entry.AtlasSize.X + PixelX)
			? Entry.EncodingRangeIndices[PixelY * Entry.AtlasSize.X + PixelX]
			: 0;
		FNDGIEncodingRange Range;
		if (Entry.EncodingRanges.IsValidIndex(RangeIndex))
		{
			Range = Entry.EncodingRanges[RangeIndex];
		}
		else
		{
			Range.Scale0 = Entry.Scale0;
			Range.Add0 = Entry.Add0;
			Range.Scale1 = Entry.Scale1;
			Range.Add1 = Entry.Add1;
		}
		UE_LOG(LogNDGI, Display,
			TEXT("  encoding range %d/%d S0 (%.5f %.5f %.5f %.5f) A0 (%.5f %.5f %.5f %.5f) S1 (%.5f %.5f %.5f %.5f) A1 (%.5f %.5f %.5f %.5f)."),
			RangeIndex, Entry.EncodingRanges.Num(),
			Range.Scale0.X, Range.Scale0.Y, Range.Scale0.Z, Range.Scale0.W,
			Range.Add0.X, Range.Add0.Y, Range.Add0.Z, Range.Add0.W,
			Range.Scale1.X, Range.Scale1.Y, Range.Scale1.Z, Range.Scale1.W,
			Range.Add1.X, Range.Add1.Y, Range.Add1.Z, Range.Add1.W);

		float SumRGB[3] = { 0.0f, 0.0f, 0.0f };
		float SumSH[3] = { 0.0f, 0.0f, 0.0f };
		float WeightSum = 0.0f;
		for (int32 DY = -1; DY <= 1; ++DY)
		{
			for (int32 DX = -1; DX <= 1; ++DX)
			{
				const int32 NX = CellX + DX;
				const int32 NY = CellY + DY;
				if (Entry.TileGrid.X <= 0 || Entry.TileGrid.Y <= 0
					|| NX < 0 || NX >= Entry.TileGrid.X || NY < 0 || NY >= Entry.TileGrid.Y)
				{
					continue;
				}
				const int32 Cell = NY * GridX + NX;
				const FSoftObjectPath ModelPath = Entry.GetTileModelPath(Cell);
				UNDGIModelAsset* Model = ModelPath.IsValid()
					? Cast<UNDGIModelAsset>(ModelPath.TryLoad())
					: nullptr;
				if (!Model)
				{
					continue;
				}
				const int32 OriginX = NX * TileSize - Overlap;
				const int32 OriginY = NY * TileSize - Overlap;
				if (PixelX < OriginX || PixelX >= OriginX + Extent || PixelY < OriginY || PixelY >= OriginY + Extent)
				{
					continue;
				}

				const float LX = static_cast<float>(PixelX - OriginX) + 0.5f;
				const float LY = static_cast<float>(PixelY - OriginY) + 0.5f;
				float WX = 1.0f;
				float WY = 1.0f;
				if (Overlap > 0)
				{
					WX = FMath::Clamp(FMath::Min(LX, static_cast<float>(Extent) - LX) / Overlap, 0.0f, 1.0f);
					WY = FMath::Clamp(FMath::Min(LY, static_cast<float>(Extent) - LY) / Overlap, 0.0f, 1.0f);
				}
				const float Weight = WX * WY;
				if (Weight <= 0.0f)
				{
					continue;
				}

				TArray<float> PackedParameters;
				if (!Runtime::BuildModelParameters(*Model, Config, PackedParameters))
				{
					UE_LOG(LogNDGI, Warning, TEXT("DebugAtlasPixel: model (%d,%d) could not be packed."), NX, NY);
					continue;
				}
				TArray<double> Flat;
				Flat.SetNumUninitialized(PackedParameters.Num());
				for (int32 Index = 0; Index < PackedParameters.Num(); ++Index)
				{
					Flat[Index] = static_cast<double>(PackedParameters[Index]);
				}
				Reference::FTileReference Tile(RefConfig, 0);
				Tile.ImportFlatParameters(Flat);

				TArray<FVector> Points;
				Points.Add(FVector(LX / Extent, LY / Extent, Time));
				TArray<float> Output;
				Tile.EvaluatePoints(Points, Output);

				UE_LOG(LogNDGI, Display,
					TEXT("  model (%d,%d) cell %d weight %.4f u %.5f v %.5f -> RGB (%.4f %.4f %.4f) SH (%.4f %.4f %.4f)."),
					NX, NY, Cell, Weight, LX / Extent, LY / Extent,
					Output[0], Output[1], Output[2], Output[3], Output[4], Output[5]);

				SumRGB[0] += Weight * Output[0];
				SumRGB[1] += Weight * Output[1];
				SumRGB[2] += Weight * Output[2];
				SumSH[0] += Weight * Output[3];
				SumSH[1] += Weight * Output[4];
				SumSH[2] += Weight * Output[5];
				WeightSum += Weight;
			}
		}

		if (WeightSum <= 0.0f)
		{
			UE_LOG(LogNDGI, Display, TEXT("  no model covers this texel."));
			Finish();
			return;
		}

		const float StepF = Time * static_cast<float>(FMath::Max(1, Entry.TimeSlices - 1));
		const int32 Step0 = FMath::Clamp(FMath::FloorToInt(StepF), 0, FMath::Max(0, Entry.TimeSlices - 1));
		const int32 Step1 = FMath::Min(Step0 + 1, FMath::Max(0, Entry.TimeSlices - 1));
		const float Frac = FMath::Clamp(StepF - static_cast<float>(Step0), 0.0f, 1.0f);
		auto LerpMean = [Frac](const TArray<float>& Means, int32 A, int32 B, int32 Channel)
		{
			if (!Means.IsValidIndex(A * 3 + Channel) || !Means.IsValidIndex(B * 3 + Channel))
			{
				return 0.0f;
			}
			return FMath::Lerp(Means[A * 3 + Channel], Means[B * 3 + Channel], Frac);
		};

		const FVector3f Normalized(SumRGB[0] / WeightSum, SumRGB[1] / WeightSum, SumRGB[2] / WeightSum);
		const FVector3f MeanRGB(
			LerpMean(Entry.StepMeans, Step0, Step1, 0),
			LerpMean(Entry.StepMeans, Step0, Step1, 1),
			LerpMean(Entry.StepMeans, Step0, Step1, 2));
		const FVector3f Linear(
			FMath::Pow(FMath::Max(Normalized.X + MeanRGB.X, 0.0f), Entry.Gamma),
			FMath::Pow(FMath::Max(Normalized.Y + MeanRGB.Y, 0.0f), Entry.Gamma),
			FMath::Pow(FMath::Max(Normalized.Z + MeanRGB.Z, 0.0f), Entry.Gamma));
		const float L = FMath::Max(Linear.X * 0.3f + Linear.Y * 0.59f + Linear.Z * 0.11f, 1e-4f);
		const float LogL = FMath::Log2(L + 0.01858136f);
		const FVector3f UVW = Linear / L;

		const FVector3f MeanSH(
			LerpMean(Entry.StepMeansSH, Step0, Step1, 0),
			LerpMean(Entry.StepMeansSH, Step0, Step1, 1),
			LerpMean(Entry.StepMeansSH, Step0, Step1, 2));
		const FVector3f SH(SumSH[0] / WeightSum + MeanSH.X, SumSH[1] / WeightSum + MeanSH.Y, SumSH[2] / WeightSum + MeanSH.Z);

		auto EncodeRGB = [](float Value, float Scale, float Add)
		{
			return FMath::Sqrt(FMath::Max((Value - Add) / Scale, 0.0f));
		};
		const FVector3f Raw0RGB(
			EncodeRGB(UVW.X, Range.Scale0.X, Range.Add0.X),
			EncodeRGB(UVW.Y, Range.Scale0.Y, Range.Add0.Y),
			EncodeRGB(UVW.Z, Range.Scale0.Z, Range.Add0.Z));
		const FVector3f Raw1RGB(
			FMath::Clamp((SH.X - Range.Add1.X) / Range.Scale1.X, 0.0f, 1.0f),
			FMath::Clamp((SH.Y - Range.Add1.Y) / Range.Scale1.Y, 0.0f, 1.0f),
			FMath::Clamp((SH.Z - Range.Add1.Z) / Range.Scale1.Z, 0.0f, 1.0f));

		UE_LOG(LogNDGI, Display,
			TEXT("  blended normalized (%.4f %.4f %.4f) mean (%.4f %.4f %.4f) linear (%.4f %.4f %.4f) L %.5f LogL %.5f."),
			Normalized.X, Normalized.Y, Normalized.Z, MeanRGB.X, MeanRGB.Y, MeanRGB.Z,
			Linear.X, Linear.Y, Linear.Z, L, LogL);
		UE_LOG(LogNDGI, Display,
			TEXT("  encoded coefficient0 RGB (%.4f %.4f %.4f), coefficient1 RGB (%.4f %.4f %.4f)."),
			Raw0RGB.X, Raw0RGB.Y, Raw0RGB.Z, Raw1RGB.X, Raw1RGB.Y, Raw1RGB.Z);
		Finish();
	}

	/**
	 * Reports how many distinct lightmap encoding ranges share each atlas. NDGI's
	 * runtime encoder currently assumes one range per atlas; if components differ,
	 * the same linear color needs different raw texel values in different regions.
	 */
	void RunInspectLightmapScales(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		const bool bQuitWhenDone = Args.ContainsByPredicate([](const FString& Arg)
		{
			return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		});

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogNDGI, Error, TEXT("InspectLightmapScales: no editor world."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		struct FAtlasScales
		{
			int32 Components = 0;
			TMap<FString, int32> UniqueRanges;
			TArray<FString> ComponentNames;
		};
		TMap<UTexture2D*, FAtlasScales> Atlases;

		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World || Component->Mobility != EComponentMobility::Static || Component->LODData.Num() == 0)
			{
				continue;
			}
			const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
			if (!BuildData || !BuildData->LightMap)
			{
				continue;
			}
			const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
			if (!LightMap2D || !LightMap2D->IsValid(0))
			{
				continue;
			}
			UTexture2D* Texture = const_cast<UTexture2D*>(LightMap2D->GetTexture(0));
			if (!Texture)
			{
				continue;
			}
			const FString Range = FString::Printf(
				TEXT("S0(%.5f %.5f %.5f %.5f) A0(%.5f %.5f %.5f %.5f) S1(%.5f %.5f %.5f %.5f) A1(%.5f %.5f %.5f %.5f)"),
				LightMap2D->ScaleVectors[0].X, LightMap2D->ScaleVectors[0].Y, LightMap2D->ScaleVectors[0].Z, LightMap2D->ScaleVectors[0].W,
				LightMap2D->AddVectors[0].X, LightMap2D->AddVectors[0].Y, LightMap2D->AddVectors[0].Z, LightMap2D->AddVectors[0].W,
				LightMap2D->ScaleVectors[1].X, LightMap2D->ScaleVectors[1].Y, LightMap2D->ScaleVectors[1].Z, LightMap2D->ScaleVectors[1].W,
				LightMap2D->AddVectors[1].X, LightMap2D->AddVectors[1].Y, LightMap2D->AddVectors[1].Z, LightMap2D->AddVectors[1].W);

			FAtlasScales& Atlas = Atlases.FindOrAdd(Texture);
			++Atlas.Components;
			Atlas.UniqueRanges.FindOrAdd(Range) += 1;
			Atlas.ComponentNames.Add(Component->GetPathName());
		}

		TArray<UTexture2D*> Textures;
		Atlases.GetKeys(Textures);
		Textures.Sort([](const UTexture2D& A, const UTexture2D& B) { return A.GetPathName() < B.GetPathName(); });
		for (UTexture2D* Texture : Textures)
		{
			const FAtlasScales& Atlas = Atlases.FindChecked(Texture);
			UE_LOG(LogNDGI, Display,
				TEXT("InspectLightmapScales: %s: %d component(s), %d distinct encoding range(s)."),
				*Texture->GetPathName(), Atlas.Components, Atlas.UniqueRanges.Num());
			int32 RangeIndex = 0;
			for (const TPair<FString, int32>& Pair : Atlas.UniqueRanges)
			{
				UE_LOG(LogNDGI, Display, TEXT("  [%d] x%d %s"), RangeIndex++, Pair.Value, *Pair.Key);
				if (RangeIndex >= 8)
				{
					UE_LOG(LogNDGI, Display, TEXT("  ..."));
					break;
				}
			}
		}

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void RunRuntimeSpike(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
		}

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("Runtime integration spike: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UNDGIModelAsset* Model = LoadObject<UNDGIModelAsset>(nullptr, TEXT("/Game/NDGI/TestModel.TestModel"));
		if (!Model)
		{
			UE_LOG(LogNDGI, Error, TEXT("RuntimeSpike: model not found - run ndgi.ExportTile first."));
			Finish(false);
			return;
		}

		Train::FTileTrainConfig Config;
		Config.UVSize = Model->TileSize.X;
		Config.UVTSize = Model->FeatureUVTSize.X;
		Config.UTSize = Model->UTSize;
		Config.VTSize = Model->VTSize;
		Config.TimeSlices = Model->TimeSlices;
		Config.NumTiles = 1;
		Config.BatchSize = 1;

		TArray<float> PackedParameters;
		if (!Runtime::BuildModelParameters(*Model, Config, PackedParameters))
		{
			UE_LOG(LogNDGI, Error, TEXT("RuntimeSpike: could not pack model parameters."));
			Finish(false);
			return;
		}

		const int32 TileSize = Config.UVSize;
		UTexture2D* TargetTexture = UTexture2D::CreateTransient(TileSize, TileSize * 2, PF_FloatRGBA);
		if (!TargetTexture)
		{
			UE_LOG(LogNDGI, Error, TEXT("RuntimeSpike: could not create transient RGBA16F texture."));
			Finish(false);
			return;
		}
		TargetTexture->SetForceMipLevelsToBeResident(3600.0f);
		FlushRenderingCommands();

		Runtime::FAtlasDecodeBinding Binding;
		Binding.TargetTexture = TargetTexture;
		Binding.AtlasWidth = TileSize;
		Binding.AtlasHeight = TileSize * 2;
		Binding.TileSize = TileSize;
		Binding.TimeSlices = Config.TimeSlices;
		Binding.bHighQuality = true;
		// Use a conservative encoding range so the log-encoded values fit in [0,1]:
		// LogL = rawA * 16 - 8 (luminance up to 2^8), UVW = rawRGB^2 * 16.
		Binding.Scale0 = FVector4f(16.0f, 16.0f, 16.0f, 16.0f);
		Binding.Add0 = FVector4f(0.0f, 0.0f, 0.0f, -8.0f);
		Binding.Scale1 = FVector4f(1, 1, 1, 1);
		Binding.Add1 = FVector4f(0, 0, 0, 0);
		Binding.Gamma = Model->Gamma;
		Binding.TileModelIndices.Add(0);
		Binding.PackedParameters = PackedParameters;

		const float Time = 0.37f;
		if (!Runtime::DecodeAtlas(Binding, Time))
		{
			UE_LOG(LogNDGI, Error, TEXT("RuntimeSpike: DecodeAtlas failed."));
			Finish(false);
			return;
		}
		FlushRenderingCommands();

		TArray<FLinearColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		if (!ReadbackTexturePixels(TargetTexture, Pixels, Width, Height) || Width != TileSize || Height != TileSize * 2)
		{
			UE_LOG(LogNDGI, Error, TEXT("RuntimeSpike: readback failed."));
			Finish(false);
			return;
		}

		// CPU reference from the same flat parameters.
		Reference::FTileConfig RefConfig;
		RefConfig.UVSize = Config.UVSize;
		RefConfig.UVTSize = Config.UVTSize;
		RefConfig.UTSize = Config.UTSize;
		RefConfig.VTSize = Config.VTSize;
		RefConfig.TimeSlices = Config.TimeSlices;
		RefConfig.bSimulateBC = true;
		Reference::FTileReference Tile(RefConfig, 0);

		TArray<double> FlatDoubles;
		FlatDoubles.SetNumUninitialized(PackedParameters.Num());
		for (int32 Index = 0; Index < PackedParameters.Num(); ++Index)
		{
			FlatDoubles[Index] = static_cast<double>(PackedParameters[Index]);
		}
		Tile.ImportFlatParameters(FlatDoubles);

		TArray<FVector> Points;
		Points.SetNumUninitialized(TileSize * TileSize);
		for (int32 Y = 0; Y < TileSize; ++Y)
		{
			for (int32 X = 0; X < TileSize; ++X)
			{
				Points[Y * TileSize + X] = FVector(
					(static_cast<double>(X) + 0.5) / TileSize,
					(static_cast<double>(Y) + 0.5) / TileSize,
					static_cast<double>(Time));
			}
		}
		TArray<float> CpuNormalized;
		Tile.EvaluatePoints(Points, CpuNormalized);

		const int32 Step = FMath::Clamp(FMath::RoundToInt(Time * FMath::Max(1, Config.TimeSlices - 1)), 0, Config.TimeSlices - 1);
		FVector3f Mean = FVector3f::ZeroVector;
		if (Model->StepMeans.Num() >= (Step + 1) * 3)
		{
			Mean = FVector3f(Model->StepMeans[Step * 3 + 0], Model->StepMeans[Step * 3 + 1], Model->StepMeans[Step * 3 + 2]);
		}

		double MaxAbsError = 0.0;
		int32 Violations = 0;
		int32 BottomViolations = 0;
		for (int32 Y = 0; Y < TileSize; ++Y)
		{
			for (int32 X = 0; X < TileSize; ++X)
			{
				const FLinearColor Raw0 = Pixels[Y * TileSize + X];
				const FLinearColor Raw1 = Pixels[(Y + TileSize) * TileSize + X];

				// Bottom half must be the forced SH constant (0,0,0,1).
				if (FMath::Abs(Raw1.R) > 1e-3f || FMath::Abs(Raw1.G) > 1e-3f || FMath::Abs(Raw1.B) > 1e-3f || FMath::Abs(Raw1.A - 1.0f) > 1e-3f)
				{
					++BottomViolations;
				}

				// Decode the HQ coefficients with the test encoding range (including the
				// alpha residual carried by coefficient 1, like GetLightMapColorHQ).
				const float Residual = Raw1.A / 255.0f - 0.5f / 255.0f;
				const float LogL = (Raw0.A + Residual) * 16.0f - 8.0f;
				const float L = FMath::Exp2(LogL) - 0.01858136f;
				const FVector3f UVW(Raw0.R * Raw0.R * 16.0f, Raw0.G * Raw0.G * 16.0f, Raw0.B * Raw0.B * 16.0f);
				const FVector3f GpuColor = UVW * FMath::Max(L, 0.0f);

				const int32 Texel = Y * TileSize + X;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					const double Cpu = FMath::Pow(
						FMath::Max(static_cast<double>(CpuNormalized[Texel * Reference::OutputDim + Channel]) + static_cast<double>(Mean[Channel]), 0.0),
						Model->Gamma);
					const double Gpu = Channel == 0 ? GpuColor.X : (Channel == 1 ? GpuColor.Y : GpuColor.Z);
					const double AbsError = FMath::Abs(Cpu - Gpu);
					MaxAbsError = FMath::Max(MaxAbsError, AbsError);
					if (AbsError > 5e-3 + 5e-3 * FMath::Abs(Cpu))
					{
						++Violations;
					}
				}
			}
		}

		// BGRA8 path: the actual storage format of uncompressed CPU Lightmass lightmaps.
		bool bBgraPassed = false;
		int32 MaxByteDiff = 0;
		int32 MaxChannelDiff[4] = { 0, 0, 0, 0 };
		int32 BgraBottomViolations = 0;
		UTexture2D* ColorTexture = UTexture2D::CreateTransient(TileSize, TileSize * 2, PF_B8G8R8A8);
		if (ColorTexture)
		{
			ColorTexture->SetForceMipLevelsToBeResident(3600.0f);
			Runtime::FAtlasDecodeBinding ColorBinding = Binding;
			ColorBinding.TargetTexture = ColorTexture;
			FlushRenderingCommands();
			if (Runtime::DecodeAtlas(ColorBinding, Time))
			{
				FlushRenderingCommands();
				TArray<FColor> ColorPixels;
				int32 ColorWidth = 0;
				int32 ColorHeight = 0;
				if (ReadbackColorPixels(ColorTexture, ColorPixels, ColorWidth, ColorHeight)
					&& ColorWidth == TileSize && ColorHeight == TileSize * 2)
				{
					bBgraPassed = true;
					for (int32 Y = 0; Y < TileSize; ++Y)
					{
						for (int32 X = 0; X < TileSize; ++X)
						{
							const FColor Raw0 = ColorPixels[Y * TileSize + X];
							const FColor Raw1 = ColorPixels[(Y + TileSize) * TileSize + X];
							if (Raw1.R != 0 || Raw1.G != 0 || Raw1.B != 0 || Raw1.A != 255)
							{
								++BgraBottomViolations;
							}

							const int32 Texel = Y * TileSize + X;
							const double Linear[3] =
							{
								FMath::Pow(FMath::Max(static_cast<double>(CpuNormalized[Texel * Reference::OutputDim + 0]) + static_cast<double>(Mean.X), 0.0), Model->Gamma),
								FMath::Pow(FMath::Max(static_cast<double>(CpuNormalized[Texel * Reference::OutputDim + 1]) + static_cast<double>(Mean.Y), 0.0), Model->Gamma),
								FMath::Pow(FMath::Max(static_cast<double>(CpuNormalized[Texel * Reference::OutputDim + 2]) + static_cast<double>(Mean.Z), 0.0), Model->Gamma)
							};
							const double L = FMath::Max(0.3 * Linear[0] + 0.59 * Linear[1] + 0.11 * Linear[2], 1e-4);
							const double LogL = FMath::Log2(L + 0.01858136);
							const double Residual = 1.0 / 255.0 - 0.5 / 255.0;
							const double Expected[4] =
							{
								FMath::Sqrt(FMath::Max(Linear[0] / L / 16.0, 0.0)),
								FMath::Sqrt(FMath::Max(Linear[1] / L / 16.0, 0.0)),
								FMath::Sqrt(FMath::Max(Linear[2] / L / 16.0, 0.0)),
								(LogL + 8.0) / 16.0 - Residual
							};
							const int32 Actual[4] = { Raw0.R, Raw0.G, Raw0.B, Raw0.A };
							for (int32 Channel = 0; Channel < 4; ++Channel)
							{
								const int32 ExpectedByte = FMath::Clamp(FMath::RoundToInt(Expected[Channel] * 255.0), 0, 255);
								const int32 Diff = FMath::Abs(ExpectedByte - Actual[Channel]);
								MaxByteDiff = FMath::Max(MaxByteDiff, Diff);
								MaxChannelDiff[Channel] = FMath::Max(MaxChannelDiff[Channel], Diff);
							}
						}
					}
					bBgraPassed = BgraBottomViolations == 0 && MaxByteDiff <= 2;
				}
			}
		}

		UE_LOG(LogNDGI, Display,
			TEXT("RuntimeSpike: atlas %dx%d, %d models, %d values, %d violations, %d SH violations, max abs err %.3e."),
			Width, Height, 1, TileSize * TileSize * Reference::OutputDim, Violations, BottomViolations, MaxAbsError);
		UE_LOG(LogNDGI, Display,
			TEXT("RuntimeSpike (BGRA8): %s, %d SH violations, max byte diff %d (channels %d/%d/%d/%d)."),
			bBgraPassed ? TEXT("PASSED") : TEXT("FAILED"), BgraBottomViolations, MaxByteDiff,
			MaxChannelDiff[0], MaxChannelDiff[1], MaxChannelDiff[2], MaxChannelDiff[3]);
		Finish(Violations == 0 && BottomViolations == 0 && bBgraPassed);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIRuntimeSpikeCommand(
	TEXT("ndgi.RuntimeSpike"),
	TEXT("Validates the runtime atlas decode + HQ log-encode + texture write-back against the CPU reference. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunRuntimeSpike));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestCachedDecodeMatchCommand(
	TEXT("ndgi.TestCachedDecodeMatch"),
	TEXT("Decodes with the cached direct-write path, then with the legacy readback path, and compares the engine lightmap texels. Usage: ndgi.TestCachedDecodeMatch [time] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestCachedDecodeMatch));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIValidateSeamsCommand(
	TEXT("ndgi.ValidateSeams"),
	TEXT("Compares decoded vs baked lightmaps across tile boundaries. Run at a baked step (e.g. 1.0) on a freshly loaded level. Usage: ndgi.ValidateSeams [time] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunValidateSeams));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIInspectLightmapScalesCommand(
	TEXT("ndgi.InspectLightmapScales"),
	TEXT("Reports distinct lightmap encoding ranges per atlas in the current world. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunInspectLightmapScales));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIDebugAtlasPixelCommand(
	TEXT("ndgi.DebugAtlasPixel"),
	TEXT("CPU-evaluates the overlapping tile models at one atlas texel. Usage: ndgi.DebugAtlasPixel <texIndex> <x> <y> [time] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunDebugAtlasPixel));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIValidateDirectionalityCommand(
	TEXT("ndgi.ValidateDirectionality"),
	TEXT("Compares the engine's SH directionality (including Lightmap1 alpha/SH.w) against the decoded result at a time. Run on a freshly loaded level. Usage: ndgi.ValidateDirectionality [time] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunValidateDirectionality));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIValidateSHCommand(
	TEXT("ndgi.ValidateSH"),
	TEXT("Decodes the level at a time and reports directional SH PSNR against the lightmap's current Lightmap1 contents. Run on a freshly loaded level for an engine-reference comparison. Usage: ndgi.ValidateSH [time] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunValidateSH));

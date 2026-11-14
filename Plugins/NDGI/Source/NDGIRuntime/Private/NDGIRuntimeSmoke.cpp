// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIAtlasDecoder.h"

#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Math/Float16Color.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "NDGIAtlasShaders.h"
#include "NDGIRuntime.h"
#include "NDGILevelAsset.h"
#include "RenderingThread.h"
#include "RHIGPUReadback.h"
#include "TextureResource.h"

namespace
{
	UWorld* FindSmokeWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if ((Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE) && Context.World())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	FString GetSmokeLevelAssetName(UWorld* World)
	{
		const FString MapName = UWorld::RemovePIEPrefix(World->GetName());
		return FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *MapName, *MapName);
	}

	TSet<FString> GatherLoadedCells(UWorld* World)
	{
		TSet<FString> Cells;
		if (!World)
		{
			return Cells;
		}
		for (ULevel* Level : World->GetLevels())
		{
			if (Level)
			{
				Cells.Add(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
			}
		}
		return Cells;
	}

	void RequestSmokeExit()
	{
		if (GIsEditor)
		{
			if (GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		}
		else
		{
			FPlatformMisc::RequestExit(false);
		}
	}

	/** Shipping builds compile out UE_LOG output; the result file is the machine-readable gate. */
	FString GetSmokeResultPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("NDGI/TestRuntimeSmoke.txt"));
	}

	void WriteSmokeState(const FString& State)
	{
		const FString Path = GetSmokeResultPath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		FFileHelper::SaveStringToFile(State, *Path);
	}

	FString FormatSmokeState(const TCHAR* State, const FString& Detail = FString())
	{
		const FString Timestamp = FDateTime::UtcNow().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
		return Detail.IsEmpty()
			? FString::Printf(TEXT("%s %s"), *Timestamp, State)
			: FString::Printf(TEXT("%s %s %s"), *Timestamp, State, *Detail);
	}

	/**
	 * Reads raw lightmap texels as [0,1] floats for both RGBA16F and BGRA8 atlases.
	 * The runtime smoke test uses this instead of the editor-only validation helpers
	 * so a packaged build exercises the exact production write path and readback.
	 */
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
		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.RuntimeSmokeReadback"));
		FTextureRHIRef TextureRHI = Resource->GetTextureRHI();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueueRuntimeSmokeCopy)(
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
			ENQUEUE_RENDER_COMMAND(NDGIDeleteRuntimeSmokeReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGILockRuntimeSmokeReadback)(
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

	float GetValue(const FVector4f& Value, int32 Channel)
	{
		return Channel == 0 ? Value.X : (Channel == 1 ? Value.Y : (Channel == 2 ? Value.Z : Value.W));
	}

	struct FSmokePixelStats
	{
		int64 Changed = 0;
		int64 EmptyTileChanged = 0;
		int64 OutOfRange = 0;
		int64 FiniteViolations = 0;
	};

	void ComparePixels(
		const NDGI::Runtime::FLevelDecodeCache& Cache,
		const TArray<TArray<FVector4f>>& Before,
		const TArray<TArray<FVector4f>>& After,
		FSmokePixelStats& OutStats)
	{
		for (int32 Index = 0; Index < Cache.Entries.Num(); ++Index)
		{
			const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry = Cache.Entries[Index];
			if (!Entry.bActive || !Before.IsValidIndex(Index) || !After.IsValidIndex(Index))
			{
				continue;
			}
			if (Before[Index].Num() == 0 || Before[Index].Num() != After[Index].Num())
			{
				continue;
			}

			const int32 Width = Entry.Binding.AtlasWidth;
			const int32 HalfHeight = FMath::Max(1, Entry.Binding.AtlasHeight / 2);
			const int32 GridX = FMath::Max(1, Width / FMath::Max(1, Entry.Binding.TileSize));
			for (int32 Value = 0; Value < After[Index].Num(); ++Value)
			{
				const int32 X = Value % Width;
				const int32 Y = Value / Width;
				const int32 LocalY = Y % HalfHeight;
				const int32 TileX = X / FMath::Max(1, Entry.Binding.TileSize);
				const int32 TileY = LocalY / FMath::Max(1, Entry.Binding.TileSize);
				const int32 Cell = TileY * GridX + TileX;
				const bool bHasModel = Entry.Binding.TileModelIndices.IsValidIndex(Cell)
					&& Entry.Binding.TileModelIndices[Cell] >= 0;

				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					const float BeforeValue = GetValue(Before[Index][Value], Channel);
					const float AfterValue = GetValue(After[Index][Value], Channel);
					const bool bChanged = FMath::Abs(AfterValue - BeforeValue) > 1e-6f;
					OutStats.Changed += bChanged ? 1 : 0;
					OutStats.EmptyTileChanged += (bChanged && !bHasModel) ? 1 : 0;

					if (!FMath::IsFinite(AfterValue))
					{
						++OutStats.FiniteViolations;
					}
					else if (AfterValue < -1e-4f || AfterValue > 1.0f + 1e-4f)
					{
						++OutStats.OutOfRange;
					}
				}
			}
		}
	}

	int64 CountChangedPixels(const TArray<TArray<FVector4f>>& Before, const TArray<TArray<FVector4f>>& After)
	{
		int64 Changed = 0;
		const int32 Count = FMath::Min(Before.Num(), After.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Before[Index].Num() != After[Index].Num())
			{
				continue;
			}
			for (int32 Value = 0; Value < After[Index].Num(); ++Value)
			{
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					Changed += FMath::Abs(
						GetValue(After[Index][Value], Channel) - GetValue(Before[Index][Value], Channel)) > 1e-6f ? 1 : 0;
				}
			}
		}
		return Changed;
	}

	struct FRuntimeSmokeRun
	{
		float TimeA = 0.5f;
		float TimeB = 1.0f;
		bool bQuitWhenDone = false;
		double LaunchSeconds = 0.0;
		double LastWaitStateSeconds = 0.0;
		bool bFinished = false;
		FString TargetMap;
		TWeakObjectPtr<UWorld> World;
	};

	void FinishSmoke(const TSharedRef<FRuntimeSmokeRun>& Run, bool bPassed, const FString& Detail)
	{
		WriteSmokeState(FormatSmokeState(bPassed ? TEXT("PASSED") : TEXT("FAILED"), Detail));
		if (bPassed)
		{
			UE_LOG(LogNDGIRuntime, Display, TEXT("TestRuntimeSmoke: PASSED %s"), *Detail);
		}
		else
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestRuntimeSmoke: FAILED - %s"), *Detail);
		}
		Run->bFinished = true;
		if (Run->bQuitWhenDone)
		{
			RequestSmokeExit();
		}
	}

	bool TickRuntimeSmoke(const TSharedRef<FRuntimeSmokeRun>& Run)
	{
		if (Run->bFinished)
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindSmokeWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
				{
					FinishSmoke(Run, false, FString::Printf(
						TEXT("timed out waiting for target map '%s' (no game world)"), *Run->TargetMap));
					return false;
				}
				return true;
			}

			if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
			{
				FinishSmoke(Run, false, FString::Printf(
					TEXT("timed out waiting for target map '%s' (current world '%s')"),
					*Run->TargetMap, *World->GetOutermost()->GetName()));
				return false;
			}

			if (!Run->TargetMap.IsEmpty())
			{
				FString TargetPackage = Run->TargetMap;
				int32 QueryIndex = INDEX_NONE;
				if (TargetPackage.FindChar(TEXT('?'), QueryIndex))
				{
					TargetPackage.LeftInline(QueryIndex);
				}
				if (TargetPackage.EndsWith(TEXT(".umap"), ESearchCase::IgnoreCase))
				{
					TargetPackage.LeftChopInline(5);
				}
				const FString WorldPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
				const bool bPackageMatches = WorldPackage.Equals(TargetPackage, ESearchCase::IgnoreCase);
				const bool bNameMatches = World->GetName().Equals(
					FPaths::GetBaseFilename(TargetPackage), ESearchCase::IgnoreCase);
				if (!bPackageMatches && !bNameMatches)
				{
					const double Now = FPlatformTime::Seconds();
					if (Now - Run->LastWaitStateSeconds > 1.0)
					{
						Run->LastWaitStateSeconds = Now;
						WriteSmokeState(FormatSmokeState(TEXT("WAITING"), FString::Printf(
							TEXT("current='%s' target='%s'"), *WorldPackage, *TargetPackage)));
					}
					return true;
				}
			}

			Run->World = World;
		}

		const FString AssetName = GetSmokeLevelAssetName(World);
		WriteSmokeState(FormatSmokeState(TEXT("BUILDING"), AssetName));
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			FinishSmoke(Run, false, FString::Printf(TEXT("level asset '%s' not found (was it cooked?)"), *AssetName));
			return false;
		}

		FGlobalShaderMap* ShaderMap = GetGlobalShaderMap(GMaxRHIFeatureLevel);
		if (!ShaderMap || !ShaderMap->HasShader(&FNDGIDecodeAtlasCS::GetStaticType(), 0))
		{
			FinishSmoke(Run, false, TEXT("NDGI decode compute shader is missing from the global shader map"));
			return false;
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels) || !Cache.IsValid())
		{
			FinishSmoke(Run, false, TEXT("decode-cache build failed"));
			return false;
		}

		const TSet<FString> LoadedCells = GatherLoadedCells(World);
		Cache->UpdateActiveCells(LoadedCells);
		if (Cache->Entries.Num() == 0 || Cache->Tiles <= 0 || Cache->ActiveAtlases <= 0)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("cache has no decodable work (%d atlas(es), %d model(s), %d active)"),
				Cache->Entries.Num(), Cache->Tiles, Cache->ActiveAtlases));
			return false;
		}
		WriteSmokeState(FormatSmokeState(TEXT("BUILT"), FString::Printf(
			TEXT("%d atlas(es), %d model(s), %.1f ms"), Cache->Entries.Num(), Cache->Tiles, BuildMs)));

		const int32 LimitMB = []()
		{
			const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.MaxCacheMemoryMB"));
			return Variable ? Variable->GetInt() : 2048;
		}();
		const int64 CacheBytes = Cache->CPUBytes + Cache->ActiveGPUBytes;
		const int64 LimitBytes = static_cast<int64>(FMath::Max(0, LimitMB)) * 1024 * 1024;
		if (LimitBytes > 0 && CacheBytes > LimitBytes)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("cache uses %.1f MB, over the %d MB limit"),
				CacheBytes / (1024.0 * 1024.0), LimitMB));
			return false;
		}

		TArray<UTexture2D*> Targets;
		Targets.Reserve(Cache->Entries.Num());
		for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			Targets.Add(Entry.Binding.TargetTexture);
		}

		TArray<TArray<FVector4f>> Before;
		Before.SetNum(Cache->Entries.Num());
		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			if (!Cache->Entries[Index].bActive)
			{
				continue;
			}
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadbackRawPixels(Targets[Index], Before[Index], Width, Height))
			{
				FinishSmoke(Run, false, FString::Printf(
					TEXT("pre-decode readback failed for atlas %d (compressed or non-resident lightmap)"), Index));
				return false;
			}
		}

		NDGI::Runtime::FDecodeTimings TimingsA;
		if (!NDGI::Runtime::DecodeCachedLevel(Cache, Run->TimeA, Targets, &TimingsA))
		{
			FinishSmoke(Run, false, TEXT("cached decode enqueue failed"));
			return false;
		}
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		TArray<TArray<FVector4f>> AfterA;
		AfterA.SetNum(Cache->Entries.Num());
		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			if (!Cache->Entries[Index].bActive)
			{
				continue;
			}
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadbackRawPixels(Targets[Index], AfterA[Index], Width, Height))
			{
				FinishSmoke(Run, false, FString::Printf(TEXT("post-decode readback failed for atlas %d"), Index));
				return false;
			}
		}

		WriteSmokeState(FormatSmokeState(TEXT("DECODED_A"), FString::Printf(TEXT("t=%.3f"), Run->TimeA)));
		FSmokePixelStats StatsA;
		ComparePixels(*Cache, Before, AfterA, StatsA);
		if (StatsA.Changed == 0)
		{
			FinishSmoke(Run, false, TEXT("decode wrote no changed lightmap values"));
			return false;
		}
		if (StatsA.EmptyTileChanged != 0)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("decode changed %lld value(s) in tiles without models"), StatsA.EmptyTileChanged));
			return false;
		}
		if (StatsA.FiniteViolations != 0 || StatsA.OutOfRange != 0)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("decoded lightmap has %lld non-finite and %lld out-of-range value(s)"),
				StatsA.FiniteViolations, StatsA.OutOfRange));
			return false;
		}

		const bool bHasTemporalSlices = Cache->Entries.ContainsByPredicate(
			[](const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry) { return Entry.Binding.TimeSlices > 1; });

		NDGI::Runtime::FDecodeTimings TimingsB;
		if (!NDGI::Runtime::DecodeCachedLevel(Cache, Run->TimeB, Targets, &TimingsB))
		{
			FinishSmoke(Run, false, TEXT("second cached decode enqueue failed"));
			return false;
		}
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		TArray<TArray<FVector4f>> AfterB;
		AfterB.SetNum(Cache->Entries.Num());
		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			if (!Cache->Entries[Index].bActive)
			{
				continue;
			}
			int32 Width = 0;
			int32 Height = 0;
			if (!ReadbackRawPixels(Targets[Index], AfterB[Index], Width, Height))
			{
				FinishSmoke(Run, false, FString::Printf(TEXT("second post-decode readback failed for atlas %d"), Index));
				return false;
			}
		}

		WriteSmokeState(FormatSmokeState(TEXT("DECODED_B"), FString::Printf(TEXT("t=%.3f"), Run->TimeB)));
		FSmokePixelStats StatsB;
		ComparePixels(*Cache, Before, AfterB, StatsB);
		if (StatsB.EmptyTileChanged != 0)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("second decode changed %lld value(s) in tiles without models"), StatsB.EmptyTileChanged));
			return false;
		}
		if (StatsB.FiniteViolations != 0 || StatsB.OutOfRange != 0)
		{
			FinishSmoke(Run, false, FString::Printf(
				TEXT("second decoded lightmap has %lld non-finite and %lld out-of-range value(s)"),
				StatsB.FiniteViolations, StatsB.OutOfRange));
			return false;
		}

		const int64 TemporalChanged = CountChangedPixels(AfterA, AfterB);
		if (bHasTemporalSlices && TemporalChanged == 0)
		{
			FinishSmoke(Run, false, TEXT("changing time-of-day produced no different lightmap values"));
			return false;
		}

		FinishSmoke(Run, true, FString::Printf(
			TEXT("- map '%s', %d atlas(es), %d model(s), %d active, %.1f MB CPU + %.1f MB GPU, ")
			TEXT("build %.1f ms, %d skipped, changed %lld, empty %lld, temporal %lld, out-of-range 0."),
			*World->GetName(), Cache->Entries.Num(), Cache->Tiles, Cache->ActiveAtlases,
			Cache->CPUBytes / (1024.0 * 1024.0), Cache->ActiveGPUBytes / (1024.0 * 1024.0),
			BuildMs, SkippedModels, StatsA.Changed, StatsA.EmptyTileChanged, TemporalChanged));
		return false;
	}

	void StartRuntimeSmoke(float TimeA, float TimeB, bool bQuitWhenDone, const TCHAR* Source, const FString& TargetMap = FString())
	{
		TSharedRef<FRuntimeSmokeRun> Run = MakeShared<FRuntimeSmokeRun>();
		Run->TimeA = FMath::Clamp(TimeA, 0.0f, 1.0f);
		Run->TimeB = FMath::Clamp(TimeB, 0.0f, 1.0f);
		Run->bQuitWhenDone = bQuitWhenDone;
		Run->TargetMap = TargetMap;
		Run->LaunchSeconds = FPlatformTime::Seconds();

		WriteSmokeState(FormatSmokeState(TEXT("STARTED"), FString::Printf(
			TEXT("t=%.3f then t=%.3f via %s"), Run->TimeA, Run->TimeB, Source)));
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestRuntimeSmoke: starting at t=%.3f then t=%.3f (%s)."),
			Run->TimeA, Run->TimeB, Source);
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Run](float) { return TickRuntimeSmoke(Run); }), 0.0f);
	}

	void RunTestRuntimeSmokeCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float TimeA = 0.5f;
		float TimeB = 1.0f;
		bool bQuitWhenDone = false;
		int32 NumericIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (Arg.IsNumeric())
			{
				const float Value = FCString::Atof(*Arg);
				if (NumericIndex == 0)
				{
					TimeA = Value;
				}
				else if (NumericIndex == 1)
				{
					TimeB = Value;
				}
				++NumericIndex;
			}
		}

		StartRuntimeSmoke(TimeA, TimeB, bQuitWhenDone, TEXT("console"));
	}
}

namespace NDGI
{
	namespace Runtime
	{
		void StartRuntimeSmokeFromCommandLine()
		{
			float TimeA = 0.5f;
			float TimeB = 1.0f;
			FString Value;
			FString TargetMap;
			const TCHAR* CommandLine = FCommandLine::Get();
			if (FParse::Value(CommandLine, TEXT("ndgiSmokeTimeA="), Value))
			{
				TimeA = FCString::Atof(*Value);
			}
			if (FParse::Value(CommandLine, TEXT("ndgiSmokeTimeB="), Value))
			{
				TimeB = FCString::Atof(*Value);
			}
			if (FParse::Value(CommandLine, TEXT("ndgiSmokeMap="), Value))
			{
				TargetMap = Value;
			}
			StartRuntimeSmoke(TimeA, TimeB, true, TEXT("command line"), TargetMap);
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestRuntimeSmokeCommand(
	TEXT("ndgi.TestRuntimeSmoke"),
	TEXT("Cooked/packaged smoke test: builds the decode cache, decodes two times, and verifies lightmap writes, empty-tile preservation, ranges, and temporal change. Usage: ndgi.TestRuntimeSmoke [timeA] [timeB] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestRuntimeSmokeCommand));

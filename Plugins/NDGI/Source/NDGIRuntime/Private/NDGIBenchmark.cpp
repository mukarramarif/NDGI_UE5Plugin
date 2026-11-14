// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Containers/Ticker.h"
#include "DynamicRHI.h"
#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NDGIAtlasDecoder.h"
#include "NDGILevelActor.h"
#include "NDGILevelAsset.h"
#include "NDGIModelAsset.h"
#include "NDGIRuntime.h"
#include "NDGITimeOfDayProfile.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
	UWorld* FindBenchmarkWorld()
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

	FString GetBenchmarkLevelAssetName(UWorld* World)
	{
		const FString MapName = UWorld::RemovePIEPrefix(World->GetName());
		return FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *MapName, *MapName);
	}

	FString SanitizeBenchmarkLabel(const FString& Label)
	{
		FString Result = Label;
		for (int32 Index = 0; Index < Result.Len(); ++Index)
		{
			const TCHAR Char = Result[Index];
			if (!FChar::IsAlnum(Char) && Char != TEXT('_') && Char != TEXT('-'))
			{
				Result[Index] = TEXT('_');
			}
		}
		return Result.IsEmpty() ? TEXT("benchmark") : Result;
	}

	void RequestBenchmarkExit()
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

	float Percentile(const TArray<float>& SortedValues, float Percent)
	{
		if (SortedValues.Num() == 0)
		{
			return 0.0f;
		}
		const int32 Index = FMath::Clamp(
			FMath::RoundToInt(Percent * static_cast<float>(SortedValues.Num() - 1)), 0, SortedValues.Num() - 1);
		return SortedValues[Index];
	}

	void LogTimingStats(const TCHAR* Label, TArray<float> Values, double& OutMean, double& OutP95)
	{
		OutMean = 0.0;
		OutP95 = 0.0;
		if (Values.Num() == 0)
		{
			UE_LOG(LogNDGIRuntime, Display, TEXT("%s: no samples."), Label);
			return;
		}
		double Sum = 0.0;
		for (float Value : Values)
		{
			Sum += Value;
		}
		OutMean = Sum / static_cast<double>(Values.Num());
		Values.Sort();
		OutP95 = Percentile(Values, 0.95f);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("%s: mean %.2f ms, p50 %.2f ms, p95 %.2f ms, max %.2f ms."),
			Label, OutMean, Percentile(Values, 0.5f), OutP95, Values.Last());
	}

	struct FBenchmarkRun
	{
		bool bNDGI = false;
		float NDGITime = 0.5f;
		double WarmupSeconds = 5.0;
		double DurationSeconds = 20.0;
		FString Label;
		bool bQuitWhenDone = false;

		double LaunchSeconds = 0.0;
		double WarmupEndSeconds = 0.0;
		double CaptureEndSeconds = 0.0;
		double CaptureStartSeconds = 0.0;
		double LastFrameSeconds = 0.0;
		bool bInitialized = false;
		bool bCapturing = false;

		double DecodeEnqueueMs = 0.0;
		double DecodeTotalMs = 0.0;

		TWeakObjectPtr<UWorld> World;
		TArray<float> CpuMs;
		TArray<float> GpuMs;
		TArray<float> TimeSeconds;
	};

	int32 GetCVarInt(const TCHAR* Name)
	{
		const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
		return Variable ? Variable->GetInt() : -1;
	}

	void FinishBenchmark(const TSharedRef<FBenchmarkRun>& Run, bool bSuccess)
	{
		double CpuMean = 0.0;
		double CpuP95 = 0.0;
		double GpuMean = 0.0;
		double GpuP95 = 0.0;
		LogTimingStats(TEXT("Benchmark CPU frame"), Run->CpuMs, CpuMean, CpuP95);
		LogTimingStats(TEXT("Benchmark GPU frame"), Run->GpuMs, GpuMean, GpuP95);

		if (Run->CpuMs.Num() > 0 && CpuMean > 0.0)
		{
			UE_LOG(LogNDGIRuntime, Display, TEXT("Benchmark: %.1f FPS (CPU mean), %d frames, label '%s'."),
				1000.0 / CpuMean, Run->CpuMs.Num(), *Run->Label);
		}
		if (Run->bNDGI)
		{
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("Benchmark NDGI: decode at t=%.2f took %.1f ms enqueue, %.1f ms to visible (including render thread)."),
				Run->NDGITime, Run->DecodeEnqueueMs, Run->DecodeTotalMs);
		}

		if (bSuccess && Run->CpuMs.Num() > 0)
		{
			const FString Directory = FPaths::ProjectSavedDir() / TEXT("NDGI/Benchmarks");
			IFileManager::Get().MakeDirectory(*Directory, true);
			const FString FileName = Directory / SanitizeBenchmarkLabel(Run->Label) + TEXT(".csv");

			FString Csv = TEXT("frame,time_s,cpu_ms,gpu_ms\n");
			for (int32 Index = 0; Index < Run->CpuMs.Num(); ++Index)
			{
				const double TimeSeconds = Run->TimeSeconds.IsValidIndex(Index) ? Run->TimeSeconds[Index] : 0.0;
				Csv += FString::Printf(TEXT("%d,%.6f,%.4f,%.4f\n"),
					Index, TimeSeconds, Run->CpuMs[Index], Run->GpuMs.IsValidIndex(Index) ? Run->GpuMs[Index] : 0.0f);
			}
			if (FFileHelper::SaveStringToFile(Csv, *FileName))
			{
				UE_LOG(LogNDGIRuntime, Display, TEXT("Benchmark: wrote %s."), *FileName);
			}
			else
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("Benchmark: failed to write %s."), *FileName);
			}
		}

		if (Run->bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
	}

	bool TickBenchmark(const TSharedRef<FBenchmarkRun>& Run)
	{
		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindBenchmarkWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("Benchmark: timed out waiting for a game/PIE world."));
					FinishBenchmark(Run, false);
					return false;
				}
				return true;
			}
			Run->World = World;
		}

		if (!Run->bInitialized)
		{
			if (Run->bNDGI)
			{
				ANDGIActor* Actor = nullptr;
				for (TActorIterator<ANDGIActor> It(World); It; ++It)
				{
					Actor = *It;
					break;
				}
				if (!Actor)
				{
					FActorSpawnParameters SpawnParameters;
					SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
					Actor = World->SpawnActor<ANDGIActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
				}
				const FString AssetName = GetBenchmarkLevelAssetName(World);
				UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
				if (!Actor || !LevelAsset)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("Benchmark NDGI: actor or level asset '%s' not found."), *AssetName);
					FinishBenchmark(Run, false);
					return false;
				}

				Actor->LevelAsset = LevelAsset;
				// Build the decode cache outside the timed section; otherwise the first
				// time change includes one-time parameter packing and buffer creation.
				Actor->SetTimeOfDay(0.0f);
				FlushRenderingCommands();
				GDynamicRHI->RHIBlockUntilGPUIdle();

				const double DecodeStart = FPlatformTime::Seconds();
				Actor->SetTimeOfDay(Run->NDGITime);
				Run->DecodeEnqueueMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
				FlushRenderingCommands();
				GDynamicRHI->RHIBlockUntilGPUIdle();
				Run->DecodeTotalMs = (FPlatformTime::Seconds() - DecodeStart) * 1000.0;
			}

			UE_LOG(LogNDGIRuntime, Display,
				TEXT("Benchmark: map '%s', warmup %.1f s, capture %.1f s, CVars GI=%d reflections=%d VSync=%d MaxFPS=%d."),
				*World->GetName(), Run->WarmupSeconds, Run->DurationSeconds,
				GetCVarInt(TEXT("r.DynamicGlobalIlluminationMethod")),
				GetCVarInt(TEXT("r.ReflectionMethod")),
				GetCVarInt(TEXT("r.VSync")),
				GetCVarInt(TEXT("t.MaxFPS")));

			Run->bInitialized = true;
			Run->WarmupEndSeconds = FPlatformTime::Seconds() + Run->WarmupSeconds;
			Run->LastFrameSeconds = FPlatformTime::Seconds();
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		if (!Run->bCapturing)
		{
			if (Now < Run->WarmupEndSeconds)
			{
				Run->LastFrameSeconds = Now;
				return true;
			}
			Run->bCapturing = true;
			Run->CaptureStartSeconds = Now;
			Run->CaptureEndSeconds = Now + Run->DurationSeconds;
			Run->LastFrameSeconds = Now;
			return true;
		}

		const float CpuFrameMs = static_cast<float>((Now - Run->LastFrameSeconds) * 1000.0);
		Run->LastFrameSeconds = Now;
		Run->CpuMs.Add(CpuFrameMs);
		Run->GpuMs.Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
		Run->TimeSeconds.Add(static_cast<float>(Now - Run->CaptureStartSeconds));

		if (Now >= Run->CaptureEndSeconds)
		{
			FinishBenchmark(Run, true);
			return false;
		}
		return true;
	}

	void StartBenchmark(const TArray<FString>& Args, bool bNDGI)
	{
		TSharedRef<FBenchmarkRun> Run = MakeShared<FBenchmarkRun>();
		Run->bNDGI = bNDGI;
		Run->LaunchSeconds = FPlatformTime::Seconds();
		Run->Label = FString::Printf(TEXT("%s_%s"), bNDGI ? TEXT("ndgi") : TEXT("dynamic"),
			*FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));

		int32 NumericIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else if (Arg.IsNumeric())
			{
				const double Value = FCString::Atod(*Arg);
				if (bNDGI && NumericIndex == 0)
				{
					Run->NDGITime = static_cast<float>(FMath::Clamp(Value, 0.0, 1.0));
				}
				else if ((!bNDGI && NumericIndex == 0) || (bNDGI && NumericIndex == 1))
				{
					Run->DurationSeconds = FMath::Max(1.0, Value);
				}
				else if ((!bNDGI && NumericIndex == 1) || (bNDGI && NumericIndex == 2))
				{
					Run->WarmupSeconds = FMath::Max(0.0, Value);
				}
				++NumericIndex;
			}
			else
			{
				Run->Label = Arg;
			}
		}

		UE_LOG(LogNDGIRuntime, Display, TEXT("Benchmark: label '%s', warmup %.1f s, capture %.1f s."),
			*Run->Label, Run->WarmupSeconds, Run->DurationSeconds);
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) { return TickBenchmark(Run); }), 0.0f);
	}

	void RunBenchmarkCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		StartBenchmark(Args, false);
	}

	void RunBenchmarkNDGICommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		StartBenchmark(Args, true);
	}

	struct FDecodeBenchmarkRun
	{
		int32 Iterations = 20;
		bool bQuitWhenDone = false;
		double LaunchSeconds = 0.0;
	};

	bool TickDecodeBenchmark(const TSharedRef<FDecodeBenchmarkRun>& Run)
	{
		UWorld* World = FindBenchmarkWorld();
		if (!World)
		{
			if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecode: timed out waiting for a game/PIE world."));
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}
			return true;
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecode: level asset '%s' not found."), *AssetName);
			if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
			return false;
		}

		NDGI::Runtime::FDecodeTimings ColdTimings;
		double ColdStart = FPlatformTime::Seconds();
		const bool bColdDecoded = NDGI::Runtime::DecodeLevelAsset(LevelAsset, 0.0f, &ColdTimings);
		FlushRenderingCommands();
		const double ColdMs = (FPlatformTime::Seconds() - ColdStart) * 1000.0;

		TArray<float> TotalMs;
		TArray<float> CpuMs;
		for (int32 Iteration = 0; Iteration < Run->Iterations; ++Iteration)
		{
			const float Time = static_cast<float>(Iteration % 11) / 10.0f;
			NDGI::Runtime::FDecodeTimings Timings;
			const double Start = FPlatformTime::Seconds();
			NDGI::Runtime::DecodeLevelAsset(LevelAsset, Time, &Timings);
			FlushRenderingCommands();
			TotalMs.Add(static_cast<float>((FPlatformTime::Seconds() - Start) * 1000.0));
			CpuMs.Add(static_cast<float>(Timings.CpuMs));
		}

		double TotalMean = 0.0;
		double TotalP95 = 0.0;
		double CpuMean = 0.0;
		double CpuP95 = 0.0;
		LogTimingStats(TEXT("BenchmarkDecode total (decode + flush)"), TotalMs, TotalMean, TotalP95);
		LogTimingStats(TEXT("BenchmarkDecode CPU (pack + enqueue)"), CpuMs, CpuMean, CpuP95);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("BenchmarkDecode: %d atlas(es), %d model(s), cold %.1f ms (CPU %.1f ms, %s)."),
			ColdTimings.Atlases, ColdTimings.Tiles, ColdMs, ColdTimings.CpuMs, bColdDecoded ? TEXT("ok") : TEXT("failed"));

		if (Run->bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
		return false;
	}

	void RunBenchmarkDecodeCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FDecodeBenchmarkRun> Run = MakeShared<FDecodeBenchmarkRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		bool bFoundIterations = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else if (!bFoundIterations && Arg.IsNumeric())
			{
				Run->Iterations = FMath::Clamp(FCString::Atoi(*Arg), 1, 500);
				bFoundIterations = true;
			}
		}

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) { return TickDecodeBenchmark(Run); }), 0.0f);
	}

	struct FWarmDecodeBenchmarkRun
	{
		int32 Iterations = 20;
		bool bQuitWhenDone = false;
		double LaunchSeconds = 0.0;
	};

	bool CreateBenchmarkTargets(const NDGI::Runtime::FLevelDecodeCache& Cache, EPixelFormat Format, TArray<UTexture2D*>& OutTargets)
	{
		OutTargets.Reset();
		for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Cache.Entries)
		{
			UTexture2D* Texture = UTexture2D::CreateTransient(
				Entry.Binding.AtlasWidth, Entry.Binding.AtlasHeight, Format);
			if (!Texture)
			{
				return false;
			}
			Texture->NeverStream = true;
			Texture->UpdateResource();
			OutTargets.Add(Texture);
		}
		FlushRenderingCommands();
		return OutTargets.Num() == Cache.Entries.Num();
	}

	void MeasureWarmDecode(
		const TSharedPtr<NDGI::Runtime::FLevelDecodeCache>& Cache,
		const TArray<UTexture2D*>& Targets,
		int32 Iterations,
		TArray<float>& OutEnqueueMs,
		TArray<float>& OutTotalMs)
	{
		OutEnqueueMs.Reset();
		OutTotalMs.Reset();
		for (int32 Iteration = 0; Iteration < Iterations; ++Iteration)
		{
			const float Time = static_cast<float>(Iteration % 11) / 10.0f;
			NDGI::Runtime::FDecodeTimings Timings;
			const double Start = FPlatformTime::Seconds();
			NDGI::Runtime::DecodeCachedLevel(Cache, Time, Targets, &Timings);
			const double Enqueued = FPlatformTime::Seconds();
			FlushRenderingCommands();
			// FlushRenderingCommands only waits for the render thread; the 50 ms
			// gate is about GPU completion, so wait for the GPU too.
			GDynamicRHI->RHIBlockUntilGPUIdle();
			const double Finished = FPlatformTime::Seconds();
			OutEnqueueMs.Add(static_cast<float>((Enqueued - Start) * 1000.0));
			OutTotalMs.Add(static_cast<float>((Finished - Start) * 1000.0));
		}
	}

	bool TickWarmDecodeBenchmark(const TSharedRef<FWarmDecodeBenchmarkRun>& Run)
	{
		UWorld* World = FindBenchmarkWorld();
		if (!World)
		{
			if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecodeWarm: timed out waiting for a game/PIE world."));
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}
			return true;
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecodeWarm: level asset '%s' not found."), *AssetName);
			if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
			return false;
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecodeWarm: cache build failed."));
			if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
			return false;
		}

		// A bounded page pool starts empty. Warm the nearest pages for the
		// current view before timing so the pool sweep measures real decode
		// work instead of an all-empty tile table. Pool 0 already loaded every
		// page in BuildLevelDecodeCache.
		const bool bHasBoundedPool = Cache->Entries.ContainsByPredicate(
			[](const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry)
			{
				return Entry.ResidentCapacity > 0;
			});
		if (bHasBoundedPool)
		{
			FVector CameraLocation = FVector::ZeroVector;
			if (APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				FRotator ViewRotation;
				PlayerController->GetPlayerViewPoint(CameraLocation, ViewRotation);
			}

			IConsoleVariable* SyncLoadCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.StreamingSyncLoad"));
			const int32 SavedSyncLoad = SyncLoadCVar ? SyncLoadCVar->GetInt() : 0;
			if (SyncLoadCVar)
			{
				SyncLoadCVar->Set(1, ECVF_SetByCode);
			}
			for (int32 Iteration = 0; Iteration < 8; ++Iteration)
			{
				const bool bChanged = Cache->UpdateStreaming(CameraLocation);
				FlushRenderingCommands();
				if (!bChanged)
				{
					break;
				}
			}
			if (SyncLoadCVar)
			{
				SyncLoadCVar->Set(SavedSyncLoad, ECVF_SetByCode);
			}

			UE_LOG(LogNDGIRuntime, Display,
				TEXT("BenchmarkDecodeWarm: warmed %d resident page(s) for camera (%.0f %.0f %.0f)."),
				Cache->ResidentModels, CameraLocation.X, CameraLocation.Y, CameraLocation.Z);
		}

		TArray<UTexture2D*> Fp16Targets;
		TArray<UTexture2D*> BgraTargets;
		if (!CreateBenchmarkTargets(*Cache, PF_FloatRGBA, Fp16Targets)
			|| !CreateBenchmarkTargets(*Cache, PF_B8G8R8A8, BgraTargets))
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("BenchmarkDecodeWarm: could not create benchmark targets."));
			if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
			return false;
		}

		UE_LOG(LogNDGIRuntime, Display,
			TEXT("BenchmarkDecodeWarm: map '%s', %d atlas(es), %d model(s), %d skipped, packed params %.1f MB, CPU %.1f MB, GPU %.1f MB, cold build %.1f ms."),
			*World->GetName(), Cache->Entries.Num(), Cache->Tiles, SkippedModels,
			Cache->ParameterBytes / (1024.0 * 1024.0),
			Cache->CPUBytes / (1024.0 * 1024.0), Cache->GPUBytes / (1024.0 * 1024.0), BuildMs);

		for (int32 EntryIndex = 0; EntryIndex < Cache->Entries.Num(); ++EntryIndex)
		{
			const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry = Cache->Entries[EntryIndex];
			const int32 Slots = Entry.ResidentCapacity > 0
				? FMath::Min(Entry.ResidentCapacity, Entry.ModelCount)
				: Entry.ModelCount;
			const int64 BC7Bytes = (static_cast<int64>(Entry.Binding.UVBC7BytesPerLayer)
				+ static_cast<int64>(Entry.Binding.TimeSlices) * Entry.Binding.UVTBC7BytesPerLayer)
				* FMath::Max(0, Slots);
			const int64 ParamBytes = static_cast<int64>(Entry.ResidentParameters.Num()) * sizeof(float);
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("BenchmarkDecodeWarm: atlas %d '%s': %d model(s), %d slot(s), BC7 %.2f MB, params %.2f MB."),
				EntryIndex, *Entry.TargetTexturePath.GetAssetName(), Entry.ModelCount, Slots,
				BC7Bytes / (1024.0 * 1024.0), ParamBytes / (1024.0 * 1024.0));
		}

		const bool bStaleAsset = SkippedModels > 0;
		const bool bEmptyCache = Cache->Tiles == 0;
		if (bStaleAsset)
		{
			UE_LOG(LogNDGIRuntime, Error,
				TEXT("BenchmarkDecodeWarm: stale-asset check FAILED - %d model(s) skipped because they have no BC7 features. Retrain and resave the level asset before trusting these numbers."),
				SkippedModels);
		}
		else if (bEmptyCache)
		{
			UE_LOG(LogNDGIRuntime, Error,
				TEXT("BenchmarkDecodeWarm: stale-asset check FAILED - the cache contains 0 models. Re-bake, retrain, and resave the level asset."));
		}
		else
		{
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("BenchmarkDecodeWarm: stale-asset check PASSED - 0 model(s) skipped."));
		}

		// Warm both write paths once so RHI allocations and shader state are resident.
		TArray<UTexture2D*> EngineTargets;
		EngineTargets.Reserve(Cache->Entries.Num());
		for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			EngineTargets.Add(Entry.Binding.TargetTexture);
		}

		NDGI::Runtime::DecodeCachedLevel(Cache, 0.0f, Fp16Targets);
		NDGI::Runtime::DecodeCachedLevel(Cache, 0.0f, BgraTargets);
		NDGI::Runtime::DecodeCachedLevel(Cache, 0.0f, EngineTargets);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		TArray<float> Fp16EnqueueMs;
		TArray<float> Fp16TotalMs;
		TArray<float> BgraEnqueueMs;
		TArray<float> BgraTotalMs;
		TArray<float> EngineEnqueueMs;
		TArray<float> EngineTotalMs;
		MeasureWarmDecode(Cache, Fp16Targets, Run->Iterations, Fp16EnqueueMs, Fp16TotalMs);
		MeasureWarmDecode(Cache, BgraTargets, Run->Iterations, BgraEnqueueMs, BgraTotalMs);
		MeasureWarmDecode(Cache, EngineTargets, Run->Iterations, EngineEnqueueMs, EngineTotalMs);

		double Mean = 0.0;
		double P95 = 0.0;
		LogTimingStats(TEXT("BenchmarkDecodeWarm FP16 enqueue"), Fp16EnqueueMs, Mean, P95);
		LogTimingStats(TEXT("BenchmarkDecodeWarm FP16 total (enqueue + GPU idle)"), Fp16TotalMs, Mean, P95);
		const double Fp16P95 = P95;
		LogTimingStats(TEXT("BenchmarkDecodeWarm BGRA8 enqueue"), BgraEnqueueMs, Mean, P95);
		LogTimingStats(TEXT("BenchmarkDecodeWarm BGRA8 total (enqueue + GPU idle)"), BgraTotalMs, Mean, P95);
		LogTimingStats(TEXT("BenchmarkDecodeWarm Engine lightmaps enqueue"), EngineEnqueueMs, Mean, P95);
		LogTimingStats(TEXT("BenchmarkDecodeWarm Engine lightmaps total (enqueue + GPU idle)"), EngineTotalMs, Mean, P95);
		const double EngineP95 = P95;

		const bool bPassed = !bStaleAsset && !bEmptyCache && EngineP95 <= 50.0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("BenchmarkDecodeWarm: FP16 p95 %.2f ms, live engine-lightmap p95 %.2f ms (gate: <= 50 ms), %d model(s) skipped - %s."),
			Fp16P95, EngineP95, SkippedModels, bPassed ? TEXT("PASSED") : TEXT("FAILED"));

		if (Run->bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
		return false;
	}

	struct FCadenceRun
	{
		double DurationSeconds = 5.0;
		bool bQuitWhenDone = false;
		double LaunchSeconds = 0.0;
		double StartSeconds = 0.0;
		bool bInitialized = false;
		bool bFinished = false;
		TWeakObjectPtr<UWorld> World;
		ANDGIActor* Actor = nullptr;
	};

	bool TickCadenceRun(const TSharedRef<FCadenceRun>& Run)
	{
		if (Run->bFinished)
		{
			return false;
		}

		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindBenchmarkWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 180.0)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("TestDecodeCadence: timed out waiting for a game/PIE world."));
					if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
					return false;
				}
				return true;
			}
			Run->World = World;
		}

		if (!Run->bInitialized)
		{
			for (TActorIterator<ANDGIActor> It(World); It; ++It)
			{
				Run->Actor = *It;
				break;
			}
			if (!Run->Actor)
			{
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Run->Actor = World->SpawnActor<ANDGIActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
			}
			const FString AssetName = GetBenchmarkLevelAssetName(World);
			UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
			if (!Run->Actor || !LevelAsset)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestDecodeCadence: actor or level asset '%s' not found."), *AssetName);
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				Run->bFinished = true;
				return false;
			}
			Run->Actor->LevelAsset = LevelAsset;
			// Build the cache outside the cadence window.
			Run->Actor->SetTimeOfDay(0.0f);
			FlushRenderingCommands();
			GDynamicRHI->RHIBlockUntilGPUIdle();
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestDecodeCadence: %.1f s, interval %.3f s, starting count %d."),
				Run->DurationSeconds, Run->Actor->DecodeIntervalSeconds, Run->Actor->GetCompletedDecodeCount());
			Run->bInitialized = true;
			Run->StartSeconds = FPlatformTime::Seconds();
			return true;
		}

		const double Now = FPlatformTime::Seconds();
		const double Elapsed = Now - Run->StartSeconds;
		if (Elapsed < Run->DurationSeconds)
		{
			const float Time = static_cast<float>(FMath::Fmod(Elapsed * 0.37, 1.0));
			Run->Actor->SetTimeOfDay(Time);
			return true;
		}

		// Let the last in-flight decode finish before reporting.
		if (Elapsed < Run->DurationSeconds + 0.5)
		{
			return true;
		}

		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();
		const int32 Decodes = Run->Actor->GetCompletedDecodeCount();
		const double Rate = Decodes / FMath::Max(Run->DurationSeconds, 1e-3);
		const bool bPassed = Rate >= 10.0 && Rate <= 20.0 + 1.0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestDecodeCadence: %s - %d decode(s) in %.2f s (%.2f Hz, target 10-20 Hz)."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), Decodes, Run->DurationSeconds, Rate);
		Run->bFinished = true;
		if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
		return false;
	}

	void RunTestDecodeCadenceCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FCadenceRun> Run = MakeShared<FCadenceRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		bool bFoundDuration = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else if (!bFoundDuration && Arg.IsNumeric())
			{
				Run->DurationSeconds = FMath::Clamp(FCString::Atod(*Arg), 1.0, 60.0);
				bFoundDuration = true;
			}
		}
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) { return TickCadenceRun(Run); }), 0.0f);
	}

	void RunBenchmarkDecodeWarmCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FWarmDecodeBenchmarkRun> Run = MakeShared<FWarmDecodeBenchmarkRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		bool bFoundIterations = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else if (!bFoundIterations && Arg.IsNumeric())
			{
				Run->Iterations = FMath::Clamp(FCString::Atoi(*Arg), 1, 500);
				bFoundIterations = true;
			}
		}

		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Run](float) { return TickWarmDecodeBenchmark(Run); }), 0.0f);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIBenchmarkCommand(
	TEXT("ndgi.Benchmark"),
	TEXT("Measures CPU/GPU frame times in the running game or PIE session. Usage: ndgi.Benchmark <seconds> [warmupSeconds] [label] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunBenchmarkCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIBenchmarkNDGICommand(
	TEXT("ndgi.BenchmarkNDGI"),
	TEXT("Spawns/uses ANDGIActor, applies one time-of-day decode, then measures frame times. Usage: ndgi.BenchmarkNDGI <time> <seconds> [warmupSeconds] [label] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunBenchmarkNDGICommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIBenchmarkDecodeCommand(
	TEXT("ndgi.BenchmarkDecode"),
	TEXT("Measures NDGI lightmap decode latency (pack + GPU + readback/upload). Usage: ndgi.BenchmarkDecode <iterations> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunBenchmarkDecodeCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestDecodeCadenceCommand(
	TEXT("ndgi.TestDecodeCadence"),
	TEXT("Rapidly changes ANDGIActor time-of-day and reports completed decodes; verifies the 10-20 Hz latest-wins cadence. Usage: ndgi.TestDecodeCadence <seconds> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestDecodeCadenceCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIBenchmarkDecodeWarmCommand(
	TEXT("ndgi.BenchmarkDecodeWarm"),
	TEXT("Measures cached-parameter warm decode for every atlas into FP16 (CopyTexture) and BGRA8 (readback/upload) targets. Usage: ndgi.BenchmarkDecodeWarm <iterations> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunBenchmarkDecodeWarmCommand));

namespace
{
	struct FAsyncCacheTestRun
	{
		TSharedPtr<NDGI::Runtime::FDecodeCacheBuildState> Build;
		TWeakObjectPtr<UWorld> World;
		double LaunchSeconds = 0.0;
		double MaxTickMs = 0.0;
		double BeginMs = 0.0;
		int32 LastAtlas = -1;
		int32 TotalAtlases = 0;
		bool bQuitWhenDone = false;
	};

	UWorld* FindAnyEditorOrGameWorld()
	{
		if (!GEngine)
		{
			return nullptr;
		}
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.World())
			{
				return Context.World();
			}
		}
		return nullptr;
	}

	bool TickAsyncCacheTest(const TSharedRef<FAsyncCacheTestRun>& Run)
	{
		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindAnyEditorOrGameWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 120.0)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncCache: timed out waiting for a world."));
					if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
					return false;
				}
				return true;
			}
			Run->World = World;
		}

		if (!Run->Build.IsValid())
		{
			const FString AssetName = GetBenchmarkLevelAssetName(World);
			UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
			if (!LevelAsset)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncCache: level asset '%s' not found."), *AssetName);
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}

			Run->TotalAtlases = LevelAsset->Atlases.Num();
			TSet<FString> LoadedCells;
			for (ULevel* Level : World->GetLevels())
			{
				if (Level)
				{
					LoadedCells.Add(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
				}
			}
			const double BeginStart = FPlatformTime::Seconds();
			Run->Build = NDGI::Runtime::BeginBuildLevelDecodeCacheAsync(LevelAsset, LoadedCells);
			Run->BeginMs = (FPlatformTime::Seconds() - BeginStart) * 1000.0;
			if (!Run->Build.IsValid())
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncCache: could not begin the build."));
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestAsyncCache: %d atlas(es), begin call %.2f ms."), Run->TotalAtlases, Run->BeginMs);
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		const double TickStart = FPlatformTime::Seconds();
		const bool bDone = NDGI::Runtime::TickBuildLevelDecodeCacheAsync(Run->Build, Cache, &BuildMs, &SkippedModels);
		const double TickMs = (FPlatformTime::Seconds() - TickStart) * 1000.0;
		Run->MaxTickMs = FMath::Max(Run->MaxTickMs, TickMs);

		if (Run->Build->NextAtlas != Run->LastAtlas)
		{
			Run->LastAtlas = Run->Build->NextAtlas;
			UE_LOG(LogNDGIRuntime, Display, TEXT("TestAsyncCache: packed atlas %d/%d in %.2f ms."),
				Run->LastAtlas, Run->TotalAtlases, TickMs);
		}

		if (!bDone)
		{
			if (FPlatformTime::Seconds() - Run->LaunchSeconds > 300.0)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncCache: build timed out after %.1f s."), BuildMs / 1000.0);
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}
			return true;
		}

		if (Cache.IsValid())
		{
			const bool bStaleAsset = SkippedModels > 0;
			const bool bEmptyCache = Cache->Tiles == 0;
			if (bStaleAsset)
			{
				UE_LOG(LogNDGIRuntime, Error,
					TEXT("TestAsyncCache: stale-asset check FAILED - %d model(s) skipped because they have no BC7 features. Retrain and resave the level asset."),
					SkippedModels);
			}
			else if (bEmptyCache)
			{
				UE_LOG(LogNDGIRuntime, Error,
					TEXT("TestAsyncCache: stale-asset check FAILED - the cache contains 0 models."));
			}

			if (bStaleAsset || bEmptyCache)
			{
				UE_LOG(LogNDGIRuntime, Error,
					TEXT("TestAsyncCache: FAILED - %d atlas(es), %d model(s), CPU %.1f MB, GPU %.1f MB, build %.1f ms, max tick %.2f ms, %d skipped."),
					Cache->Entries.Num(), Cache->Tiles,
					Cache->CPUBytes / (1024.0 * 1024.0), Cache->GPUBytes / (1024.0 * 1024.0),
					BuildMs, Run->MaxTickMs, SkippedModels);
			}
			else
			{
				UE_LOG(LogNDGIRuntime, Display,
					TEXT("TestAsyncCache: PASSED - %d atlas(es), %d model(s), CPU %.1f MB, GPU %.1f MB, build %.1f ms, max tick %.2f ms, %d skipped."),
					Cache->Entries.Num(), Cache->Tiles,
					Cache->CPUBytes / (1024.0 * 1024.0), Cache->GPUBytes / (1024.0 * 1024.0),
					BuildMs, Run->MaxTickMs, SkippedModels);
			}
		}
		else
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncCache: FAILED - %s."),
				Run->Build.IsValid() && !Run->Build->Error.IsEmpty() ? *Run->Build->Error : TEXT("no cache"));
		}
		if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
		return false;
	}

	void RunTestAsyncCacheCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FAsyncCacheTestRun> Run = MakeShared<FAsyncCacheTestRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
		}
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Run](float) { return TickAsyncCacheTest(Run); }), 0.0f);
	}

	void RunTestStreamingCellsCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestStreamingCells: no world."));
			return;
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestStreamingCells: level asset '%s' not found."), *AssetName);
			return;
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestStreamingCells: cache build failed."));
			return;
		}

		TSet<FString> LoadedCells;
		for (ULevel* Level : World->GetLevels())
		{
			if (Level)
			{
				LoadedCells.Add(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
			}
		}

		for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
		{
			const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
			UE_LOG(LogNDGIRuntime, Display, TEXT("TestStreamingCells: atlas %d, %d cell(s): %s"),
				Index, Entry.SourceCells.Num(), *FString::Join(Entry.SourceCells, TEXT(", ")));
		}

		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestStreamingCells: built in %.1f ms, %d atlas(es), %d model(s), %d loaded level(s), %d skipped."),
			BuildMs, Cache->Entries.Num(), Cache->Tiles, LoadedCells.Num(), SkippedModels);

		TSet<FString> RequestedCells;
		if (Args.Num() > 0 && !Args[0].Equals(TEXT("none"), ESearchCase::IgnoreCase))
		{
			TArray<FString> Parts;
			Args[0].ParseIntoArray(Parts, TEXT(";"), true);
			for (const FString& Part : Parts)
			{
				if (!Part.IsEmpty())
				{
					RequestedCells.Add(Part);
				}
			}
		}

		const bool bChanged = Cache->UpdateActiveCells(RequestedCells);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestStreamingCells: %s -> %d/%d atlas(es) active, GPU %.1f MB (requested %d cell(s))."),
			bChanged ? TEXT("subset applied") : TEXT("no atlas state change"),
			Cache->ActiveAtlases, Cache->Entries.Num(),
			Cache->ActiveGPUBytes / (1024.0 * 1024.0), RequestedCells.Num());

		NDGI::Runtime::FDecodeTimings Timings;
		const int32 ActiveAtDecode = Cache->ActiveAtlases;
		const bool bDecoded = NDGI::Runtime::DecodeCachedLevel(Cache, 1.0f, &Timings);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestStreamingCells: decode %s, enqueue %.3f ms, %d active atlas(es) queued."),
			bDecoded ? TEXT("ok") : TEXT("FAILED"), Timings.CpuMs, Timings.Atlases);

		const bool bRestored = Cache->UpdateActiveCells(LoadedCells);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestStreamingCells: %s -> restored %d/%d atlas(es) active, GPU %.1f MB."),
			bRestored ? TEXT("restored") : TEXT("already restored"),
			Cache->ActiveAtlases, Cache->Entries.Num(), Cache->ActiveGPUBytes / (1024.0 * 1024.0));

		const bool bPassed = bDecoded
			&& Timings.Atlases == ActiveAtDecode
			&& Cache->ActiveAtlases <= Cache->Entries.Num();
		UE_LOG(LogNDGIRuntime, Display, TEXT("TestStreamingCells: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
	}

	void RunTestVTStreamingCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestVTStreaming: no world."));
			return;
		}

		int32 PoolSize = 4;
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				PoolSize = FMath::Max(1, FCString::Atoi(*Arg));
			}
		}

		IConsoleVariable* PoolCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.ResidentModelsPerAtlas"));
		IConsoleVariable* SyncCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.StreamingSyncLoad"));
		const int32 PreviousPool = PoolCVar ? PoolCVar->GetInt() : 0;
		const int32 PreviousSync = SyncCVar ? SyncCVar->GetInt() : 0;
		if (PoolCVar)
		{
			PoolCVar->Set(PoolSize, ECVF_SetByCode);
		}
		if (SyncCVar)
		{
			SyncCVar->Set(1, ECVF_SetByCode);
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestVTStreaming: level asset '%s' not found."), *AssetName);
			if (PoolCVar) { PoolCVar->Set(PreviousPool, ECVF_SetByCode); }
			if (SyncCVar) { SyncCVar->Set(PreviousSync, ECVF_SetByCode); }
			return;
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestVTStreaming: cache build failed."));
			if (PoolCVar) { PoolCVar->Set(PreviousPool, ECVF_SetByCode); }
			if (SyncCVar) { SyncCVar->Set(PreviousSync, ECVF_SetByCode); }
			return;
		}

		TSet<FString> LoadedCells;
		for (ULevel* Level : World->GetLevels())
		{
			if (Level)
			{
				LoadedCells.Add(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
			}
		}
		Cache->UpdateActiveCells(LoadedCells);

		TArray<FVector> Cameras;
		for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			for (const FBox& Box : Entry.ModelBounds)
			{
				if (Box.IsValid)
				{
					Cameras.AddUnique(Box.GetCenter());
					if (Cameras.Num() >= 4)
					{
						break;
					}
				}
			}
			if (Cameras.Num() >= 4)
			{
				break;
			}
		}
		if (Cameras.Num() == 0)
		{
			Cameras.Add(FVector::ZeroVector);
		}

		const int32 TotalModels = Cache->Tiles;
		bool bAnyUpload = false;
		bool bAnyDecode = false;
		int32 MaxResidentDuringTest = 0;
		const uint64 InitialTextureId = Cache->GetFeatureTextureResourceId(0);
		bool bTextureReused = true;
		for (const FVector& Camera : Cameras)
		{
			Cache->LastStreamingUpdateSeconds = -MAX_dbl;
			const bool bChanged = Cache->UpdateStreaming(Camera);
			bAnyUpload |= bChanged;
			MaxResidentDuringTest = FMath::Max(MaxResidentDuringTest, Cache->ResidentModels);
			bTextureReused &= InitialTextureId == 0 || Cache->GetFeatureTextureResourceId(0) == InitialTextureId;
			NDGI::Runtime::FDecodeTimings Timings;
			const bool bDecoded = NDGI::Runtime::DecodeCachedLevel(Cache, 0.5f, &Timings);
			bAnyDecode |= bDecoded;
			FlushRenderingCommands();
			GDynamicRHI->RHIBlockUntilGPUIdle();
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestVTStreaming: camera (%.0f %.0f %.0f) -> %d resident/%d model(s), GPU %.2f MB, %d page upload(s), %d eviction(s), decode %s."),
				Camera.X, Camera.Y, Camera.Z, Cache->ResidentModels, TotalModels,
				Cache->ActiveGPUBytes / (1024.0 * 1024.0), Cache->StreamingPageUploads,
				Cache->StreamingEvictions, bDecoded ? TEXT("ok") : TEXT("FAILED"));
		}

		if (PoolCVar)
		{
			PoolCVar->Set(PreviousPool, ECVF_SetByCode);
		}
		if (SyncCVar)
		{
			SyncCVar->Set(PreviousSync, ECVF_SetByCode);
		}
		Cache->LastStreamingUpdateSeconds = -MAX_dbl;
		Cache->UpdateStreaming(Cameras.Last());

		const bool bPassed = TotalModels > PoolSize
			&& MaxResidentDuringTest <= PoolSize * Cache->Entries.Num()
			&& bAnyUpload
			&& bAnyDecode
			&& bTextureReused
			&& SkippedModels == 0
			&& Cache->PageLoadFailures == 0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestVTStreaming: %s - built in %.1f ms, %d atlas(es), %d model(s), pool %d, peak resident %d, %d page upload(s), %d eviction(s), texture reused %s, %d load failure(s)."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), BuildMs, Cache->Entries.Num(), TotalModels,
			PoolSize, MaxResidentDuringTest, Cache->StreamingPageUploads, Cache->StreamingEvictions,
			bTextureReused ? TEXT("yes") : TEXT("no"), Cache->PageLoadFailures);
		if (bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
	}

	struct FAsyncPageTestRun
	{
		TWeakObjectPtr<UWorld> World;
		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		int32 PoolSize = 2;
		int32 PreviousPool = 0;
		int32 PreviousSync = 0;
		bool bQuitWhenDone = false;
		bool bStarted = false;
		bool bSawResident = false;
		double LaunchSeconds = 0.0;
		FVector Camera = FVector::ZeroVector;
	};

	bool TickAsyncPageTest(const TSharedRef<FAsyncPageTestRun>& Run)
	{
		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindAnyEditorOrGameWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 120.0)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncPages: timed out waiting for a world."));
					if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
					return false;
				}
				return true;
			}
			Run->World = World;
		}

		IConsoleVariable* PoolCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.ResidentModelsPerAtlas"));
		IConsoleVariable* SyncCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.StreamingSyncLoad"));
		if (!Run->bStarted)
		{
			Run->bStarted = true;
			if (PoolCVar) { Run->PreviousPool = PoolCVar->GetInt(); PoolCVar->Set(Run->PoolSize, ECVF_SetByCode); }
			if (SyncCVar) { Run->PreviousSync = SyncCVar->GetInt(); SyncCVar->Set(0, ECVF_SetByCode); }

			const FString AssetName = GetBenchmarkLevelAssetName(World);
			UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
			if (!LevelAsset)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncPages: level asset '%s' not found."), *AssetName);
				if (PoolCVar) { PoolCVar->Set(Run->PreviousPool, ECVF_SetByCode); }
				if (SyncCVar) { SyncCVar->Set(Run->PreviousSync, ECVF_SetByCode); }
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}

			double BuildMs = 0.0;
			int32 Skipped = 0;
			if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Run->Cache, &BuildMs, &Skipped))
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAsyncPages: cache build failed."));
				if (PoolCVar) { PoolCVar->Set(Run->PreviousPool, ECVF_SetByCode); }
				if (SyncCVar) { SyncCVar->Set(Run->PreviousSync, ECVF_SetByCode); }
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}

			for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Run->Cache->Entries)
			{
				for (const FBox& Box : Entry.ModelBounds)
				{
					if (Box.IsValid)
					{
						Run->Camera = Box.GetCenter();
						break;
					}
				}
				if (!Run->Camera.IsNearlyZero()) { break; }
			}
			Run->Cache->LastStreamingUpdateSeconds = -MAX_dbl;
			Run->Cache->UpdateStreaming(Run->Camera);
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestAsyncPages: built in %.1f ms, %d model path(s), queued %d page load(s), %d resident page(s)."),
				BuildMs, Run->Cache->Tiles, Run->Cache->PageLoadRequests, Run->Cache->ResidentModels);
			return true;
		}

		Run->Cache->LastStreamingUpdateSeconds = -MAX_dbl;
		Run->Cache->UpdateStreaming(Run->Camera);
		Run->bSawResident |= Run->Cache->ResidentModels > 0;

		const bool bDone = Run->bSawResident && Run->Cache->InFlightPageLoads == 0;
		if (!bDone && FPlatformTime::Seconds() - Run->LaunchSeconds > 30.0)
		{
			UE_LOG(LogNDGIRuntime, Error,
				TEXT("TestAsyncPages: timed out; %d resident, %d in flight, %d request(s), %d completion(s), %d failure(s)."),
				Run->Cache->ResidentModels, Run->Cache->InFlightPageLoads,
				Run->Cache->PageLoadRequests, Run->Cache->PageLoadCompletions, Run->Cache->PageLoadFailures);
			if (PoolCVar) { PoolCVar->Set(Run->PreviousPool, ECVF_SetByCode); }
			if (SyncCVar) { SyncCVar->Set(Run->PreviousSync, ECVF_SetByCode); }
			if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
			return false;
		}
		if (!bDone)
		{
			return true;
		}

		NDGI::Runtime::FDecodeTimings Timings;
		const bool bDecoded = NDGI::Runtime::DecodeCachedLevel(Run->Cache, 0.5f, &Timings);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		const int32 Expected = FMath::Min(Run->PoolSize * Run->Cache->Entries.Num(), Run->Cache->Tiles);
		const bool bPassed = Run->Cache->ResidentModels > 0
			&& Run->Cache->ResidentModels <= Expected
			&& Run->Cache->PageLoadCompletions > 0
			&& Run->Cache->LoadedPageHandles == 0
			&& Run->Cache->PageLoadFailures == 0
			&& bDecoded;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestAsyncPages: %s - %d resident/%d expected, %d load request(s), %d completion(s), %d failure(s), %d handle(s) held, decode %s."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), Run->Cache->ResidentModels, Expected,
			Run->Cache->PageLoadRequests, Run->Cache->PageLoadCompletions, Run->Cache->PageLoadFailures,
			Run->Cache->LoadedPageHandles, bDecoded ? TEXT("ok") : TEXT("FAILED"));

		if (PoolCVar) { PoolCVar->Set(Run->PreviousPool, ECVF_SetByCode); }
		if (SyncCVar) { SyncCVar->Set(Run->PreviousSync, ECVF_SetByCode); }
		if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
		return false;
	}

	void RunTestAsyncPagesCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FAsyncPageTestRun> Run = MakeShared<FAsyncPageTestRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else
			{
				Run->PoolSize = FMath::Max(1, FCString::Atoi(*Arg));
			}
		}
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Run](float) { return TickAsyncPageTest(Run); }), 0.0f);
	}

	void RunTestDiskStreamingCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestDiskStreaming: no world."));
			return;
		}

		int32 PoolSize = 2;
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				PoolSize = FMath::Max(1, FCString::Atoi(*Arg));
			}
		}

		IConsoleVariable* PoolCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.ResidentModelsPerAtlas"));
		IConsoleVariable* SyncCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("ndgi.StreamingSyncLoad"));
		const int32 PreviousPool = PoolCVar ? PoolCVar->GetInt() : 0;
		const int32 PreviousSync = SyncCVar ? SyncCVar->GetInt() : 0;
		if (PoolCVar)
		{
			PoolCVar->Set(PoolSize, ECVF_SetByCode);
		}
		if (SyncCVar)
		{
			SyncCVar->Set(1, ECVF_SetByCode);
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestDiskStreaming: level asset '%s' not found."), *AssetName);
			if (PoolCVar) { PoolCVar->Set(PreviousPool, ECVF_SetByCode); }
			if (SyncCVar) { SyncCVar->Set(PreviousSync, ECVF_SetByCode); }
			return;
		}

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (!NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels))
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestDiskStreaming: cache build failed."));
			if (PoolCVar) { PoolCVar->Set(PreviousPool, ECVF_SetByCode); }
			if (SyncCVar) { SyncCVar->Set(PreviousSync, ECVF_SetByCode); }
			return;
		}

		const bool bBuiltWithoutLoading = Cache->LoadedPageHandles == 0
			&& Cache->ResidentModels == 0
			&& Cache->PageLoadCompletions == 0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestDiskStreaming: build %.1f ms, %d model path(s), %d loaded handle(s), %d resident page(s)."),
			BuildMs, Cache->Tiles, Cache->LoadedPageHandles, Cache->ResidentModels);

		TArray<FVector> Cameras;
		for (const NDGI::Runtime::FAtlasDecodeCacheEntry& Entry : Cache->Entries)
		{
			for (const FBox& Box : Entry.ModelBounds)
			{
				if (Box.IsValid)
				{
					Cameras.AddUnique(Box.GetCenter());
					if (Cameras.Num() >= 2)
					{
						break;
					}
				}
			}
			if (Cameras.Num() >= 2)
			{
				break;
			}
		}
		if (Cameras.Num() == 0)
		{
			Cameras.Add(FVector::ZeroVector);
		}

		Cache->LastStreamingUpdateSeconds = -MAX_dbl;
		const bool bFirstChanged = Cache->UpdateStreaming(Cameras[0]);
		const int32 FirstResident = Cache->ResidentModels;
		const int32 FirstCompletions = Cache->PageLoadCompletions;
		const int32 FirstHandles = Cache->LoadedPageHandles;
		const int32 UploadsAfterFirst = Cache->StreamingPageUploads;
		const uint64 TextureId = Cache->GetFeatureTextureResourceId(0);

		bool bSecondChanged = false;
		int32 SecondUploads = 0;
		if (Cameras.Num() > 1)
		{
			Cache->LastStreamingUpdateSeconds = -MAX_dbl;
			bSecondChanged = Cache->UpdateStreaming(Cameras[1]);
			SecondUploads = Cache->StreamingPageUploads - UploadsAfterFirst;
		}
		const bool bTextureReused = TextureId == 0 || Cache->GetFeatureTextureResourceId(0) == TextureId;

		NDGI::Runtime::FDecodeTimings Timings;
		const bool bDecoded = NDGI::Runtime::DecodeCachedLevel(Cache, 0.5f, &Timings);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();

		const bool bPassed = bBuiltWithoutLoading
			&& bFirstChanged
			&& FirstResident > 0
			&& FirstResident <= PoolSize * Cache->Entries.Num()
			&& FirstCompletions >= FirstResident
			&& FirstHandles == 0
			&& SecondUploads <= PoolSize * Cache->Entries.Num()
			&& bTextureReused
			&& bDecoded
			&& SkippedModels == 0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestDiskStreaming: %s - pool %d, first %d resident/%d loaded, %d page upload(s) first, %d page upload(s) after move (%s), handles after upload %d, texture reused %s, decode %s."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), PoolSize, FirstResident, FirstCompletions,
			UploadsAfterFirst, SecondUploads, bSecondChanged ? TEXT("changed") : TEXT("steady"),
			FirstHandles, bTextureReused ? TEXT("yes") : TEXT("no"), bDecoded ? TEXT("ok") : TEXT("FAILED"));

		if (PoolCVar) { PoolCVar->Set(PreviousPool, ECVF_SetByCode); }
		if (SyncCVar) { SyncCVar->Set(PreviousSync, ECVF_SetByCode); }
		if (bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
	}

	void RunResaveLevelAssetCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
#if WITH_EDITOR
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("ResaveLevelAsset: no world."));
			return;
		}

		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			bQuitWhenDone |= Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("ResaveLevelAsset: level asset '%s' not found."), *AssetName);
			if (bQuitWhenDone) { RequestBenchmarkExit(); }
			return;
		}

		int32 PathCount = 0;
		for (const FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
		{
			for (int32 Cell = 0; Cell < Entry.TileModelPaths.Num(); ++Cell)
			{
				PathCount += Entry.GetTileModelPath(Cell).IsValid() ? 1 : 0;
			}
		}

		UPackage* Package = LevelAsset->GetOutermost();
		Package->SetDirtyFlag(true);
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		const bool bSaved = UPackage::SavePackage(Package, LevelAsset, *FileName, SaveArgs);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("ResaveLevelAsset: %s - %d soft model path(s), saved '%s'."),
			bSaved ? TEXT("PASSED") : TEXT("FAILED"), PathCount, *FileName);
		if (bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
#else
		UE_LOG(LogNDGIRuntime, Error, TEXT("ResaveLevelAsset is editor-only."));
#endif
	}

	void RunVerifyStreamingDepsCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("VerifyStreamingDeps: no world."));
			return;
		}

		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			bQuitWhenDone |= Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("VerifyStreamingDeps: level asset '%s' not found."), *AssetName);
			if (bQuitWhenDone) { RequestBenchmarkExit(); }
			return;
		}

		int32 PathCount = 0;
		int32 MissingCount = 0;
		int32 SoftDepCount = 0;
		TSet<FName> SoftDependencies;
		if (IAssetRegistry* Registry = IAssetRegistry::Get())
		{
			TArray<FName> Dependencies;
			Registry->GetDependencies(LevelAsset->GetOutermost()->GetFName(), Dependencies,
				UE::AssetRegistry::EDependencyCategory::Package,
				UE::AssetRegistry::EDependencyQuery::Game | UE::AssetRegistry::EDependencyQuery::Soft);
			SoftDependencies.Append(Dependencies);
		}

		for (const FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
		{
			for (int32 Cell = 0; Cell < Entry.TileModelPaths.Num(); ++Cell)
			{
				const FSoftObjectPath Path = Entry.GetTileModelPath(Cell);
				if (!Path.IsValid())
				{
					continue;
				}
				++PathCount;
				if (!FPackageName::DoesPackageExist(Path.GetLongPackageName()))
				{
					++MissingCount;
					UE_LOG(LogNDGIRuntime, Warning,
						TEXT("VerifyStreamingDeps: missing model package '%s'."), *Path.GetLongPackageName());
				}
				if (SoftDependencies.Contains(FName(*Path.GetLongPackageName())))
				{
					++SoftDepCount;
				}
			}
		}

		const bool bDirty = LevelAsset->GetOutermost()->IsDirty();
		const bool bPassed = PathCount > 0 && MissingCount == 0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("VerifyStreamingDeps: %s - %d model path(s), %d missing package(s), %d soft dependency edge(s)%s."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), PathCount, MissingCount, SoftDepCount,
			bDirty ? TEXT(" (level asset has unsaved migration; save it to persist soft paths for cook)") : TEXT(""));

		if (bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
	}

	void RunReportStorageCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = FindAnyEditorOrGameWorld();
		if (!World)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("ReportStorage: no world."));
			return;
		}

		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			bQuitWhenDone |= Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		}

		const FString AssetName = GetBenchmarkLevelAssetName(World);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetName);
		if (!LevelAsset)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("ReportStorage: level asset '%s' not found."), *AssetName);
			if (bQuitWhenDone) { RequestBenchmarkExit(); }
			return;
		}

		int32 ModelCount = 0;
		int32 LegacyModels = 0;
		int32 AtlasCount = 0;
		int64 RawTODSetBytes = 0;
		int64 AtlasTexelSlices = 0;
		int64 RawFeatureBytes = 0;
		int64 BC7FeatureBytes = 0;
		int64 CompactParamBytes = 0;
		int64 LegacyFlatParamBytes = 0;

		for (const FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
		{
			const int64 CoefTexels = static_cast<int64>(Entry.AtlasSize.X) * (Entry.AtlasSize.Y / 2);
			if (CoefTexels <= 0 || Entry.TimeSlices <= 0)
			{
				continue;
			}
			++AtlasCount;
			RawTODSetBytes += CoefTexels * 2 * 4 * Entry.TimeSlices;
			AtlasTexelSlices += CoefTexels * Entry.TimeSlices;

			for (int32 Cell = 0; Cell < Entry.TileModelPaths.Num(); ++Cell)
			{
				const FSoftObjectPath ModelPath = Entry.GetTileModelPath(Cell);
				const UNDGIModelAsset* Model = ModelPath.IsValid()
					? Cast<UNDGIModelAsset>(ModelPath.TryLoad())
					: nullptr;
				if (!Model)
				{
					continue;
				}
				if (!Model->HasBC7Features())
				{
					++LegacyModels;
				}
				const int32 Extent = Model->TileSize.X;
				const int32 UVTSize = Model->FeatureUVTSize.X;
				const int32 TimeSlices = Model->TimeSlices;
				if (Extent <= 0 || UVTSize <= 0 || TimeSlices <= 0)
				{
					continue;
				}

				NDGI::Train::FTileTrainConfig Config;
				Config.UVSize = Extent;
				Config.UVTSize = UVTSize;
				Config.UTSize = Model->UTSize;
				Config.VTSize = Model->VTSize;
				Config.TimeSlices = TimeSlices;
				const int64 CompactBytes =
					static_cast<int64>(NDGI::Runtime::ComputeCompactTileLayout(Config).TotalParams) * sizeof(float);
				const int64 LegacyBytes =
					static_cast<int64>(NDGI::Train::ComputeTotalParameters(Config)) * sizeof(float);
				const int64 RawBytes =
					static_cast<int64>(Extent) * Extent * 4
					+ static_cast<int64>(TimeSlices) * UVTSize * UVTSize * 4
					+ CompactBytes;
				const int64 BC7Bytes =
					static_cast<int64>(Extent / 4) * (Extent / 4) * 16
					+ static_cast<int64>(TimeSlices) * (UVTSize / 4) * (UVTSize / 4) * 16
					+ CompactBytes;

				RawFeatureBytes += RawBytes;
				BC7FeatureBytes += BC7Bytes;
				CompactParamBytes += CompactBytes;
				LegacyFlatParamBytes += LegacyBytes;
				++ModelCount;
			}
		}

		const double MB = 1.0 / (1024.0 * 1024.0);
		const double RawRatio = BC7FeatureBytes > 0 ? static_cast<double>(RawTODSetBytes) / BC7FeatureBytes : 0.0;
		const double BPP = AtlasTexelSlices > 0 ? static_cast<double>(BC7FeatureBytes) * 8.0 / AtlasTexelSlices : 0.0;
		const double LegacyRatio = LegacyFlatParamBytes > 0
			? static_cast<double>(RawTODSetBytes) / LegacyFlatParamBytes : 0.0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("ReportStorage: %d atlas(es), %d model(s), %lld coeff0 texel-slices."),
			AtlasCount, ModelCount, AtlasTexelSlices);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("ReportStorage: raw 8-slice BGRA8 TOD set %.2f MB; raw RGBA8 features + compact params %.2f MB; BC7 features + compact params %.2f MB (features %.2f, compact %.2f); legacy flat float params %.2f MB."),
			RawTODSetBytes * MB, RawFeatureBytes * MB, BC7FeatureBytes * MB,
			(BC7FeatureBytes - CompactParamBytes) * MB, CompactParamBytes * MB, LegacyFlatParamBytes * MB);
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("ReportStorage: BC7/raw-TOD-set ratio %.2fx, BC7/raw-feature-assets %.2fx, legacy-flat/raw-TOD-set %.2fx, BC7 BPP %.4f bits/coeff0-texel-slice."),
			RawRatio, RawFeatureBytes > 0 ? static_cast<double>(RawFeatureBytes) / BC7FeatureBytes : 0.0,
			LegacyRatio, BPP);
		if (LegacyModels > 0)
		{
			UE_LOG(LogNDGIRuntime, Error,
				TEXT("ReportStorage: stale-asset check FAILED - %d of %d model(s) have no BC7 features. Retrain and resave the level asset."),
				LegacyModels, ModelCount);
			UE_LOG(LogNDGIRuntime, Error, TEXT("ReportStorage: FAILED."));
		}
		else
		{
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("ReportStorage: stale-asset check PASSED - 0 legacy model(s)."));
			UE_LOG(LogNDGIRuntime, Display, TEXT("ReportStorage: PASSED (projected BC7 sizes; package/cook overhead excluded)."));
		}

		if (bQuitWhenDone)
		{
			RequestBenchmarkExit();
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestStreamingCellsCommand(
	TEXT("ndgi.TestStreamingCells"),
	TEXT("Tests per-cell atlas activation: lists cell ownership, activates a subset, releases the rest, decodes, then restores. Usage: ndgi.TestStreamingCells [cellA;cellB | none]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestStreamingCellsCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestVTStreamingCommand(
	TEXT("ndgi.TestVTStreaming"),
	TEXT("Exercises the BC7 virtual-texture page cache: bounds the resident pool, moves the camera across tile centers, incrementally uploads pages, and decodes. Usage: ndgi.TestVTStreaming [poolSize] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestVTStreamingCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestAsyncPagesCommand(
	TEXT("ndgi.TestAsyncPages"),
	TEXT("Exercises asynchronous model-page streaming and reports requests, completions, resident pages, and held handles. Usage: ndgi.TestAsyncPages [poolSize] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestAsyncPagesCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestDiskStreamingCommand(
	TEXT("ndgi.TestDiskStreaming"),
	TEXT("Verifies that model packages load only when their pages become resident and that page streams are incremental. Usage: ndgi.TestDiskStreaming [poolSize] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestDiskStreamingCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIResaveLevelAssetCommand(
	TEXT("ndgi.ResaveLevelAsset"),
	TEXT("Saves the current level's NDGI level asset after soft-path migration, so cook dependency data is updated. Usage: ndgi.ResaveLevelAsset [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunResaveLevelAssetCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIVerifyStreamingDepsCommand(
	TEXT("ndgi.VerifyStreamingDeps"),
	TEXT("Checks that every soft tile-model path in the current level asset resolves to a package, and reports cook soft-dependency edges. Usage: ndgi.VerifyStreamingDeps [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunVerifyStreamingDepsCommand));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIReportStorageCommand(
	TEXT("ndgi.ReportStorage"),
	TEXT("Reports projected BC7 feature/compact-parameter bytes, legacy flat-parameter bytes, raw TOD-set bytes, compression ratios, and BPP for the current level asset. Usage: ndgi.ReportStorage [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunReportStorageCommand));

namespace
{
	UNDGITimeOfDayProfile* CreateDefaultProfileAsset()
	{
		const TCHAR* PackageName = TEXT("/Game/NDGI/TOD/NDGITODProfile");
		UNDGITimeOfDayProfile* Profile = LoadObject<UNDGITimeOfDayProfile>(nullptr, TEXT("/Game/NDGI/TOD/NDGITODProfile.NDGITODProfile"));
		UPackage* Package = Profile ? Profile->GetOutermost() : CreatePackage(PackageName);
		if (!Profile)
		{
			Profile = NewObject<UNDGITimeOfDayProfile>(Package, TEXT("NDGITODProfile"), RF_Public | RF_Standalone);
		}
		if (!Profile || !Package)
		{
			return nullptr;
		}

		Profile->Modify();
		Profile->Keyframes.Reset();

		auto AddKey = [Profile](float Time, float Pitch, float Yaw, const FLinearColor& SunColor, float SunIntensity,
			const FLinearColor& SkyColor, float SkyIntensity)
		{
			FNDGITimeOfDayKey& Key = Profile->Keyframes.AddDefaulted_GetRef();
			Key.Time = Time;
			Key.SunPitch = Pitch;
			Key.SunYaw = Yaw;
			Key.SunColor = SunColor;
			Key.SunIntensity = SunIntensity;
			Key.SkyColor = SkyColor;
			Key.SkyIntensity = SkyIntensity;
		};

		AddKey(0.00f, 8.0f, 20.0f, FLinearColor(1.0f, 0.45f, 0.25f), 2.0f, FLinearColor(0.25f, 0.28f, 0.45f), 0.6f);
		AddKey(0.25f, 75.0f, 0.0f, FLinearColor(1.0f, 0.96f, 0.9f), 8.0f, FLinearColor(0.55f, 0.7f, 0.95f), 1.0f);
		AddKey(0.50f, 12.0f, -20.0f, FLinearColor(1.0f, 0.5f, 0.2f), 2.5f, FLinearColor(0.3f, 0.3f, 0.5f), 0.7f);
		AddKey(0.75f, -10.0f, -40.0f, FLinearColor(0.35f, 0.45f, 0.9f), 0.4f, FLinearColor(0.05f, 0.07f, 0.15f), 0.2f);
		AddKey(1.00f, 8.0f, 20.0f, FLinearColor(1.0f, 0.45f, 0.25f), 2.0f, FLinearColor(0.25f, 0.28f, 0.45f), 0.6f);

		Profile->CycleDurationSeconds = 120.0f;
		Profile->bPingPong = false;
		Profile->BakeSteps = 8;
		Profile->TileSize = 128;
		Profile->TrainSteps = 3000;
		Profile->OverlapTexels = 8;

		Package->MarkPackageDirty();
		const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (!UPackage::SavePackage(Package, Profile, *FileName, SaveArgs))
		{
			return nullptr;
		}
		return Profile;
	}

	void RunCreateTODProfile(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UNDGITimeOfDayProfile* Profile = CreateDefaultProfileAsset();
		if (!Profile)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("CreateTODProfile: failed to save /Game/NDGI/TOD/NDGITODProfile."));
			return;
		}
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("CreateTODProfile: saved %s with %d keyframes, %d bake steps, tile %d, %d train steps, overlap %d."),
			*Profile->GetPathName(), Profile->Keyframes.Num(), Profile->BakeSteps, Profile->TileSize,
			Profile->TrainSteps, Profile->OverlapTexels);
		if (Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase); }) && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void RunTestTODProfile(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UNDGITimeOfDayProfile* Profile = UNDGITimeOfDayProfile::LoadDefaultProfile();
		if (!Profile)
		{
			UE_LOG(LogNDGIRuntime, Error, TEXT("TestTODProfile: default profile missing; run ndgi.CreateTODProfile first."));
			return;
		}

		bool bPassed = Profile->Keyframes.Num() >= 2;
		for (float Time = 0.0f; Time <= 1.0001f; Time += 0.125f)
		{
			const FNDGITimeOfDayKey Key = Profile->Evaluate(Time);
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestTODProfile: t=%.3f pitch %.2f yaw %.2f sun %.3f %.3f %.3f x%.2f sky %.3f %.3f %.3f x%.2f."),
				Time, Key.SunPitch, Key.SunYaw, Key.SunColor.R, Key.SunColor.G, Key.SunColor.B, Key.SunIntensity,
				Key.SkyColor.R, Key.SkyColor.G, Key.SkyColor.B, Key.SkyIntensity);
			bPassed &= FMath::IsFinite(Key.SunPitch) && FMath::IsFinite(Key.SunIntensity) && FMath::IsFinite(Key.SkyIntensity);
		}

		const FNDGITimeOfDayKey Noon = Profile->Evaluate(0.25f);
		const FNDGITimeOfDayKey Night = Profile->Evaluate(0.75f);
		const FNDGITimeOfDayKey Dawn = Profile->Evaluate(0.0f);
		const FVector NoonForward = FRotator(-Noon.SunPitch, Noon.SunYaw, 0.0f).Vector();
		const FVector DawnForward = FRotator(-Dawn.SunPitch, Dawn.SunYaw, 0.0f).Vector();
		bPassed &= Noon.SunPitch > Night.SunPitch;
		bPassed &= Noon.SunIntensity > Night.SunIntensity;
		// The directional-light forward vector must point down for a sun above
		// the horizon; otherwise the light comes from below.
		bPassed &= NoonForward.Z < -0.1f;
		bPassed &= DawnForward.Z <= 1e-4f;
		bPassed &= Profile->BakeSteps >= 2 && Profile->TileSize % 16 == 0;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestTODProfile: sun forward Z noon %.3f (< -0.1), dawn %.3f (<= 0)."),
			NoonForward.Z, DawnForward.Z);
		UE_LOG(LogNDGIRuntime, Display, TEXT("TestTODProfile: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
		if (Args.ContainsByPredicate([](const FString& Arg) { return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase); }) && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	struct FAutoCycleTestRun
	{
		TWeakObjectPtr<UWorld> World;
		ANDGIActor* Actor = nullptr;
		UNDGITimeOfDayProfile* Profile = nullptr;
		double StartSeconds = 0.0;
		double LaunchSeconds = 0.0;
		double Duration = 3.0;
		float StartTime = 0.0f;
		int32 StartDecodes = 0;
		bool bInitialized = false;
		bool bQuitWhenDone = false;
	};

	bool TickAutoCycleTest(const TSharedRef<FAutoCycleTestRun>& Run)
	{
		UWorld* World = Run->World.Get();
		if (!World)
		{
			World = FindAnyEditorOrGameWorld();
			if (!World)
			{
				if (FPlatformTime::Seconds() - Run->LaunchSeconds > 120.0)
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("TestAutoCycle: timed out waiting for a world."));
					if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
					return false;
				}
				return true;
			}
			Run->World = World;
		}

		if (!Run->bInitialized)
		{
			for (TActorIterator<ANDGIActor> It(World); It; ++It)
			{
				Run->Actor = *It;
				break;
			}
			if (!Run->Actor)
			{
				FActorSpawnParameters SpawnParameters;
				SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
				Run->Actor = World->SpawnActor<ANDGIActor>(FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
			}
			Run->Profile = UNDGITimeOfDayProfile::LoadDefaultProfile();
			if (!Run->Actor || !Run->Profile)
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("TestAutoCycle: actor or TOD profile missing; run ndgi.CreateTODProfile first."));
				if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
				return false;
			}

			Run->Actor->Profile = Run->Profile;
			Run->Actor->LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *GetBenchmarkLevelAssetName(World));
			Run->Actor->CycleDurationSeconds = 5.0f;
			Run->Actor->bAutoCycle = true;
			Run->Actor->ApplyTimeOfDay();
			Run->StartSeconds = FPlatformTime::Seconds();
			Run->StartTime = Run->Actor->TimeOfDay;
			Run->StartDecodes = Run->Actor->GetCompletedDecodeCount();

			UE_LOG(LogNDGIRuntime, Display,
				TEXT("TestAutoCycle: %.1f s at 5 s/cycle, start time %.3f, decodes %d."),
				Run->Duration, Run->StartTime, Run->StartDecodes);
			Run->bInitialized = true;
			return true;
		}

		if (FPlatformTime::Seconds() - Run->StartSeconds < Run->Duration)
		{
			return true;
		}

		const float EndTime = Run->Actor->TimeOfDay;
		const int32 EndDecodes = Run->Actor->GetCompletedDecodeCount();
		Run->Actor->bAutoCycle = false;
		Run->Actor->ApplyTimeOfDay();

		const float Advance = FMath::Abs(EndTime - Run->StartTime);
		const bool bPassed = Advance > 0.1f && EndDecodes >= Run->StartDecodes;
		UE_LOG(LogNDGIRuntime, Display,
			TEXT("TestAutoCycle: %s - time %.3f -> %.3f (%.3f), %d -> %d decode(s)."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), Run->StartTime, EndTime, Advance, Run->StartDecodes, EndDecodes);
		if (Run->bQuitWhenDone) { RequestBenchmarkExit(); }
		return false;
	}

	void RunTestAutoCycle(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		TSharedRef<FAutoCycleTestRun> Run = MakeShared<FAutoCycleTestRun>();
		Run->LaunchSeconds = FPlatformTime::Seconds();
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				Run->bQuitWhenDone = true;
			}
			else if (Arg.IsNumeric())
			{
				Run->Duration = FMath::Clamp(FCString::Atod(*Arg), 0.5, 60.0);
			}
		}
		FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateLambda([Run](float) { return TickAutoCycleTest(Run); }), 0.0f);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGICreateTODProfileCommand(
	TEXT("ndgi.CreateTODProfile"),
	TEXT("Creates/updates the default stylized time-of-day profile at /Game/NDGI/TOD/NDGITODProfile. Usage: ndgi.CreateTODProfile [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunCreateTODProfile));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestTODProfileCommand(
	TEXT("ndgi.TestTODProfile"),
	TEXT("Evaluates the default TOD profile at keyframes and midpoints. Usage: ndgi.TestTODProfile [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestTODProfile));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestAutoCycleCommand(
	TEXT("ndgi.TestAutoCycle"),
	TEXT("Runs ANDGIActor auto-cycle for N seconds and reports time/decode advancement. Usage: ndgi.TestAutoCycle <seconds> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestAutoCycle));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestAsyncCacheCommand(
	TEXT("ndgi.TestAsyncCache"),
	TEXT("Builds the current level's decode cache in per-atlas ticks and reports the worst tick time. Usage: ndgi.TestAsyncCache [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestAsyncCacheCommand));

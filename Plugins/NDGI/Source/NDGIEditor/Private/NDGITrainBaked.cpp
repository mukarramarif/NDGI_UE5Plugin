// Copyright Epic Games, Inc. All Rights Reserved.

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"
#include "NDGI.h"
#include "NDGIEditor.h"
#include "NDGIBakeManifest.h"
#include "NDGILightmapSet.h"
#include "NDGIPipelineStatus.h"
#include "NDGIModelExport.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"

#define LOCTEXT_NAMESPACE "NDGITrainBaked"

namespace
{
	using namespace NDGI;
	namespace BakeManifest = NDGI::Editor::BakeManifest;

	FString GetBakeDirectory()
	{
		return FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
	}

	/** Merges the per-time-step files written by ndgi.BakeTOD into one temporal set. */
	bool MergeOneBakedSet(const FString& Directory, int32 NumSteps, int32 TextureIndex)
	{
		FLightmapSet Merged;
		for (int32 Step = 0; Step < NumSteps; ++Step)
		{
			const FString FileName = FString::Printf(TEXT("Step%02d_Tex%02d.ndgi"), Step, TextureIndex);
			FLightmapSet StepSet;
			if (!StepSet.Load(Directory / FileName))
			{
				UE_LOG(LogNDGI, Error, TEXT("MergeBakedSets: missing %s (run ndgi.BakeTOD first)."), *FileName);
				return false;
			}
			if (Merged.Steps.Num() == 0)
			{
				Merged.Width = StepSet.Width;
				Merged.Height = StepSet.Height;
				Merged.Gamma = StepSet.Gamma;
				Merged.EncodingRanges = StepSet.EncodingRanges;
				Merged.EncodingRangeIndices = StepSet.EncodingRangeIndices;
			}
			if (StepSet.Width != Merged.Width || StepSet.Height != Merged.Height)
			{
				UE_LOG(LogNDGI, Error, TEXT("MergeBakedSets: %s has mismatched dimensions."), *FileName);
				return false;
			}
			Merged.Steps.Append(StepSet.Steps);
		}

		const FString OutputName = FString::Printf(TEXT("Merged_Tex%02d.ndgi"), TextureIndex);
		if (Merged.Save(Directory / OutputName))
		{
			UE_LOG(LogNDGI, Display, TEXT("MergeBakedSets: wrote %s (%dx%d, %d steps)."),
				*(Directory / OutputName), Merged.Width, Merged.Height, Merged.Steps.Num());
			return true;
		}

		UE_LOG(LogNDGI, Error, TEXT("MergeBakedSets: failed to save %s."), *(Directory / OutputName));
		return false;
	}

	void RunMergeBakedSets(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 NumSteps = 8;
		int32 TextureIndex = 0;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			const int32 Value = FCString::Atoi(*Arg);
			if (NumericArgIndex == 0) { NumSteps = FMath::Clamp(Value, 2, 64); }
			else if (NumericArgIndex == 1) { TextureIndex = FMath::Max(0, Value); }
			++NumericArgIndex;
		}

		MergeOneBakedSet(GetBakeDirectory(), NumSteps, TextureIndex);
	}

	/** Merges every atlas that has all requested steps (skips incomplete leftovers). */
	void RunMergeAllBakedSets(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 NumSteps = 8;
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				NumSteps = FMath::Clamp(FCString::Atoi(*Arg), 2, 64);
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Merging,
			TEXT("Merging bake slices"), 0, TEXT("Scanning bake folder..."));

		const FString Directory = GetBakeDirectory();
		TArray<int32> TextureIndices;

		BakeManifest::FManifest Manifest;
		if (BakeManifest::Load(Manifest))
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString WorldPackage = World ? World->GetOutermost()->GetName() : FString();
			if (!WorldPackage.IsEmpty() && Manifest.LevelPackage != WorldPackage)
			{
				UE_LOG(LogNDGI, Error,
					TEXT("MergeAllBakedSets: bake manifest belongs to '%s', but the current level is '%s'. Re-bake the current level."),
					*Manifest.LevelPackage, *WorldPackage);
				NDGI::Editor::Pipeline::Fail(TEXT("The bake belongs to a different level."));
				if (bQuitWhenDone && GEngine)
				{
					GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
				}
				return;
			}

			for (const BakeManifest::FAtlasEntry& Atlas : Manifest.Atlases)
			{
				TextureIndices.AddUnique(Atlas.TexIndex);
			}
			TextureIndices.Sort();
			UE_LOG(LogNDGI, Display,
				TEXT("MergeAllBakedSets: using bake manifest for '%s' (%d atlas(es), %d baked step(s))."),
				*Manifest.LevelPackage, TextureIndices.Num(), Manifest.Steps);
			if (Manifest.Steps != NumSteps)
			{
				UE_LOG(LogNDGI, Warning,
					TEXT("MergeAllBakedSets: merging %d step(s), but the manifest was baked with %d. Incomplete sets will be skipped."),
					NumSteps, Manifest.Steps);
			}
		}
		else
		{
			// Legacy bake folders predate the manifest; fall back to the old scan.
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Directory / TEXT("Step00_Tex*.ndgi")), true, false);
			for (const FString& File : Files)
			{
				const FString BaseName = FPaths::GetBaseFilename(File);
				TextureIndices.AddUnique(FCString::Atoi(*BaseName.RightChop(10)));
			}
			TextureIndices.Sort();
			UE_LOG(LogNDGI, Warning,
				TEXT("MergeAllBakedSets: no bake manifest in %s; scanning all step files. Re-bake for a level-scoped merge."),
				*Directory);
		}

		if (TextureIndices.Num() == 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("MergeAllBakedSets: no bake atlases found in %s. Run ndgi.BakeTOD first."), *Directory);
			NDGI::Editor::Pipeline::Fail(TEXT("No bake step files found. Run Bake first."));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
			return;
		}

		int32 MergedCount = 0;
		int32 SkippedCount = 0;
		for (int32 MergeIndex = 0; MergeIndex < TextureIndices.Num(); ++MergeIndex)
		{
			const int32 TextureIndex = TextureIndices[MergeIndex];
			NDGI::Editor::Pipeline::SetProgress(MergeIndex, TextureIndices.Num(),
				FString::Printf(TEXT("Merging atlas %d/%d (Tex%02d)"), MergeIndex + 1, TextureIndices.Num(), TextureIndex));
			bool bComplete = true;
			for (int32 Step = 0; Step < NumSteps && bComplete; ++Step)
			{
				const FString FileName = FString::Printf(TEXT("Step%02d_Tex%02d.ndgi"), Step, TextureIndex);
				bComplete = IFileManager::Get().FileExists(*(Directory / FileName));
			}
			if (!bComplete)
			{
				++SkippedCount;
				continue;
			}
			MergedCount += MergeOneBakedSet(Directory, NumSteps, TextureIndex) ? 1 : 0;
		}

		UE_LOG(LogNDGI, Display, TEXT("MergeAllBakedSets: %d atlas(es) merged, %d skipped (incomplete step set)."),
			MergedCount, SkippedCount);

		NDGI::Editor::Pipeline::Complete(MergedCount > 0,
			FString::Printf(TEXT("%d atlas(es) merged, %d skipped."), MergedCount, SkippedCount));

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	struct FTrainSetContext
	{
		Reference::FTileConfig ReferenceConfig;
		TArray<Reference::FTrainSample> EvalSamples;
		TArray<FVector> EvalCoords;
		TArray<float> StepMeans;
		TArray<float> SHMeans;
		float Gamma = 2.2f;
		FString PackageName;
		FString AssetName;
		bool bQuitWhenDone = false;
	};

	/** Trains one tile of a temporal lightmap set on the GPU and exports a model asset. */
	void TrainLightmapSetAtPath(const FString& SetPath, int32 TileX, int32 TileY, int32 TileSize, int32 TrainSteps, bool bQuitWhenDone)
	{
		auto Finish = [bQuitWhenDone]()
		{
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		FLightmapSet Set;
		if (!Set.Load(SetPath))
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: could not load %s."), *SetPath);
			Finish();
			return;
		}
		if (TileSize % 16 != 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: tile size must be a multiple of 16 (got %d)."), TileSize);
			Finish();
			return;
		}
		if ((TileX + 1) * TileSize > Set.Width || (TileY + 1) * TileSize > Set.Height)
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: tile (%d,%d) with size %d exceeds the %dx%d atlas."),
				TileX, TileY, TileSize, Set.Width, Set.Height);
			Finish();
			return;
		}

		Set.Preprocess(2.2f);
		UE_LOG(LogNDGI, Display, TEXT("TrainLightmapSet: %s (%dx%d, %d steps), tile (%d,%d) size %d, %d steps of training."),
			*SetPath, Set.Width, Set.Height, Set.Steps.Num(), TileX, TileY, TileSize, TrainSteps);

		const int32 BatchSize = 64;
		Reference::FTileConfig ReferenceConfig;
		ReferenceConfig.UVSize = TileSize;
		ReferenceConfig.UVTSize = TileSize / 4;
		ReferenceConfig.UTSize = TileSize / 2;
		ReferenceConfig.VTSize = TileSize / 2;
		ReferenceConfig.TimeSlices = Set.Steps.Num();
		ReferenceConfig.bSimulateBC = true;

		Train::FTileTrainConfig Config;
		Config.UVSize = ReferenceConfig.UVSize;
		Config.UVTSize = ReferenceConfig.UVTSize;
		Config.UTSize = ReferenceConfig.UTSize;
		Config.VTSize = ReferenceConfig.VTSize;
		Config.TimeSlices = ReferenceConfig.TimeSlices;
		Config.NumTiles = 1;
		Config.BatchSize = BatchSize;

		TArray<FVector> TrainCoords;
		TArray<float> TrainTargets;
		Set.GenerateTileSamples(TileX, TileY, TileSize, TrainSteps * BatchSize, 1337 ^ (TileX * 73856093) ^ (TileY * 19349663), TrainCoords, TrainTargets, 0);

		TSharedRef<FTrainSetContext> Context = MakeShared<FTrainSetContext>();
		Context->ReferenceConfig = ReferenceConfig;
		Context->StepMeans = Set.StepMeans;
		Context->SHMeans = Set.StepMeansSH;
		Context->Gamma = Set.Gamma;
		Context->bQuitWhenDone = bQuitWhenDone;
		Context->PackageName = FString::Printf(TEXT("/Game/NDGI/Trained/LightmapTile_%d_%d"), TileX, TileY);
		Context->AssetName = FString::Printf(TEXT("LightmapTile_%d_%d"), TileX, TileY);

		TArray<FVector> EvalCoords;
		TArray<float> EvalTargets;
		Set.GenerateTileSamples(TileX, TileY, TileSize, 4096, 4242 ^ (TileX * 73856093) ^ (TileY * 19349663), EvalCoords, EvalTargets);
		Context->EvalCoords = EvalCoords;
		Context->EvalSamples.SetNumUninitialized(EvalCoords.Num());
		for (int32 Index = 0; Index < EvalCoords.Num(); ++Index)
		{
			Reference::FTrainSample& Sample = Context->EvalSamples[Index];
			Sample.U = EvalCoords[Index].X;
			Sample.V = EvalCoords[Index].Y;
			Sample.Time = EvalCoords[Index].Z;
			for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
			{
				Sample.Target[Channel] = EvalTargets[Index * Reference::OutputDim + Channel];
			}
		}

		if (TrainCoords.Num() == 0 || EvalCoords.Num() == 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: tile (%d,%d) has no valid (masked) texels."), TileX, TileY);
			Finish();
			return;
		}

		Reference::FTileReference InitialTile(ReferenceConfig, 1337);
		TArray<double> InitialFlat;
		InitialTile.ExportFlatParameters(InitialFlat);
		TArray<float> Parameters;
		Parameters.SetNumUninitialized(InitialFlat.Num());
		for (int32 Index = 0; Index < InitialFlat.Num(); ++Index)
		{
			Parameters[Index] = static_cast<float>(InitialFlat[Index]);
		}

		TArray<float> CoordsFlat;
		CoordsFlat.Reserve(TrainCoords.Num() * 3);
		for (const FVector& Coord : TrainCoords)
		{
			CoordsFlat.Add(static_cast<float>(Coord.X));
			CoordsFlat.Add(static_cast<float>(Coord.Y));
			CoordsFlat.Add(static_cast<float>(Coord.Z));
		}
		TArray<float> Noise;
		Noise.SetNumUninitialized(TrainCoords.Num() * 12);
		FRandomStream NoiseStream(0x9e3779b9 ^ (TileX * 73856093) ^ (TileY * 19349663));
		const float Alpha = static_cast<float>(ReferenceConfig.NoiseAlpha);
		for (float& Value : Noise)
		{
			Value = (NoiseStream.FRand() - 0.5f) * Alpha;
		}

		Train::RunTrainingSteps(
			Config, Parameters, CoordsFlat, TrainTargets, Noise, TrainSteps,
			1e-3f, 0.9f, 0.999f, 1e-8f, 0,
			[Context](bool bSuccess, TArray<float>&& GpuParameters)
			{
				auto Finish = [Context]()
				{
					if (Context->bQuitWhenDone && GEngine)
					{
						GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
					}
				};

				if (!bSuccess || GpuParameters.Num() == 0)
				{
					UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: GPU training failed."));
					Finish();
					return;
				}

				TArray<double> Flat;
				Flat.SetNumUninitialized(GpuParameters.Num());
				for (int32 Index = 0; Index < GpuParameters.Num(); ++Index)
				{
					Flat[Index] = static_cast<double>(GpuParameters[Index]);
				}

				Reference::FTileReference Tile(Context->ReferenceConfig, 0);
				Tile.ImportFlatParameters(Flat);

				double Mse = 0.0;
				double Psnr = 0.0;
				Tile.Evaluate(Context->EvalSamples, Mse, Psnr);

				// Also report PSNR in linear RGB (the metric used by the paper).
				TArray<float> Normalized;
				Tile.EvaluatePoints(Context->EvalCoords, Normalized);
				double LinearSquared = 0.0;
				double MaxLinear = 0.0;
				for (int32 Index = 0; Index < Context->EvalSamples.Num(); ++Index)
				{
					const Reference::FTrainSample& Sample = Context->EvalSamples[Index];
					const int32 Step = FMath::Clamp(FMath::RoundToInt(Sample.Time * FMath::Max(1, Context->ReferenceConfig.TimeSlices - 1)),
						0, Context->ReferenceConfig.TimeSlices - 1);
					for (int32 Channel = 0; Channel < 3; ++Channel)
					{
						const double Mean = Context->StepMeans.Num() >= (Step + 1) * 3 ? Context->StepMeans[Step * 3 + Channel] : 0.0;
						const double PredictedLinear = FMath::Pow(FMath::Max(static_cast<double>(Normalized[Index * Reference::OutputDim + Channel]) + Mean, 0.0), Context->Gamma);
						const double TargetLinear = FMath::Pow(FMath::Max(static_cast<double>(Sample.Target[Channel]) + Mean, 0.0), Context->Gamma);
						const double Diff = PredictedLinear - TargetLinear;
						LinearSquared += Diff * Diff;
						MaxLinear = FMath::Max(MaxLinear, FMath::Abs(TargetLinear));
					}
				}
				const double LinearMse = LinearSquared / FMath::Max<int32>(1, Context->EvalSamples.Num() * 3);
				const double LinearRange = FMath::Max(MaxLinear, 1e-6);
				const double LinearPsnr = LinearMse > 0.0 ? 10.0 * FMath::LogX(10.0, LinearRange * LinearRange / LinearMse) : 99.0;

				UE_LOG(LogNDGI, Display, TEXT("TrainLightmapSet: tile (%s) normalized PSNR %.2f dB, linear PSNR %.2f dB."),
					*Context->AssetName, Psnr, LinearPsnr);

				FString Error;
				if (EditorTools::WriteModelAsset(Context->PackageName, Context->AssetName, Tile,
					Context->ReferenceConfig, Context->StepMeans, Context->SHMeans, Context->Gamma, Error))
				{
					SetNDGIPreviewModel(Context->PackageName + TEXT(".") + Context->AssetName);
					UE_LOG(LogNDGI, Display, TEXT("TrainLightmapSet: PASSED - saved %s.%s and selected it in the preview."),
						*Context->PackageName, *Context->AssetName);
				}
				else
				{
					UE_LOG(LogNDGI, Error, TEXT("TrainLightmapSet: model export failed: %s"), *Error);
				}
				Finish();
			});
	}

	void RunTrainLightmapSet(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		FString SetPath;
		bool bQuitWhenDone = false;
		int32 TileX = 0;
		int32 TileY = 0;
		int32 TileSize = 128;
		int32 TrainSteps = 1000;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (!Arg.IsNumeric() && SetPath.IsEmpty())
			{
				SetPath = Arg;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				switch (NumericArgIndex)
				{
				case 0: TileX = FMath::Max(0, Value); break;
				case 1: TileY = FMath::Max(0, Value); break;
				case 2: TileSize = FMath::Max(16, Value); break;
				case 3: TrainSteps = FMath::Max(1, Value); break;
				default: break;
				}
				++NumericArgIndex;
			}
		}

		if (SetPath.IsEmpty())
		{
			UE_LOG(LogNDGI, Error, TEXT("ndgi.TrainLightmapSet <setFilePath> <tileX> <tileY> <tileSize> <steps> [quit]"));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}
		TrainLightmapSetAtPath(SetPath, TileX, TileY, TileSize, TrainSteps, bQuitWhenDone);
	}

	void RunTrainBakedTile(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 TextureIndex = 0;
		int32 TileX = 0;
		int32 TileY = 0;
		int32 TileSize = 128;
		int32 TrainSteps = 1000;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				switch (NumericArgIndex)
				{
				case 0: TextureIndex = FMath::Max(0, Value); break;
				case 1: TileX = FMath::Max(0, Value); break;
				case 2: TileY = FMath::Max(0, Value); break;
				case 3: TileSize = FMath::Max(16, Value); break;
				case 4: TrainSteps = FMath::Max(1, Value); break;
				default: break;
				}
				++NumericArgIndex;
			}
		}

		const FString SetPath = GetBakeDirectory() / FString::Printf(TEXT("Merged_Tex%02d.ndgi"), TextureIndex);
		TrainLightmapSetAtPath(SetPath, TileX, TileY, TileSize, TrainSteps, bQuitWhenDone);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIMergeBakedSetsCommand(
	TEXT("ndgi.MergeBakedSets"),
	TEXT("Merges per-step bake exports into one temporal set: ndgi.MergeBakedSets <steps> <texIndex>."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunMergeBakedSets));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIMergeAllBakedSetsCommand(
	TEXT("ndgi.MergeAllBakedSets"),
	TEXT("Merges every complete atlas written by ndgi.BakeTOD. Usage: ndgi.MergeAllBakedSets <steps> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunMergeAllBakedSets));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITrainLightmapSetCommand(
	TEXT("ndgi.TrainLightmapSet"),
	TEXT("Trains one tile from a temporal lightmap set and exports/selects a model: ndgi.TrainLightmapSet <setFilePath> <tileX> <tileY> <tileSize> <steps> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTrainLightmapSet));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITrainBakedTileCommand(
	TEXT("ndgi.TrainBakedTile"),
	TEXT("Trains one tile from the merged bake export: ndgi.TrainBakedTile <texIndex> <tileX> <tileY> <tileSize> <steps> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTrainBakedTile));

#undef LOCTEXT_NAMESPACE

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "NDGILightmapSet.h"
#include "NDGI.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"

namespace
{
	using namespace NDGI;

	struct FLightmapSetTestContext
	{
		Train::FTileTrainConfig Config;
		Reference::FTileConfig ReferenceConfig;
		TArray<Reference::FTrainSample> EvalSamples;
		TArray<float> InitialParameters;
		double InitialPsnr = 0.0;
		bool bQuitWhenDone = false;
	};

	void EvalWithImportedParameters(const FLightmapSetTestContext& Context, const TArray<double>& FlatParameters, double& OutPsnr)
	{
		Reference::FTileReference Tile(Context.ReferenceConfig, 0);
		Tile.ImportFlatParameters(FlatParameters);
		double Mse = 0.0;
		Tile.Evaluate(Context.EvalSamples, Mse, OutPsnr);
	}

	void RunLightmapSetTest(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 TrainSteps = 1500;
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
				if (NumericArgIndex == 0) { TrainSteps = FMath::Max(1, FCString::Atoi(*Arg)); }
				++NumericArgIndex;
			}
		}

		const int32 Seed = 1337;
		const int32 Width = 64;
		const int32 Height = 64;
		const int32 NumSteps = 8;
		const int32 TileSize = 32;

		// 1) Synthetic set -> disk -> load round trip.
		FLightmapSet Set;
		GenerateSyntheticLightmapSet(Width, Height, NumSteps, Seed, Set);

		const FString SetPath = FPaths::ProjectSavedDir() / TEXT("NDGI/TestLightmapSet.ndgi");
		if (!Set.Save(SetPath))
		{
			UE_LOG(LogNDGI, Error, TEXT("Lightmap set test FAILED: could not save %s."), *SetPath);
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		FLightmapSet Loaded;
		bool bRoundTripOk = Loaded.Load(SetPath)
			&& Loaded.Width == Set.Width && Loaded.Height == Set.Height && Loaded.Steps.Num() == Set.Steps.Num();
		if (bRoundTripOk)
		{
			for (int32 StepIndex = 0; StepIndex < Set.Steps.Num() && bRoundTripOk; ++StepIndex)
			{
				bRoundTripOk &=
					FMemory::Memcmp(Loaded.Steps[StepIndex].RGB.GetData(), Set.Steps[StepIndex].RGB.GetData(),
						Set.Steps[StepIndex].RGB.Num() * sizeof(float)) == 0
					&& FMemory::Memcmp(Loaded.Steps[StepIndex].Mask.GetData(), Set.Steps[StepIndex].Mask.GetData(),
						Set.Steps[StepIndex].Mask.Num() * sizeof(uint8)) == 0
					&& FMemory::Memcmp(Loaded.Steps[StepIndex].SH.GetData(), Set.Steps[StepIndex].SH.GetData(),
						Set.Steps[StepIndex].SH.Num() * sizeof(float)) == 0;
			}
		}
		UE_LOG(LogNDGI, Display, TEXT("Lightmap set round trip: %s (%dx%d, %d steps, %.1f KB)."),
			bRoundTripOk ? TEXT("PASSED") : TEXT("FAILED"), Loaded.Width, Loaded.Height, Loaded.Steps.Num(),
			IFileManager::Get().FileSize(*SetPath) / 1024.0f);

		// 2) Preprocessing: gamma + masked mean normalization (RGB and SH separately).
		Loaded.Preprocess(2.2f);
		double MaxMaskedMean = 0.0;
		for (int32 StepIndex = 0; StepIndex < Loaded.Steps.Num(); ++StepIndex)
		{
			double Sum[3] = { 0.0, 0.0, 0.0 };
			double SumSH[3] = { 0.0, 0.0, 0.0 };
			int64 ValidCount = 0;
			const FLightmapStep& Step = Loaded.Steps[StepIndex];
			for (int32 Texel = 0; Texel < Loaded.Width * Loaded.Height; ++Texel)
			{
				if (Step.Mask[Texel] == 0) { continue; }
				++ValidCount;
				for (int32 Channel = 0; Channel < 3; ++Channel)
				{
					Sum[Channel] += Step.RGB[Texel * 3 + Channel];
					SumSH[Channel] += Step.SH[Texel * 3 + Channel];
				}
			}
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				MaxMaskedMean = FMath::Max(MaxMaskedMean, FMath::Abs(Sum[Channel] / FMath::Max<int64>(ValidCount, 1)));
				MaxMaskedMean = FMath::Max(MaxMaskedMean, FMath::Abs(SumSH[Channel] / FMath::Max<int64>(ValidCount, 1)));
			}
		}
		const bool bPreprocessOk = MaxMaskedMean < 1e-6;
		UE_LOG(LogNDGI, Display, TEXT("Preprocessing (gamma %.1f + mean norm): %s (max residual mean %.3e)."),
			Loaded.Gamma, bPreprocessOk ? TEXT("PASSED") : TEXT("FAILED"), MaxMaskedMean);

		// 3) Tile samples from the loaded (normalized) set.
		TSharedRef<FLightmapSetTestContext> Context = MakeShared<FLightmapSetTestContext>();
		Context->bQuitWhenDone = bQuitWhenDone;
		Context->Config.UVSize = TileSize;   // F_uv matches the tile resolution
		Context->Config.UVTSize = TileSize / 4;
		Context->Config.UTSize = TileSize / 2;
		Context->Config.VTSize = TileSize / 2;
		Context->Config.TimeSlices = NumSteps;
		Context->Config.BatchSize = 64;
		Context->ReferenceConfig.UVSize = Context->Config.UVSize;
		Context->ReferenceConfig.UVTSize = Context->Config.UVTSize;
		Context->ReferenceConfig.UTSize = Context->Config.UTSize;
		Context->ReferenceConfig.VTSize = Context->Config.VTSize;
		Context->ReferenceConfig.TimeSlices = Context->Config.TimeSlices;
		Context->ReferenceConfig.bSimulateBC = true;

		const int32 TotalParams = Train::ComputeTotalParameters(Context->Config);
		const int32 BatchSize = Context->Config.BatchSize;

		TArray<FVector> TrainCoords;
		TArray<float> TrainTargets;
		Loaded.GenerateTileSamples(0, 0, TileSize, TrainSteps * BatchSize, Seed ^ 0x11111111, TrainCoords, TrainTargets);

		TArray<FVector> EvalCoords;
		TArray<float> EvalTargets;
		Loaded.GenerateTileSamples(0, 0, TileSize, 4096, Seed ^ 0x22222222, EvalCoords, EvalTargets);

		const bool bSamplesOk = TrainCoords.Num() == TrainSteps * BatchSize && EvalCoords.Num() == 4096;
		UE_LOG(LogNDGI, Display, TEXT("Tile samples: %s (%d train, %d eval, %d params)."),
			bSamplesOk ? TEXT("PASSED") : TEXT("FAILED"), TrainCoords.Num(), EvalCoords.Num(), TotalParams);

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

		// 4) GPU training through the real data path.
		Reference::FTileReference InitialTile(Context->ReferenceConfig, Seed);
		TArray<double> InitialFlat;
		InitialTile.ExportFlatParameters(InitialFlat);
		for (double Value : InitialFlat)
		{
			Context->InitialParameters.Add(static_cast<float>(Value));
		}
		EvalWithImportedParameters(*Context, InitialFlat, Context->InitialPsnr);

		TArray<float> CoordsFlat;
		CoordsFlat.Reserve(TrainCoords.Num() * 3);
		for (const FVector& Coord : TrainCoords)
		{
			CoordsFlat.Add(static_cast<float>(Coord.X));
			CoordsFlat.Add(static_cast<float>(Coord.Y));
			CoordsFlat.Add(static_cast<float>(Coord.Z));
		}
		TArray<float> Noise;
		Noise.SetNumZeroed(TrainCoords.Num() * 12);
		FRandomStream NoiseStream(Seed ^ 0x9e3779b9);
		const float Alpha = static_cast<float>(Context->ReferenceConfig.NoiseAlpha);
		for (float& Value : Noise)
		{
			Value = (NoiseStream.FRand() - 0.5f) * Alpha;
		}

		// Diagnostic control: train the CPU reference on the exact same samples/noise.
		{
			TArray<Reference::FTrainSample> TrainSamples;
			TrainSamples.SetNumUninitialized(TrainCoords.Num());
			for (int32 Index = 0; Index < TrainCoords.Num(); ++Index)
			{
				Reference::FTrainSample& Sample = TrainSamples[Index];
				Sample.U = TrainCoords[Index].X;
				Sample.V = TrainCoords[Index].Y;
				Sample.Time = TrainCoords[Index].Z;
				for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
				{
					Sample.Target[Channel] = TrainTargets[Index * Reference::OutputDim + Channel];
				}
			}

			Reference::FTileReference CpuTile(Context->ReferenceConfig, Seed);
			const int32 CpuSteps = FMath::Min(TrainSteps, 2000);
			for (int32 Step = 0; Step < CpuSteps; ++Step)
			{
				const TConstArrayView<Reference::FTrainSample> BatchView(TrainSamples.GetData() + static_cast<int64>(Step) * BatchSize, BatchSize);
				const TConstArrayView<float> NoiseView(Noise.GetData() + static_cast<int64>(Step) * BatchSize * 12, BatchSize * 12);
				double MeanLoss = 0.0;
				CpuTile.TrainStep(BatchView, NoiseView, MeanLoss);
				if ((Step + 1) % 500 == 0)
				{
					UE_LOG(LogNDGI, Display, TEXT("  [CPU control] step %d/%d, train loss %.4e"), Step + 1, CpuSteps, MeanLoss);
				}
			}
			double CpuMse = 0.0;
			double CpuPsnr = 0.0;
			CpuTile.Evaluate(Context->EvalSamples, CpuMse, CpuPsnr);
			UE_LOG(LogNDGI, Display, TEXT("  [CPU control] final PSNR %.2f dB (same samples/noise)."), CpuPsnr);
		}

		UE_LOG(LogNDGI, Display, TEXT("Training tile 0 (32x32) on GPU for %d steps: initial PSNR %.2f dB."),
			TrainSteps, Context->InitialPsnr);

		Train::RunTrainingSteps(
			Context->Config,
			Context->InitialParameters,
			CoordsFlat,
			TrainTargets,
			Noise,
			TrainSteps,
			1e-3f, 0.9f, 0.999f, 1e-8f, 0,
			[Context, bRoundTripOk, bPreprocessOk, bSamplesOk](bool bSuccess, TArray<float>&& GpuParameters)
			{
				double FinalPsnr = 0.0;
				double PerOutputPsnr[Reference::OutputDim] = {};
				if (bSuccess && GpuParameters.Num() == Context->InitialParameters.Num())
				{
					TArray<double> Flat;
					Flat.SetNumUninitialized(GpuParameters.Num());
					for (int32 Index = 0; Index < GpuParameters.Num(); ++Index)
					{
						Flat[Index] = static_cast<double>(GpuParameters[Index]);
					}
					EvalWithImportedParameters(*Context, Flat, FinalPsnr);

					Reference::FTileReference Tile(Context->ReferenceConfig, 0);
					Tile.ImportFlatParameters(Flat);
					Tile.EvaluatePerOutput(Context->EvalSamples, PerOutputPsnr);
				}

				// 6 outputs (RGB + SH) need a larger training budget than the original
				// 3-output pipeline, so the gate is on a clear learning gain rather than
				// the old 24 dB / +15 dB RGB-only threshold.
				const double Improvement = FinalPsnr - Context->InitialPsnr;
				const bool bTrained = bSuccess && FinalPsnr >= 16.0 && Improvement >= 8.0;
				const bool bAllPassed = bRoundTripOk && bPreprocessOk && bSamplesOk && bTrained;

				const double RgbPsnr = (PerOutputPsnr[0] + PerOutputPsnr[1] + PerOutputPsnr[2]) / 3.0;
				const double SHPsnr = (PerOutputPsnr[3] + PerOutputPsnr[4] + PerOutputPsnr[5]) / 3.0;
				UE_LOG(LogNDGI, Display,
					TEXT("Lightmap-set training: %s (PSNR %.2f dB -> %.2f dB, +%.2f dB; RGB %.2f dB, SH %.2f dB)."),
					bTrained ? TEXT("PASSED") : TEXT("FAILED"), Context->InitialPsnr, FinalPsnr, Improvement, RgbPsnr, SHPsnr);

				if (bAllPassed)
				{
					UE_LOG(LogNDGI, Display, TEXT("Lightmap set test PASSED (round trip + preprocessing + GPU training)."));
				}
				else
				{
					UE_LOG(LogNDGI, Error, TEXT("Lightmap set test FAILED (round trip %s, preprocess %s, samples %s, trained %s)."),
						bRoundTripOk ? TEXT("ok") : TEXT("bad"),
						bPreprocessOk ? TEXT("ok") : TEXT("bad"),
						bSamplesOk ? TEXT("ok") : TEXT("bad"),
						bTrained ? TEXT("ok") : TEXT("bad"));
				}

				if (Context->bQuitWhenDone && GEngine)
				{
					GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
				}
			});
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestLightmapSetCommand(
	TEXT("ndgi.TestLightmapSet"),
	TEXT("Exercises the temporal lightmap set format, preprocessing and GPU training data path. Optional args: steps, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunLightmapSetTest));

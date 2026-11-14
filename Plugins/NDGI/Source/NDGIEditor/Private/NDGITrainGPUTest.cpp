// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "NDGI.h"
#include "NDGIReference.h"
#include "NDGITestGroundTruth.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"

namespace
{
	using namespace NDGI;
	using TestUtil::FGroundTruth;
	using TestUtil::GenerateSamples;

	/** Everything the async comparison callback needs to keep alive. */
	struct FGPUTrainContext
	{
		Train::FTileTrainConfig Config;
		Reference::FTileConfig ReferenceConfig;
		int32 NumSteps = 0;
		int32 Batch = 64;
		float LearningRate = 1e-3f;
		float Beta1 = 0.9f;
		float Beta2 = 0.999f;
		float Epsilon = 1e-8f;
		bool bQuitWhenDone = false;

		TArray<int32> TileSeeds;
		TArray<TArray<Reference::FTrainSample>> EvalPools;
		TArray<double> CpuFlatParameters; // [NumTiles * TotalParams]
		TArray<double> CpuPsnr;
		int32 TotalParams = 0;
	};

	Reference::FTileConfig MakeReferenceConfig(const Train::FTileTrainConfig& Config)
	{
		Reference::FTileConfig ReferenceConfig;
		ReferenceConfig.UVSize = Config.UVSize;
		ReferenceConfig.UVTSize = Config.UVTSize;
		ReferenceConfig.UTSize = Config.UTSize;
		ReferenceConfig.VTSize = Config.VTSize;
		ReferenceConfig.TimeSlices = Config.TimeSlices;
		ReferenceConfig.bSimulateBC = true;
		return ReferenceConfig;
	}

	void RunGPUTrainTest(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 Seed = 1337;
		int32 NumSteps = 20;
		int32 NumTiles = 1;
		int32 UVSize = 16;
		int32 TimeSlices = 4;
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
				const int32 Value = FCString::Atoi(*Arg);
				if (NumericArgIndex == 0) { Seed = Value; }
				else if (NumericArgIndex == 1) { NumSteps = FMath::Max(1, Value); }
				else if (NumericArgIndex == 2) { NumTiles = FMath::Max(1, Value); }
				else if (NumericArgIndex == 3) { UVSize = FMath::Max(4, Value); }
				else if (NumericArgIndex == 4) { TimeSlices = FMath::Max(2, Value); }
				++NumericArgIndex;
			}
		}

		TSharedRef<FGPUTrainContext> Context = MakeShared<FGPUTrainContext>();
		Context->Config.NumTiles = NumTiles;
		Context->Config.BatchSize = 64;
		Context->Config.UVSize = UVSize;
		Context->Config.UVTSize = UVSize / 4;
		Context->Config.UTSize = UVSize / 2;
		Context->Config.VTSize = UVSize / 2;
		Context->Config.TimeSlices = TimeSlices;
		Context->ReferenceConfig = MakeReferenceConfig(Context->Config);
		Context->NumSteps = NumSteps;
		Context->Batch = 64;
		Context->bQuitWhenDone = bQuitWhenDone;
		Context->TotalParams = Train::ComputeTotalParameters(Context->Config);

		UE_LOG(LogNDGI, Display, TEXT("GPU train test: %d tiles, %d steps, batch %d, seed %d (%d params/tile)."),
			NumTiles, NumSteps, Context->Batch, Seed, Context->TotalParams);

		TArray<float> InitialParameters;
		TArray<float> Coords;
		TArray<float> Targets;
		TArray<float> Noise;

		for (int32 TileIndex = 0; TileIndex < NumTiles; ++TileIndex)
		{
			const int32 TileSeed = Seed + TileIndex * 7919;
			Context->TileSeeds.Add(TileSeed);

			Reference::FTileReference Tile(Context->ReferenceConfig, TileSeed);
			const FGroundTruth GroundTruth = FGroundTruth::Make(TileSeed);

			TArray<Reference::FTrainSample> TrainPool = GenerateSamples(GroundTruth, TileSeed ^ 0x11111111, NumSteps * Context->Batch);
			Context->EvalPools.Add(GenerateSamples(GroundTruth, TileSeed ^ 0x22222222, 1024));

			FRandomStream NoiseStream(TileSeed ^ 0x9e3779b9);
			TArray<float> NoiseTile;
			NoiseTile.SetNumUninitialized(NumSteps * Context->Batch * 12);
			const float Alpha = static_cast<float>(Context->ReferenceConfig.NoiseAlpha);
			for (float& Value : NoiseTile)
			{
				Value = (NoiseStream.FRand() - 0.5f) * Alpha;
			}

			// Initial parameters, shared by the CPU and GPU runs.
			TArray<double> FlatParameters;
			Tile.ExportFlatParameters(FlatParameters);
			if (FlatParameters.Num() != Context->TotalParams)
			{
				UE_LOG(LogNDGI, Error, TEXT("Flat layout mismatch: CPU %d vs GPU %d."), FlatParameters.Num(), Context->TotalParams);
				if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
				return;
			}
			for (double Value : FlatParameters)
			{
				InitialParameters.Add(static_cast<float>(Value));
			}

			for (const Reference::FTrainSample& Sample : TrainPool)
			{
				Coords.Add(static_cast<float>(Sample.U));
				Coords.Add(static_cast<float>(Sample.V));
				Coords.Add(static_cast<float>(Sample.Time));
				for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
				{
					Targets.Add(static_cast<float>(Sample.Target[Channel]));
				}
			}
			Noise.Append(NoiseTile);

			// CPU reference training with the same samples and noise.
			for (int32 Step = 0; Step < NumSteps; ++Step)
			{
				const TConstArrayView<Reference::FTrainSample> BatchView(TrainPool.GetData() + static_cast<int64>(Step) * Context->Batch, Context->Batch);
				const TConstArrayView<float> NoiseView(NoiseTile.GetData() + static_cast<int64>(Step) * Context->Batch * 12, Context->Batch * 12);
				double MeanLoss = 0.0;
				Tile.TrainStep(BatchView, NoiseView, MeanLoss);
			}

			double Mse = 0.0;
			double Psnr = 0.0;
			Tile.Evaluate(Context->EvalPools.Last(), Mse, Psnr);
			Context->CpuPsnr.Add(Psnr);

			TArray<double> FinalFlat;
			Tile.ExportFlatParameters(FinalFlat);
			Context->CpuFlatParameters.Append(FinalFlat);

			UE_LOG(LogNDGI, Display, TEXT("  tile %d (seed %d): CPU final PSNR %.2f dB."), TileIndex, TileSeed, Psnr);
		}

		Train::RunTrainingSteps(
			Context->Config,
			InitialParameters,
			Coords,
			Targets,
			Noise,
			NumSteps,
			Context->LearningRate,
			Context->Beta1,
			Context->Beta2,
			Context->Epsilon,
			0,
			[Context](bool bSuccess, TArray<float>&& GpuParameters)
			{
				bool bAllPassed = bSuccess;
				const int32 NumTiles = Context->Config.NumTiles;

				UE_LOG(LogNDGI, Display, TEXT("GPU tile training completed: %s (%d params)."),
					bSuccess ? TEXT("ok") : TEXT("readback failed"), GpuParameters.Num());

				if (bSuccess && GpuParameters.Num() == NumTiles * Context->TotalParams)
				{
					for (int32 TileIndex = 0; TileIndex < NumTiles; ++TileIndex)
					{
						TArray<double> GpuFlat;
						GpuFlat.SetNumUninitialized(Context->TotalParams);
						double MaxAbsError = 0.0;
						for (int32 Index = 0; Index < Context->TotalParams; ++Index)
						{
							GpuFlat[Index] = static_cast<double>(GpuParameters[TileIndex * Context->TotalParams + Index]);
							const double CpuValue = Context->CpuFlatParameters[TileIndex * Context->TotalParams + Index];
							MaxAbsError = FMath::Max(MaxAbsError, FMath::Abs(GpuFlat[Index] - CpuValue));
						}

						Reference::FTileReference Imported(Context->ReferenceConfig, Context->TileSeeds[TileIndex]);
						Imported.ImportFlatParameters(GpuFlat);

						double Mse = 0.0;
						double Psnr = 0.0;
						Imported.Evaluate(Context->EvalPools[TileIndex], Mse, Psnr);

						const double PsnrDelta = FMath::Abs(Psnr - Context->CpuPsnr[TileIndex]);
						// Adam normalizes updates to +-lr, so a parameter whose gradient is near
						// zero in both runs can flip sign between float (GPU) and double (CPU) and
						// diverge by 2 * lr per step. The theoretical bound 2 * lr * steps catches
						// gross math errors; functional equivalence is measured by the PSNR delta.
						const double ParamBound = 2.0 * Context->LearningRate * Context->NumSteps;
						const bool bTilePassed = PsnrDelta < 0.25 && MaxAbsError < ParamBound;
						bAllPassed &= bTilePassed;

						UE_LOG(LogNDGI, Display,
							TEXT("%s tile %d: CPU PSNR %.2f dB, GPU PSNR %.2f dB (delta %.3f), max abs param diff %.3e"),
							bTilePassed ? TEXT("PASSED") : TEXT("FAILED"),
							TileIndex, Context->CpuPsnr[TileIndex], Psnr, PsnrDelta, MaxAbsError);
					}
				}
				else
				{
					bAllPassed = false;
				}

				if (bAllPassed)
				{
					UE_LOG(LogNDGI, Display, TEXT("GPU train test PASSED (CPU/GPU equivalence across %d tile(s))."), NumTiles);
				}
				else
				{
					UE_LOG(LogNDGI, Error, TEXT("GPU train test FAILED."));
				}

				if (Context->bQuitWhenDone && GEngine)
				{
					GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
				}
			});
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestTrainGPUCommand(
	TEXT("ndgi.TestTrainGPU"),
	TEXT("Runs GPU tile training steps and compares the result against the CPU tile reference. Optional args: seed, steps, tiles, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunGPUTrainTest));

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "NDGI.h"
#include "NDGIReference.h"
#include "NDGITestGroundTruth.h"
#include "NDGITileReference.h"

namespace
{
	using namespace NDGI;
	using TestUtil::FGroundTruth;
	using TestUtil::GenerateSamples;

	void LogGradientCheck(const TCHAR* Label, const Reference::FGradientCheckResult& Result)
	{
		const TCHAR* Verdict = Result.bPassed ? TEXT("PASSED") : TEXT("FAILED");
		UE_LOG(LogNDGI, Display,
			TEXT("%s %s: %d parameters, max abs err %.3e, max rel err %.3e (worst numeric %.6f vs analytic %.6f)"),
			Label, Verdict, Result.NumCheckedParameters, Result.MaxAbsError, Result.MaxRelError,
			Result.WorstNumeric, Result.WorstAnalytic);
	}

	Reference::FTileConfig MakeSmallCheckConfig(bool bSimulateBC)
	{
		Reference::FTileConfig Config;
		Config.UVSize = 8;
		Config.UVTSize = 4;
		Config.UTSize = 4;
		Config.VTSize = 4;
		Config.TimeSlices = 2;
		Config.bSimulateBC = bSimulateBC;
		return Config;
	}

	struct FOverfitOutcome
	{
		int32 NumParameters = 0;
		double InitialMSE = 0.0;
		double InitialPSNR = 0.0;
		double TrainedMSE = 0.0;
		double TrainedPSNR = 0.0;
		double FinalMSE = 0.0;
		double FinalPSNR = 0.0;
		double LastTrainLoss = 0.0;
	};

	FOverfitOutcome RunOverfit(int32 Seed, int32 Steps, int32 BatchSize, bool bSimulateBC)
	{
		Reference::FTileConfig Config;
		Config.bSimulateBC = bSimulateBC;

		Reference::FTileReference Tile(Config, Seed);
		const FGroundTruth GroundTruth = FGroundTruth::Make(Seed);

		const TArray<Reference::FTrainSample> TrainPool = GenerateSamples(GroundTruth, Seed ^ 0x11111111, BatchSize * Steps);
		const TArray<Reference::FTrainSample> EvalPool = GenerateSamples(GroundTruth, Seed ^ 0x22222222, 2048);

		FOverfitOutcome Outcome;
		Outcome.NumParameters = Tile.GetNumTrainableParameters();
		Tile.Evaluate(EvalPool, Outcome.InitialMSE, Outcome.InitialPSNR);

		FRandomStream NoiseStream(Seed ^ 0x9e3779b9);
		for (int32 Step = 0; Step < Steps; ++Step)
		{
			const TConstArrayView<Reference::FTrainSample> Batch(TrainPool.GetData() + static_cast<int64>(Step) * BatchSize, BatchSize);
			double MeanLoss = 0.0;
			Tile.TrainStep(Batch, NoiseStream, MeanLoss);
			Outcome.LastTrainLoss = MeanLoss;

			if ((Step + 1) % 100 == 0)
			{
				UE_LOG(LogNDGI, Display, TEXT("  [%s] step %d/%d, train loss %.4e"),
					bSimulateBC ? TEXT("BC sim") : TEXT("raw"), Step + 1, Steps, MeanLoss);
			}
		}

		Tile.Evaluate(EvalPool, Outcome.TrainedMSE, Outcome.TrainedPSNR);
		Tile.FinalizeCompression();
		Tile.Evaluate(EvalPool, Outcome.FinalMSE, Outcome.FinalPSNR);
		return Outcome;
	}

	void LogOverfitOutcome(const TCHAR* Label, const FOverfitOutcome& Outcome)
	{
		UE_LOG(LogNDGI, Display,
			TEXT("%s: %d params, eval MSE %.3e -> %.3e (trained) -> %.3e (8-bit final), PSNR %.2f dB -> %.2f dB -> %.2f dB, last train loss %.3e"),
			Label, Outcome.NumParameters, Outcome.InitialMSE, Outcome.TrainedMSE, Outcome.FinalMSE,
			Outcome.InitialPSNR, Outcome.TrainedPSNR, Outcome.FinalPSNR, Outcome.LastTrainLoss);
	}

	void RunTileOverfit(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 Seed = 1337;
		int32 Steps = 2000;
		int32 BatchSize = 64;
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
				if (NumericArgIndex == 0)
				{
					Seed = Value;
				}
				else if (NumericArgIndex == 1)
				{
					Steps = FMath::Max(1, Value);
				}
				else if (NumericArgIndex == 2)
				{
					BatchSize = FMath::Max(1, Value);
				}
				++NumericArgIndex;
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("Tile reference test: %d steps, batch %d, seed %d."), Steps, BatchSize, Seed);

		// 1) Scatter + BC backward finite-difference checks (both parameterizations).
		bool bChecksPassed = true;
		{
			Reference::FTileReference Tile(MakeSmallCheckConfig(true), Seed);
			const Reference::FGradientCheckResult Result = Tile.CheckScatterGradients(Seed, 8);
			LogGradientCheck(TEXT("Scatter+BC gradients (sim)"), Result);
			bChecksPassed &= Result.bPassed;
		}
		{
			Reference::FTileReference Tile(MakeSmallCheckConfig(false), Seed);
			const Reference::FGradientCheckResult Result = Tile.CheckScatterGradients(Seed, 8);
			LogGradientCheck(TEXT("Scatter gradients (raw)"), Result);
			bChecksPassed &= Result.bPassed;
		}

		// 2) Overfit the same tile with and without BC simulation (paper Table 7 direction).
		const FOverfitOutcome SimOutcome = RunOverfit(Seed, Steps, BatchSize, true);
		LogOverfitOutcome(TEXT("Overfit [BC sim  ]"), SimOutcome);

		const FOverfitOutcome RawOutcome = RunOverfit(Seed, Steps, BatchSize, false);
		LogOverfitOutcome(TEXT("Overfit [raw     ]"), RawOutcome);

		// 3) Verdict.
		const bool bLearnedBoth = SimOutcome.FinalMSE < SimOutcome.InitialMSE * 0.1
			&& RawOutcome.FinalMSE < RawOutcome.InitialMSE * 0.1;
		const bool bDirectionHolds = SimOutcome.FinalPSNR >= RawOutcome.FinalPSNR - 0.5;

		if (bChecksPassed && bLearnedBoth && bDirectionHolds)
		{
			UE_LOG(LogNDGI, Display, TEXT("Tile reference test PASSED (checks + overfit + BC-sim direction)."));
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("Tile reference test FAILED: checks %s, learned %s, direction %s."),
				bChecksPassed ? TEXT("ok") : TEXT("bad"),
				bLearnedBoth ? TEXT("ok") : TEXT("bad"),
				bDirectionHolds ? TEXT("ok") : TEXT("bad"));
		}

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestOverfitCommand(
	TEXT("ndgi.TestOverfit"),
	TEXT("Runs the tile-reference scatter/BC gradient checks and overfits a synthetic tile with/without BC simulation. Optional args: seed, steps, batch, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTileOverfit));

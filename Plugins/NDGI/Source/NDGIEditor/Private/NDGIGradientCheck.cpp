// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "NDGI.h"
#include "NDGIReference.h"

namespace
{
	void LogGradientCheckResult(const TCHAR* Name, const NDGI::Reference::FGradientCheckResult& Result)
	{
		const TCHAR* Verdict = Result.bPassed ? TEXT("PASSED") : TEXT("FAILED");
		UE_LOG(LogNDGI, Display,
			TEXT("%s %s: %d parameters checked, max abs err %.3e, max rel err %.3e (worst numeric %.6f vs analytic %.6f)"),
			Name, Verdict, Result.NumCheckedParameters, Result.MaxAbsError, Result.MaxRelError,
			Result.WorstNumeric, Result.WorstAnalytic);
	}

	void RunGradientChecks(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 Seed = 1337;
		int32 NumSamples = 16;
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
					NumSamples = FMath::Max(1, Value);
				}
				++NumericArgIndex;
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("Running finite-difference gradient checks (seed %d, %d samples)..."), Seed, NumSamples);

		const NDGI::Reference::FGradientCheckResult MLPResult = NDGI::Reference::CheckMLPGradients(Seed, NumSamples);
		LogGradientCheckResult(TEXT("MLP backward"), MLPResult);

		const NDGI::Reference::FGradientCheckResult BCResult = NDGI::Reference::CheckBCGradients(Seed, NDGI::Reference::FeatureDimUV);
		LogGradientCheckResult(TEXT("BC simulation backward"), BCResult);

		if (MLPResult.bPassed && BCResult.bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("Gradient checks PASSED."));
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("Gradient checks FAILED."));
		}

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestGradientsCommand(
	TEXT("ndgi.TestGradients"),
	TEXT("Runs finite-difference gradient checks of the CPU reference (MLP + BC simulation). Optional args: seed, samples, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunGradientChecks));

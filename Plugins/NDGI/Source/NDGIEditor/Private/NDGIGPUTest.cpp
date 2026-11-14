// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "NDGI.h"
#include "NDGIMLPTest.h"
#include "NDGIReference.h"

namespace
{
	using namespace NDGI;

	static_assert(Test::NumParameters == Reference::NumMLPParameters, "Kernel/reference parameter count mismatch");

	struct FArrayComparison
	{
		int32 NumValues = 0;
		int32 NumViolations = 0;
		double MaxAbsError = 0.0;
		double MaxRelError = 0.0;
		int32 WorstIndex = INDEX_NONE;
	};

	FArrayComparison CompareArrays(const TArray<float>& Actual, const TArray<float>& Expected,
		double AbsTolerance, double RelTolerance)
	{
		FArrayComparison Result;
		if (Actual.Num() != Expected.Num())
		{
			Result.NumViolations = -1;
			return Result;
		}

		Result.NumValues = Actual.Num();
		for (int32 Index = 0; Index < Actual.Num(); ++Index)
		{
			const double A = Actual[Index];
			const double E = Expected[Index];
			const double AbsError = FMath::Abs(A - E);
			const double Scale = FMath::Max(FMath::Max(FMath::Abs(A), FMath::Abs(E)), 1e-12);
			const double RelError = AbsError / Scale;

			if (AbsError > AbsTolerance + RelTolerance * Scale)
			{
				++Result.NumViolations;
			}
			if (RelError > Result.MaxRelError)
			{
				Result.MaxRelError = RelError;
				Result.WorstIndex = Index;
			}
			Result.MaxAbsError = FMath::Max(Result.MaxAbsError, AbsError);
		}
		return Result;
	}

	bool LogComparison(const TCHAR* Label, const TArray<float>& GPU, const TArray<float>& CPU,
		double AbsTolerance, double RelTolerance)
	{
		const FArrayComparison Comparison = CompareArrays(GPU, CPU, AbsTolerance, RelTolerance);
		const bool bPassed = (Comparison.NumViolations == 0);
		const TCHAR* Verdict = bPassed ? TEXT("PASSED") : TEXT("FAILED");
		const double WorstGPU = (Comparison.WorstIndex != INDEX_NONE) ? GPU[Comparison.WorstIndex] : 0.0;
		const double WorstCPU = (Comparison.WorstIndex != INDEX_NONE) ? CPU[Comparison.WorstIndex] : 0.0;

		UE_LOG(LogNDGI, Display,
			TEXT("%s %s: %d values, %d violations, max abs err %.3e, max rel err %.3e (worst @%d: GPU %.7f vs CPU %.7f)"),
			Label, Verdict, Comparison.NumValues, Comparison.NumViolations,
			Comparison.MaxAbsError, Comparison.MaxRelError, Comparison.WorstIndex, WorstGPU, WorstCPU);
		return bPassed;
	}

	/** Shared state for the async forward -> backward test chain. */
	struct FGPUTestContext
	{
		int32 NumSamples = 0;
		bool bQuitWhenDone = false;
		bool bForwardPassed = false;

		TArray<float> Parameters;
		TArray<float> Inputs;
		TArray<float> Targets;
		TArray<float> CPUForward;
		TArray<float> CPUGradients;
	};

	TSharedRef<FGPUTestContext> PrepareContext(int32 Seed, int32 NumSamples, bool bQuitWhenDone)
	{
		TSharedRef<FGPUTestContext> Context = MakeShared<FGPUTestContext>();
		Context->NumSamples = NumSamples;
		Context->bQuitWhenDone = bQuitWhenDone;

		// Weights: PyTorch-default init, rounded to float so the GPU and CPU operate on identical values.
		Reference::FMLPWeights Weights;
		Weights.Initialize(Seed);
		Weights.QuantizeToFloat();

		TArray<double> FlatParameters;
		Reference::FlattenParameters(Weights, FlatParameters);
		Context->Parameters.SetNumUninitialized(FlatParameters.Num());
		for (int32 Index = 0; Index < FlatParameters.Num(); ++Index)
		{
			Context->Parameters[Index] = static_cast<float>(FlatParameters[Index]);
		}

		// Samples: same generator pattern as the reference checks.
		FRandomStream Stream(Seed ^ 0x5bd1e995);
		Context->Inputs.SetNumUninitialized(NumSamples * Reference::InputDim);
		for (float& Value : Context->Inputs)
		{
			Value = Stream.FRand();
		}
		Context->Targets.SetNumUninitialized(NumSamples * Reference::OutputDim);
		for (float& Value : Context->Targets)
		{
			Value = Stream.FRand();
		}

		// CPU ground truth (double math on the exact float inputs).
		Context->CPUForward.SetNumUninitialized(NumSamples * (Reference::OutputDim + 1));
		Context->CPUGradients.SetNumUninitialized(NumSamples * Test::SampleGradientCount);
		for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
		{
			double Input[Reference::InputDim];
			double Target[Reference::OutputDim];
			for (int32 k = 0; k < Reference::InputDim; ++k)
			{
				Input[k] = static_cast<double>(Context->Inputs[SampleIndex * Reference::InputDim + k]);
			}
			for (int32 j = 0; j < Reference::OutputDim; ++j)
			{
				Target[j] = static_cast<double>(Context->Targets[SampleIndex * Reference::OutputDim + j]);
			}

			Reference::FMLPForwardCache Cache;
			Reference::Forward(Weights, Input, Cache);
			for (int32 j = 0; j < Reference::OutputDim; ++j)
			{
				Context->CPUForward[SampleIndex * Reference::OutputDim + j] = static_cast<float>(Cache.Output[j]);
			}
			Context->CPUForward[NumSamples * Reference::OutputDim + SampleIndex] = static_cast<float>(Reference::Loss(Cache, Target));

			Reference::FMLPWeights Gradients;
			Weights.ZeroGradients(Gradients);
			double InputGradient[Reference::InputDim];
			Reference::Backward(Weights, Input, Cache, Target, Gradients, InputGradient);

			TArray<double> FlatGradients;
			Reference::FlattenParameters(Gradients, FlatGradients);

			const int32 Base = SampleIndex * Test::SampleGradientCount;
			for (int32 p = 0; p < Test::NumParameters; ++p)
			{
				Context->CPUGradients[Base + p] = static_cast<float>(FlatGradients[p]);
			}
			for (int32 k = 0; k < Reference::InputDim; ++k)
			{
				Context->CPUGradients[Base + Test::NumParameters + k] = static_cast<float>(InputGradient[k]);
			}
		}

		return Context;
	}

	void FinishTest(const TSharedRef<FGPUTestContext>& Context, bool bAllPassed)
	{
		if (bAllPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("GPU kernel test PASSED (forward + backward vs CPU reference)."));
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("GPU kernel test FAILED (see mismatches above)."));
		}

		if (Context->bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void RunGPUTest(const TArray<FString>& Args, FOutputDevice& OutputDevice)
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
					NumSamples = FMath::Clamp(Value, 1, 4096);
				}
				++NumericArgIndex;
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("GPU kernel test: %d samples, seed %d."), NumSamples, Seed);

		TSharedRef<FGPUTestContext> Context = PrepareContext(Seed, NumSamples, bQuitWhenDone);

		Test::MLPForward(Context->Parameters, Context->Inputs, Context->Targets, NumSamples,
			[Context](bool bForwardSuccess, TArray<float>&& ForwardResults)
			{
				if (!bForwardSuccess)
				{
					UE_LOG(LogNDGI, Error, TEXT("Forward kernel readback failed."));
				}

				Context->bForwardPassed = LogComparison(TEXT("Forward kernel"), ForwardResults, Context->CPUForward, 1e-5, 1e-4);

				Test::MLPBackward(Context->Parameters, Context->Inputs, Context->Targets, Context->NumSamples,
					[Context](bool bBackwardSuccess, TArray<float>&& Gradients)
					{
						if (!bBackwardSuccess)
						{
							UE_LOG(LogNDGI, Error, TEXT("Backward kernel readback failed."));
						}

						const bool bBackwardPassed = LogComparison(TEXT("Backward kernel"), Gradients, Context->CPUGradients, 1e-4, 1e-3);
						FinishTest(Context, Context->bForwardPassed && bBackwardPassed);
					});
			});
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestGPUCommand(
	TEXT("ndgi.TestGPU"),
	TEXT("Runs the GPU MLP forward/backward kernels and diffs them against the CPU reference. Optional args: seed, samples, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunGPUTest));

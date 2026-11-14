// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "Math/RandomStream.h"
#include "NDGI.h"
#include "NDGIRuntimeDecode.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"

namespace
{
	using namespace NDGI;

	struct FDecodeTestContext
	{
		TArray<float> CpuOutputs;
		TArray<float> Parameters;
		TArray<float> Coords;
		bool bQuitWhenDone = false;
	};

	void RunDecodeTest(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 NumSamples = 256;
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
				if (NumericArgIndex == 0) { NumSamples = FMath::Clamp(FCString::Atoi(*Arg), 1, 65536); }
				++NumericArgIndex;
			}
		}

		Reference::FTileConfig ReferenceConfig;
		ReferenceConfig.bSimulateBC = true;
		Reference::FTileReference Tile(ReferenceConfig, 1337);

		Train::FTileTrainConfig Config;
		Config.NumTiles = 1;
		Config.BatchSize = 64;

		TSharedRef<FDecodeTestContext> Context = MakeShared<FDecodeTestContext>();
		Context->bQuitWhenDone = bQuitWhenDone;

		TArray<double> Flat;
		Tile.ExportFlatParameters(Flat);
		Context->Parameters.SetNumUninitialized(Flat.Num());
		for (int32 Index = 0; Index < Flat.Num(); ++Index)
		{
			Context->Parameters[Index] = static_cast<float>(Flat[Index]);
		}

		FRandomStream Stream(1337 ^ 0x51ed270b);
		TArray<FVector> Points;
		Points.SetNumUninitialized(NumSamples);
		Context->Coords.SetNumUninitialized(NumSamples * 3);
		for (int32 Index = 0; Index < NumSamples; ++Index)
		{
			Points[Index] = FVector(Stream.FRand(), Stream.FRand(), Stream.FRand());
			Context->Coords[Index * 3 + 0] = static_cast<float>(Points[Index].X);
			Context->Coords[Index * 3 + 1] = static_cast<float>(Points[Index].Y);
			Context->Coords[Index * 3 + 2] = static_cast<float>(Points[Index].Z);
		}

		Tile.EvaluatePoints(Points, Context->CpuOutputs);

		Decode::DecodeTile(Config, Context->Parameters, Context->Coords, 0,
			[Context](bool bSuccess, TArray<float>&& GpuOutputs)
			{
				bool bPassed = bSuccess && GpuOutputs.Num() == Context->CpuOutputs.Num();
				double MaxAbsError = 0.0;
				double MaxRelError = 0.0;
				if (bPassed)
				{
					for (int32 Index = 0; Index < GpuOutputs.Num(); ++Index)
					{
						const double A = GpuOutputs[Index];
						const double E = Context->CpuOutputs[Index];
						const double AbsError = FMath::Abs(A - E);
						const double Scale = FMath::Max(FMath::Max(FMath::Abs(A), FMath::Abs(E)), 1e-12);
						MaxAbsError = FMath::Max(MaxAbsError, AbsError);
						MaxRelError = FMath::Max(MaxRelError, AbsError / Scale);
						if (AbsError > 1e-5 + 1e-4 * Scale)
						{
							bPassed = false;
						}
					}
				}

				UE_LOG(LogNDGI, Display,
					TEXT("Runtime decode vs CPU: %s (%d values, max abs err %.3e, max rel err %.3e)."),
					bPassed ? TEXT("PASSED") : TEXT("FAILED"), GpuOutputs.Num(), MaxAbsError, MaxRelError);

				if (Context->bQuitWhenDone && GEngine)
				{
					GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
				}
			});
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestRuntimeDecodeCommand(
	TEXT("ndgi.TestRuntimeDecode"),
	TEXT("Runs the runtime decode kernel and validates it against the CPU reference. Optional args: samples, quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunDecodeTest));

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"

/**
 * Shared synthetic ground truth for the editor-side tile tests: a smooth analytic
 * field over (u, v, t) that the feature maps and MLP can represent.
 */
namespace NDGI
{
	namespace TestUtil
	{
		struct FGroundTruth
		{
			double FreqU[Reference::OutputDim] = {};
			double FreqV[Reference::OutputDim] = {};
			double FreqT[Reference::OutputDim] = {};
			double Phase[Reference::OutputDim] = {};
			double Amp[Reference::OutputDim] = {};

			static FGroundTruth Make(int32 Seed)
			{
				FRandomStream Stream(Seed ^ 0x68bc21eb);
				FGroundTruth Result;
				for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
				{
					Result.FreqU[Channel] = 1.0 + static_cast<double>(Stream.FRand());
					Result.FreqV[Channel] = 0.5 + 1.5 * static_cast<double>(Stream.FRand());
					Result.FreqT[Channel] = 0.25 + 0.75 * static_cast<double>(Stream.FRand());
					Result.Phase[Channel] = static_cast<double>(Stream.FRand());
					Result.Amp[Channel] = 0.25 + 0.2 * static_cast<double>(Stream.FRand());
				}
				return Result;
			}

			void Evaluate(double U, double V, double Time, double OutTarget[Reference::OutputDim]) const
			{
				for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
				{
					const double Value = 0.5 + Amp[Channel]
						* FMath::Sin(2.0 * PI * (FreqU[Channel] * U + Phase[Channel]))
						* FMath::Cos(2.0 * PI * FreqV[Channel] * V)
						* FMath::Cos(2.0 * PI * FreqT[Channel] * Time);
					OutTarget[Channel] = FMath::Clamp(Value, 0.0, 1.0);
				}
			}
		};

		inline TArray<Reference::FTrainSample> GenerateSamples(const FGroundTruth& GroundTruth, int32 Seed, int32 Count)
		{
			FRandomStream Stream(Seed);
			TArray<Reference::FTrainSample> Samples;
			Samples.SetNumUninitialized(Count);
			for (Reference::FTrainSample& Sample : Samples)
			{
				Sample.U = static_cast<double>(Stream.FRand());
				Sample.V = static_cast<double>(Stream.FRand());
				Sample.Time = static_cast<double>(Stream.FRand());
				GroundTruth.Evaluate(Sample.U, Sample.V, Sample.Time, Sample.Target);
			}
			return Samples;
		}
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIReference.h"

#include "Math/RandomStream.h"

#include <cmath>

namespace NDGI
{
	namespace Reference
	{
		namespace
		{
			constexpr double Sqrt2 = 1.41421356237309504880168872420969808;
			constexpr double Sqrt2Pi = 2.50662827463100050241576528481104525;

			double GeluDerivative(double X)
			{
				return 0.5 * (1.0 + std::erf(X / Sqrt2)) + X * std::exp(-0.5 * X * X) / Sqrt2Pi;
			}

			double UniformNoiseBand(FRandomStream& Stream)
			{
				// Uniform in [-1, 1].
				return 2.0 * static_cast<double>(Stream.FRand()) - 1.0;
			}

			int32 TotalParameters()
			{
				return HiddenDim * InputDim + HiddenDim
					+ HiddenDim * HiddenDim + HiddenDim
					+ OutputDim * HiddenDim + OutputDim;
			}

			/** Flat-index accessor over all MLP parameters (weights and biases). */
			void SetParameter(FMLPWeights& Weights, int32 Index, double Value)
			{
				check(Index >= 0 && Index < TotalParameters());
				int32 Offset = Index;
				if (Offset < HiddenDim * InputDim) { Weights.W1[Offset] = Value; return; }
				Offset -= HiddenDim * InputDim;
				if (Offset < HiddenDim) { Weights.B1[Offset] = Value; return; }
				Offset -= HiddenDim;
				if (Offset < HiddenDim * HiddenDim) { Weights.W2[Offset] = Value; return; }
				Offset -= HiddenDim * HiddenDim;
				if (Offset < HiddenDim) { Weights.B2[Offset] = Value; return; }
				Offset -= HiddenDim;
				if (Offset < OutputDim * HiddenDim) { Weights.W3[Offset] = Value; return; }
				Offset -= OutputDim * HiddenDim;
				Weights.B3[Offset] = Value;
			}

			double GetParameter(const FMLPWeights& Weights, int32 Index)
			{
				check(Index >= 0 && Index < TotalParameters());
				int32 Offset = Index;
				if (Offset < HiddenDim * InputDim) { return Weights.W1[Offset]; }
				Offset -= HiddenDim * InputDim;
				if (Offset < HiddenDim) { return Weights.B1[Offset]; }
				Offset -= HiddenDim;
				if (Offset < HiddenDim * HiddenDim) { return Weights.W2[Offset]; }
				Offset -= HiddenDim * HiddenDim;
				if (Offset < HiddenDim) { return Weights.B2[Offset]; }
				Offset -= HiddenDim;
				if (Offset < OutputDim * HiddenDim) { return Weights.W3[Offset]; }
				Offset -= OutputDim * HiddenDim;
				return Weights.B3[Offset];
			}

			double BatchLoss(const FMLPWeights& Weights, const TArray<double>& Inputs, const TArray<double>& Targets, int32 NumSamples)
			{
				double Total = 0.0;
				for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
				{
					FMLPForwardCache Cache;
					Forward(Weights, &Inputs[SampleIndex * InputDim], Cache);
					Total += Loss(Cache, &Targets[SampleIndex * OutputDim]);
				}
				return Total;
			}
		}

		void EncodeTime(double T, double OutTime[4])
		{
			OutTime[0] = FMath::Sin(2.0 * PI * T);
			OutTime[1] = FMath::Cos(2.0 * PI * T);
			OutTime[2] = FMath::Sin(4.0 * PI * T);
			OutTime[3] = FMath::Cos(4.0 * PI * T);
		}

		double Gelu(double X)
		{
			return 0.5 * X * (1.0 + std::erf(X / Sqrt2));
		}

		void FMLPWeights::Initialize(int32 Seed)
		{
			FRandomStream Stream(Seed);

			// PyTorch nn.Linear defaults: U(-1/sqrt(fan_in), 1/sqrt(fan_in)) for weights and bias
			// (kaiming_uniform with a=sqrt(5) collapses to this bound).
			auto InitLayer = [&Stream](TArray<double>& Weights, TArray<double>& Biases, int32 FanIn, int32 FanOut)
			{
				const double Bound = 1.0 / FMath::Sqrt(static_cast<double>(FanIn));
				Weights.SetNumUninitialized(FanOut * FanIn);
				for (double& Value : Weights)
				{
					Value = UniformNoiseBand(Stream) * Bound;
				}
				Biases.SetNumUninitialized(FanOut);
				for (double& Value : Biases)
				{
					Value = UniformNoiseBand(Stream) * Bound;
				}
			};

			InitLayer(W1, B1, InputDim, HiddenDim);
			InitLayer(W2, B2, HiddenDim, HiddenDim);
			InitLayer(W3, B3, HiddenDim, OutputDim);
		}

		void FMLPWeights::ZeroGradients(FMLPWeights& OutGradients) const
		{
			OutGradients.W1.SetNumZeroed(W1.Num());
			OutGradients.B1.SetNumZeroed(B1.Num());
			OutGradients.W2.SetNumZeroed(W2.Num());
			OutGradients.B2.SetNumZeroed(B2.Num());
			OutGradients.W3.SetNumZeroed(W3.Num());
			OutGradients.B3.SetNumZeroed(B3.Num());
		}

		void FMLPWeights::QuantizeToFloat()
		{
			auto Quantize = [](TArray<double>& Values)
			{
				for (double& Value : Values)
				{
					Value = static_cast<double>(static_cast<float>(Value));
				}
			};
			Quantize(W1);
			Quantize(B1);
			Quantize(W2);
			Quantize(B2);
			Quantize(W3);
			Quantize(B3);
		}

		void FlattenParameters(const FMLPWeights& Weights, TArray<double>& OutParameters)
		{
			OutParameters.Reset(NumMLPParameters);
			OutParameters.Append(Weights.W1);
			OutParameters.Append(Weights.B1);
			OutParameters.Append(Weights.W2);
			OutParameters.Append(Weights.B2);
			OutParameters.Append(Weights.W3);
			OutParameters.Append(Weights.B3);
			check(OutParameters.Num() == NumMLPParameters);
		}

		void UnflattenParameters(const TArray<double>& Parameters, FMLPWeights& OutWeights)
		{
			check(Parameters.Num() == NumMLPParameters);
			int32 Offset = 0;
			auto CopyInto = [&Parameters, &Offset](TArray<double>& Target, int32 Count)
			{
				Target.SetNumUninitialized(Count);
				FMemory::Memcpy(Target.GetData(), &Parameters[Offset], Count * sizeof(double));
				Offset += Count;
			};
			CopyInto(OutWeights.W1, HiddenDim * InputDim);
			CopyInto(OutWeights.B1, HiddenDim);
			CopyInto(OutWeights.W2, HiddenDim * HiddenDim);
			CopyInto(OutWeights.B2, HiddenDim);
			CopyInto(OutWeights.W3, OutputDim * HiddenDim);
			CopyInto(OutWeights.B3, OutputDim);
		}

		int32 FMLPWeights::NumParameters() const
		{
			return TotalParameters();
		}

		void Forward(const FMLPWeights& Weights, const double Input[InputDim], FMLPForwardCache& OutCache)
		{
			for (int32 Row = 0; Row < HiddenDim; ++Row)
			{
				double Sum = Weights.B1[Row];
				const double* WRow = &Weights.W1[Row * InputDim];
				for (int32 Col = 0; Col < InputDim; ++Col)
				{
					Sum += WRow[Col] * Input[Col];
				}
				OutCache.Hidden1Pre[Row] = Sum;
				OutCache.Hidden1[Row] = Gelu(Sum);
			}

			for (int32 Row = 0; Row < HiddenDim; ++Row)
			{
				double Sum = Weights.B2[Row];
				const double* WRow = &Weights.W2[Row * HiddenDim];
				for (int32 Col = 0; Col < HiddenDim; ++Col)
				{
					Sum += WRow[Col] * OutCache.Hidden1[Col];
				}
				OutCache.Hidden2Pre[Row] = Sum;
				OutCache.Hidden2[Row] = Gelu(Sum);
			}

			for (int32 Row = 0; Row < OutputDim; ++Row)
			{
				double Sum = Weights.B3[Row];
				const double* WRow = &Weights.W3[Row * HiddenDim];
				for (int32 Col = 0; Col < HiddenDim; ++Col)
				{
					Sum += WRow[Col] * OutCache.Hidden2[Col];
				}
				OutCache.Output[Row] = Sum;
			}
		}

		double Loss(const FMLPForwardCache& Cache, const double Target[OutputDim])
		{
			double Sum = 0.0;
			for (int32 Index = 0; Index < OutputDim; ++Index)
			{
				const double Diff = Cache.Output[Index] - Target[Index];
				Sum += OutputWeight(Index) * Diff * Diff;
			}
			return Sum;
		}

		void Backward(const FMLPWeights& Weights, const double Input[InputDim], const FMLPForwardCache& Cache,
			const double Target[OutputDim], FMLPWeights& OutGradients, double* OutInputGradients)
		{
			if (OutInputGradients)
			{
				for (int32 Index = 0; Index < InputDim; ++Index)
				{
					OutInputGradients[Index] = 0.0;
				}
			}
			double DHidden2[HiddenDim] = {};
			for (int32 Row = 0; Row < OutputDim; ++Row)
			{
				const double DOutput = 2.0 * OutputWeight(Row) * (Cache.Output[Row] - Target[Row]);
				OutGradients.B3[Row] += DOutput;
				double* DWRow = &OutGradients.W3[Row * HiddenDim];
				for (int32 Col = 0; Col < HiddenDim; ++Col)
				{
					DWRow[Col] += DOutput * Cache.Hidden2[Col];
					DHidden2[Col] += DOutput * Weights.W3[Row * HiddenDim + Col];
				}
			}

			double DHidden1[HiddenDim] = {};
			for (int32 Row = 0; Row < HiddenDim; ++Row)
			{
				const double DPre = DHidden2[Row] * GeluDerivative(Cache.Hidden2Pre[Row]);
				OutGradients.B2[Row] += DPre;
				double* DWRow = &OutGradients.W2[Row * HiddenDim];
				for (int32 Col = 0; Col < HiddenDim; ++Col)
				{
					DWRow[Col] += DPre * Cache.Hidden1[Col];
					DHidden1[Col] += DPre * Weights.W2[Row * HiddenDim + Col];
				}
			}

			for (int32 Row = 0; Row < HiddenDim; ++Row)
			{
				const double DPre = DHidden1[Row] * GeluDerivative(Cache.Hidden1Pre[Row]);
				OutGradients.B1[Row] += DPre;
				double* DWRow = &OutGradients.W1[Row * InputDim];
				for (int32 Col = 0; Col < InputDim; ++Col)
				{
					DWRow[Col] += DPre * Input[Col];
					if (OutInputGradients)
					{
						OutInputGradients[Col] += DPre * Weights.W1[Row * InputDim + Col];
					}
				}
			}
		}

		void FBCBlock::Resize(int32 InChannels)
		{
			Channels = InChannels;
			E1.SetNumUninitialized(Channels);
			E2.SetNumUninitialized(Channels);
			F.SetNumUninitialized(16 * Channels);
		}

		void BCForward(FBCBlock& Block)
		{
			for (int32 Texel = 0; Texel < 16; ++Texel)
			{
				const double Weight = Block.W[Texel];
				for (int32 Channel = 0; Channel < Block.Channels; ++Channel)
				{
					Block.F[Texel * Block.Channels + Channel] =
						(1.0 - Weight) * Block.E1[Channel] + Weight * Block.E2[Channel];
				}
			}
		}

		void BCBackward(const FBCBlock& Block, const TArray<double>& UpstreamGrad,
			TArray<double>& OutDE1, TArray<double>& OutDE2, double OutDW[16])
		{
			OutDE1.SetNumZeroed(Block.Channels);
			OutDE2.SetNumZeroed(Block.Channels);
			for (int32 Texel = 0; Texel < 16; ++Texel)
			{
				const double Weight = Block.W[Texel];
				double DTexel = 0.0;
				for (int32 Channel = 0; Channel < Block.Channels; ++Channel)
				{
					const double D = UpstreamGrad[Texel * Block.Channels + Channel];
					DTexel += D * (Block.E2[Channel] - Block.E1[Channel]);
					OutDE1[Channel] += (1.0 - Weight) * D;
					OutDE2[Channel] += Weight * D;
				}
				OutDW[Texel] = DTexel;
			}
		}

		void FAdamState::Initialize(int32 NumParameters)
		{
			M.SetNumZeroed(NumParameters);
			V.SetNumZeroed(NumParameters);
			Step = 0;
		}

		void FAdamState::Update(TArray<double>& Parameters, const TArray<double>& Gradients)
		{
			check(Parameters.Num() == Gradients.Num() && Parameters.Num() == M.Num() && Parameters.Num() == V.Num());
			++Step;
			const double Beta1Correction = 1.0 - FMath::Pow(Beta1, static_cast<double>(Step));
			const double Beta2Correction = 1.0 - FMath::Pow(Beta2, static_cast<double>(Step));
			for (int32 Index = 0; Index < Parameters.Num(); ++Index)
			{
				const double G = Gradients[Index];
				M[Index] = Beta1 * M[Index] + (1.0 - Beta1) * G;
				V[Index] = Beta2 * V[Index] + (1.0 - Beta2) * G * G;
				const double MHat = M[Index] / Beta1Correction;
				const double VHat = V[Index] / Beta2Correction;
				Parameters[Index] -= LearningRate * MHat / (FMath::Sqrt(VHat) + Epsilon);
			}
		}

		FGradientCheckResult CheckMLPGradients(int32 Seed, int32 NumSamples)
		{
			FGradientCheckResult Result;

			FMLPWeights Weights;
			Weights.Initialize(Seed);

			FRandomStream Stream(Seed ^ 0x5bd1e995);
			TArray<double> Inputs;
			TArray<double> Targets;
			Inputs.SetNumUninitialized(NumSamples * InputDim);
			Targets.SetNumUninitialized(NumSamples * OutputDim);
			for (double& Value : Inputs)
			{
				Value = static_cast<double>(Stream.FRand());
			}
			for (double& Value : Targets)
			{
				Value = static_cast<double>(Stream.FRand());
			}

			// Analytic gradients: sum of per-sample loss gradients.
			FMLPWeights Gradients;
			Weights.ZeroGradients(Gradients);
			for (int32 SampleIndex = 0; SampleIndex < NumSamples; ++SampleIndex)
			{
				FMLPForwardCache Cache;
				Forward(Weights, &Inputs[SampleIndex * InputDim], Cache);
				Backward(Weights, &Inputs[SampleIndex * InputDim], Cache, &Targets[SampleIndex * OutputDim], Gradients);
			}

			// Central finite differences over every parameter.
			// Near-zero gradients make relative error meaningless, so the pass criterion is the
			// standard |num - ana| <= atol + rtol * scale used by numpy.allclose-style checks.
			const double Epsilon = 1e-5;
			const double AbsTolerance = 1e-8;
			const double RelTolerance = 1e-5;
			double MaxViolation = -MAX_dbl;
			const int32 NumParameters = Weights.NumParameters();
			for (int32 ParameterIndex = 0; ParameterIndex < NumParameters; ++ParameterIndex)
			{
				const double Original = GetParameter(Weights, ParameterIndex);

				SetParameter(Weights, ParameterIndex, Original + Epsilon);
				const double LossPlus = BatchLoss(Weights, Inputs, Targets, NumSamples);

				SetParameter(Weights, ParameterIndex, Original - Epsilon);
				const double LossMinus = BatchLoss(Weights, Inputs, Targets, NumSamples);

				SetParameter(Weights, ParameterIndex, Original);

				const double Numeric = (LossPlus - LossMinus) / (2.0 * Epsilon);
				const double Analytic = GetParameter(Gradients, ParameterIndex);
				const double AbsError = FMath::Abs(Numeric - Analytic);
				const double Scale = FMath::Max(FMath::Abs(Numeric), FMath::Abs(Analytic));
				const double RelError = AbsError / FMath::Max(Scale, 1e-12);

				MaxViolation = FMath::Max(MaxViolation, AbsError - (AbsTolerance + RelTolerance * Scale));
				if (RelError > Result.MaxRelError)
				{
					Result.MaxRelError = RelError;
					Result.WorstNumeric = Numeric;
					Result.WorstAnalytic = Analytic;
				}
				Result.MaxAbsError = FMath::Max(Result.MaxAbsError, AbsError);
			}

			Result.NumCheckedParameters = NumParameters;
			Result.bPassed = MaxViolation < 0.0;
			return Result;
		}

		FGradientCheckResult CheckBCGradients(int32 Seed, int32 Channels)
		{
			FGradientCheckResult Result;

			FRandomStream Stream(Seed);

			FBCBlock Block;
			Block.Resize(Channels);
			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				Block.E1[Channel] = static_cast<double>(Stream.FRand());
				Block.E2[Channel] = static_cast<double>(Stream.FRand());
			}
			for (int32 Texel = 0; Texel < 16; ++Texel)
			{
				Block.W[Texel] = static_cast<double>(Stream.FRand());
			}

			TArray<double> UpstreamGrad;
			UpstreamGrad.SetNumUninitialized(16 * Channels);
			for (double& Value : UpstreamGrad)
			{
				Value = UniformNoiseBand(Stream);
			}

			// Scalar "loss" is the dot product of the upstream gradient with the reconstructed features.
			auto LossFn = [&](const FBCBlock& InBlock) -> double
			{
				FBCBlock Work = InBlock;
				BCForward(Work);
				double Dot = 0.0;
				for (int32 Index = 0; Index < Work.F.Num(); ++Index)
				{
					Dot += UpstreamGrad[Index] * Work.F[Index];
				}
				return Dot;
			};

			BCForward(Block);
			TArray<double> AnalyticDE1;
			TArray<double> AnalyticDE2;
			double AnalyticDW[16] = {};
			BCBackward(Block, UpstreamGrad, AnalyticDE1, AnalyticDE2, AnalyticDW);

			const double Epsilon = 1e-6;
			const double AbsTolerance = 1e-8;
			const double RelTolerance = 1e-5;
			double MaxViolation = -MAX_dbl;

			auto CheckScalar = [&Result, &MaxViolation, Epsilon, AbsTolerance, RelTolerance](double Numeric, double Analytic)
			{
				const double AbsError = FMath::Abs(Numeric - Analytic);
				const double Scale = FMath::Max(FMath::Abs(Numeric), FMath::Abs(Analytic));
				const double RelError = AbsError / FMath::Max(Scale, 1e-12);
				MaxViolation = FMath::Max(MaxViolation, AbsError - (AbsTolerance + RelTolerance * Scale));
				if (RelError > Result.MaxRelError)
				{
					Result.MaxRelError = RelError;
					Result.WorstNumeric = Numeric;
					Result.WorstAnalytic = Analytic;
				}
				Result.MaxAbsError = FMath::Max(Result.MaxAbsError, AbsError);
				++Result.NumCheckedParameters;
			};

			for (int32 Channel = 0; Channel < Channels; ++Channel)
			{
				for (int32 Endpoint = 0; Endpoint < 2; ++Endpoint)
				{
					TArray<double>* EndpointArray = (Endpoint == 0) ? &Block.E1 : &Block.E2;
					const double Original = (*EndpointArray)[Channel];

					(*EndpointArray)[Channel] = Original + Epsilon;
					const double LossPlus = LossFn(Block);
					(*EndpointArray)[Channel] = Original - Epsilon;
					const double LossMinus = LossFn(Block);
					(*EndpointArray)[Channel] = Original;

					const double Analytic = (Endpoint == 0) ? AnalyticDE1[Channel] : AnalyticDE2[Channel];
					CheckScalar((LossPlus - LossMinus) / (2.0 * Epsilon), Analytic);
				}
			}

			for (int32 Texel = 0; Texel < 16; ++Texel)
			{
				const double Original = Block.W[Texel];

				Block.W[Texel] = Original + Epsilon;
				const double LossPlus = LossFn(Block);
				Block.W[Texel] = Original - Epsilon;
				const double LossMinus = LossFn(Block);
				Block.W[Texel] = Original;

				CheckScalar((LossPlus - LossMinus) / (2.0 * Epsilon), AnalyticDW[Texel]);
			}

			Result.bPassed = MaxViolation < 0.0;
			return Result;
		}
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGITileLayout.h"

/**
 * Double-precision CPU reference implementation of the NDGI math (arXiv 2604.12625).
 *
 * This is the ground truth the HLSL forward/backward passes are validated against.
 * Layout follows the paper:
 *   - Input: F_uv(4) | F_ut(2) | F_vt(2) | F_uvt(4) | gamma(t)(4)  = 16
 *   - MLP: 16 -> Hidden(GELU) -> Hidden(GELU) -> 6 (linear)
 *   - Output: normalized RGB (0..2) followed by normalized SH.xyz (3..5)
 *   - Loss: squared L2 against the lightmap RGB + SH targets
 *   - BC simulation: per 4x4 block, per-channel endpoints E1/E2 in [0,1],
 *     one scalar weight W in [0,1] per texel, F_p = (1 - W_p) * E1 + W_p * E2
 *   - Optimizer: Adam (lr 1e-3 per the paper)
 */
namespace NDGI
{
	namespace Reference
	{
		constexpr int32 InputDim = Train::MLPInputDim;
		constexpr int32 HiddenDim = Train::MLPHiddenDim;
		constexpr int32 OutputDim = Train::MLPOutputDim;
		constexpr int32 FeatureDimUV = 4;
		constexpr int32 FeatureDimUT = 2;
		constexpr int32 FeatureDimVT = 2;
		constexpr int32 FeatureDimUVT = 4;

		constexpr int32 NumMLPParameters =
			HiddenDim * InputDim + HiddenDim
			+ HiddenDim * HiddenDim + HiddenDim
			+ OutputDim * HiddenDim + OutputDim;

		/** Must match NDGI_OUTPUT_WEIGHT_* in NDGIMLP.ush. */
		constexpr double OutputWeightRGB = 1.0;
		constexpr double OutputWeightSH = 4.0;

		inline double OutputWeight(int32 Index)
		{
			return Index < 3 ? OutputWeightRGB : OutputWeightSH;
		}

		/** Time encoding gamma(t) = [sin(2^0 pi t), cos(2^0 pi t), sin(2^1 pi t), cos(2^1 pi t)]. */
		void EncodeTime(double T, double OutTime[4]);

		/** GELU (exact, erf-based) and its derivative. */
		double Gelu(double X);

		struct FMLPWeights
		{
			// Row-major weights: W1 [HiddenDim x InputDim], W2 [HiddenDim x HiddenDim], W3 [OutputDim x HiddenDim].
			TArray<double> W1, B1;
			TArray<double> W2, B2;
			TArray<double> W3, B3;

			void Initialize(int32 Seed);
			void ZeroGradients(FMLPWeights& OutGradients) const;

			/** Rounds every parameter to float precision so GPU (float) and CPU (double) see identical values. */
			void QuantizeToFloat();

			int32 NumParameters() const;
		};

		/** Flattens parameters into the canonical GPU buffer layout: W1, B1, W2, B2, W3, B3. */
		void FlattenParameters(const FMLPWeights& Weights, TArray<double>& OutParameters);

		/** Inverse of FlattenParameters. */
		void UnflattenParameters(const TArray<double>& Parameters, FMLPWeights& OutWeights);

		struct FMLPForwardCache
		{
			double Hidden1Pre[HiddenDim] = {};
			double Hidden1[HiddenDim] = {};
			double Hidden2Pre[HiddenDim] = {};
			double Hidden2[HiddenDim] = {};
			double Output[OutputDim] = {};
		};

		/** Forward pass. Input must have InputDim entries. */
		void Forward(const FMLPWeights& Weights, const double Input[InputDim], FMLPForwardCache& OutCache);

		/** Per-sample loss: sum_j (Output_j - Target_j)^2. */
		double Loss(const FMLPForwardCache& Cache, const double Target[OutputDim]);

		/**
		 * Accumulates gradients of the per-sample loss into OutGradients (call ZeroGradients first).
		 * If OutInputGradients is non-null it receives the dLoss/dInput gradient (InputDim entries).
		 */
		void Backward(const FMLPWeights& Weights, const double Input[InputDim], const FMLPForwardCache& Cache,
			const double Target[OutputDim], FMLPWeights& OutGradients, double* OutInputGradients = nullptr);

		/** One 4x4 feature-map block stored in BC-simulation form. */
		struct FBCBlock
		{
			int32 Channels = 4;
			TArray<double> E1; // [Channels]
			TArray<double> E2; // [Channels]
			double W[16] = {};
			TArray<double> F;  // [16 * Channels], reconstructed feature values

			void Resize(int32 InChannels);
		};

		/** F_p = (1 - W_p) * E1 + W_p * E2 for every texel and channel. */
		void BCForward(FBCBlock& Block);

		/**
		 * Backward for the BC simulation. UpstreamGrad is dL/dF with the same layout as F.
		 * dW_p = dot(dF_p, E2 - E1); dE1 += (1 - W_p) * dF_p; dE2 += W_p * dF_p.
		 * The outputs are written (not accumulated).
		 */
		void BCBackward(const FBCBlock& Block, const TArray<double>& UpstreamGrad,
			TArray<double>& OutDE1, TArray<double>& OutDE2, double OutDW[16]);

		/** Adam optimizer state and update (paper: initial lr 1e-3). */
		struct FAdamState
		{
			double LearningRate = 1e-3;
			double Beta1 = 0.9;
			double Beta2 = 0.999;
			double Epsilon = 1e-8;
			int32 Step = 0;

			TArray<double> M;
			TArray<double> V;

			void Initialize(int32 NumParameters);
			void Update(TArray<double>& Parameters, const TArray<double>& Gradients);
		};

		struct FGradientCheckResult
		{
			int32 NumCheckedParameters = 0;
			double MaxAbsError = 0.0;
			double MaxRelError = 0.0;
			double WorstNumeric = 0.0;
			double WorstAnalytic = 0.0;
			bool bPassed = false;
		};

		/** Finite-difference check of the MLP backward pass over NumSamples random samples. */
		FGradientCheckResult CheckMLPGradients(int32 Seed, int32 NumSamples);

		/** Finite-difference check of the BC-simulation backward pass for one random block. */
		FGradientCheckResult CheckBCGradients(int32 Seed, int32 Channels);
	}
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "NDGIReference.h"

/**
 * CPU reference for one NDGI training tile (arXiv 2604.12625).
 *
 * Holds the complete parameter set for one tile:
 *   - F_uv   [UVSize x UVSize x 4]          BC-simulated (E1/E2/W) or raw texels
 *   - F_uvt  [UVTSize x UVTSize x T x 4]    BC-simulated per time slice, or raw
 *   - F_ut   [UTSize x T x 2]               raw, 8-bit-noise-emulated
 *   - F_vt   [VTSize x T x 2]               raw, 8-bit-noise-emulated
 *   - MLP                                          RGB + SH (see NDGITileLayout.h)
 *
 * A training step is: sample all four maps at (u, v, t) with noise, run the MLP
 * forward/backward, scatter the input gradient back through the interpolation
 * weights, convert texel gradients to BC endpoint/weight gradients, then apply
 * one Adam update to every parameter group.
 *
 * This implementation is the specification the GPU training pipelines must match.
 */
namespace NDGI
{
	namespace Reference
	{
		struct FTileConfig
		{
			int32 UVSize = 16;      // F_uv spatial resolution (multiple of 4)
			int32 UVTSize = 8;      // F_uvt spatial resolution per time slice (multiple of 4)
			int32 UTSize = 8;       // F_ut spatial (u) resolution
			int32 VTSize = 8;       // F_vt spatial (v) resolution
			int32 TimeSlices = 4;   // temporal resolution of F_ut / F_vt / F_uvt
			double NoiseAlpha = 1.0 / 256.0;
			double LearningRate = 1e-3;
			bool bSimulateBC = true;
		};

		struct FTrainSample
		{
			double U = 0.0;     // [0, 1)
			double V = 0.0;     // [0, 1)
			double Time = 0.0;  // [0, 1]
			double Target[OutputDim] = {};
		};

		/** Interpolation taps captured during sampling, required for the gradient scatter. */
		struct FSampleInterpolation
		{
			// F_uv bilinear (u, v)
			int32 UVX0 = 0, UVY0 = 0, UVX1 = 0, UVY1 = 0;
			double UVW00 = 0.0, UVW10 = 0.0, UVW01 = 0.0, UVW11 = 0.0;
			// F_ut bilinear (u, t)
			int32 UTX0 = 0, UTT0 = 0, UTX1 = 0, UTT1 = 0;
			double UTW00 = 0.0, UTW10 = 0.0, UTW01 = 0.0, UTW11 = 0.0;
			// F_vt bilinear (v, t)
			int32 VTX0 = 0, VTT0 = 0, VTX1 = 0, VTT1 = 0;
			double VTW00 = 0.0, VTW10 = 0.0, VTW01 = 0.0, VTW11 = 0.0;
			// F_uvt trilinear (u, v, t): UVTW000/100/010/110 are the four spatial (x, y) weights,
			// UVTW001/111 are the two temporal weights (T0 / T1).
			int32 UVTX0 = 0, UVTY0 = 0, UVTT0 = 0, UVTX1 = 0, UVTY1 = 0, UVTT1 = 0;
			double UVTW000 = 0.0, UVTW100 = 0.0, UVTW010 = 0.0, UVTW110 = 0.0;
			double UVTW001 = 0.0, UVTW101 = 0.0, UVTW011 = 0.0, UVTW111 = 0.0;
		};

		/** A flat parameter buffer with matching gradients and Adam state. */
		struct FParameterGroup
		{
			TArray<double> Values;
			TArray<double> Gradients;
			FAdamState Adam;

			void Init(int32 Count, double LearningRate)
			{
				Values.SetNumZeroed(Count);
				Gradients.SetNumZeroed(Count);
				Adam.Initialize(Count);
				Adam.LearningRate = LearningRate;
			}

			void ZeroGradients()
			{
				FMemory::Memzero(Gradients.GetData(), Gradients.Num() * sizeof(double));
			}

			void Update()
			{
				Adam.Update(Values, Gradients);
			}

			void Clamp01()
			{
				for (double& Value : Values)
				{
					Value = FMath::Clamp(Value, 0.0, 1.0);
				}
			}
		};

		/** 2D feature map in BC-simulation form: per 4x4 block endpoints E1/E2 (per channel) and per-texel weights W. */
		struct FBCMap
		{
			int32 SizeX = 0;
			int32 SizeY = 0;
			int32 Channels = 0;
			int32 BlocksX = 0;
			int32 BlocksY = 0;

			TArray<double> E1; // [NumBlocks * Channels]
			TArray<double> E2; // [NumBlocks * Channels]
			TArray<double> W;  // [NumBlocks * 16]

			// Transient per-step gradient storage.
			TArray<double> TexelGrad; // [SizeX * SizeY * Channels]
			TArray<double> DE1;
			TArray<double> DE2;
			TArray<double> DW;

			void Resize(int32 InSizeX, int32 InSizeY, int32 InChannels);
			int32 NumBlocks() const { return BlocksX * BlocksY; }

			int32 BlockIndex(int32 X, int32 Y) const;
			int32 TexelIndex(int32 X, int32 Y, int32 Channel) const;

			double GetTexel(int32 X, int32 Y, int32 Channel) const;
			void ZeroGradients();
			void ScatterTexelGradient(int32 X, int32 Y, int32 Channel, double Grad);
			void Backward(); // TexelGrad -> DE1/DE2/DW
		};

		class FTileReference
		{
		public:
			explicit FTileReference(const FTileConfig& InConfig, int32 Seed);

			const FTileConfig& GetConfig() const { return Config; }
			int32 GetNumTrainableParameters() const;

			/**
			 * Samples the four feature maps at (u, v, t) and builds the 16-wide MLP input.
			 * If Noise12 is non-null it must hold 12 per-sample values (already scaled and
			 * centered) that are added to the first 12 input components.
			 */
			void Sample(double U, double V, double Time, const double* Noise12, double OutInput[InputDim], FSampleInterpolation& OutInterpolation) const;

			/** One Adam training step over the batch with externally provided noise ([Batch * 12], already scaled). */
			void TrainStep(TConstArrayView<FTrainSample> Batch, TConstArrayView<float> Noise, double& OutMeanLoss);

			/** One Adam training step, generating uniform U(-0.5, 0.5) * NoiseAlpha noise from the stream. */
			void TrainStep(TConstArrayView<FTrainSample> Batch, FRandomStream& NoiseStream, double& OutMeanLoss);

			/** Exports every parameter group in the canonical GPU flat layout (requires BC simulation). */
			void ExportFlatParameters(TArray<double>& Out) const;

			/** Inverse of ExportFlatParameters; also refreshes the sampling structures. */
			void ImportFlatParameters(const TArray<double>& In);

			/** Evaluates the tile at (u, v, t) coordinates and writes [Num * OutputDim] RGB + SH outputs. */
			void EvaluatePoints(const TArray<FVector>& Coords, TArray<float>& OutRGB) const;

			/** Reconstructs the finalized F_uv and F_uvt texels ([Size*Size*4] RGBA layout, slice-major for UVT). */
			void ReconstructFeatureTexels(TArray<double>& OutUV, TArray<double>& OutUVT) const;

			/** Applies the final compression stage (8-bit quantization; post-hoc BC encoding when not simulating BC). */
			void FinalizeCompression();

			/** Evaluates MSE and PSNR against sample targets using the current representation (no noise). */
			void Evaluate(const TArray<FTrainSample>& Samples, double& OutMSE, double& OutPSNR) const;

			/** Per-output PSNR (0..2 RGB, 3..5 SH) for diagnostics. */
			void EvaluatePerOutput(const TArray<FTrainSample>& Samples, double OutPSNR[OutputDim]) const;

			// --- Probe loss used only for the scatter/BC finite-difference check ---

			/** Sets the 12 probe weights applied to [V_uv(4), V_ut(2), V_vt(2), V_uvt(4)]. */
			void SetProbeWeights(const double ProbeWeights[12]);

			double ComputeProbeLoss(double U, double V, double Time) const;

			/** Finite-difference check of the scatter + BC backward path against the probe loss. */
			FGradientCheckResult CheckScatterGradients(int32 Seed, int32 NumSamples);

		private:
			void InitializeStorage(int32 Seed);
			void ZeroAllGradients();
			void ScatterInputGradient(const FSampleInterpolation& Interpolation, const double InputGrad[InputDim]);
			void ConvertBCGradients();
			void UpdateParameters();
			void SyncBCGroupsToMaps();

			bool UsesBCSampling() const { return Config.bSimulateBC || bFinalized; }
			void SampleUV(double U, double V, double OutValues[4], FSampleInterpolation& Interpolation) const;
			void SampleUT(double U, double Time, double OutValues[2], FSampleInterpolation& Interpolation) const;
			void SampleVT(double V, double Time, double OutValues[2], FSampleInterpolation& Interpolation) const;
			void SampleUVT(double U, double V, double Time, double OutValues[4], FSampleInterpolation& Interpolation) const;

			FTileConfig Config;
			FMLPWeights MLP;
			bool bFinalized = false;

			// F_uv
			FBCMap UVMap;            // used when UsesBCSampling()
			FParameterGroup UVE1Group, UVE2Group, UVWGroup;
			FParameterGroup UVRawGroup; // used when !UsesBCSampling()

			// F_uvt (one BC map per time slice)
			TArray<FBCMap> UVTMaps;
			FParameterGroup UVTE1Group, UVTE2Group, UVTWGroup;
			FParameterGroup UVTRawGroup;

			// F_ut / F_vt are always raw.
			FParameterGroup UTGroup;
			FParameterGroup VTGroup;

			FParameterGroup MLPGroup;

			double ProbeWeights[12] = {};
		};
	}
}

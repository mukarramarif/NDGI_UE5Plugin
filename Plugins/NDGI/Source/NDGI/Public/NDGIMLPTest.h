// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGITileLayout.h"

/**
 * Kernel-validation entry points (Phase 2).
 *
 * These run the MLP kernels over synthetic data and read the results back so the
 * editor-side tests can diff them against NDGI::Reference on the CPU. They are
 * superseded by the full training-step dispatch in Phase 3.
 *
 * Buffer layout constants (must match Shaders/Private/NDGIMLP.ush):
 *   - Parameters: 646 floats (W1, B1, W2, B2, W3, B3)
 *   - Input dimension: 16, output dimension: 6 (RGB + SH)
 */
namespace NDGI
{
	namespace Test
	{
		constexpr int32 InputDim = Train::MLPInputDim;
		constexpr int32 OutputDim = Train::MLPOutputDim;
		constexpr int32 NumParameters = Train::MLPParameterCount;
		constexpr int32 SampleGradientCount = NumParameters + InputDim;

		/**
		 * Runs the forward kernel over NumSamples samples.
		 * Results layout: [NumSamples * 6 outputs][NumSamples losses].
		 */
		NDGI_API void MLPForward(
			const TArray<float>& Parameters,
			const TArray<float>& Inputs,
			const TArray<float>& Targets,
			int32 NumSamples,
			TFunction<void(bool bSuccess, TArray<float>&& Results)> OnComplete);

		/**
		 * Runs the backward kernel over NumSamples samples.
		 * Results layout per sample: [646 parameter gradients][16 input gradients].
		 */
		NDGI_API void MLPBackward(
			const TArray<float>& Parameters,
			const TArray<float>& Inputs,
			const TArray<float>& Targets,
			int32 NumSamples,
			TFunction<void(bool bSuccess, TArray<float>&& Gradients)> OnComplete);
	}
}

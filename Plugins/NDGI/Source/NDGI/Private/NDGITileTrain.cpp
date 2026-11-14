// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGITileTrain.h"

#include "NDGI.h"
#include "NDGIReadback.h"
#include "NDGITileShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

IMPLEMENT_GLOBAL_SHADER(FNDGITrainTileCS, "/Plugin/NDGI/Private/NDGITrainTile.usf", "TrainTileCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FNDGIAdamCS, "/Plugin/NDGI/Private/NDGIAdam.usf", "AdamCS", SF_Compute);

void NDGI::Train::RunTrainingSteps(
	const FTileTrainConfig& Config,
	const TArray<float>& InitialParameters,
	const TArray<float>& SampleCoords,
	const TArray<float>& SampleTargets,
	const TArray<float>& SampleNoise,
	int32 NumSteps,
	float LearningRate,
	float Beta1,
	float Beta2,
	float Epsilon,
	int32 FirstAdamStep,
	TFunction<void(bool bSuccess, TArray<float>&& OutParameters)> OnComplete)
{
	const int32 TotalParams = ComputeTotalParameters(Config);
	const int32 NumTiles = Config.NumTiles;
	const int32 BatchSize = Config.BatchSize;

	const bool bValidInputs =
		NumTiles > 0
		&& BatchSize > 0 && BatchSize <= static_cast<int32>(NDGITileShader::ThreadGroupSize)
		&& NumSteps >= 0
		&& InitialParameters.Num() == NumTiles * TotalParams
		&& SampleCoords.Num() == NumTiles * NumSteps * BatchSize * 3
		&& SampleTargets.Num() == NumTiles * NumSteps * BatchSize * MLPOutputDim
		&& SampleNoise.Num() == NumTiles * NumSteps * BatchSize * 12;

	if (!bValidInputs)
	{
		if (OnComplete)
		{
			OnComplete(false, TArray<float>());
		}
		return;
	}

	TArray<float> ZeroMoments;
	ZeroMoments.SetNumZeroed(NumTiles * TotalParams);
	TArray<int32> ZeroGradients;
	ZeroGradients.SetNumZeroed(NumTiles * TotalParams);

	const FTileLayout Layout = ComputeTileLayout(Config);
	check(Layout.TotalParams == static_cast<uint32>(TotalParams));

	const uint32 NumElements = static_cast<uint32>(NumTiles * TotalParams);

	Internal::DispatchAndReadbackBytes(
		NumElements * sizeof(float),
		[Config, InitialParameters, ZeroMoments, ZeroGradients, SampleCoords, SampleTargets, SampleNoise, NumSteps,
			LearningRate, Beta1, Beta2, Epsilon, FirstAdamStep, NumElements, Layout]
		(FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)
		{
			FRDGBufferRef ParametersBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.Parameters"), InitialParameters);
			FRDGBufferRef MomentsMBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.MomentsM"), ZeroMoments);
			FRDGBufferRef MomentsVBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.MomentsV"), ZeroMoments);
			FRDGBufferRef GradientsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.GradientInt"), ZeroGradients);
			FRDGBufferRef CoordsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.SampleCoords"), SampleCoords);
			FRDGBufferRef TargetsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.SampleTargets"), SampleTargets);
			FRDGBufferRef NoiseBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Train.SampleNoise"), SampleNoise);

			for (int32 Step = 0; Step < NumSteps; ++Step)
			{
				{
					FNDGITrainTileCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGITrainTileCS::FParameters>();
					PassParameters->TileParameters = GraphBuilder.CreateSRV(ParametersBuffer);
					PassParameters->SampleCoords = GraphBuilder.CreateSRV(CoordsBuffer);
					PassParameters->SampleTargets = GraphBuilder.CreateSRV(TargetsBuffer);
					PassParameters->SampleNoise = GraphBuilder.CreateSRV(NoiseBuffer);
					PassParameters->GradientInt = GraphBuilder.CreateUAV(GradientsBuffer);
					PassParameters->NumSteps = static_cast<uint32>(NumSteps);
					PassParameters->BatchSize = static_cast<uint32>(Config.BatchSize);
					PassParameters->StepIndex = static_cast<uint32>(Step);
					PassParameters->TileParams.UVSize = static_cast<uint32>(Config.UVSize);
					PassParameters->TileParams.UVTSize = static_cast<uint32>(Config.UVTSize);
					PassParameters->TileParams.UTSize = static_cast<uint32>(Config.UTSize);
					PassParameters->TileParams.VTSize = static_cast<uint32>(Config.VTSize);
					PassParameters->TileParams.TimeSlices = static_cast<uint32>(Config.TimeSlices);
					PassParameters->TileParams.UVBlocksX = Layout.UVBlocksX;
					PassParameters->TileParams.UVTBlocksX = Layout.UVTBlocksX;
					PassParameters->TileParams.MLPOffset = Layout.MLPOffset;
					PassParameters->TileParams.UVE1Offset = Layout.UVE1Offset;
					PassParameters->TileParams.UVE2Offset = Layout.UVE2Offset;
					PassParameters->TileParams.UVWOffset = Layout.UVWOffset;
					PassParameters->TileParams.UVTE1Offset = Layout.UVTE1Offset;
					PassParameters->TileParams.UVTE2Offset = Layout.UVTE2Offset;
					PassParameters->TileParams.UVTWOffset = Layout.UVTWOffset;
					PassParameters->TileParams.UTOffset = Layout.UTOffset;
					PassParameters->TileParams.VTOffset = Layout.VTOffset;
					PassParameters->TileParams.TotalParams = Layout.TotalParams;

					TShaderMapRef<FNDGITrainTileCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGITrainTile"),
						ComputeShader, PassParameters, FIntVector(Config.NumTiles, 1, 1));
				}

				{
					const int32 AdamStep = FirstAdamStep + Step + 1;
					const float BetaCorrection1 = 1.0f - FMath::Pow(Beta1, static_cast<float>(AdamStep));
					const float BetaCorrection2 = 1.0f - FMath::Pow(Beta2, static_cast<float>(AdamStep));

					FNDGIAdamCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIAdamCS::FParameters>();
					PassParameters->TileParameters = GraphBuilder.CreateUAV(ParametersBuffer);
					PassParameters->MomentsM = GraphBuilder.CreateUAV(MomentsMBuffer);
					PassParameters->MomentsV = GraphBuilder.CreateUAV(MomentsVBuffer);
					PassParameters->GradientInt = GraphBuilder.CreateUAV(GradientsBuffer);
					PassParameters->NumElements = NumElements;
					PassParameters->TotalParams = Layout.TotalParams;
					PassParameters->NumMLPParameters = static_cast<uint32>(MLPParameterCount);
					PassParameters->LearningRate = LearningRate;
					PassParameters->Beta1 = Beta1;
					PassParameters->Beta2 = Beta2;
					PassParameters->BetaCorrection1 = BetaCorrection1;
					PassParameters->BetaCorrection2 = BetaCorrection2;
					PassParameters->Epsilon = Epsilon;

					TShaderMapRef<FNDGIAdamCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
					const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(static_cast<int32>(NumElements), static_cast<int32>(NDGITileShader::ThreadGroupSize));
					FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGIAdam"), ComputeShader, PassParameters, GroupCount);
				}
			}

			AddEnqueueCopyPass(GraphBuilder, Readback, ParametersBuffer, static_cast<uint32>(NumBytes));
		},
		[OnComplete = MoveTemp(OnComplete)](bool bSuccess, TArray<uint8>&& Bytes)
		{
			TArray<float> Parameters;
			if (bSuccess && (Bytes.Num() % sizeof(float)) == 0)
			{
				Parameters.SetNumUninitialized(Bytes.Num() / sizeof(float));
				FMemory::Memcpy(Parameters.GetData(), Bytes.GetData(), Bytes.Num());
			}
			if (OnComplete)
			{
				OnComplete(Parameters.Num() > 0, MoveTemp(Parameters));
			}
		});
}

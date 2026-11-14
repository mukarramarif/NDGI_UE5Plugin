// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIShaders.h"

#include "NDGI.h"
#include "NDGIReadback.h"
#include "NDGIMLPShaders.h"
#include "NDGIMLPTest.h"
#include "RHIGPUReadback.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"

IMPLEMENT_GLOBAL_SHADER(FNDGISmokeTestCS, "/Plugin/NDGI/Private/NDGISmokeTest.usf", "MainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FNDGIMLPForwardCS, "/Plugin/NDGI/Private/NDGIMLPForward.usf", "ForwardMainCS", SF_Compute);
IMPLEMENT_GLOBAL_SHADER(FNDGIMLPBackwardCS, "/Plugin/NDGI/Private/NDGIMLPBackward.usf", "BackwardMainCS", SF_Compute);

void NDGI::DispatchSmokeTest(uint32 Count, uint32 Offset, TFunction<void(bool bSuccess, TArray<uint32>&& Values)> OnComplete)
{
	if (Count == 0)
	{
		if (OnComplete)
		{
			OnComplete(false, TArray<uint32>());
		}
		return;
	}

	Internal::DispatchAndReadbackBytes(
		Count * sizeof(uint32),
		[Count, Offset](FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)
		{
			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), Count),
				TEXT("NDGI.SmokeTest.Buffer"));

			FNDGISmokeTestCS::FParameters* Parameters = GraphBuilder.AllocParameters<FNDGISmokeTestCS::FParameters>();
			Parameters->OutputBuffer = GraphBuilder.CreateUAV(OutputBuffer);
			Parameters->Count = Count;
			Parameters->Offset = Offset;

			TShaderMapRef<FNDGISmokeTestCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount((int32)Count, (int32)FNDGISmokeTestCS::ThreadGroupSize);
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGISmokeTest"), ComputeShader, Parameters, GroupCount);

			AddEnqueueCopyPass(GraphBuilder, Readback, OutputBuffer, NumBytes);
		},
		[OnComplete = MoveTemp(OnComplete)](bool bSuccess, TArray<uint8>&& Bytes)
		{
			TArray<uint32> Values;
			if (bSuccess && (Bytes.Num() % sizeof(uint32)) == 0)
			{
				Values.SetNumUninitialized(Bytes.Num() / sizeof(uint32));
				FMemory::Memcpy(Values.GetData(), Bytes.GetData(), Bytes.Num());
			}
			if (OnComplete)
			{
				OnComplete(Values.Num() > 0, MoveTemp(Values));
			}
		});
}

void NDGI::Test::MLPForward(
	const TArray<float>& Parameters,
	const TArray<float>& Inputs,
	const TArray<float>& Targets,
	int32 NumSamples,
	TFunction<void(bool bSuccess, TArray<float>&& Results)> OnComplete)
{
	if (NumSamples <= 0
		|| Parameters.Num() != NumParameters
		|| Inputs.Num() != NumSamples * InputDim
		|| Targets.Num() != NumSamples * OutputDim)
	{
		if (OnComplete)
		{
			OnComplete(false, TArray<float>());
		}
		return;
	}

	const int32 ResultsPerSample = OutputDim + 1;
	Internal::DispatchAndReadbackBytes(
		static_cast<uint32>(NumSamples * ResultsPerSample * sizeof(float)),
		[Parameters, Inputs, Targets, NumSamples, ResultsPerSample](FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)
		{
			FRDGBufferRef ParametersBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.MLPParameters"), Parameters);
			FRDGBufferRef InputsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.SampleInputs"), Inputs);
			FRDGBufferRef TargetsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.SampleTargets"), Targets);
			FRDGBufferRef ResultsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumSamples * ResultsPerSample),
				TEXT("NDGI.Test.ForwardResults"));

			FNDGIMLPForwardCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIMLPForwardCS::FParameters>();
			PassParameters->MLPParameters = GraphBuilder.CreateSRV(ParametersBuffer);
			PassParameters->SampleInputs = GraphBuilder.CreateSRV(InputsBuffer);
			PassParameters->SampleTargets = GraphBuilder.CreateSRV(TargetsBuffer);
			PassParameters->ForwardResults = GraphBuilder.CreateUAV(ResultsBuffer);
			PassParameters->NumSamples = static_cast<uint32>(NumSamples);

			TShaderMapRef<FNDGIMLPForwardCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(NumSamples, static_cast<int32>(NDGIMLPShader::ThreadGroupSize));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGIMLPForward"), ComputeShader, PassParameters, GroupCount);

			AddEnqueueCopyPass(GraphBuilder, Readback, ResultsBuffer, NumBytes);
		},
		[OnComplete = MoveTemp(OnComplete)](bool bSuccess, TArray<uint8>&& Bytes)
		{
			TArray<float> Results;
			if (bSuccess && (Bytes.Num() % sizeof(float)) == 0)
			{
				Results.SetNumUninitialized(Bytes.Num() / sizeof(float));
				FMemory::Memcpy(Results.GetData(), Bytes.GetData(), Bytes.Num());
			}
			if (OnComplete)
			{
				OnComplete(Results.Num() > 0, MoveTemp(Results));
			}
		});
}

void NDGI::Test::MLPBackward(
	const TArray<float>& Parameters,
	const TArray<float>& Inputs,
	const TArray<float>& Targets,
	int32 NumSamples,
	TFunction<void(bool bSuccess, TArray<float>&& Gradients)> OnComplete)
{
	if (NumSamples <= 0
		|| Parameters.Num() != NumParameters
		|| Inputs.Num() != NumSamples * InputDim
		|| Targets.Num() != NumSamples * OutputDim)
	{
		if (OnComplete)
		{
			OnComplete(false, TArray<float>());
		}
		return;
	}

	Internal::DispatchAndReadbackBytes(
		static_cast<uint32>(NumSamples * SampleGradientCount * sizeof(float)),
		[Parameters, Inputs, Targets, NumSamples](FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)
		{
			FRDGBufferRef ParametersBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.MLPParameters"), Parameters);
			FRDGBufferRef InputsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.SampleInputs"), Inputs);
			FRDGBufferRef TargetsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Test.SampleTargets"), Targets);
			FRDGBufferRef GradientsBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumSamples * SampleGradientCount),
				TEXT("NDGI.Test.SampleGradients"));

			FNDGIMLPBackwardCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIMLPBackwardCS::FParameters>();
			PassParameters->MLPParameters = GraphBuilder.CreateSRV(ParametersBuffer);
			PassParameters->SampleInputs = GraphBuilder.CreateSRV(InputsBuffer);
			PassParameters->SampleTargets = GraphBuilder.CreateSRV(TargetsBuffer);
			PassParameters->SampleGradients = GraphBuilder.CreateUAV(GradientsBuffer);
			PassParameters->NumSamples = static_cast<uint32>(NumSamples);

			TShaderMapRef<FNDGIMLPBackwardCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(NumSamples, static_cast<int32>(NDGIMLPShader::ThreadGroupSize));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGIMLPBackward"), ComputeShader, PassParameters, GroupCount);

			AddEnqueueCopyPass(GraphBuilder, Readback, GradientsBuffer, NumBytes);
		},
		[OnComplete = MoveTemp(OnComplete)](bool bSuccess, TArray<uint8>&& Bytes)
		{
			TArray<float> Gradients;
			if (bSuccess && (Bytes.Num() % sizeof(float)) == 0)
			{
				Gradients.SetNumUninitialized(Bytes.Num() / sizeof(float));
				FMemory::Memcpy(Gradients.GetData(), Bytes.GetData(), Bytes.Num());
			}
			if (OnComplete)
			{
				OnComplete(Gradients.Num() > 0, MoveTemp(Gradients));
			}
		});
}

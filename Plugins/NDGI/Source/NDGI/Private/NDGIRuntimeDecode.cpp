// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIRuntimeDecode.h"

#include "NDGI.h"
#include "NDGIReadback.h"
#include "NDGITileShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RHIGPUReadback.h"

IMPLEMENT_GLOBAL_SHADER(FNDGIDecodeCS, "/Plugin/NDGI/Private/NDGIRuntimeDecode.usf", "DecodeCS", SF_Compute);

namespace
{
	void ComputeDecodeTileLayout(const NDGI::Train::FTileTrainConfig& Config,
		uint32& OutUVBlocksX, uint32& OutUVTBlocksX,
		uint32& OutMLPOffset, uint32& OutUVE1Offset, uint32& OutUVE2Offset, uint32& OutUVWOffset,
		uint32& OutUVTE1Offset, uint32& OutUVTE2Offset, uint32& OutUVTWOffset,
		uint32& OutUTOffset, uint32& OutVTOffset, uint32& OutTotalParams)
	{
		OutUVBlocksX = static_cast<uint32>(Config.UVSize / 4);
		OutUVTBlocksX = static_cast<uint32>(Config.UVTSize / 4);
		const uint32 UVBlocks = OutUVBlocksX * OutUVBlocksX;
		const uint32 UVTBlocksTotal = OutUVTBlocksX * OutUVTBlocksX * static_cast<uint32>(Config.TimeSlices);

		uint32 Offset = 0;
		OutMLPOffset = Offset; Offset += NDGI::Train::MLPParameterCount;
		OutUVE1Offset = Offset; Offset += UVBlocks * 4;
		OutUVE2Offset = Offset; Offset += UVBlocks * 4;
		OutUVWOffset = Offset; Offset += UVBlocks * 16;
		OutUVTE1Offset = Offset; Offset += UVTBlocksTotal * 4;
		OutUVTE2Offset = Offset; Offset += UVTBlocksTotal * 4;
		OutUVTWOffset = Offset; Offset += UVTBlocksTotal * 16;
		OutUTOffset = Offset; Offset += static_cast<uint32>(Config.UTSize * Config.TimeSlices * 2);
		OutVTOffset = Offset; Offset += static_cast<uint32>(Config.VTSize * Config.TimeSlices * 2);
		OutTotalParams = Offset;
	}
}

void NDGI::Decode::DecodeTile(
	const Train::FTileTrainConfig& Config,
	const TArray<float>& Parameters,
	const TArray<float>& SampleCoords,
	int32 TileIndex,
	TFunction<void(bool bSuccess, TArray<float>&& OutRGB)> OnComplete)
{
	const int32 TotalParams = Train::ComputeTotalParameters(Config);
	const int32 NumSamples = SampleCoords.Num() / 3;
	if (NumSamples <= 0
		|| Parameters.Num() != Config.NumTiles * TotalParams
		|| TileIndex < 0 || TileIndex >= Config.NumTiles)
	{
		if (OnComplete)
		{
			OnComplete(false, TArray<float>());
		}
		return;
	}

	uint32 UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset;
	uint32 UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal;
	ComputeDecodeTileLayout(Config, UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset,
		UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal);

	Internal::DispatchAndReadbackBytes(
		static_cast<uint32>(NumSamples * Train::MLPOutputDim * sizeof(float)),
		[Config, Parameters, SampleCoords, NumSamples, TileIndex,
			UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset,
			UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal]
		(FRDGBuilder& GraphBuilder, FRHIGPUBufferReadback* Readback, uint32 NumBytes)
		{
			FRDGBufferRef ParametersBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Decode.Parameters"), Parameters);
			FRDGBufferRef CoordsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Decode.Coords"), SampleCoords);
			FRDGBufferRef OutputBuffer = GraphBuilder.CreateBuffer(
				FRDGBufferDesc::CreateStructuredDesc(sizeof(float), NumSamples * Train::MLPOutputDim),
				TEXT("NDGI.Decode.Output"));

			FNDGIDecodeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIDecodeCS::FParameters>();
			PassParameters->TileParameters = GraphBuilder.CreateSRV(ParametersBuffer);
			PassParameters->SampleCoords = GraphBuilder.CreateSRV(CoordsBuffer);
			PassParameters->DecodedOutput = GraphBuilder.CreateUAV(OutputBuffer);
			PassParameters->NumSamples = static_cast<uint32>(NumSamples);
			PassParameters->TileIndex = static_cast<uint32>(TileIndex);
			PassParameters->TileParams.UVSize = static_cast<uint32>(Config.UVSize);
			PassParameters->TileParams.UVTSize = static_cast<uint32>(Config.UVTSize);
			PassParameters->TileParams.UTSize = static_cast<uint32>(Config.UTSize);
			PassParameters->TileParams.VTSize = static_cast<uint32>(Config.VTSize);
			PassParameters->TileParams.TimeSlices = static_cast<uint32>(Config.TimeSlices);
			PassParameters->TileParams.UVBlocksX = UVBlocksX;
			PassParameters->TileParams.UVTBlocksX = UVTBlocksX;
			PassParameters->TileParams.MLPOffset = MLPOffset;
			PassParameters->TileParams.UVE1Offset = UVE1Offset;
			PassParameters->TileParams.UVE2Offset = UVE2Offset;
			PassParameters->TileParams.UVWOffset = UVWOffset;
			PassParameters->TileParams.UVTE1Offset = UVTE1Offset;
			PassParameters->TileParams.UVTE2Offset = UVTE2Offset;
			PassParameters->TileParams.UVTWOffset = UVTWOffset;
			PassParameters->TileParams.UTOffset = UTOffset;
			PassParameters->TileParams.VTOffset = VTOffset;
			PassParameters->TileParams.TotalParams = LayoutTotal;

			TShaderMapRef<FNDGIDecodeCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(NumSamples, static_cast<int32>(NDGITileShader::ThreadGroupSize));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGIDecode"), ComputeShader, PassParameters, GroupCount);

			AddEnqueueCopyPass(GraphBuilder, Readback, OutputBuffer, NumBytes);
		},
		[OnComplete = MoveTemp(OnComplete)](bool bSuccess, TArray<uint8>&& Bytes)
		{
			TArray<float> Outputs;
			if (bSuccess && (Bytes.Num() % sizeof(float)) == 0)
			{
				Outputs.SetNumUninitialized(Bytes.Num() / sizeof(float));
				FMemory::Memcpy(Outputs.GetData(), Bytes.GetData(), Bytes.Num());
			}
			if (OnComplete)
			{
				OnComplete(Outputs.Num() > 0, MoveTemp(Outputs));
			}
		});
}

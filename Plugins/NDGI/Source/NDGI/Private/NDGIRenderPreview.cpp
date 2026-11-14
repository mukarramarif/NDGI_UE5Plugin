// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIRenderPreview.h"

#include "Engine/TextureRenderTarget2D.h"
#include "NDGI.h"
#include "NDGITileShaders.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "UnrealClient.h"

IMPLEMENT_GLOBAL_SHADER(FNDGIDecodeTextureCS, "/Plugin/NDGI/Private/NDGIDecodeTexture.usf", "DecodeTextureCS", SF_Compute);

namespace
{
	void ComputePreviewTileLayout(const NDGI::Train::FTileTrainConfig& Config,
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

void NDGI::Decode::DecodeTileToRenderTarget(
	const Train::FTileTrainConfig& Config,
	const TArray<float>& Parameters,
	int32 TileIndex,
	float Time,
	const FVector3f& MeanRGB,
	float Gamma,
	UTextureRenderTarget2D* Target)
{
	const int32 TotalParams = Train::ComputeTotalParameters(Config);
	if (!Target
		|| Parameters.Num() != Config.NumTiles * TotalParams
		|| TileIndex < 0 || TileIndex >= Config.NumTiles)
	{
		return;
	}

	Target->UpdateResourceImmediate(false);
	FTextureRenderTargetResource* Resource = Target->GameThread_GetRenderTargetResource();
	if (!Resource)
	{
		UE_LOG(LogNDGI, Warning, TEXT("DecodeTileToRenderTarget: render target has no resource."));
		return;
	}

	const uint32 OutputWidth = static_cast<uint32>(Target->SizeX);
	const uint32 OutputHeight = static_cast<uint32>(Target->SizeY);

	uint32 UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset;
	uint32 UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal;
	ComputePreviewTileLayout(Config, UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset,
		UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal);

	ENQUEUE_RENDER_COMMAND(NDGIDecodeToRenderTarget)(
		[Parameters, Resource, OutputWidth, OutputHeight, Time, MeanRGB, Gamma, TileIndex,
			UVBlocksX, UVTBlocksX, MLPOffset, UVE1Offset, UVE2Offset, UVWOffset,
			UVTE1Offset, UVTE2Offset, UVTWOffset, UTOffset, VTOffset, LayoutTotal,
			UVSize = static_cast<uint32>(Config.UVSize), UVTSize = static_cast<uint32>(Config.UVTSize),
			UTSize = static_cast<uint32>(Config.UTSize), VTSize = static_cast<uint32>(Config.VTSize),
			TimeSlices = static_cast<uint32>(Config.TimeSlices)]
		(FRHICommandListImmediate& RHICmdList)
		{
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef OutputRDG = Resource->GetRenderTargetTexture(GraphBuilder);
			FRDGBufferRef ParametersBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Preview.Parameters"), Parameters);

			FNDGIDecodeTextureCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIDecodeTextureCS::FParameters>();
			PassParameters->TileParameters = GraphBuilder.CreateSRV(ParametersBuffer);
			PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputRDG);
			PassParameters->OutputWidth = OutputWidth;
			PassParameters->OutputHeight = OutputHeight;
			PassParameters->Time = Time;
			PassParameters->MeanRGB = MeanRGB;
			PassParameters->Gamma = Gamma;
			PassParameters->TileIndex = static_cast<uint32>(TileIndex);
			PassParameters->TileParams.UVSize = UVSize;
			PassParameters->TileParams.UVTSize = UVTSize;
			PassParameters->TileParams.UTSize = UTSize;
			PassParameters->TileParams.VTSize = VTSize;
			PassParameters->TileParams.TimeSlices = TimeSlices;
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

			TShaderMapRef<FNDGIDecodeTextureCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
			const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
				FIntPoint(static_cast<int32>(OutputWidth), static_cast<int32>(OutputHeight)), FIntPoint(8, 8));
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("NDGIDecodeTexture"), ComputeShader, PassParameters, GroupCount);

			GraphBuilder.Execute();
		});
}

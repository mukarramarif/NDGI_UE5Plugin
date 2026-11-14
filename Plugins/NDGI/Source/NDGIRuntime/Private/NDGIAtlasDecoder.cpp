// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIAtlasDecoder.h"

#include "Async/Async.h"
#include "Engine/AssetManager.h"
#include "Engine/StreamableManager.h"
#include "Engine/Texture2D.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProperties.h"
#include "NDGIAtlasShaders.h"
#include "NDGILevelAsset.h"
#include "NDGIModelAsset.h"
#include "NDGIRuntime.h"
#include "RenderGraphBuilder.h"
#include "RenderGraphResources.h"
#include "RenderGraphUtils.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "RHITransition.h"
#include "TextureResource.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "UObject/SoftObjectPath.h"

IMPLEMENT_GLOBAL_SHADER(FNDGIDecodeAtlasCS, "/Plugin/NDGI/Private/NDGIDecodeAtlas.usf", "DecodeAtlasCS", SF_Compute);

static TAutoConsoleVariable<int32> CVarNDGIMaxCacheMemoryMB(
	TEXT("ndgi.MaxCacheMemoryMB"),
	2048,
	TEXT("Upper bound for the NDGI decode cache (packed CPU + GPU buffers). 0 disables the guard."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarNDGIResidentModelsPerAtlas(
	TEXT("ndgi.ResidentModelsPerAtlas"),
	0,
	TEXT("Virtual-texture page-pool size per atlas. 0 keeps every model resident; a positive value streams the nearest N model pages into that many physical BC7 slots."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarNDGIStreamingUpdateSeconds(
	TEXT("ndgi.StreamingUpdateSeconds"),
	0.5f,
	TEXT("Minimum seconds between NDGI virtual-texture page-cache updates."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarNDGIStreamingMaxLoadsInFlight(
	TEXT("ndgi.StreamingMaxLoadsInFlight"),
	16,
	TEXT("Maximum number of model-page packages that may be loading at once across all atlases."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarNDGIStreamingUploadBudgetKB(
	TEXT("ndgi.StreamingUploadBudgetKB"),
	4096,
	TEXT("Maximum BC7 page bytes uploaded per virtual-texture update (0 = unlimited)."),
	ECVF_Default);

static TAutoConsoleVariable<int32> CVarNDGIStreamingSyncLoad(
	TEXT("ndgi.StreamingSyncLoad"),
	0,
	TEXT("1 = load model pages synchronously; used by tests and validation commands."),
	ECVF_Default);

static TAutoConsoleVariable<float> CVarNDGIStreamingHysteresis(
	TEXT("ndgi.StreamingHysteresis"),
	0.1f,
	TEXT("Distance bonus (fraction) that keeps already-resident model pages from being evicted by a slightly nearer page."),
	ECVF_Default);

namespace
{
	/**
	 * Makes sure the lightmap RHI texture is resident at full mip 0.
	 *
	 * In the editor, NeverStream + UpdateResource can rebuild the texture from
	 * derived data. In a cooked build that path asserts in TextureDerivedData
	 * because cooked mips are not all inlined. For cooked data, ask the streaming
	 * system for full residency and wait for it instead.
	 */
	bool EnsureFullResolution(
		UTexture2D* Texture,
		int32 AtlasWidth,
		int32 AtlasHeight,
		FTextureResource*& OutResource)
	{
		OutResource = Texture ? Texture->GetResource() : nullptr;
		auto IsFullResolution = [&OutResource, AtlasWidth, AtlasHeight]()
		{
			const FRHITexture* RHI = OutResource ? OutResource->GetTextureRHI() : nullptr;
			return RHI
				&& static_cast<int32>(RHI->GetSizeX()) >= AtlasWidth
				&& static_cast<int32>(RHI->GetSizeY()) >= AtlasHeight;
		};

		if (IsFullResolution())
		{
			return true;
		}

		if (FPlatformProperties::RequiresCookedData())
		{
			// Cooked builds must not call UpdateResource on a lightmap whose mips
			// are partially inlined; the texture builder asserts.
			Texture->SetForceMipLevelsToBeResident(3600.0f);
			Texture->WaitForStreaming();
			FlushRenderingCommands();
		}
		else
		{
			Texture->NeverStream = true;
			Texture->UpdateResource();
			FlushRenderingCommands();
		}

		OutResource = Texture->GetResource();
		return IsFullResolution();
	}

	/**
	 * Copies only the tiles that own a model from the decode target back into the
	 * engine lightmap. Tiles without a model keep their original baked content.
	 */
	void CopyModelTiles(
		FRHICommandListImmediate& RHICmdList,
		FRHITexture* Source,
		FRHITexture* Target,
		uint32 AtlasWidth,
		uint32 AtlasHeight,
		uint32 TileSize,
		const TArray<uint32>& ModelIndices)
	{
		const uint32 GridX = AtlasWidth / FMath::Max<uint32>(1, TileSize);
		const uint32 CoefHeight = AtlasHeight / 2;
		RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::Unknown, ERHIAccess::CopyDest));
		for (int32 Cell = 0; Cell < ModelIndices.Num(); ++Cell)
		{
			if (ModelIndices[Cell] == 0xFFFFFFFFu)
			{
				continue;
			}
			const uint32 TileX = static_cast<uint32>(Cell) % GridX;
			const uint32 TileY = static_cast<uint32>(Cell) / GridX;
			FRHICopyTextureInfo CopyInfo;
			CopyInfo.Size = FIntVector(static_cast<int32>(TileSize), static_cast<int32>(TileSize), 1);
			CopyInfo.SourcePosition = FIntVector(static_cast<int32>(TileX * TileSize), static_cast<int32>(TileY * TileSize), 0);
			CopyInfo.DestPosition = CopyInfo.SourcePosition;
			RHICmdList.CopyTexture(Source, Target, CopyInfo);
			CopyInfo.SourcePosition.Y += static_cast<int32>(CoefHeight);
			CopyInfo.DestPosition.Y += static_cast<int32>(CoefHeight);
			RHICmdList.CopyTexture(Source, Target, CopyInfo);
		}
		RHICmdList.Transition(FRHITransitionInfo(Target, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
	}

	/**
	 * Flattens the per-texel encoding range table for the GPU. The index map is
	 * packed four uint8 entries per uint; legacy bindings get one range and a
	 * single zero index.
	 */
	void BuildEncodingRangeData(
		const NDGI::Runtime::FAtlasDecodeBinding& Binding,
		TArray<FVector4f>& OutRanges,
		TArray<uint32>& OutPackedIndices)
	{
		OutRanges.Reset();
		OutPackedIndices.Reset();

		const int32 CoefHeight = Binding.AtlasHeight / 2;
		const int32 TexelCount = Binding.AtlasWidth * CoefHeight;
		const bool bHasRanges = Binding.EncodingRanges.Num() > 0
			&& Binding.EncodingRangeIndices.Num() == TexelCount;

		if (bHasRanges)
		{
			OutRanges.Reserve(Binding.EncodingRanges.Num() * 4);
			for (const FNDGIEncodingRange& Range : Binding.EncodingRanges)
			{
				OutRanges.Add(Range.Scale0);
				OutRanges.Add(Range.Add0);
				OutRanges.Add(Range.Scale1);
				OutRanges.Add(Range.Add1);
			}
			OutPackedIndices.SetNumZeroed((TexelCount + 3) / 4);
			for (int32 Texel = 0; Texel < TexelCount; ++Texel)
			{
				OutPackedIndices[Texel >> 2] |= static_cast<uint32>(Binding.EncodingRangeIndices[Texel]) << ((Texel & 3) * 8);
			}
		}
		else
		{
			OutRanges.Add(Binding.Scale0);
			OutRanges.Add(Binding.Add0);
			OutRanges.Add(Binding.Scale1);
			OutRanges.Add(Binding.Add1);
			OutPackedIndices.Add(0u);
		}
	}

	bool AppendModelToBinding(
		UNDGIModelAsset* Model,
		const NDGI::Train::FTileTrainConfig& Config,
		NDGI::Runtime::FAtlasDecodeBinding& Binding,
		int32& ModelSlot)
	{
		if (!Model || !Model->HasBC7Features())
		{
			return false;
		}

		const int32 UVSize = Config.UVSize;
		const int32 UVTSize = Config.UVTSize;
		const int32 UVLayerBytes = (UVSize / 4) * (UVSize / 4) * 16;
		const int32 UVTLayerBytes = (UVTSize / 4) * (UVTSize / 4) * 16;
		if (Model->TileSize.X != UVSize
			|| Model->FeatureUVTSize.X != UVTSize
			|| Model->FeatureUVBC7BytesPerLayer != UVLayerBytes
			|| Model->FeatureUVTBC7BytesPerLayer != UVTLayerBytes
			|| Model->FeatureUVBC7.Num() != UVLayerBytes
			|| Model->FeatureUVTBC7.Num() != UVTLayerBytes * Config.TimeSlices)
		{
			return false;
		}

		TArray<float> CompactParameters;
		if (!NDGI::Runtime::BuildCompactModelParameters(*Model, Config, CompactParameters))
		{
			return false;
		}

		NDGI::Runtime::FModelBC7Features Features;
		Features.UVBC7 = Model->FeatureUVBC7;
		Features.UVTBC7 = Model->FeatureUVTBC7;

		Binding.PackedParameters.Append(CompactParameters);
		Binding.ModelFeatures.Add(MoveTemp(Features));
		Binding.UVBC7BytesPerLayer = UVLayerBytes;
		Binding.UVTBC7BytesPerLayer = UVTLayerBytes;
		return true;
	}

	void EncodeTexelsToBC(const TArray<uint8>& Texels, int32 SizeX, int32 SizeY,
		int32 E1Offset, int32 E2Offset, int32 WOffset, TArray<float>& OutParameters)
	{
		const int32 BlocksX = SizeX / 4;
		const int32 BlocksY = SizeY / 4;
		for (int32 BlockY = 0; BlockY < BlocksY; ++BlockY)
		{
			for (int32 BlockX = 0; BlockX < BlocksX; ++BlockX)
			{
				const int32 Block = BlockY * BlocksX + BlockX;
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					float MinValue = 1.0f;
					float MaxValue = 0.0f;
					for (int32 P = 0; P < 16; ++P)
					{
						const int32 X = BlockX * 4 + (P % 4);
						const int32 Y = BlockY * 4 + (P / 4);
						const float Value = Texels[(Y * SizeX + X) * 4 + Channel] / 255.0f;
						MinValue = FMath::Min(MinValue, Value);
						MaxValue = FMath::Max(MaxValue, Value);
					}
					OutParameters[E1Offset + Block * 4 + Channel] = MinValue;
					OutParameters[E2Offset + Block * 4 + Channel] = MaxValue;
					const float Range = MaxValue - MinValue;
					for (int32 P = 0; P < 16; ++P)
					{
						const int32 X = BlockX * 4 + (P % 4);
						const int32 Y = BlockY * 4 + (P / 4);
						const float Value = Texels[(Y * SizeX + X) * 4 + Channel] / 255.0f;
						OutParameters[WOffset + Block * 16 + P] = (Range > 1e-6f)
							? FMath::Clamp((Value - MinValue) / Range, 0.0f, 1.0f)
							: 0.0f;
					}
				}
			}
		}
	}
}

namespace
{
	struct FAtlasPassSetup
	{
		uint32 AtlasWidth = 0;
		uint32 AtlasHeight = 0;
		uint32 TileSize = 0;
		uint32 OverlapTexels = 0;
		uint32 GridX = 0;
		uint32 GridY = 0;
		uint32 TimeSlices = 0;
		uint32 TotalParams = 0;
		float Time = 0.0f;
		FVector3f MeanRGB = FVector3f::ZeroVector;
		FVector3f MeanSH = FVector3f::ZeroVector;
		bool bDirectionalSH = false;
		float Gamma = 2.2f;
		uint32 bHasEncodingRanges = 0;
		uint32 UVSize = 0;
		uint32 UVTSize = 0;
		uint32 UTSize = 0;
		uint32 VTSize = 0;
		uint32 LayoutTimeSlices = 0;
		uint32 MLPOffset = 0;
		uint32 UTOffset = 0;
		uint32 VTOffset = 0;
	};

	void AddDecodeAtlasPass(
		FRDGBuilder& GraphBuilder,
		const FAtlasPassSetup& Setup,
		FRDGBufferRef ParametersBuffer,
		FRDGBufferRef IndicesBuffer,
		FRDGBufferRef EncodingRangesBuffer,
		FRDGBufferRef EncodingIndicesBuffer,
		FRDGTextureRef SourceTexture,
		FRDGTextureRef FeatureUVTexture,
		FRDGTextureRef FeatureUVTTexture,
		FRDGTextureRef OutputTexture,
		const TCHAR* EventName)
	{
		FNDGIDecodeAtlasCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FNDGIDecodeAtlasCS::FParameters>();
		PassParameters->TileParameters = GraphBuilder.CreateSRV(ParametersBuffer);
		PassParameters->TileModelIndices = GraphBuilder.CreateSRV(IndicesBuffer);
		PassParameters->EncodingRanges = GraphBuilder.CreateSRV(EncodingRangesBuffer);
		PassParameters->EncodingIndices = GraphBuilder.CreateSRV(EncodingIndicesBuffer);
		PassParameters->SourceTexture = SourceTexture;
		PassParameters->FeatureUVTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(FeatureUVTexture));
		PassParameters->FeatureUVTTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc(FeatureUVTTexture));
		PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);
		PassParameters->AtlasWidth = Setup.AtlasWidth;
		PassParameters->AtlasHeight = Setup.AtlasHeight;
		PassParameters->TileSize = Setup.TileSize;
		PassParameters->OverlapTexels = Setup.OverlapTexels;
		PassParameters->GridX = Setup.GridX;
		PassParameters->GridY = Setup.GridY;
		PassParameters->TimeSlices = Setup.TimeSlices;
		PassParameters->TotalParams = Setup.TotalParams;
		PassParameters->bHighQuality = 1;
		PassParameters->Time = Setup.Time;
		PassParameters->MeanRGB = Setup.MeanRGB;
		PassParameters->MeanSH = Setup.MeanSH;
		PassParameters->bDirectionalSH = Setup.bDirectionalSH ? 1u : 0u;
		PassParameters->Gamma = Setup.Gamma;
		PassParameters->bHasEncodingRanges = Setup.bHasEncodingRanges;
		PassParameters->UVSize = Setup.UVSize;
		PassParameters->UVTSize = Setup.UVTSize;
		PassParameters->UTSize = Setup.UTSize;
		PassParameters->VTSize = Setup.VTSize;
		PassParameters->LayoutTimeSlices = Setup.LayoutTimeSlices;
		PassParameters->MLPOffset = Setup.MLPOffset;
		PassParameters->UTOffset = Setup.UTOffset;
		PassParameters->VTOffset = Setup.VTOffset;

		TShaderMapRef<FNDGIDecodeAtlasCS> ComputeShader(GetGlobalShaderMap(GMaxRHIFeatureLevel));
		const FIntVector GroupCount = FComputeShaderUtils::GetGroupCount(
			FIntPoint(static_cast<int32>(Setup.AtlasWidth), static_cast<int32>(Setup.AtlasHeight / 2)), FIntPoint(8, 8));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("%s", EventName), ComputeShader, PassParameters, GroupCount);
	}

	FAtlasPassSetup MakePassSetup(
		const NDGI::Runtime::FAtlasDecodeBinding& Binding,
		const NDGI::Runtime::FCompactTileLayout& Layout,
		const NDGI::Train::FTileTrainConfig& Config,
		uint32 GridX, uint32 GridY, float Time)
	{
		FAtlasPassSetup Setup;
		Setup.AtlasWidth = static_cast<uint32>(Binding.AtlasWidth);
		Setup.AtlasHeight = static_cast<uint32>(Binding.AtlasHeight);
		Setup.TileSize = static_cast<uint32>(Binding.TileSize);
		Setup.OverlapTexels = static_cast<uint32>(FMath::Max(0, Binding.OverlapTexels));
		Setup.GridX = GridX;
		Setup.GridY = GridY;
		Setup.TimeSlices = static_cast<uint32>(Config.TimeSlices);
		Setup.TotalParams = static_cast<uint32>(Layout.TotalParams);
		Setup.Time = Time;
		Setup.MeanRGB = FVector3f(Binding.MeanRGB.X, Binding.MeanRGB.Y, Binding.MeanRGB.Z);
		Setup.MeanSH = FVector3f(Binding.MeanSH.X, Binding.MeanSH.Y, Binding.MeanSH.Z);
		Setup.bDirectionalSH = Binding.bDirectionalSH;
		Setup.Gamma = Binding.Gamma;
		Setup.bHasEncodingRanges = (Binding.EncodingRanges.Num() > 0
			&& Binding.EncodingRangeIndices.Num() == Binding.AtlasWidth * (Binding.AtlasHeight / 2)) ? 1u : 0u;
		Setup.UVSize = static_cast<uint32>(Config.UVSize);
		Setup.UVTSize = static_cast<uint32>(Config.UVTSize);
		Setup.UTSize = static_cast<uint32>(Config.UTSize);
		Setup.VTSize = static_cast<uint32>(Config.VTSize);
		Setup.LayoutTimeSlices = static_cast<uint32>(Config.TimeSlices);
		Setup.MLPOffset = static_cast<uint32>(Layout.MLPOffset);
		Setup.UTOffset = static_cast<uint32>(Layout.UTOffset);
		Setup.VTOffset = static_cast<uint32>(Layout.VTOffset);
		return Setup;
	}
}

NDGI::Runtime::FCompactTileLayout NDGI::Runtime::ComputeCompactTileLayout(const Train::FTileTrainConfig& Config)
{
	FCompactTileLayout Layout;
	Layout.MLPOffset = 0;
	Layout.UTOffset = Train::MLPParameterCount;
	Layout.VTOffset = Layout.UTOffset + Config.UTSize * Config.TimeSlices * 2;
	Layout.TotalParams = Layout.VTOffset + Config.VTSize * Config.TimeSlices * 2;
	return Layout;
}

bool NDGI::Runtime::BuildCompactModelParameters(
	const UNDGIModelAsset& Model,
	const Train::FTileTrainConfig& Config,
	TArray<float>& OutParameters)
{
	if (Model.MLPParameters.Num() != Train::MLPParameterCount
		|| Model.UT.Num() != Config.UTSize * Config.TimeSlices * 2
		|| Model.VT.Num() != Config.VTSize * Config.TimeSlices * 2)
	{
		return false;
	}

	const FCompactTileLayout Layout = ComputeCompactTileLayout(Config);
	OutParameters.SetNumZeroed(Layout.TotalParams);
	for (int32 Index = 0; Index < Train::MLPParameterCount; ++Index)
	{
		OutParameters[Layout.MLPOffset + Index] = Model.MLPParameters[Index];
	}
	for (int32 Index = 0; Index < Model.UT.Num(); ++Index)
	{
		OutParameters[Layout.UTOffset + Index] = Model.UT[Index];
	}
	for (int32 Index = 0; Index < Model.VT.Num(); ++Index)
	{
		OutParameters[Layout.VTOffset + Index] = Model.VT[Index];
	}
	return true;
}

#if WITH_EDITORONLY_DATA
bool NDGI::Runtime::BuildModelParameters(
	const UNDGIModelAsset& Model,
	const Train::FTileTrainConfig& Config,
	TArray<float>& OutParameters)
{
	const bool bLegacyRGBModel = Train::MLPHiddenDim == 16 && Model.MLPParameters.Num() == 595;
	if (Model.TileSize.X <= 0 || Model.TileSize.X % 4 != 0
		|| Model.FeatureUVTSize.X <= 0 || Model.FeatureUVTSize.X % 4 != 0
		|| Model.TimeSlices <= 0
		|| Model.FeatureUV.Num() != Model.TileSize.X * Model.TileSize.Y * 4
		|| Model.FeatureUVT.Num() != Model.TimeSlices * Model.FeatureUVTSize.X * Model.FeatureUVTSize.Y * 4
		|| (Model.MLPParameters.Num() != Train::MLPParameterCount && !bLegacyRGBModel)
		|| Config.UVSize != Model.TileSize.X
		|| Config.UVTSize != Model.FeatureUVTSize.X
		|| Config.UTSize != Model.UTSize
		|| Config.VTSize != Model.VTSize
		|| Config.TimeSlices != Model.TimeSlices)
	{
		return false;
	}

	const Train::FTileLayout Layout = Train::ComputeTileLayout(Config);
	OutParameters.SetNumZeroed(Layout.TotalParams);

	if (bLegacyRGBModel)
	{
		// 595-param models have W3 [3*16] followed by B3 [3]. Expand them in place
		// to the 646-param RGB+SH layout: copy the three RGB rows, leave the three
		// SH rows zero, and append three zero biases. The RGB outputs are unchanged
		// and SH stays at zero until the model is retrained.
		constexpr int32 LegacyW3Offset = 544;
		constexpr int32 LegacyB3Offset = 592;
		for (int32 Index = 0; Index < LegacyW3Offset; ++Index)
		{
			OutParameters[Layout.MLPOffset + Index] = Model.MLPParameters[Index];
		}
		for (int32 Index = 0; Index < 48; ++Index)
		{
			OutParameters[Layout.MLPOffset + LegacyW3Offset + Index] = Model.MLPParameters[LegacyW3Offset + Index];
		}
		for (int32 Index = 0; Index < 3; ++Index)
		{
			OutParameters[Layout.MLPOffset + 640 + Index] = Model.MLPParameters[LegacyB3Offset + Index];
		}
	}
	else
	{
		for (int32 Index = 0; Index < Train::MLPParameterCount; ++Index)
		{
			OutParameters[Layout.MLPOffset + Index] = Model.MLPParameters[Index];
		}
	}

	EncodeTexelsToBC(Model.FeatureUV, Config.UVSize, Config.UVSize,
		static_cast<int32>(Layout.UVE1Offset), static_cast<int32>(Layout.UVE2Offset), static_cast<int32>(Layout.UVWOffset), OutParameters);

	const int32 SliceTexels = Config.UVTSize * Config.UVTSize * 4;
	const int32 SliceBlocks = (Config.UVTSize / 4) * (Config.UVTSize / 4);
	for (int32 Slice = 0; Slice < Config.TimeSlices; ++Slice)
	{
		TArray<uint8> SliceData;
		SliceData.SetNumUninitialized(SliceTexels);
		FMemory::Memcpy(SliceData.GetData(), Model.FeatureUVT.GetData() + static_cast<int64>(Slice) * SliceTexels, SliceTexels);
		EncodeTexelsToBC(SliceData, Config.UVTSize, Config.UVTSize,
			static_cast<int32>(Layout.UVTE1Offset) + Slice * SliceBlocks * 4,
			static_cast<int32>(Layout.UVTE2Offset) + Slice * SliceBlocks * 4,
			static_cast<int32>(Layout.UVTWOffset) + Slice * SliceBlocks * 16,
			OutParameters);
	}

	for (int32 Index = 0; Index < Model.UT.Num(); ++Index)
	{
		OutParameters[Layout.UTOffset + Index] = Model.UT[Index];
	}
	for (int32 Index = 0; Index < Model.VT.Num(); ++Index)
	{
		OutParameters[Layout.VTOffset + Index] = Model.VT[Index];
	}
	return true;
}
#endif // WITH_EDITORONLY_DATA

namespace NDGI
{
	namespace Runtime
	{
		namespace
		{
			bool CreateFeatureTextureArrays(
				FRHICommandListImmediate& RHICmdList,
				const FAtlasDecodeBinding& Binding,
				TRefCountPtr<FRHITexture>& OutUV,
				TRefCountPtr<FRHITexture>& OutUVT);
		}
	}
}

bool NDGI::Runtime::DecodeAtlas(const FAtlasDecodeBinding& Binding, float Time)
{
	if (!Binding.TargetTexture || Binding.AtlasWidth <= 0 || Binding.AtlasHeight <= 0
		|| Binding.TileSize <= 0 || Binding.TileSize % 16 != 0 || Binding.TimeSlices <= 0
		|| Binding.OverlapTexels < 0 || Binding.OverlapTexels % 8 != 0)
	{
		return false;
	}
	if (!Binding.bHighQuality)
	{
		UE_LOG(LogNDGIRuntime, Error, TEXT("DecodeAtlas: only uncompressed HQ lightmaps are supported in this version."));
		return false;
	}

	UTexture2D* Target = Binding.TargetTexture;
	FTexturePlatformData* PlatformData = Target->GetPlatformData();
	const EPixelFormat TargetFormat = PlatformData ? PlatformData->PixelFormat : PF_Unknown;
	if (!PlatformData || (TargetFormat != PF_FloatRGBA && TargetFormat != PF_B8G8R8A8))
	{
		UE_LOG(LogNDGIRuntime, Error, TEXT("DecodeAtlas: lightmap '%s' must be uncompressed (RGBA16F or BGRA8); enable World Settings > Lightmass > Compress Lightmaps off and rebuild lighting."),
			*Target->GetName());
		return false;
	}

	const int32 CoefHeight = Binding.AtlasHeight / 2;
	const int32 GridX = Binding.AtlasWidth / Binding.TileSize;
	const int32 GridY = CoefHeight / Binding.TileSize;
	if (GridX <= 0 || GridY <= 0 || GridX * Binding.TileSize != Binding.AtlasWidth || GridY * Binding.TileSize != CoefHeight
		|| Binding.TileModelIndices.Num() != GridX * GridY)
	{
		UE_LOG(LogNDGIRuntime, Error, TEXT("DecodeAtlas: invalid grid for atlas '%s'."), *Target->GetName());
		return false;
	}

	const int32 ModelExtent = Binding.TileSize + 2 * FMath::Max(0, Binding.OverlapTexels);
	Train::FTileTrainConfig Config;
	Config.UVSize = Binding.FeatureUVSize > 0 ? Binding.FeatureUVSize : ModelExtent;
	Config.UVTSize = Binding.FeatureUVTSize > 0 ? Binding.FeatureUVTSize : ModelExtent / 4;
	Config.UTSize = Binding.FeatureUTSize > 0 ? Binding.FeatureUTSize : ModelExtent / 2;
	Config.VTSize = Binding.FeatureVTSize > 0 ? Binding.FeatureVTSize : ModelExtent / 2;
	Config.TimeSlices = Binding.TimeSlices;
	Config.NumTiles = 1;
	Config.BatchSize = 1;

	const FCompactTileLayout Layout = ComputeCompactTileLayout(Config);
	if (Layout.TotalParams == 0 || Binding.PackedParameters.Num() % Layout.TotalParams != 0
		|| Binding.ModelFeatures.Num() * Layout.TotalParams != Binding.PackedParameters.Num())
	{
		UE_LOG(LogNDGIRuntime, Error, TEXT("DecodeAtlas: packed parameter buffer size does not match the tile layout."));
		return false;
	}

	TArray<uint32> ModelIndices;
	ModelIndices.SetNumUninitialized(Binding.TileModelIndices.Num());
	for (int32 Index = 0; Index < Binding.TileModelIndices.Num(); ++Index)
	{
		const int32 ModelIndex = Binding.TileModelIndices[Index];
		ModelIndices[Index] = (ModelIndex >= 0) ? static_cast<uint32>(ModelIndex) : 0xFFFFFFFFu;
	}

	FTextureResource* TargetResource = nullptr;
	if (!EnsureFullResolution(Target, Binding.AtlasWidth, Binding.AtlasHeight, TargetResource))
	{
		const FRHITexture* RHI = TargetResource ? TargetResource->GetTextureRHI() : nullptr;
		UE_LOG(LogNDGIRuntime, Warning,
			TEXT("DecodeAtlas: lightmap texture '%s' could not be made fully resident (RHI %dx%d, atlas %dx%d)."),
			*Target->GetName(),
			RHI ? static_cast<int32>(RHI->GetSizeX()) : -1,
			RHI ? static_cast<int32>(RHI->GetSizeY()) : -1,
			Binding.AtlasWidth, Binding.AtlasHeight);
		return false;
	}
	{
		FTexturePlatformData* DiagData = Target->GetPlatformData();
		const FRHITexture* DiagRHI = TargetResource->GetTextureRHI();
		UE_LOG(LogNDGIRuntime, Verbose,
			TEXT("DecodeAtlas: '%s' binding %dx%d, mip0 %dx%d, mips %d, RHI %dx%d."),
			*Target->GetName(), Binding.AtlasWidth, Binding.AtlasHeight,
			DiagData && DiagData->Mips.Num() > 0 ? DiagData->Mips[0].SizeX : -1,
			DiagData && DiagData->Mips.Num() > 0 ? DiagData->Mips[0].SizeY : -1,
			DiagData ? DiagData->Mips.Num() : 0,
			DiagRHI ? static_cast<int32>(DiagRHI->GetSizeX()) : -1,
			DiagRHI ? static_cast<int32>(DiagRHI->GetSizeY()) : -1);
	}
	Target->SetForceMipLevelsToBeResident(3600.0f);

	const uint32 AtlasWidth = static_cast<uint32>(Binding.AtlasWidth);
	const uint32 AtlasHeight = static_cast<uint32>(Binding.AtlasHeight);
	const uint32 TileSize = static_cast<uint32>(Binding.TileSize);

	ENQUEUE_RENDER_COMMAND(NDGIDecodeAtlas)(
		[Binding, TargetResource, AtlasWidth, AtlasHeight, TileSize, TargetFormat,
			GridX = static_cast<uint32>(GridX), GridY = static_cast<uint32>(GridY),
			CoefHeight = static_cast<uint32>(CoefHeight), Time, Layout, ModelIndices, Config]
		(FRHICommandListImmediate& RHICmdList)
		{
			TRefCountPtr<FRHITexture> FeatureUV;
			TRefCountPtr<FRHITexture> FeatureUVT;
			if (!CreateFeatureTextureArrays(RHICmdList, Binding, FeatureUV, FeatureUVT))
			{
				UE_LOG(LogNDGIRuntime, Error, TEXT("DecodeAtlas: could not create BC7 feature textures."));
				return;
			}

			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGBufferRef ParamsBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Atlas.Parameters"), Binding.PackedParameters);
			FRDGBufferRef IndicesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Atlas.ModelIndices"), ModelIndices);

			TArray<FVector4f> EncodingRangeData;
			TArray<uint32> PackedEncodingIndices;
			BuildEncodingRangeData(Binding, EncodingRangeData, PackedEncodingIndices);
			FRDGBufferRef EncodingRangesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Atlas.EncodingRanges"), EncodingRangeData);
			FRDGBufferRef EncodingIndicesBuffer = CreateStructuredBuffer(GraphBuilder, TEXT("NDGI.Atlas.EncodingIndices"), PackedEncodingIndices);

			const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
				FIntPoint(static_cast<int32>(AtlasWidth), static_cast<int32>(AtlasHeight)),
				TargetFormat, FClearValueBinding::Black,
				TexCreate_ShaderResource | TexCreate_UAV);
			FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("NDGI.Atlas.Output"));

			FRHITexture* SourceRHI = TargetResource ? TargetResource->GetTextureRHI() : nullptr;
			FRDGTextureRef SourceTexture = SourceRHI
				? RegisterExternalTexture(GraphBuilder, SourceRHI, TEXT("NDGI.Atlas.Source"))
				: nullptr;
			FRDGTextureRef FeatureUVRDG = RegisterExternalTexture(GraphBuilder, FeatureUV, TEXT("NDGI.FeatureUV"));
			FRDGTextureRef FeatureUVTRDG = RegisterExternalTexture(GraphBuilder, FeatureUVT, TEXT("NDGI.FeatureUVT"));

			const FAtlasPassSetup Setup = MakePassSetup(Binding, Layout, Config, GridX, GridY, Time);
			if (SourceTexture)
			{
				AddDecodeAtlasPass(GraphBuilder, Setup, ParamsBuffer, IndicesBuffer,
					EncodingRangesBuffer, EncodingIndicesBuffer, SourceTexture,
					FeatureUVRDG, FeatureUVTRDG, OutputTexture, TEXT("NDGIDecodeAtlas"));
			}

			TRefCountPtr<IPooledRenderTarget> PooledOutput;
			GraphBuilder.QueueTextureExtraction(OutputTexture, &PooledOutput);
			GraphBuilder.Execute();

			FRHITexture* TargetRHI = TargetResource ? TargetResource->GetTextureRHI() : nullptr;
			if (PooledOutput.IsValid() && TargetRHI && PooledOutput->GetRHI()->GetFormat() == TargetFormat)
			{
				CopyModelTiles(RHICmdList, PooledOutput->GetRHI(), TargetRHI,
					AtlasWidth, AtlasHeight, TileSize, ModelIndices);
			}
		});

	return true;
}

bool NDGI::Runtime::DecodeLevelAsset(UNDGILevelAsset* LevelAsset, float Time, FDecodeTimings* OutTimings)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(NDGIDecodeLevelAsset);
	const double StartSeconds = FPlatformTime::Seconds();
	if (OutTimings)
	{
		*OutTimings = FDecodeTimings();
	}
	if (!LevelAsset)
	{
		return false;
	}

	bool bAnyDecoded = false;
	for (FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
	{
		UTexture2D* LightmapTexture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
		if (!LightmapTexture || Entry.TileGrid.X <= 0 || Entry.TileGrid.Y <= 0 || Entry.TimeSlices <= 0)
		{
			continue;
		}

		FAtlasDecodeBinding Binding;
		Binding.TargetTexture = LightmapTexture;
		Binding.AtlasWidth = Entry.AtlasSize.X;
		Binding.AtlasHeight = Entry.AtlasSize.Y;
		Binding.TileSize = Entry.TileSize;
		Binding.OverlapTexels = Entry.OverlapTexels;
		Binding.TimeSlices = Entry.TimeSlices;
		Binding.bHighQuality = Entry.bHighQuality;
		Binding.Scale0 = Entry.Scale0;
		Binding.Add0 = Entry.Add0;
		Binding.Scale1 = Entry.Scale1;
		Binding.Add1 = Entry.Add1;
		Binding.Gamma = Entry.Gamma;
		Binding.EncodingRanges = Entry.EncodingRanges;
		Binding.EncodingRangeIndices = Entry.EncodingRangeIndices;

		const float StepF = Time * static_cast<float>(FMath::Max(1, Entry.TimeSlices - 1));
		const int32 Step0 = FMath::Clamp(FMath::FloorToInt(StepF), 0, FMath::Max(0, Entry.TimeSlices - 1));
		const int32 Step1 = FMath::Min(Step0 + 1, FMath::Max(0, Entry.TimeSlices - 1));
		const float Frac = FMath::Clamp(StepF - static_cast<float>(Step0), 0.0f, 1.0f);
		auto LerpMean = [Frac](const TArray<float>& Means, int32 A, int32 B) -> FVector4f
		{
			if (!Means.IsValidIndex(A * 3 + 2) || !Means.IsValidIndex(B * 3 + 2))
			{
				return FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			}
			return FVector4f(
				FMath::Lerp(Means[A * 3 + 0], Means[B * 3 + 0], Frac),
				FMath::Lerp(Means[A * 3 + 1], Means[B * 3 + 1], Frac),
				FMath::Lerp(Means[A * 3 + 2], Means[B * 3 + 2], Frac),
				0.0f);
		};
		Binding.MeanRGB = LerpMean(Entry.StepMeans, Step0, Step1);
		// SH is only valid when every time slice has means; partial arrays are
		// legacy/zero data, so keep the non-directional (0,0,0,1) encoding.
		if (Entry.StepMeansSH.Num() >= FMath::Max(1, Entry.TimeSlices) * 3)
		{
			Binding.MeanSH = LerpMean(Entry.StepMeansSH, Step0, Step1);
			Binding.bDirectionalSH = true;
		}

		Train::FTileTrainConfig Config;
		const int32 ModelExtent = Entry.TileSize + 2 * FMath::Max(0, Entry.OverlapTexels);
		Config.UVSize = ModelExtent;
		Config.UVTSize = ModelExtent / 4;
		Config.UTSize = ModelExtent / 2;
		Config.VTSize = ModelExtent / 2;
		Config.TimeSlices = Entry.TimeSlices;
		Config.NumTiles = 1;
		Config.BatchSize = 1;
		Binding.FeatureUVSize = Config.UVSize;
		Binding.FeatureUVTSize = Config.UVTSize;
		Binding.FeatureUTSize = Config.UTSize;
		Binding.FeatureVTSize = Config.VTSize;

		const int32 GridCells = Entry.TileGrid.X * Entry.TileGrid.Y;
		Binding.TileModelIndices.SetNumUninitialized(GridCells);
		int32 ModelSlot = 0;
		for (int32 Cell = 0; Cell < GridCells; ++Cell)
		{
			const FSoftObjectPath ModelPath = Entry.GetTileModelPath(Cell);
			UNDGIModelAsset* Model = ModelPath.IsValid()
				? Cast<UNDGIModelAsset>(ModelPath.TryLoad())
				: nullptr;
			if (!Model)
			{
				Binding.TileModelIndices[Cell] = -1;
				continue;
			}

			if (!AppendModelToBinding(Model, Config, Binding, ModelSlot))
			{
				Binding.TileModelIndices[Cell] = -1;
				continue;
			}
			Binding.TileModelIndices[Cell] = ModelSlot++;
		}

		bAnyDecoded |= DecodeAtlas(Binding, Time);
		if (OutTimings)
		{
			++OutTimings->Atlases;
			OutTimings->Tiles += ModelSlot;
		}
	}

	if (OutTimings)
	{
		OutTimings->CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}
	return bAnyDecoded;
}

namespace NDGI
{
	namespace Runtime
	{
		/** Persistent RHI buffers for one atlas, created once at load. */
		struct FNDGIAtlasGPUResources
		{
			TRefCountPtr<FRDGPooledBuffer> Parameters;
			TRefCountPtr<FRDGPooledBuffer> Indices;
			TRefCountPtr<FRDGPooledBuffer> EncodingRanges;
			TRefCountPtr<FRDGPooledBuffer> EncodingIndices;
			/** BC7 texture arrays: one layer per physical page slot (F_uv) and per slot/time slice (F_uvt). */
			TRefCountPtr<FRHITexture> FeatureUV;
			TRefCountPtr<FRHITexture> FeatureUVT;
			int32 SlotCapacity = 0;
			int32 UVSize = 0;
			int32 UVTSize = 0;
			int32 TimeSlices = 0;
			int32 TotalParams = 0;
		};

		namespace
		{
			Train::FTileTrainConfig MakeConfig(const FAtlasDecodeBinding& Binding)
			{
				Train::FTileTrainConfig Config;
				const int32 ModelExtent = Binding.TileSize + 2 * FMath::Max(0, Binding.OverlapTexels);
				Config.UVSize = Binding.FeatureUVSize > 0 ? Binding.FeatureUVSize : ModelExtent;
				Config.UVTSize = Binding.FeatureUVTSize > 0 ? Binding.FeatureUVTSize : ModelExtent / 4;
				Config.UTSize = Binding.FeatureUTSize > 0 ? Binding.FeatureUTSize : ModelExtent / 2;
				Config.VTSize = Binding.FeatureVTSize > 0 ? Binding.FeatureVTSize : ModelExtent / 2;
				Config.TimeSlices = Binding.TimeSlices;
				Config.NumTiles = 1;
				Config.BatchSize = 1;
				return Config;
			}

			int32 GetEffectiveSlotCapacity(const FAtlasDecodeCacheEntry& Cached)
			{
				return Cached.ResidentCapacity > 0
					? FMath::Min(Cached.ResidentCapacity, Cached.ModelCount)
					: Cached.ModelCount;
			}

			/** Packs one level-asset atlas into metadata and soft page references. */
			bool PackAtlasEntry(FNDGIAtlasEntry& Entry, FAtlasDecodeCacheEntry& Cached, int32& OutSkippedModels, bool bLoadTargetTexture = true)
			{
				if (Entry.TileGrid.X <= 0 || Entry.TileGrid.Y <= 0 || Entry.TimeSlices <= 0)
				{
					return false;
				}

				Cached.TargetTexturePath = Entry.LightmapTexturePath;
				Cached.SourceCells = Entry.SourceCells;

				FAtlasDecodeBinding& Binding = Cached.Binding;
				if (bLoadTargetTexture)
				{
					UTexture2D* LightmapTexture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
					if (!LightmapTexture)
					{
						return false;
					}

					FTextureResource* Resource = nullptr;
					if (!EnsureFullResolution(LightmapTexture, Entry.AtlasSize.X, Entry.AtlasSize.Y, Resource))
					{
						UE_LOG(LogNDGIRuntime, Warning,
							TEXT("NDGI cache: lightmap '%s' is not resident at full resolution (%dx%d)."),
							*LightmapTexture->GetName(), Entry.AtlasSize.X, Entry.AtlasSize.Y);
						return false;
					}
					Binding.TargetTexture = LightmapTexture;
				}

				Binding.AtlasWidth = Entry.AtlasSize.X;
				Binding.AtlasHeight = Entry.AtlasSize.Y;
				Binding.TileSize = Entry.TileSize;
				Binding.OverlapTexels = Entry.OverlapTexels;
				Binding.TimeSlices = Entry.TimeSlices;
				Binding.bHighQuality = Entry.bHighQuality;
				Binding.Scale0 = Entry.Scale0;
				Binding.Add0 = Entry.Add0;
				Binding.Scale1 = Entry.Scale1;
				Binding.Add1 = Entry.Add1;
				Binding.bDirectionalSH = Entry.StepMeansSH.Num() >= FMath::Max(1, Entry.TimeSlices) * 3;
				Binding.Gamma = Entry.Gamma;
				Binding.EncodingRanges = Entry.EncodingRanges;
				Binding.EncodingRangeIndices = Entry.EncodingRangeIndices;
				Cached.bDirectionalSH = Binding.bDirectionalSH;

				const int32 NumSteps = FMath::Max(1, Entry.TimeSlices);
				Cached.StepMeansRGB.SetNumUninitialized(NumSteps);
				Cached.StepMeansSH.SetNumUninitialized(NumSteps);
				for (int32 Step = 0; Step < NumSteps; ++Step)
				{
					Cached.StepMeansRGB[Step] = FVector4f(0, 0, 0, 0);
					Cached.StepMeansSH[Step] = FVector4f(0, 0, 0, 0);
					if (Entry.StepMeans.Num() >= (Step + 1) * 3)
					{
						Cached.StepMeansRGB[Step] = FVector4f(
							Entry.StepMeans[Step * 3 + 0], Entry.StepMeans[Step * 3 + 1], Entry.StepMeans[Step * 3 + 2], 0.0f);
					}
					if (Entry.StepMeansSH.Num() >= (Step + 1) * 3)
					{
						Cached.StepMeansSH[Step] = FVector4f(
							Entry.StepMeansSH[Step * 3 + 0], Entry.StepMeansSH[Step * 3 + 1], Entry.StepMeansSH[Step * 3 + 2], 0.0f);
					}
				}

				const Train::FTileTrainConfig Config = MakeConfig(Binding);
				Binding.FeatureUVSize = Config.UVSize;
				Binding.FeatureUVTSize = Config.UVTSize;
				Binding.FeatureUTSize = Config.UTSize;
				Binding.FeatureVTSize = Config.VTSize;

				const int32 GridCells = Entry.TileGrid.X * Entry.TileGrid.Y;
				Binding.TileModelIndices.SetNumUninitialized(GridCells);
				Cached.Pages.Reset();
				Cached.ModelBounds.Reset();
				for (int32 Cell = 0; Cell < GridCells; ++Cell)
				{
					const FSoftObjectPath Path = Entry.GetTileModelPath(Cell);
					if (!Path.IsValid())
					{
						Binding.TileModelIndices[Cell] = -1;
						continue;
					}

					FModelPage& Page = Cached.Pages.AddDefaulted_GetRef();
					Page.Path = Path;
					Cached.ModelBounds.Add(Entry.TileBounds.IsValidIndex(Cell) ? Entry.TileBounds[Cell] : FBox());
					Binding.TileModelIndices[Cell] = Cached.Pages.Num() - 1;
				}

				Cached.ModelCount = Cached.Pages.Num();
				if (Cached.ModelCount == 0)
				{
					return false;
				}

				const int32 Requested = FMath::Max(0, CVarNDGIResidentModelsPerAtlas.GetValueOnGameThread());
				Cached.ResidentCapacity = (Requested <= 0 || Requested >= Cached.ModelCount) ? 0 : Requested;
				Cached.ModelToResidentSlot.Init(-1, Cached.ModelCount);
				Cached.GPUTileModelIndices.Init(-1, GridCells);
				Cached.ResidentModels.Reset();
				Cached.ResidentSlotModels.Reset();
				Cached.ResidentSlotUploaded.Reset();
				Cached.ResidentParameters.Reset();
				return true;
			}

			bool UploadBC7Layer(
				FRHICommandListImmediate& RHICmdList,
				FRHITexture* Texture,
				int32 Layer,
				const uint8* Data,
				int32 Width,
				int32 Height)
			{
				uint32 DestStride = 0;
				void* Dest = RHICmdList.GetAsImmediate().LockTexture2DArray(
					Texture, Layer, 0, RLM_WriteOnly, DestStride, false);
				if (!Dest)
				{
					return false;
				}

				const int32 SourceRowBytes = (Width / 4) * 16;
				const int32 BlockRows = Height / 4;
				const uint32 RowStride = DestStride > 0 ? DestStride : static_cast<uint32>(SourceRowBytes);
				for (int32 Row = 0; Row < BlockRows; ++Row)
				{
					FMemory::Memcpy(
						static_cast<uint8*>(Dest) + static_cast<int64>(Row) * RowStride,
						Data + static_cast<int64>(Row) * SourceRowBytes,
						SourceRowBytes);
				}
				RHICmdList.GetAsImmediate().UnlockTexture2DArray(Texture, Layer, 0, false);
				return true;
			}

			bool CreateFeatureTextureArrays(
				FRHICommandListImmediate& RHICmdList,
				const FAtlasDecodeBinding& Binding,
				TRefCountPtr<FRHITexture>& OutUV,
				TRefCountPtr<FRHITexture>& OutUVT)
			{
				OutUV.SafeRelease();
				OutUVT.SafeRelease();

				const int32 NumModels = Binding.ModelFeatures.Num();
				const int32 UVSize = Binding.FeatureUVSize > 0
					? Binding.FeatureUVSize
					: Binding.TileSize + 2 * FMath::Max(0, Binding.OverlapTexels);
				const int32 UVTSize = Binding.FeatureUVTSize > 0
					? Binding.FeatureUVTSize
					: UVSize / 4;
				if (NumModels <= 0 || UVSize <= 0 || UVTSize <= 0 || Binding.TimeSlices <= 0)
				{
					return false;
				}

				const FRHITextureCreateDesc UVDesc =
					FRHITextureCreateDesc::Create2DArray(TEXT("NDGI.FeatureUV"),
						UVSize, UVSize, NumModels, PF_BC7)
					.SetNumMips(1)
					.SetFlags(TexCreate_ShaderResource)
					.SetInitialState(ERHIAccess::CopyDest);
				const FRHITextureCreateDesc UVTDesc =
					FRHITextureCreateDesc::Create2DArray(TEXT("NDGI.FeatureUVT"),
						UVTSize, UVTSize, NumModels * Binding.TimeSlices, PF_BC7)
					.SetNumMips(1)
					.SetFlags(TexCreate_ShaderResource)
					.SetInitialState(ERHIAccess::CopyDest);

				FTextureRHIRef UVTexture = RHICmdList.CreateTexture(UVDesc);
				FTextureRHIRef UVTTexture = RHICmdList.CreateTexture(UVTDesc);
				if (!UVTexture.IsValid() || !UVTTexture.IsValid())
				{
					return false;
				}

				for (int32 ModelIndex = 0; ModelIndex < NumModels; ++ModelIndex)
				{
					const FModelBC7Features& Features = Binding.ModelFeatures[ModelIndex];
					if (!UploadBC7Layer(RHICmdList, UVTexture, ModelIndex, Features.UVBC7.GetData(), UVSize, UVSize))
					{
						return false;
					}
					for (int32 Slice = 0; Slice < Binding.TimeSlices; ++Slice)
					{
						const int32 Layer = ModelIndex * Binding.TimeSlices + Slice;
						const uint8* SliceData = Features.UVTBC7.GetData()
							+ static_cast<int64>(Slice) * Binding.UVTBC7BytesPerLayer;
						if (!UploadBC7Layer(RHICmdList, UVTTexture, Layer, SliceData, UVTSize, UVTSize))
						{
							return false;
						}
					}
				}

				RHICmdList.Transition(FRHITransitionInfo(UVTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				RHICmdList.Transition(FRHITransitionInfo(UVTTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				OutUV = UVTexture;
				OutUVT = UVTTexture;
				return true;
			}

			/** Creates an empty physical page pool. Pages are uploaded into slots later. */
			bool CreateEmptyFeatureArrays(
				FRHICommandListImmediate& RHICmdList,
				int32 UVSize,
				int32 UVTSize,
				int32 TimeSlices,
				int32 Capacity,
				TRefCountPtr<FRHITexture>& OutUV,
				TRefCountPtr<FRHITexture>& OutUVT)
			{
				OutUV.SafeRelease();
				OutUVT.SafeRelease();
				if (Capacity <= 0 || UVSize <= 0 || UVTSize <= 0 || TimeSlices <= 0)
				{
					return false;
				}

				const FRHITextureCreateDesc UVDesc =
					FRHITextureCreateDesc::Create2DArray(TEXT("NDGI.FeatureUV"), UVSize, UVSize, Capacity, PF_BC7)
					.SetNumMips(1)
					.SetFlags(TexCreate_ShaderResource)
					.SetInitialState(ERHIAccess::CopyDest);
				const FRHITextureCreateDesc UVTDesc =
					FRHITextureCreateDesc::Create2DArray(TEXT("NDGI.FeatureUVT"), UVTSize, UVTSize, Capacity * TimeSlices, PF_BC7)
					.SetNumMips(1)
					.SetFlags(TexCreate_ShaderResource)
					.SetInitialState(ERHIAccess::CopyDest);

				FTextureRHIRef UVTexture = RHICmdList.CreateTexture(UVDesc);
				FTextureRHIRef UVTTexture = RHICmdList.CreateTexture(UVTDesc);
				if (!UVTexture.IsValid() || !UVTTexture.IsValid())
				{
					return false;
				}

				RHICmdList.Transition(FRHITransitionInfo(UVTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				RHICmdList.Transition(FRHITransitionInfo(UVTTexture, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				OutUV = UVTexture;
				OutUVT = UVTTexture;
				return true;
			}

			struct FPageUploadPacket
			{
				int32 Slot = -1;
				TArray<uint8> UVBC7;
				TArray<uint8> UVTBC7;
				TArray<float> Parameters;
			};

			int32 GetUVLayerBytes(int32 UVSize)
			{
				return (UVSize / 4) * (UVSize / 4) * 16;
			}

			int32 GetUVTLayerBytes(int32 UVTSize)
			{
				return (UVTSize / 4) * (UVTSize / 4) * 16;
			}

			int64 EstimateEntryGPUBytes(const FAtlasDecodeCacheEntry& Entry)
			{
				const int32 Capacity = GetEffectiveSlotCapacity(Entry);
				if (Capacity <= 0)
				{
					return 0;
				}

				const Train::FTileTrainConfig Config = MakeConfig(Entry.Binding);
				const FCompactTileLayout Layout = ComputeCompactTileLayout(Config);
				const int64 FeatureBytes = static_cast<int64>(Capacity)
					* (GetUVLayerBytes(Config.UVSize) + static_cast<int64>(Config.TimeSlices) * GetUVTLayerBytes(Config.UVTSize));
				const int64 RangeBytes = static_cast<int64>(Entry.Binding.EncodingRanges.Num()) * sizeof(FNDGIEncodingRange);
				const int64 PackedIndices = static_cast<int64>(FMath::DivideAndRoundUp(Entry.Binding.EncodingRangeIndices.Num(), 4)) * sizeof(uint32);
				return static_cast<int64>(Capacity) * Layout.TotalParams * sizeof(float)
					+ static_cast<int64>(Entry.GPUTileModelIndices.Num()) * sizeof(int32)
					+ RangeBytes + PackedIndices + FeatureBytes;
			}

			int64 EstimateEntryStagingBytes(const FAtlasDecodeCacheEntry& Entry)
			{
				int64 Bytes = 0;
				for (const FModelPage& Page : Entry.Pages)
				{
					Bytes += Page.PendingUVBC7.Num() + Page.PendingUVTBC7.Num()
						+ static_cast<int64>(Page.PendingParameters.Num()) * sizeof(float);
				}
				return Bytes;
			}

			void RecomputeCacheMemory(FLevelDecodeCache& Cache)
			{
				Cache.ActiveGPUBytes = 0;
				Cache.GPUBytes = 0;
				Cache.ResidentModels = 0;
				Cache.ResidentPageBytes = 0;
				Cache.ParameterBytes = 0;
				Cache.CPUBytes = 0;
				Cache.StagingBytes = 0;
				Cache.LoadedPageHandles = 0;
				Cache.InFlightPageLoads = 0;
				for (const FAtlasDecodeCacheEntry& Entry : Cache.Entries)
				{
					if (!Entry.bActive)
					{
						continue;
					}
					const int64 Bytes = EstimateEntryGPUBytes(Entry);
					Cache.ActiveGPUBytes += Bytes;
					Cache.GPUBytes += Bytes;
					Cache.ParameterBytes += static_cast<int64>(Entry.ResidentParameters.Num()) * sizeof(float);
					const Train::FTileTrainConfig Config = MakeConfig(Entry.Binding);
					const int64 PageBytes = GetUVLayerBytes(Config.UVSize)
						+ static_cast<int64>(Config.TimeSlices) * GetUVTLayerBytes(Config.UVTSize);
					for (const FModelPage& Page : Entry.Pages)
					{
						if (Page.State == EModelPageState::Uploaded)
						{
							++Cache.ResidentModels;
							Cache.ResidentPageBytes += PageBytes;
						}
						if (Page.LoadHandle.IsValid())
						{
							++Cache.LoadedPageHandles;
						}
						if (Page.State == EModelPageState::Loading)
						{
							++Cache.InFlightPageLoads;
						}
					}
					const int64 Staging = EstimateEntryStagingBytes(Entry);
					Cache.StagingBytes += Staging;
					Cache.CPUBytes += Staging;
				}
			}

			void ClearPagePayload(FModelPage& Page)
			{
				Page.PendingUVBC7.Reset();
				Page.PendingUVTBC7.Reset();
				Page.PendingParameters.Reset();
			}

			bool BuildPagePayload(
				const FAtlasDecodeCacheEntry& Entry,
				UNDGIModelAsset& Model,
				FModelPage& Page)
			{
				if (!Model.HasBC7Features())
				{
					return false;
				}

				const Train::FTileTrainConfig Config = MakeConfig(Entry.Binding);
				const int32 UVLayerBytes = GetUVLayerBytes(Config.UVSize);
				const int32 UVTLayerBytes = GetUVTLayerBytes(Config.UVTSize);
				if (Model.TileSize.X != Config.UVSize
					|| Model.FeatureUVTSize.X != Config.UVTSize
					|| Model.FeatureUVBC7BytesPerLayer != UVLayerBytes
					|| Model.FeatureUVTBC7BytesPerLayer != UVTLayerBytes
					|| Model.FeatureUVBC7.Num() != UVLayerBytes
					|| Model.FeatureUVTBC7.Num() != static_cast<int64>(UVTLayerBytes) * Config.TimeSlices)
				{
					return false;
				}

				TArray<float> Parameters;
				if (!NDGI::Runtime::BuildCompactModelParameters(Model, Config, Parameters))
				{
					return false;
				}

				Page.PendingUVBC7 = Model.FeatureUVBC7;
				Page.PendingUVTBC7 = Model.FeatureUVTBC7;
				Page.PendingParameters = MoveTemp(Parameters);
				return true;
			}

			void ReleasePage(FAtlasDecodeCacheEntry& Entry, int32 PageIndex, bool bMarkEvicted)
			{
				if (!Entry.Pages.IsValidIndex(PageIndex))
				{
					return;
				}

				FModelPage& Page = Entry.Pages[PageIndex];
				if (Page.LoadHandle.IsValid())
				{
					Page.LoadHandle->CancelHandle();
					Page.LoadHandle.Reset();
				}
				ClearPagePayload(Page);
				if (Page.Slot >= 0 && Entry.ResidentSlotModels.IsValidIndex(Page.Slot)
					&& Entry.ResidentSlotModels[Page.Slot] == PageIndex)
				{
					Entry.ResidentSlotModels[Page.Slot] = -1;
					if (Entry.ResidentSlotUploaded.IsValidIndex(Page.Slot))
					{
						Entry.ResidentSlotUploaded[Page.Slot] = 0;
					}
				}
				Page.Slot = -1;
				if (Entry.ModelToResidentSlot.IsValidIndex(PageIndex))
				{
					Entry.ModelToResidentSlot[PageIndex] = -1;
				}
				if (Page.State != EModelPageState::Failed)
				{
					Page.State = EModelPageState::Unloaded;
				}
				(void)bMarkEvicted;
			}

			void ReleaseEntryPages(FAtlasDecodeCacheEntry& Entry)
			{
				for (int32 PageIndex = 0; PageIndex < Entry.Pages.Num(); ++PageIndex)
				{
					ReleasePage(Entry, PageIndex, false);
				}
				Entry.ResidentModels.Reset();
				Entry.ResidentSlotModels.Init(-1, GetEffectiveSlotCapacity(Entry));
				Entry.ResidentSlotUploaded.Init(0, GetEffectiveSlotCapacity(Entry));
				Entry.ModelToResidentSlot.Init(-1, Entry.ModelCount);
				Entry.GPUTileModelIndices.Init(-1, Entry.Binding.TileModelIndices.Num());
				Entry.ResidentParameters.Init(0.0f, Entry.ResidentParameters.Num());
			}

			bool CreateEntryGPUResources(FRHICommandListImmediate& RHICmdList, FAtlasDecodeCacheEntry& Entry)
			{
				if (Entry.GPU.IsValid())
				{
					return true;
				}

				const int32 Capacity = GetEffectiveSlotCapacity(Entry);
				const Train::FTileTrainConfig Config = MakeConfig(Entry.Binding);
				const FCompactTileLayout Layout = ComputeCompactTileLayout(Config);
				if (Capacity <= 0 || Layout.TotalParams <= 0)
				{
					return false;
				}

				TRefCountPtr<FRHITexture> FeatureUV;
				TRefCountPtr<FRHITexture> FeatureUVT;
				if (!CreateEmptyFeatureArrays(RHICmdList, Config.UVSize, Config.UVTSize, Config.TimeSlices,
					Capacity, FeatureUV, FeatureUVT))
				{
					UE_LOG(LogNDGIRuntime, Error, TEXT("NDGI cache: could not create BC7 feature textures for '%s'."),
						*Entry.TargetTexturePath.ToString());
					return false;
				}

				Entry.ResidentSlotModels.Init(-1, Capacity);
				Entry.ResidentSlotUploaded.Init(0, Capacity);
				Entry.ResidentParameters.Init(0.0f, Capacity * Layout.TotalParams);
				Entry.GPUTileModelIndices.Init(-1, Entry.Binding.TileModelIndices.Num());

				TArray<FVector4f> EncodingRangeData;
				TArray<uint32> PackedEncodingIndices;
				BuildEncodingRangeData(Entry.Binding, EncodingRangeData, PackedEncodingIndices);

				FRDGBuilder GraphBuilder(RHICmdList);
				FRDGBufferRef Parameters = CreateStructuredBuffer(
					GraphBuilder, TEXT("NDGI.Cache.Parameters"), Entry.ResidentParameters);
				FRDGBufferRef Indices = CreateStructuredBuffer(
					GraphBuilder, TEXT("NDGI.Cache.Indices"), Entry.GPUTileModelIndices);
				FRDGBufferRef EncodingRanges = CreateStructuredBuffer(
					GraphBuilder, TEXT("NDGI.Cache.EncodingRanges"), EncodingRangeData);
				FRDGBufferRef EncodingIndices = CreateStructuredBuffer(
					GraphBuilder, TEXT("NDGI.Cache.EncodingIndices"), PackedEncodingIndices);

				TRefCountPtr<FRDGPooledBuffer> PooledParameters;
				TRefCountPtr<FRDGPooledBuffer> PooledIndices;
				TRefCountPtr<FRDGPooledBuffer> PooledEncodingRanges;
				TRefCountPtr<FRDGPooledBuffer> PooledEncodingIndices;
				GraphBuilder.QueueBufferExtraction(Parameters, &PooledParameters);
				GraphBuilder.QueueBufferExtraction(Indices, &PooledIndices);
				GraphBuilder.QueueBufferExtraction(EncodingRanges, &PooledEncodingRanges);
				GraphBuilder.QueueBufferExtraction(EncodingIndices, &PooledEncodingIndices);
				GraphBuilder.Execute();

				Entry.GPU = MakeShared<FNDGIAtlasGPUResources>();
				Entry.GPU->Parameters = PooledParameters;
				Entry.GPU->Indices = PooledIndices;
				Entry.GPU->EncodingRanges = PooledEncodingRanges;
				Entry.GPU->EncodingIndices = PooledEncodingIndices;
				Entry.GPU->FeatureUV = FeatureUV;
				Entry.GPU->FeatureUVT = FeatureUVT;
				Entry.GPU->SlotCapacity = Capacity;
				Entry.GPU->UVSize = Config.UVSize;
				Entry.GPU->UVTSize = Config.UVTSize;
				Entry.GPU->TimeSlices = Config.TimeSlices;
				Entry.GPU->TotalParams = Layout.TotalParams;
				return true;
			}

			void CreateGPUResourcesForCache(FRHICommandListImmediate& RHICmdList, const TSharedPtr<FLevelDecodeCache>& Cache)
			{
				for (FAtlasDecodeCacheEntry& Entry : Cache->Entries)
				{
					if (Entry.bActive)
					{
						CreateEntryGPUResources(RHICmdList, Entry);
					}
				}
			}

			void AppendIndexBuffer(FAtlasDecodeCacheEntry& Entry)
			{
				Entry.GPUTileModelIndices.SetNumUninitialized(Entry.Binding.TileModelIndices.Num());
				for (int32 Cell = 0; Cell < Entry.Binding.TileModelIndices.Num(); ++Cell)
				{
					const int32 Logical = Entry.Binding.TileModelIndices[Cell];
					const int32 Slot = Entry.ModelToResidentSlot.IsValidIndex(Logical)
						? Entry.ModelToResidentSlot[Logical]
						: -1;
					const bool bUploaded = Slot >= 0 && Entry.ResidentSlotUploaded.IsValidIndex(Slot)
						&& Entry.ResidentSlotUploaded[Slot] != 0
						&& Entry.ResidentSlotModels.IsValidIndex(Slot) && Entry.ResidentSlotModels[Slot] == Logical;
					Entry.GPUTileModelIndices[Cell] = bUploaded ? Slot : -1;
				}
			}

			void UploadPagesToGPU(
				FRHICommandListImmediate& RHICmdList,
				const TSharedPtr<FNDGIAtlasGPUResources>& GPU,
				TArray<FPageUploadPacket>&& Uploads,
				TArray<float>&& Parameters,
				TArray<int32>&& Indices)
			{
				if (!GPU.IsValid())
				{
					return;
				}

				if (Uploads.Num() > 0 && GPU->FeatureUV.IsValid() && GPU->FeatureUVT.IsValid())
				{
					RHICmdList.Transition(FRHITransitionInfo(GPU->FeatureUV, ERHIAccess::SRVMask, ERHIAccess::CopyDest));
					RHICmdList.Transition(FRHITransitionInfo(GPU->FeatureUVT, ERHIAccess::SRVMask, ERHIAccess::CopyDest));
					for (const FPageUploadPacket& Packet : Uploads)
					{
						if (Packet.Slot < 0 || Packet.Slot >= GPU->SlotCapacity)
						{
							continue;
						}
						UploadBC7Layer(RHICmdList, GPU->FeatureUV, Packet.Slot,
							Packet.UVBC7.GetData(), GPU->UVSize, GPU->UVSize);
						for (int32 Slice = 0; Slice < GPU->TimeSlices; ++Slice)
						{
							const int32 Layer = Packet.Slot * GPU->TimeSlices + Slice;
							UploadBC7Layer(RHICmdList, GPU->FeatureUVT, Layer,
								Packet.UVTBC7.GetData() + static_cast<int64>(Slice) * GetUVTLayerBytes(GPU->UVTSize),
								GPU->UVTSize, GPU->UVTSize);
						}
					}
					RHICmdList.Transition(FRHITransitionInfo(GPU->FeatureUV, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
					RHICmdList.Transition(FRHITransitionInfo(GPU->FeatureUVT, ERHIAccess::CopyDest, ERHIAccess::SRVMask));
				}

				if (Parameters.Num() > 0 && GPU->Parameters.IsValid())
				{
					const uint32 Bytes = static_cast<uint32>(Parameters.Num()) * sizeof(float);
					void* Dest = RHICmdList.LockBuffer(GPU->Parameters->GetRHI(), 0, Bytes, RLM_WriteOnly);
					if (Dest)
					{
						FMemory::Memcpy(Dest, Parameters.GetData(), Bytes);
						RHICmdList.UnlockBuffer(GPU->Parameters->GetRHI());
					}
				}
				if (Indices.Num() > 0 && GPU->Indices.IsValid())
				{
					const uint32 Bytes = static_cast<uint32>(Indices.Num()) * sizeof(int32);
					void* Dest = RHICmdList.LockBuffer(GPU->Indices->GetRHI(), 0, Bytes, RLM_WriteOnly);
					if (Dest)
					{
						FMemory::Memcpy(Dest, Indices.GetData(), Bytes);
						RHICmdList.UnlockBuffer(GPU->Indices->GetRHI());
					}
				}
			}

			bool SetCacheEntryActive(FLevelDecodeCache& Cache, FAtlasDecodeCacheEntry& Entry, bool bActive)
			{
				if (Entry.bActive == bActive)
				{
					return true;
				}

				const int64 EntryGPUBytes = EstimateEntryGPUBytes(Entry);
				if (bActive)
				{
					const int32 LimitMB = CVarNDGIMaxCacheMemoryMB.GetValueOnGameThread();
					const int64 Limit = static_cast<int64>(FMath::Max(0, LimitMB)) * 1024 * 1024;
					if (Limit > 0 && Cache.CPUBytes + Cache.ActiveGPUBytes + EntryGPUBytes > Limit)
					{
						UE_LOG(LogNDGIRuntime, Warning,
							TEXT("NDGI streaming: activating '%s' would exceed the %d MB decode-cache limit."),
							*Entry.TargetTexturePath.ToString(), LimitMB);
						return false;
					}

					UTexture2D* Texture = Entry.Binding.TargetTexture;
					if (!Texture)
					{
						Texture = Cast<UTexture2D>(Entry.TargetTexturePath.TryLoad());
					}
					if (!Texture)
					{
						UE_LOG(LogNDGIRuntime, Warning,
							TEXT("NDGI streaming: lightmap '%s' could not be loaded."),
							*Entry.TargetTexturePath.ToString());
						return false;
					}

					FTextureResource* Resource = nullptr;
					if (!EnsureFullResolution(Texture, Entry.Binding.AtlasWidth, Entry.Binding.AtlasHeight, Resource))
					{
						UE_LOG(LogNDGIRuntime, Warning,
							TEXT("NDGI streaming: lightmap '%s' is not resident at full resolution (%dx%d)."),
							*Texture->GetName(), Entry.Binding.AtlasWidth, Entry.Binding.AtlasHeight);
						return false;
					}

					ReleaseEntryPages(Entry);
					Entry.Binding.TargetTexture = Texture;
					Entry.bActive = true;
					FAtlasDecodeCacheEntry* EntryPtr = &Entry;
					ENQUEUE_RENDER_COMMAND(NDGIActivateAtlas)(
						[EntryPtr](FRHICommandListImmediate& RHICmdList)
						{
							CreateEntryGPUResources(RHICmdList, *EntryPtr);
						});

					Cache.ActiveGPUBytes += EntryGPUBytes;
					++Cache.ActiveAtlases;
					return true;
				}

				ReleaseEntryPages(Entry);
				Entry.GPU.Reset();
				Entry.Binding.TargetTexture = nullptr;
				Entry.bActive = false;
				Cache.ActiveGPUBytes = FMath::Max<int64>(0, Cache.ActiveGPUBytes - EntryGPUBytes);
				Cache.ActiveAtlases = FMath::Max(0, Cache.ActiveAtlases - 1);
				return true;
			}

			bool CheckCacheMemoryBudget(const FLevelDecodeCache& Cache, FString* OutError)
			{
				const int32 LimitMB = CVarNDGIMaxCacheMemoryMB.GetValueOnGameThread();
				const int64 Limit = static_cast<int64>(FMath::Max(0, LimitMB)) * 1024 * 1024;
				const int64 Used = Cache.CPUBytes + Cache.ActiveGPUBytes;
				if (Limit > 0 && Used > Limit)
				{
					if (OutError)
					{
						*OutError = FString::Printf(
							TEXT("decode cache needs %.1f MB, over the %d MB limit (ndgi.MaxCacheMemoryMB)"),
							Used / (1024.0 * 1024.0), LimitMB);
					}
					return false;
				}
				return true;
			}

			bool LoadPageBlocking(FLevelDecodeCache& Cache, FAtlasDecodeCacheEntry& Entry, int32 PageIndex)
			{
				if (!Entry.Pages.IsValidIndex(PageIndex))
				{
					return false;
				}

				FModelPage& Page = Entry.Pages[PageIndex];
				if (Page.State == EModelPageState::Uploaded)
				{
					return true;
				}
				if (Page.State == EModelPageState::Failed)
				{
					return false;
				}

				Page.State = EModelPageState::Loading;
				++Cache.PageLoadRequests;
				UNDGIModelAsset* Model = Cast<UNDGIModelAsset>(Page.Path.TryLoad());
				if (Model && BuildPagePayload(Entry, *Model, Page))
				{
					Page.State = EModelPageState::Ready;
					++Cache.PageLoadCompletions;
					return true;
				}

				ClearPagePayload(Page);
				Page.State = EModelPageState::Failed;
				++Cache.PageLoadFailures;
				return false;
			}

			void RequestPageLoad(FLevelDecodeCache& Cache, int32 EntryIndex, int32 PageIndex)
			{
				if (!Cache.Entries.IsValidIndex(EntryIndex))
				{
					return;
				}
				FAtlasDecodeCacheEntry& Entry = Cache.Entries[EntryIndex];
				if (!Entry.Pages.IsValidIndex(PageIndex))
				{
					return;
				}

				FModelPage& Page = Entry.Pages[PageIndex];
				if (Page.State != EModelPageState::Unloaded)
				{
					return;
				}

				Page.State = EModelPageState::Loading;
				++Cache.PageLoadRequests;
				++Cache.InFlightPageLoads;

				TArray<FSoftObjectPath> Paths;
				Paths.Add(Page.Path);
				TWeakPtr<FLevelDecodeCache> WeakCache = Cache.AsShared();
				Page.LoadHandle = UAssetManager::GetStreamableManager().RequestAsyncLoad(
					Paths,
					FStreamableDelegate::CreateLambda([WeakCache, EntryIndex, PageIndex]()
					{
						if (TSharedPtr<FLevelDecodeCache> Pinned = WeakCache.Pin())
						{
							Pinned->OnPageLoadComplete(EntryIndex, PageIndex);
						}
					}),
					0,
					false,
					false,
					FString::Printf(TEXT("NDGI.Page.%d.%d"), EntryIndex, PageIndex));

				if (!Page.LoadHandle.IsValid())
				{
					Cache.InFlightPageLoads = FMath::Max(0, Cache.InFlightPageLoads - 1);
					Page.State = EModelPageState::Failed;
					++Cache.PageLoadFailures;
				}
			}
		}

		bool FLevelDecodeCache::UpdateActiveCells(const TSet<FString>& LoadedCells)
		{
			bool bChanged = false;
			bool bActivatedAny = false;
			ActiveCells = LoadedCells;
			for (FAtlasDecodeCacheEntry& Entry : Entries)
			{
				const bool bShouldBeActive = Entry.SourceCells.Num() == 0
					|| Entry.SourceCells.ContainsByPredicate([&LoadedCells](const FString& Cell)
					{
						return LoadedCells.Contains(Cell);
					});
				if (bShouldBeActive == Entry.bActive)
				{
					continue;
				}

				if (SetCacheEntryActive(*this, Entry, bShouldBeActive))
				{
					bChanged = true;
					bActivatedAny |= bShouldBeActive;
				}
				else if (bShouldBeActive)
				{
					UE_LOG(LogNDGIRuntime, Warning,
						TEXT("NDGI streaming: atlas '%s' stays inactive."), *Entry.TargetTexturePath.ToString());
				}
			}

			if (bActivatedAny)
			{
				// One flush covers every atlas activated by this streaming change.
				FlushRenderingCommands();
				LoadAllPagesBlocking();
			}
			RecomputeCacheMemory(*this);
			return bChanged;
		}

		bool FLevelDecodeCache::LoadAllPagesBlocking(int32* OutFailedPages)
		{
			bool bEnqueued = false;
			for (FAtlasDecodeCacheEntry& Entry : Entries)
			{
				if (!Entry.bActive || !Entry.GPU.IsValid())
				{
					continue;
				}
				const int32 Capacity = GetEffectiveSlotCapacity(Entry);
				if (Capacity <= 0 || Capacity != Entry.ModelCount)
				{
					// Only the no-pool configuration is fully resident.
					continue;
				}

				for (int32 PageIndex = 0; PageIndex < Entry.Pages.Num(); ++PageIndex)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.State == EModelPageState::Uploaded || Page.State == EModelPageState::Failed)
					{
						continue;
					}
					if (Page.LoadHandle.IsValid())
					{
						Page.LoadHandle->CancelHandle();
						Page.LoadHandle.Reset();
					}
					LoadPageBlocking(*this, Entry, PageIndex);
				}

				TArray<FPageUploadPacket> Uploads;
				for (int32 PageIndex = 0; PageIndex < Entry.Pages.Num(); ++PageIndex)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.State != EModelPageState::Ready)
					{
						continue;
					}

					if (Page.Slot < 0)
					{
						int32 FreeSlot = INDEX_NONE;
						for (int32 Slot = 0; Slot < Entry.ResidentSlotModels.Num(); ++Slot)
						{
							if (Entry.ResidentSlotModels[Slot] < 0)
							{
								FreeSlot = Slot;
								break;
							}
						}
						if (FreeSlot == INDEX_NONE)
						{
							break;
						}
						Page.Slot = FreeSlot;
						Entry.ResidentSlotModels[FreeSlot] = PageIndex;
						Entry.ModelToResidentSlot[PageIndex] = FreeSlot;
					}

					FPageUploadPacket Packet;
					Packet.Slot = Page.Slot;
					Packet.UVBC7 = MoveTemp(Page.PendingUVBC7);
					Packet.UVTBC7 = MoveTemp(Page.PendingUVTBC7);
					Packet.Parameters = MoveTemp(Page.PendingParameters);
					if (Entry.ResidentParameters.Num() >= (Packet.Slot + 1) * Entry.GPU->TotalParams)
					{
						FMemory::Memcpy(
							Entry.ResidentParameters.GetData() + static_cast<int64>(Packet.Slot) * Entry.GPU->TotalParams,
							Packet.Parameters.GetData(),
							static_cast<size_t>(Packet.Parameters.Num()) * sizeof(float));
					}
					if (Entry.ResidentSlotUploaded.IsValidIndex(Packet.Slot))
					{
						Entry.ResidentSlotUploaded[Packet.Slot] = 1;
					}
					Page.State = EModelPageState::Uploaded;
					Page.LoadHandle.Reset();
					Uploads.Add(MoveTemp(Packet));
				}

				if (Uploads.Num() > 0)
				{
					StreamingPageUploads += Uploads.Num();
					for (const FPageUploadPacket& Packet : Uploads)
					{
						StreamingPageUploadBytes += Packet.UVBC7.Num() + Packet.UVTBC7.Num()
							+ static_cast<int64>(Packet.Parameters.Num()) * sizeof(float);
					}
					++StreamingUploads;
					AppendIndexBuffer(Entry);
					const TSharedPtr<FNDGIAtlasGPUResources> GPU = Entry.GPU;
					TArray<float> Parameters = Entry.ResidentParameters;
					TArray<int32> Indices = Entry.GPUTileModelIndices;
					ENQUEUE_RENDER_COMMAND(NDGIBlockingPageUpload)(
						[GPU, Uploads = MoveTemp(Uploads), Parameters = MoveTemp(Parameters), Indices = MoveTemp(Indices)]
						(FRHICommandListImmediate& RHICmdList) mutable
						{
							UploadPagesToGPU(RHICmdList, GPU, MoveTemp(Uploads), MoveTemp(Parameters), MoveTemp(Indices));
						});
					bEnqueued = true;
				}

				Entry.ResidentModels.Reset();
				for (int32 PageIndex = 0; PageIndex < Entry.Pages.Num(); ++PageIndex)
				{
					if (Entry.Pages[PageIndex].State == EModelPageState::Uploaded)
					{
						Entry.ResidentModels.Add(PageIndex);
					}
				}
			}

			if (bEnqueued)
			{
				FlushRenderingCommands();
			}

			int32 FailedPages = 0;
			for (const FAtlasDecodeCacheEntry& Entry : Entries)
			{
				for (const FModelPage& Page : Entry.Pages)
				{
					FailedPages += Page.State == EModelPageState::Failed ? 1 : 0;
				}
			}
			RecomputeCacheMemory(*this);
			if (OutFailedPages)
			{
				*OutFailedPages = FailedPages;
			}
			return FailedPages == 0;
		}

		void FLevelDecodeCache::ReleaseAllPages()
		{
			for (FAtlasDecodeCacheEntry& Entry : Entries)
			{
				ReleaseEntryPages(Entry);
			}
			RecomputeCacheMemory(*this);
		}

		void FLevelDecodeCache::OnPageLoadComplete(int32 EntryIndex, int32 PageIndex)
		{
			if (!Entries.IsValidIndex(EntryIndex))
			{
				return;
			}
			FAtlasDecodeCacheEntry& Entry = Entries[EntryIndex];
			if (!Entry.Pages.IsValidIndex(PageIndex))
			{
				return;
			}
			FModelPage& Page = Entry.Pages[PageIndex];
			if (Page.State != EModelPageState::Loading)
			{
				return;
			}

			InFlightPageLoads = FMath::Max(0, InFlightPageLoads - 1);
			UNDGIModelAsset* Model = Page.LoadHandle.IsValid()
				? Cast<UNDGIModelAsset>(Page.LoadHandle->GetLoadedAsset())
				: nullptr;
			Page.LoadHandle.Reset();

			if (Model && BuildPagePayload(Entry, *Model, Page))
			{
				Page.State = EModelPageState::Ready;
				++PageLoadCompletions;
			}
			else
			{
				ClearPagePayload(Page);
				Page.State = EModelPageState::Failed;
				++PageLoadFailures;
				if (Page.Slot >= 0 && Entry.ResidentSlotModels.IsValidIndex(Page.Slot))
				{
					Entry.ResidentSlotModels[Page.Slot] = -1;
					Entry.ResidentSlotUploaded[Page.Slot] = 0;
				}
				Page.Slot = -1;
				if (Entry.ModelToResidentSlot.IsValidIndex(PageIndex))
				{
					Entry.ModelToResidentSlot[PageIndex] = -1;
				}
			}
		}

		uint64 FLevelDecodeCache::GetFeatureTextureResourceId(int32 EntryIndex) const
		{
			if (!Entries.IsValidIndex(EntryIndex))
			{
				return 0;
			}
			const FAtlasDecodeCacheEntry& Entry = Entries[EntryIndex];
			if (!Entry.GPU.IsValid() || !Entry.GPU->FeatureUV.IsValid())
			{
				return 0;
			}
			return reinterpret_cast<uint64>(Entry.GPU->FeatureUV.GetReference());
		}

		bool FLevelDecodeCache::UpdateStreaming(const FVector& CameraLocation)
		{
			const float Hysteresis = FMath::Max(0.0f, CVarNDGIStreamingHysteresis.GetValueOnGameThread());
			const int32 MaxInFlight = FMath::Max(0, CVarNDGIStreamingMaxLoadsInFlight.GetValueOnGameThread());
			const int64 BudgetBytes = static_cast<int64>(FMath::Max(0, CVarNDGIStreamingUploadBudgetKB.GetValueOnGameThread())) * 1024;
			const bool bSyncLoad = CVarNDGIStreamingSyncLoad.GetValueOnGameThread() != 0;
			int64 RemainingBudget = BudgetBytes > 0 ? BudgetBytes : MAX_int64;
			int32 InFlight = 0;
			for (const FAtlasDecodeCacheEntry& Entry : Entries)
			{
				for (const FModelPage& Page : Entry.Pages)
				{
					InFlight += Page.State == EModelPageState::Loading ? 1 : 0;
				}
			}

			bool bAnyChanged = false;
			for (FAtlasDecodeCacheEntry& Entry : Entries)
			{
				if (!Entry.bActive || !Entry.GPU.IsValid())
				{
					continue;
				}

				const int32 ModelCount = Entry.ModelCount;
				const int32 Capacity = GetEffectiveSlotCapacity(Entry);
				if (ModelCount <= 0 || Capacity <= 0)
				{
					continue;
				}
				if (Entry.ResidentSlotModels.Num() != Capacity)
				{
					Entry.ResidentSlotModels.Init(-1, Capacity);
					Entry.ResidentSlotUploaded.Init(0, Capacity);
				}

				TArray<int32> Candidates;
				TArray<float> Scores;
				Candidates.Reserve(ModelCount);
				Scores.Init(0.0f, ModelCount);
				for (int32 PageIndex = 0; PageIndex < ModelCount; ++PageIndex)
				{
					const FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.State == EModelPageState::Failed)
					{
						continue;
					}
					float Distance = 0.0f;
					if (Entry.ModelBounds.IsValidIndex(PageIndex) && Entry.ModelBounds[PageIndex].IsValid)
					{
						Distance = static_cast<float>(FVector::DistSquared(CameraLocation, Entry.ModelBounds[PageIndex].GetCenter()));
					}
					if (Entry.ModelToResidentSlot.IsValidIndex(PageIndex) && Entry.ModelToResidentSlot[PageIndex] >= 0)
					{
						Distance /= (1.0f + Hysteresis);
					}
					Candidates.Add(PageIndex);
					Scores[PageIndex] = Distance;
				}
				Candidates.Sort([&Scores](int32 A, int32 B)
				{
					return Scores[A] == Scores[B] ? A < B : Scores[A] < Scores[B];
				});

				TArray<int32> Desired;
				Desired.Reserve(FMath::Min(Capacity, Candidates.Num()));
				for (int32 Index = 0; Index < Candidates.Num() && Desired.Num() < Capacity; ++Index)
				{
					Desired.Add(Candidates[Index]);
				}

				TArray<uint8> DesiredMask;
				DesiredMask.Init(0, ModelCount);
				for (int32 PageIndex : Desired)
				{
					DesiredMask[PageIndex] = 1;
				}

				int32 Evicted = 0;
				bool bGPUChanged = false;
				for (int32 PageIndex = 0; PageIndex < ModelCount; ++PageIndex)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (DesiredMask[PageIndex] != 0 || Page.State == EModelPageState::Unloaded || Page.State == EModelPageState::Failed)
					{
						continue;
					}
					if (Page.State == EModelPageState::Loading)
					{
						InFlight = FMath::Max(0, InFlight - 1);
					}
					if (Page.State == EModelPageState::Uploaded)
					{
						bGPUChanged = true;
					}
					ReleasePage(Entry, PageIndex, true);
					++Evicted;
				}
				StreamingEvictions += Evicted;

				auto FindFreeSlot = [&Entry]() -> int32
				{
					for (int32 Slot = 0; Slot < Entry.ResidentSlotModels.Num(); ++Slot)
					{
						if (Entry.ResidentSlotModels[Slot] < 0)
						{
							return Slot;
						}
					}
					return INDEX_NONE;
				};

				for (int32 PageIndex : Desired)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.Slot >= 0)
					{
						continue;
					}
					const int32 Slot = FindFreeSlot();
					if (Slot == INDEX_NONE)
					{
						break;
					}
					Page.Slot = Slot;
					Entry.ResidentSlotModels[Slot] = PageIndex;
					Entry.ModelToResidentSlot[PageIndex] = Slot;
					Entry.ResidentSlotUploaded[Slot] = 0;
				}

				const int32 EntryIndex = static_cast<int32>(&Entry - Entries.GetData());
				for (int32 PageIndex : Desired)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.State != EModelPageState::Unloaded || Page.Slot < 0)
					{
						continue;
					}
					if (bSyncLoad)
					{
						LoadPageBlocking(*this, Entry, PageIndex);
					}
					else if (MaxInFlight > 0 && InFlight < MaxInFlight)
					{
						RequestPageLoad(*this, EntryIndex, PageIndex);
						++InFlight;
					}
				}

				TArray<FPageUploadPacket> Uploads;
				int64 UploadedBytes = 0;
				for (int32 PageIndex : Desired)
				{
					FModelPage& Page = Entry.Pages[PageIndex];
					if (Page.State != EModelPageState::Ready || Page.Slot < 0)
					{
						continue;
					}
					const int64 PageBytes = Page.PendingUVBC7.Num() + Page.PendingUVTBC7.Num()
						+ static_cast<int64>(Page.PendingParameters.Num()) * sizeof(float);
					if (BudgetBytes > 0 && Uploads.Num() > 0 && PageBytes > RemainingBudget)
					{
						break;
					}

					FPageUploadPacket Packet;
					Packet.Slot = Page.Slot;
					Packet.UVBC7 = MoveTemp(Page.PendingUVBC7);
					Packet.UVTBC7 = MoveTemp(Page.PendingUVTBC7);
					Packet.Parameters = MoveTemp(Page.PendingParameters);
					if (Entry.ResidentParameters.Num() >= (Packet.Slot + 1) * Entry.GPU->TotalParams)
					{
						FMemory::Memcpy(
							Entry.ResidentParameters.GetData() + static_cast<int64>(Packet.Slot) * Entry.GPU->TotalParams,
							Packet.Parameters.GetData(),
							static_cast<size_t>(Packet.Parameters.Num()) * sizeof(float));
					}
					Entry.ResidentSlotUploaded[Packet.Slot] = 1;
					Page.State = EModelPageState::Uploaded;
					Page.LoadHandle.Reset();
					UploadedBytes += PageBytes;
					Uploads.Add(MoveTemp(Packet));
				}

				if (Uploads.Num() > 0 || bGPUChanged)
				{
					bGPUChanged = true;
					AppendIndexBuffer(Entry);
					const int32 NumUploads = Uploads.Num();
					StreamingPageUploads += NumUploads;
					StreamingPageUploadBytes += UploadedBytes;
					if (NumUploads > 0)
					{
						++StreamingUploads;
					}
					if (BudgetBytes > 0)
					{
						RemainingBudget = FMath::Max<int64>(0, RemainingBudget - UploadedBytes);
					}

					const TSharedPtr<FNDGIAtlasGPUResources> GPU = Entry.GPU;
					TArray<float> Parameters = Entry.ResidentParameters;
					TArray<int32> Indices = Entry.GPUTileModelIndices;
					ENQUEUE_RENDER_COMMAND(NDGIUpdateStreamingPages)(
						[GPU, Uploads = MoveTemp(Uploads), Parameters = MoveTemp(Parameters), Indices = MoveTemp(Indices)]
						(FRHICommandListImmediate& RHICmdList) mutable
						{
							UploadPagesToGPU(RHICmdList, GPU, MoveTemp(Uploads), MoveTemp(Parameters), MoveTemp(Indices));
						});
					bAnyChanged = true;
				}

				Entry.ResidentModels.Reset();
				for (int32 PageIndex = 0; PageIndex < ModelCount; ++PageIndex)
				{
					if (Entry.Pages[PageIndex].State == EModelPageState::Uploaded)
					{
						Entry.ResidentModels.Add(PageIndex);
					}
				}
			}

			RecomputeCacheMemory(*this);
			LastStreamingCamera = CameraLocation;
			LastStreamingUpdateSeconds = FPlatformTime::Seconds();
			return bAnyChanged;
		}
	}
}

float NDGI::Runtime::GetStreamingUpdateIntervalSeconds()
{
	return FMath::Max(0.0f, CVarNDGIStreamingUpdateSeconds.GetValueOnGameThread());
}

bool NDGI::Runtime::BuildLevelDecodeCache(
	UNDGILevelAsset* LevelAsset,
	TSharedPtr<FLevelDecodeCache>& OutCache,
	double* OutBuildMs,
	int32* OutSkippedModels)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (OutBuildMs)
	{
		*OutBuildMs = 0.0;
	}
	if (OutSkippedModels)
	{
		*OutSkippedModels = 0;
	}
	OutCache.Reset();
	if (!LevelAsset)
	{
		return false;
	}

	TSharedPtr<FLevelDecodeCache> Cache = MakeShared<FLevelDecodeCache>();
	int32 SkippedModels = 0;

	for (FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
	{
		FAtlasDecodeCacheEntry Cached;
		if (!PackAtlasEntry(Entry, Cached, SkippedModels))
		{
			continue;
		}
		Cached.bActive = true;
		const int64 EntryGPUBytes = EstimateEntryGPUBytes(Cached);
		Cache->Tiles += Cached.ModelCount;
		Cache->GPUBytes += EntryGPUBytes;
		Cache->ActiveGPUBytes += EntryGPUBytes;
		++Cache->ActiveAtlases;
		Cache->Entries.Add(MoveTemp(Cached));
	}

	if (Cache->Entries.Num() == 0)
	{
		if (OutBuildMs)
		{
			*OutBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		}
		if (OutSkippedModels)
		{
			*OutSkippedModels = SkippedModels;
		}
		return false;
	}

	FString MemoryError;
	if (!CheckCacheMemoryBudget(*Cache, &MemoryError))
	{
		UE_LOG(LogNDGIRuntime, Error, TEXT("NDGI decode cache: %s."), *MemoryError);
		if (OutBuildMs)
		{
			*OutBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		}
		if (OutSkippedModels)
		{
			*OutSkippedModels = SkippedModels;
		}
		return false;
	}

	// Create the persistent page pools and buffers. Pages stay unloaded unless
	// this is the no-pool configuration, which keeps the old fully resident
	// behavior for synchronous callers.
	ENQUEUE_RENDER_COMMAND(NDGIBuildDecodeCache)(
		[Cache](FRHICommandListImmediate& RHICmdList)
		{
			CreateGPUResourcesForCache(RHICmdList, Cache);
		});
	FlushRenderingCommands();

	int32 FailedPages = 0;
	Cache->LoadAllPagesBlocking(&FailedPages);
	SkippedModels += FailedPages;
	RecomputeCacheMemory(*Cache);

	UE_LOG(LogNDGIRuntime, Display,
		TEXT("NDGI decode cache: %d atlas(es), %d model(s), CPU %.1f MB, GPU %.1f MB (active %d atlas(es), %.1f MB, %d resident page(s))."),
		Cache->Entries.Num(), Cache->Tiles,
		Cache->CPUBytes / (1024.0 * 1024.0), Cache->GPUBytes / (1024.0 * 1024.0),
		Cache->ActiveAtlases, Cache->ActiveGPUBytes / (1024.0 * 1024.0), Cache->ResidentModels);

	OutCache = MoveTemp(Cache);
	if (OutBuildMs)
	{
		*OutBuildMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
	}
	if (OutSkippedModels)
	{
		*OutSkippedModels = SkippedModels;
	}
	return true;
}

TSharedPtr<NDGI::Runtime::FDecodeCacheBuildState> NDGI::Runtime::BeginBuildLevelDecodeCacheAsync(
	UNDGILevelAsset* LevelAsset,
	const TSet<FString>& LoadedCells)
{
	if (!LevelAsset)
	{
		return nullptr;
	}

	TSharedPtr<FDecodeCacheBuildState> Build = MakeShared<FDecodeCacheBuildState>();
	Build->Cache = MakeShared<FLevelDecodeCache>();
	Build->Cache->ActiveCells = LoadedCells;
	Build->LoadedCells = LoadedCells;
	Build->LevelAsset = LevelAsset;
	Build->StartSeconds = FPlatformTime::Seconds();
	return Build;
}

bool NDGI::Runtime::TickBuildLevelDecodeCacheAsync(
	const TSharedPtr<FDecodeCacheBuildState>& Build,
	TSharedPtr<FLevelDecodeCache>& OutCache,
	double* OutBuildMs,
	int32* OutSkippedModels)
{
	if (!Build.IsValid() || Build->bDone)
	{
		return true;
	}

	UNDGILevelAsset* LevelAsset = Build->LevelAsset.Get();
	if (!LevelAsset)
	{
		Build->bFailed = true;
		Build->Error = TEXT("the level asset was unloaded during the cache build");
	}

	auto Finish = [&Build, &OutCache, OutBuildMs, OutSkippedModels](bool bSuccess)
	{
		Build->bDone = true;
		Build->bFailed = !bSuccess;
		if (OutBuildMs)
		{
			*OutBuildMs = (FPlatformTime::Seconds() - Build->StartSeconds) * 1000.0;
		}
		if (OutSkippedModels)
		{
			*OutSkippedModels = Build->SkippedModels;
		}
		if (bSuccess)
		{
			OutCache = Build->Cache;
		}
		return true;
	};

	if (Build->bFailed)
	{
		return Finish(false);
	}

	if (!Build->bUploadQueued)
	{
		if (Build->NextAtlas < LevelAsset->Atlases.Num())
		{
			const int32 AtlasIndex = Build->NextAtlas++;
			FNDGIAtlasEntry& AtlasEntry = LevelAsset->Atlases[AtlasIndex];
			const bool bCellFilter = Build->LoadedCells.Num() > 0;
			const bool bShouldBeActive = !bCellFilter || AtlasEntry.SourceCells.Num() == 0
				|| AtlasEntry.SourceCells.ContainsByPredicate([&Build](const FString& Cell)
				{
					return Build->LoadedCells.Contains(Cell);
				});

			FAtlasDecodeCacheEntry Cached;
			if (PackAtlasEntry(AtlasEntry, Cached, Build->SkippedModels, bShouldBeActive))
			{
				Cached.bActive = bShouldBeActive;
				const int64 EntryGPUBytes = EstimateEntryGPUBytes(Cached);
				Build->Cache->Tiles += Cached.ModelCount;
				Build->Cache->GPUBytes += EntryGPUBytes;
				if (bShouldBeActive)
				{
					Build->Cache->ActiveGPUBytes += EntryGPUBytes;
					++Build->Cache->ActiveAtlases;
				}
				Build->Cache->Entries.Add(MoveTemp(Cached));

				FString BudgetError;
				if (!CheckCacheMemoryBudget(*Build->Cache, &BudgetError))
				{
					Build->Error = BudgetError;
					return Finish(false);
				}
			}
			return false;
		}

		if (Build->Cache->Entries.Num() == 0)
		{
			Build->Error = TEXT("no atlas had usable models");
			return Finish(false);
		}

		FString MemoryError;
		if (!CheckCacheMemoryBudget(*Build->Cache, &MemoryError))
		{
			Build->Error = MemoryError;
			return Finish(false);
		}

		const TSharedPtr<FLevelDecodeCache> CacheToUpload = Build->Cache;
		ENQUEUE_RENDER_COMMAND(NDGIAsyncBuildDecodeCache)(
			[CacheToUpload](FRHICommandListImmediate& RHICmdList)
			{
				CreateGPUResourcesForCache(RHICmdList, CacheToUpload);
			});
		Build->UploadFence.BeginFence();
		Build->bUploadQueued = true;
		return false;
	}

	if (!Build->UploadFence.IsFenceComplete())
	{
		return false;
	}

	RecomputeCacheMemory(*Build->Cache);
	UE_LOG(LogNDGIRuntime, Display,
		TEXT("NDGI decode cache (async): %d atlas(es), %d model(s), CPU %.1f MB, GPU %.1f MB (active %d atlas(es), %.1f MB, %d resident page(s)), %.1f ms."),
		Build->Cache->Entries.Num(), Build->Cache->Tiles,
		Build->Cache->CPUBytes / (1024.0 * 1024.0), Build->Cache->GPUBytes / (1024.0 * 1024.0),
		Build->Cache->ActiveAtlases, Build->Cache->ActiveGPUBytes / (1024.0 * 1024.0),
		Build->Cache->ResidentModels,
		(FPlatformTime::Seconds() - Build->StartSeconds) * 1000.0);
	return Finish(true);
}

bool NDGI::Runtime::DecodeCachedLevel(
	const TSharedPtr<FLevelDecodeCache>& Cache,
	float Time,
	const TArray<UTexture2D*>& TargetTextures,
	FDecodeTimings* OutTimings,
	TFunction<void()> OnComplete)
{
	const double StartSeconds = FPlatformTime::Seconds();
	if (OutTimings)
	{
		*OutTimings = FDecodeTimings();
	}
	if (!Cache.IsValid() || !Cache->IsValid() || TargetTextures.Num() != Cache->Entries.Num())
	{
		return false;
	}

	struct FRenderTargetInfo
	{
		FTextureResource* Resource = nullptr;
		EPixelFormat Format = PF_Unknown;
	};

	TArray<FRenderTargetInfo> Targets;
	Targets.SetNumUninitialized(Cache->Entries.Num());
	for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
	{
		const FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
		if (!Entry.bActive)
		{
			continue;
		}

		UTexture2D* Texture = TargetTextures[Index];
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Texture || !PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		const int32 Width = PlatformData->Mips[0].SizeX;
		const int32 Height = PlatformData->Mips[0].SizeY;
		if (Width != Entry.Binding.AtlasWidth || Height != Entry.Binding.AtlasHeight)
		{
			return false;
		}
		const EPixelFormat Format = PlatformData->PixelFormat;
		if (Format != PF_FloatRGBA && Format != PF_B8G8R8A8)
		{
			return false;
		}
		FTextureResource* Resource = Texture->GetResource();
		const FRHITexture* RHI = Resource ? Resource->GetTextureRHI() : nullptr;
		if (!RHI
			|| static_cast<int32>(RHI->GetSizeX()) < Width
			|| static_cast<int32>(RHI->GetSizeY()) < Height)
		{
			return false;
		}
		Targets[Index].Resource = Resource;
		Targets[Index].Format = Format;
	}

	struct FPreparedEntry
	{
		FVector3f MeanRGB = FVector3f::ZeroVector;
		FVector3f MeanSH = FVector3f::ZeroVector;
		bool bDirectionalSH = false;
	};

	TArray<FPreparedEntry> Prepared;
	Prepared.SetNumUninitialized(Cache->Entries.Num());
	for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
	{
		const FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
		const float StepF = Time * static_cast<float>(FMath::Max(1, Entry.Binding.TimeSlices - 1));
		const int32 Step0 = FMath::Clamp(FMath::FloorToInt(StepF), 0, FMath::Max(0, Entry.Binding.TimeSlices - 1));
		const int32 Step1 = FMath::Min(Step0 + 1, FMath::Max(0, Entry.Binding.TimeSlices - 1));
		const float Frac = FMath::Clamp(StepF - static_cast<float>(Step0), 0.0f, 1.0f);
		FPreparedEntry& Out = Prepared[Index];
		if (Entry.StepMeansRGB.IsValidIndex(Step0) && Entry.StepMeansRGB.IsValidIndex(Step1))
		{
			Out.MeanRGB = FVector3f(
				FMath::Lerp(Entry.StepMeansRGB[Step0].X, Entry.StepMeansRGB[Step1].X, Frac),
				FMath::Lerp(Entry.StepMeansRGB[Step0].Y, Entry.StepMeansRGB[Step1].Y, Frac),
				FMath::Lerp(Entry.StepMeansRGB[Step0].Z, Entry.StepMeansRGB[Step1].Z, Frac));
		}
		if (Entry.bDirectionalSH && Entry.StepMeansSH.IsValidIndex(Step0) && Entry.StepMeansSH.IsValidIndex(Step1))
		{
			Out.MeanSH = FVector3f(
				FMath::Lerp(Entry.StepMeansSH[Step0].X, Entry.StepMeansSH[Step1].X, Frac),
				FMath::Lerp(Entry.StepMeansSH[Step0].Y, Entry.StepMeansSH[Step1].Y, Frac),
				FMath::Lerp(Entry.StepMeansSH[Step0].Z, Entry.StepMeansSH[Step1].Z, Frac));
			Out.bDirectionalSH = true;
		}
	}

	ENQUEUE_RENDER_COMMAND(NDGIDecodeCachedLevel)(
		[Cache, Targets, Prepared, Time, OnComplete = MoveTemp(OnComplete)] (FRHICommandListImmediate& RHICmdList) mutable
		{
			struct FJob
			{
				TRefCountPtr<IPooledRenderTarget> Output;
				FTextureResource* TargetResource = nullptr;
				EPixelFormat Format = PF_Unknown;
				TArray<uint32> ModelIndices;
			};

			FRDGBuilder GraphBuilder(RHICmdList);
			TArray<FJob> Jobs;
			// SetNum (not SetNumUninitialized): TRefCountPtr must be constructed before
			// QueueTextureExtraction writes into it.
			Jobs.SetNum(Cache->Entries.Num());

			for (int32 Index = 0; Index < Cache->Entries.Num(); ++Index)
			{
				const FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
				if (!Entry.GPU.IsValid())
				{
					continue;
				}

				// Persistent GPU buffers, built once with the cache.
				FRDGBufferRef ParametersBuffer = GraphBuilder.RegisterExternalBuffer(Entry.GPU->Parameters);
				FRDGBufferRef IndicesBuffer = GraphBuilder.RegisterExternalBuffer(Entry.GPU->Indices);
				FRDGBufferRef EncodingRangesBuffer = GraphBuilder.RegisterExternalBuffer(Entry.GPU->EncodingRanges);
				FRDGBufferRef EncodingIndicesBuffer = GraphBuilder.RegisterExternalBuffer(Entry.GPU->EncodingIndices);

				// Decode directly in the target's format. BGRA8 targets then take a
				// same-format CopyTexture, avoiding the readback/UpdateTexture2D stall;
				// RGBA16F targets use CopyTexture as before.
				const EPixelFormat OutputFormat = Targets[Index].Format;
				const FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
					FIntPoint(Entry.Binding.AtlasWidth, Entry.Binding.AtlasHeight),
					OutputFormat, FClearValueBinding::Black,
					TexCreate_ShaderResource | TexCreate_UAV);
				FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(OutputDesc, TEXT("NDGI.Cache.Output"));

				const int32 ModelExtent = Entry.Binding.TileSize + 2 * FMath::Max(0, Entry.Binding.OverlapTexels);
				Train::FTileTrainConfig Config;
				Config.UVSize = Entry.Binding.FeatureUVSize > 0 ? Entry.Binding.FeatureUVSize : ModelExtent;
				Config.UVTSize = Entry.Binding.FeatureUVTSize > 0 ? Entry.Binding.FeatureUVTSize : ModelExtent / 4;
				Config.UTSize = Entry.Binding.FeatureUTSize > 0 ? Entry.Binding.FeatureUTSize : ModelExtent / 2;
				Config.VTSize = Entry.Binding.FeatureVTSize > 0 ? Entry.Binding.FeatureVTSize : ModelExtent / 2;
				Config.TimeSlices = Entry.Binding.TimeSlices;

				FAtlasPassSetup Setup;
				Setup.AtlasWidth = static_cast<uint32>(Entry.Binding.AtlasWidth);
				Setup.AtlasHeight = static_cast<uint32>(Entry.Binding.AtlasHeight);
				Setup.TileSize = static_cast<uint32>(Entry.Binding.TileSize);
				Setup.OverlapTexels = static_cast<uint32>(FMath::Max(0, Entry.Binding.OverlapTexels));
				Setup.GridX = static_cast<uint32>(Entry.Binding.AtlasWidth / FMath::Max(1, Entry.Binding.TileSize));
				Setup.GridY = static_cast<uint32>((Entry.Binding.AtlasHeight / 2) / FMath::Max(1, Entry.Binding.TileSize));
				Setup.TimeSlices = static_cast<uint32>(Entry.Binding.TimeSlices);
				Setup.Time = Time;
				Setup.MeanRGB = Prepared[Index].MeanRGB;
				Setup.MeanSH = Prepared[Index].MeanSH;
				Setup.bDirectionalSH = Prepared[Index].bDirectionalSH;
				Setup.Gamma = Entry.Binding.Gamma;
				Setup.UVSize = static_cast<uint32>(Config.UVSize);
				Setup.UVTSize = static_cast<uint32>(Config.UVTSize);
				Setup.UTSize = static_cast<uint32>(Config.UTSize);
				Setup.VTSize = static_cast<uint32>(Config.VTSize);
				Setup.LayoutTimeSlices = static_cast<uint32>(Config.TimeSlices);
				Setup.bHasEncodingRanges = (Entry.Binding.EncodingRanges.Num() > 0
					&& Entry.Binding.EncodingRangeIndices.Num() == Entry.Binding.AtlasWidth * (Entry.Binding.AtlasHeight / 2)) ? 1u : 0u;

				FRHITexture* SourceRHI = Targets[Index].Resource ? Targets[Index].Resource->GetTextureRHI() : nullptr;
				FRDGTextureRef SourceTexture = SourceRHI
					? RegisterExternalTexture(GraphBuilder, SourceRHI, TEXT("NDGI.Cache.Source"))
					: nullptr;
				FRDGTextureRef FeatureUVRDG = Entry.GPU->FeatureUV.IsValid()
					? RegisterExternalTexture(GraphBuilder, Entry.GPU->FeatureUV, TEXT("NDGI.Cache.FeatureUV"))
					: nullptr;
				FRDGTextureRef FeatureUVTRDG = Entry.GPU->FeatureUVT.IsValid()
					? RegisterExternalTexture(GraphBuilder, Entry.GPU->FeatureUVT, TEXT("NDGI.Cache.FeatureUVT"))
					: nullptr;
				if (!SourceTexture || !FeatureUVRDG || !FeatureUVTRDG)
				{
					continue;
				}

				const FCompactTileLayout Layout = ComputeCompactTileLayout(Config);
				Setup.TotalParams = Layout.TotalParams;
				Setup.MLPOffset = static_cast<uint32>(Layout.MLPOffset);
				Setup.UTOffset = static_cast<uint32>(Layout.UTOffset);
				Setup.VTOffset = static_cast<uint32>(Layout.VTOffset);

				AddDecodeAtlasPass(GraphBuilder, Setup, ParametersBuffer, IndicesBuffer,
					EncodingRangesBuffer, EncodingIndicesBuffer, SourceTexture,
					FeatureUVRDG, FeatureUVTRDG, OutputTexture, TEXT("NDGIDecodeCachedAtlas"));

				FJob& Job = Jobs[Index];
				Job.TargetResource = Targets[Index].Resource;
				Job.Format = Targets[Index].Format;
				Job.ModelIndices.SetNumUninitialized(Entry.GPUTileModelIndices.Num());
				for (int32 Cell = 0; Cell < Entry.GPUTileModelIndices.Num(); ++Cell)
				{
					Job.ModelIndices[Cell] = Entry.GPUTileModelIndices[Cell] >= 0
						? static_cast<uint32>(Entry.GPUTileModelIndices[Cell])
						: 0xFFFFFFFFu;
				}
				GraphBuilder.QueueTextureExtraction(OutputTexture, &Job.Output);
			}

			GraphBuilder.Execute();

			for (int32 Index = 0; Index < Jobs.Num(); ++Index)
			{
				FJob& Job = Jobs[Index];
				FRHITexture* TargetRHI = Job.TargetResource ? Job.TargetResource->GetTextureRHI() : nullptr;
				if (!Job.Output.IsValid() || !TargetRHI || !Job.Output->GetRHI()
					|| Job.Output->GetRHI()->GetFormat() != Job.Format)
				{
					continue;
				}
				const FAtlasDecodeCacheEntry& Entry = Cache->Entries[Index];
				CopyModelTiles(RHICmdList, Job.Output->GetRHI(), TargetRHI,
					static_cast<uint32>(Entry.Binding.AtlasWidth),
					static_cast<uint32>(Entry.Binding.AtlasHeight),
					static_cast<uint32>(Entry.Binding.TileSize),
					Job.ModelIndices);
			}

			if (OnComplete)
			{
				AsyncTask(ENamedThreads::GameThread, [OnComplete = MoveTemp(OnComplete)]()
				{
					OnComplete();
				});
			}
		});

	if (OutTimings)
	{
		OutTimings->CpuMs = (FPlatformTime::Seconds() - StartSeconds) * 1000.0;
		OutTimings->Atlases = Cache->ActiveAtlases;
		OutTimings->Tiles = Cache->Tiles;
	}
	return true;
}

bool NDGI::Runtime::DecodeCachedLevel(
	const TSharedPtr<FLevelDecodeCache>& Cache,
	float Time,
	FDecodeTimings* OutTimings,
	TFunction<void()> OnComplete)
{
	if (!Cache.IsValid())
	{
		return false;
	}
	TArray<UTexture2D*> Targets;
	Targets.Reserve(Cache->Entries.Num());
	for (const FAtlasDecodeCacheEntry& Entry : Cache->Entries)
	{
		Targets.Add(Entry.Binding.TargetTexture);
	}
	return DecodeCachedLevel(Cache, Time, Targets, OutTimings, MoveTemp(OnComplete));
}

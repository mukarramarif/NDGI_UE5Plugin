// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIModelExport.h"

#include "Misc/PackageName.h"
#include "NDGIBC7Compress.h"
#include "NDGIModelAsset.h"
#include "NDGITileTrain.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace NDGI
{
	namespace EditorTools
	{
		bool WriteModelAsset(
			const FString& PackageName,
			const FString& AssetName,
			Reference::FTileReference& Tile,
			const Reference::FTileConfig& Config,
			const TArray<float>& StepMeans,
			const TArray<float>& SHMeans,
			float Gamma,
			FString& OutError,
			int32 OverlapTexels)
		{
			Tile.FinalizeCompression();

			TArray<double> UV;
			TArray<double> UVT;
			Tile.ReconstructFeatureTexels(UV, UVT);
			TArray<double> Flat;
			Tile.ExportFlatParameters(Flat);

			const int32 UVSize = Config.UVSize;
			const int32 UVTSize = Config.UVTSize;
			const int32 TimeSlices = Config.TimeSlices;
			const int32 UVBlocks = (UVSize / 4) * (UVSize / 4);
			const int32 UVTBlocksTotal = (UVTSize / 4) * (UVTSize / 4) * TimeSlices;
			const int32 UTOffset = Train::MLPParameterCount + UVBlocks * 24 + UVTBlocksTotal * 24;
			const int32 VTOffset = UTOffset + Config.UTSize * TimeSlices * 2;

			UPackage* Package = FPackageName::DoesPackageExist(PackageName) ? LoadPackage(nullptr, *PackageName, LOAD_None) : nullptr;
			UNDGIModelAsset* Asset = Package ? FindObject<UNDGIModelAsset>(Package, *AssetName) : nullptr;
			if (!Package)
			{
				Package = CreatePackage(*PackageName);
			}
			if (!Asset)
			{
				Asset = NewObject<UNDGIModelAsset>(Package, *AssetName, RF_Public | RF_Standalone);
			}
			if (!Package || !Asset)
			{
				OutError = TEXT("Could not create the model package.");
				return false;
			}
			Package->SetDirtyFlag(true);

			Asset->TileSize = FIntPoint(UVSize, UVSize);
			Asset->OverlapTexels = FMath::Max(0, OverlapTexels);
			Asset->FeatureUVTSize = FIntPoint(UVTSize, UVTSize);
			Asset->TimeSlices = TimeSlices;
			Asset->UTSize = Config.UTSize;
			Asset->VTSize = Config.VTSize;
			Asset->Gamma = Gamma;
			Asset->StepMeans = StepMeans;
			Asset->SHMeans = SHMeans;

			auto QuantizeFeature = [](const TArray<double>& Source, TArray<uint8>& Dest)
			{
				Dest.SetNumUninitialized(Source.Num());
				for (int32 Index = 0; Index < Source.Num(); ++Index)
				{
					Dest[Index] = static_cast<uint8>(FMath::Clamp(FMath::RoundToInt(Source[Index] * 255.0), 0, 255));
				}
			};

#if WITH_EDITORONLY_DATA
			QuantizeFeature(UV, Asset->FeatureUV);
			QuantizeFeature(UVT, Asset->FeatureUVT);
#endif

			// BC7 is the shipping representation. F_uv is one layer; F_uvt is
			// TimeSlices layers and is compressed slice-by-slice.
			TArray<uint8> UVRGBA8;
			QuantizeFeature(UV, UVRGBA8);
			if (!EditorTools::CompressRGBA8ToBC7(UVRGBA8, UVSize, UVSize, Asset->FeatureUVBC7, OutError))
			{
				return false;
			}
			Asset->FeatureUVBC7BytesPerLayer = Asset->FeatureUVBC7.Num();

			const int32 SliceTexels = UVTSize * UVTSize * 4;
			Asset->FeatureUVTBC7.Reset();
			Asset->FeatureUVTBC7.Reserve(TimeSlices * ((UVTSize / 4) * (UVTSize / 4) * 16));
			for (int32 Slice = 0; Slice < TimeSlices; ++Slice)
			{
				TArray<uint8> SliceRGBA8;
				SliceRGBA8.SetNumUninitialized(SliceTexels);
				for (int32 Index = 0; Index < SliceTexels; ++Index)
				{
					SliceRGBA8[Index] = static_cast<uint8>(FMath::Clamp(
						FMath::RoundToInt(UVT[static_cast<int64>(Slice) * SliceTexels + Index] * 255.0), 0, 255));
				}
				TArray<uint8> SliceBC7;
				if (!EditorTools::CompressRGBA8ToBC7(SliceRGBA8, UVTSize, UVTSize, SliceBC7, OutError))
				{
					return false;
				}
				if (Slice == 0)
				{
					Asset->FeatureUVTBC7BytesPerLayer = SliceBC7.Num();
				}
				Asset->FeatureUVTBC7.Append(SliceBC7);
			}
			Asset->MLPParameters.SetNumUninitialized(Train::MLPParameterCount);
			for (int32 Index = 0; Index < Train::MLPParameterCount; ++Index)
			{
				Asset->MLPParameters[Index] = static_cast<float>(Flat[Index]);
			}
			const int32 UTCount = Config.UTSize * TimeSlices * 2;
			const int32 VTCount = Config.VTSize * TimeSlices * 2;
			Asset->UT.SetNumUninitialized(UTCount);
			Asset->VT.SetNumUninitialized(VTCount);
			for (int32 Index = 0; Index < UTCount; ++Index)
			{
				Asset->UT[Index] = static_cast<float>(Flat[UTOffset + Index]);
			}
			for (int32 Index = 0; Index < VTCount; ++Index)
			{
				Asset->VT[Index] = static_cast<float>(Flat[VTOffset + Index]);
			}

			const FString FileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
			if (!UPackage::SavePackage(Package, Asset, *FileName, SaveArgs))
			{
				OutError = FString::Printf(TEXT("Failed to save %s."), *FileName);
				return false;
			}

			// Verify the serialized asset.
			UPackage* LoadedPackage = LoadPackage(nullptr, *PackageName, LOAD_None);
			UNDGIModelAsset* Loaded = LoadedPackage ? FindObject<UNDGIModelAsset>(LoadedPackage, *AssetName) : nullptr;
			bool bVerified = Loaded
				&& Loaded->TileSize == Asset->TileSize
				&& Loaded->OverlapTexels == Asset->OverlapTexels
				&& Loaded->TimeSlices == Asset->TimeSlices
				&& Loaded->FeatureUVBC7 == Asset->FeatureUVBC7
				&& Loaded->FeatureUVTBC7 == Asset->FeatureUVTBC7
				&& Loaded->FeatureUVBC7BytesPerLayer == Asset->FeatureUVBC7BytesPerLayer
				&& Loaded->FeatureUVTBC7BytesPerLayer == Asset->FeatureUVTBC7BytesPerLayer
				&& Loaded->MLPParameters == Asset->MLPParameters
				&& Loaded->SHMeans == Asset->SHMeans;
#if WITH_EDITORONLY_DATA
			bVerified = bVerified
				&& Loaded->FeatureUV == Asset->FeatureUV
				&& Loaded->FeatureUVT == Asset->FeatureUVT;
#endif
			if (!bVerified)
			{
				OutError = TEXT("Model asset failed to reload.");
				return false;
			}
			return true;
		}
	}
}

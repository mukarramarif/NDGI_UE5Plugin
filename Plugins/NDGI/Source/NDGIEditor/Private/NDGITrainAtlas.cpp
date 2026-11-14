// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "DynamicRHI.h"
#include "Components/SkyLightComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "LightMap.h"
#include "Math/RandomStream.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NDGI.h"
#include "NDGIAtlasDecoder.h"
#include "NDGIBakeManifest.h"
#include "NDGILevelAsset.h"
#include "NDGIPipelineStatus.h"
#include "NDGILightmapSet.h"
#include "NDGIModelAsset.h"
#include "NDGIModelExport.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"
#include "RenderingThread.h"
#include "RHICommandList.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "TextureResource.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectIterator.h"

namespace
{
	using namespace NDGI;
	namespace BakeManifest = NDGI::Editor::BakeManifest;

	struct FLightmapAtlasInfo
	{
		UTexture2D* Texture = nullptr;
		FVector4f Scale0 = FVector4f(1, 1, 1, 1);
		FVector4f Add0 = FVector4f(0, 0, 0, 0);
		FVector4f Scale1 = FVector4f(1, 1, 1, 1);
		FVector4f Add1 = FVector4f(0, 0, 0, 0);
		FIntPoint Size = FIntPoint(0, 0);
	};

	/** Resolves the HQ lightmap atlas by the same sorted order used by ndgi.BakeTOD's export. */
	bool ResolveLightmapAtlas(UWorld* World, int32 TexIndex, FLightmapAtlasInfo& OutInfo)
	{
		if (!World)
		{
			return false;
		}

		TMap<UTexture2D*, FLightmapAtlasInfo> Unique;
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World || Component->Mobility != EComponentMobility::Static || Component->LODData.Num() == 0)
			{
				continue;
			}
			const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
			if (!BuildData || !BuildData->LightMap)
			{
				continue;
			}
			const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
			if (!LightMap2D || !LightMap2D->IsValid(0))
			{
				continue;
			}
			UTexture2D* Texture = const_cast<UTexture2D*>(LightMap2D->GetTexture(0));
			if (!Texture || Unique.Contains(Texture))
			{
				continue;
			}

			FLightmapAtlasInfo Info;
			Info.Texture = Texture;
			Info.Scale0 = LightMap2D->ScaleVectors[0];
			Info.Add0 = LightMap2D->AddVectors[0];
			Info.Scale1 = LightMap2D->ScaleVectors[1];
			Info.Add1 = LightMap2D->AddVectors[1];
			if (Texture->GetPlatformData() && Texture->GetPlatformData()->Mips.Num() > 0)
			{
				Info.Size = FIntPoint(Texture->GetPlatformData()->Mips[0].SizeX, Texture->GetPlatformData()->Mips[0].SizeY);
			}
			Unique.Add(Texture, Info);
		}

		TArray<UTexture2D*> Keys;
		Unique.GetKeys(Keys);
		Keys.Sort([](const UTexture2D& A, const UTexture2D& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		if (!Keys.IsValidIndex(TexIndex))
		{
			return false;
		}
		OutInfo = Unique.FindChecked(Keys[TexIndex]);
		return true;
	}

	/**
	 * Approximate world bounds for every tile of an atlas. A component's world
	 * bounds are assigned to every tile overlapped by its lightmap-UV bounding
	 * rectangle. This is intentionally conservative: it is only used to rank
	 * model pages for the runtime virtual-texture cache, never for shading.
	 */
	void BuildTileBounds(
		UWorld* World,
		UTexture2D* AtlasTexture,
		int32 Width,
		int32 Height,
		int32 TileSize,
		int32 GridX,
		int32 GridY,
		TArray<FBox>& OutBounds)
	{
		OutBounds.Init(FBox(ForceInitToZero), GridX * GridY);
		if (!World || !AtlasTexture || Width <= 0 || Height <= 0 || TileSize <= 0 || GridX <= 0 || GridY <= 0)
		{
			return;
		}

		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World
				|| Component->Mobility != EComponentMobility::Static
				|| Component->LODData.Num() == 0)
			{
				continue;
			}
			const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
			if (!BuildData || !BuildData->LightMap)
			{
				continue;
			}
			const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
			if (!LightMap2D || !LightMap2D->IsValid(0) || LightMap2D->GetTexture(0) != AtlasTexture)
			{
				continue;
			}

			UStaticMesh* Mesh = Component->GetStaticMesh();
			const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
			if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
			{
				continue;
			}
			const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
			const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
			if (VertexBuffer.GetNumTexCoords() < 2 || VertexBuffer.GetNumVertices() == 0)
			{
				continue;
			}

			const FVector2D CoordScale = LightMap2D->GetCoordinateScale();
			const FVector2D CoordBias = LightMap2D->GetCoordinateBias();
			const FVector2D Size(static_cast<double>(Width), static_cast<double>(Height));
			FVector2D MinUV(MAX_dbl, MAX_dbl);
			FVector2D MaxUV(-MAX_dbl, -MAX_dbl);
			for (uint32 VertexIndex = 0; VertexIndex < VertexBuffer.GetNumVertices(); ++VertexIndex)
			{
				const FVector2f UV = VertexBuffer.GetVertexUV(VertexIndex, 1);
				const FVector2D AtlasUV(
					(static_cast<double>(UV.X) * CoordScale.X + CoordBias.X) * Size.X,
					(static_cast<double>(UV.Y) * CoordScale.Y + CoordBias.Y) * Size.Y);
				MinUV.X = FMath::Min(MinUV.X, AtlasUV.X);
				MinUV.Y = FMath::Min(MinUV.Y, AtlasUV.Y);
				MaxUV.X = FMath::Max(MaxUV.X, AtlasUV.X);
				MaxUV.Y = FMath::Max(MaxUV.Y, AtlasUV.Y);
			}
			if (!FMath::IsFinite(MinUV.X) || !FMath::IsFinite(MinUV.Y)
				|| !FMath::IsFinite(MaxUV.X) || !FMath::IsFinite(MaxUV.Y))
			{
				continue;
			}

			const FBox ComponentBounds = Component->Bounds.GetBox();
			if (!ComponentBounds.IsValid)
			{
				continue;
			}

			const int32 TileX0 = FMath::Clamp(FMath::FloorToInt(MinUV.X / TileSize), 0, GridX - 1);
			const int32 TileX1 = FMath::Clamp(FMath::FloorToInt(MaxUV.X / TileSize), 0, GridX - 1);
			const int32 TileY0 = FMath::Clamp(FMath::FloorToInt(MinUV.Y / TileSize), 0, GridY - 1);
			const int32 TileY1 = FMath::Clamp(FMath::FloorToInt(MaxUV.Y / TileSize), 0, GridY - 1);
			for (int32 TileY = TileY0; TileY <= TileY1; ++TileY)
			{
				for (int32 TileX = TileX0; TileX <= TileX1; ++TileX)
				{
					FBox& TileBox = OutBounds[TileY * GridX + TileX];
					if (TileBox.IsValid)
					{
						TileBox += ComponentBounds;
					}
					else
					{
						TileBox = ComponentBounds;
					}
				}
			}
		}
	}

	struct FAtlasRecord
	{
		FSoftObjectPath TexturePath;
		FIntPoint AtlasSize = FIntPoint(0, 0);
		int32 TileSize = 128;
		int32 OverlapTexels = 0;
		FIntPoint TileGrid = FIntPoint(0, 0);
		FVector4f Scale0 = FVector4f(1, 1, 1, 1);
		FVector4f Add0 = FVector4f(0, 0, 0, 0);
		FVector4f Scale1 = FVector4f(1, 1, 1, 1);
		FVector4f Add1 = FVector4f(0, 0, 0, 0);
		int32 TimeSlices = 0;
		float Gamma = 2.2f;
		TArray<float> StepMeans;
		TArray<float> StepMeansSH;
		TArray<FString> ModelPaths;
		TArray<FNDGIEncodingRange> EncodingRanges;
		TArray<uint8> EncodingRangeIndices;
		TArray<FString> SourceCells;
		TArray<FBox> TileBounds;
	};

	/** One atlas's held-out-time result, written to Saved/NDGI/TemporalHoldout.json. */
	struct FHoldoutAtlasResult
	{
		int32 TexIndex = 0;
		int32 Width = 0;
		int32 Height = 0;
		int32 TimeSlices = 0;
		int32 TrainedSlices = 0;
		int32 HoldoutStep = -1;
		int32 Tiles = 0;
		double OverallPsnr = 0.0;
		double RgbPsnr = 0.0;
		double ShPsnr = 0.0;
		double MaxError[6] = {};
	};

	/** One trained tile's validation scores, written to Saved/NDGI/Training/TileMetrics.json. */
	struct FTileMetric
	{
		int32 TexIndex = 0;
		int32 TileX = 0;
		int32 TileY = 0;
		double OverallPsnr = 0.0;
		double RgbPsnr = 0.0;
		double ShPsnr = 0.0;
	};

	struct FTrainAtlasContext
	{
		/** Merged-set indices to train, in order. */
		TArray<int32> AtlasIndices;
		int32 AtlasCursor = 0;

		int32 TileSize = 128;
		int32 ActiveTileSize = 128;
		int32 OverlapTexels = 8;
		int32 Steps = 500;
		bool bBatch = false;
		bool bQuitWhenDone = false;
		bool bAborted = false;
		int32 FailedAtlases = 0;
		int32 TotalTrainedTiles = 0;

		/**
		 * Held-out-time validation: train without one baked time slice, then
		 * evaluate that slice. Models are not exported and the level asset is
		 * not written; results go to Saved/NDGI/TemporalHoldout.json.
		 */
		bool bHoldout = false;
		/** Diagnostic baseline: train every slice, evaluate the same held-out time. */
		bool bHoldoutBaseline = false;
		int32 HoldoutStep = -1;
		float HoldoutTime = 0.0f;
		int32 MaxTiles = 0;
		int32 HoldoutTilesEvaluated = 0;
		TArray<int32> HoldoutTrainSteps;
		/** Normalized time for each entry in HoldoutTrainSteps (skips HoldoutStep). */
		TArray<float> HoldoutStepTimes;
		TArray<FHoldoutAtlasResult> HoldoutResults;
		double HoldoutOverallPsnrSum = 0.0;
		double HoldoutRgbPsnrSum = 0.0;
		double HoldoutShPsnrSum = 0.0;
		double HoldoutMaxError[6] = {};

		/** Per-atlas accumulators, reset in StartAtlas. */
		int32 HoldoutAtlasTiles = 0;
		double HoldoutAtlasOverallPsnrSum = 0.0;
		double HoldoutAtlasRgbPsnrSum = 0.0;
		double HoldoutAtlasShPsnrSum = 0.0;
		double HoldoutAtlasMaxError[6] = {};

		/** Current atlas state. */
		FLightmapSet Set;
		Reference::FTileConfig RefConfig;
		Train::FTileTrainConfig GPUConfig;
		FLightmapAtlasInfo AtlasInfo;
		bool bHaveAtlas = false;
		int32 TexIndex = 0;
		int32 GridX = 0;
		int32 GridY = 0;
		int32 NextCell = 0;
		int32 TrainedTiles = 0;
		double AtlasPsnrSum = 0.0;
		int32 AtlasPsnrCount = 0;
		bool bCanceled = false;
		TArray<FString> ModelPaths;
		TArray<FNDGIEncodingRange> EncodingRanges;
		TArray<uint8> EncodingRangeIndices;
		TArray<FString> SourceCells;
		TArray<FBox> TileBounds;
		/** Atlas index -> streaming-level packages from the bake manifest. */
		TMap<int32, TArray<FString>> SourceCellsByAtlas;

		/** Completed atlases, written to the level asset at the end. */
		TArray<FAtlasRecord> Records;

		/** Per-tile validation scores from this run, written to TileMetrics.json. */
		TArray<FTileMetric> TileMetrics;

		FString LevelName;
		FString LevelPackageName;
		FString LevelAssetName;
	};

	bool StartAtlas(const TSharedRef<FTrainAtlasContext>& Context, int32 TexIndex);

	void BeginAtlasLoop(const TSharedRef<FTrainAtlasContext>& Context);

	FString EscapeHoldoutJson(const FString& Value)
	{
		return Value.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
	}

	void WriteHoldoutJson(const TSharedRef<FTrainAtlasContext>& Context)
	{
		const double TileCount = static_cast<double>(FMath::Max(1, Context->HoldoutTilesEvaluated));
		const double OverallPsnr = Context->HoldoutOverallPsnrSum / TileCount;
		const double RgbPsnr = Context->HoldoutRgbPsnrSum / TileCount;
		const double ShPsnr = Context->HoldoutShPsnrSum / TileCount;

		FString Json = TEXT("{\n");
		Json += FString::Printf(TEXT("  \"level\": \"%s\",\n"), *EscapeHoldoutJson(Context->LevelName));
		Json += FString::Printf(TEXT("  \"tileSize\": %d,\n"), Context->ActiveTileSize);
		Json += FString::Printf(TEXT("  \"overlap\": %d,\n"), Context->OverlapTexels);
		Json += FString::Printf(TEXT("  \"trainSteps\": %d,\n"), Context->Steps);
		Json += FString::Printf(TEXT("  \"mode\": \"%s\",\n"),
			Context->bHoldoutBaseline ? TEXT("baseline") : TEXT("holdout"));
		Json += FString::Printf(TEXT("  \"tilesEvaluated\": %d,\n"), Context->HoldoutTilesEvaluated);
		Json += FString::Printf(TEXT("  \"psnr\": {\"overall\": %.4f, \"rgb\": %.4f, \"sh\": %.4f},\n"),
			OverallPsnr, RgbPsnr, ShPsnr);
		Json += FString::Printf(
			TEXT("  \"maxAbsError\": {\"rgb\": %.6f, \"sh\": %.6f},\n"),
			FMath::Max3(Context->HoldoutMaxError[0], Context->HoldoutMaxError[1], Context->HoldoutMaxError[2]),
			FMath::Max3(Context->HoldoutMaxError[3], Context->HoldoutMaxError[4], Context->HoldoutMaxError[5]));
		Json += TEXT("  \"atlases\": [\n");
		for (int32 Index = 0; Index < Context->HoldoutResults.Num(); ++Index)
		{
			const FHoldoutAtlasResult& Result = Context->HoldoutResults[Index];
			Json += FString::Printf(
				TEXT("    {\"texIndex\": %d, \"width\": %d, \"height\": %d, \"timeSlices\": %d, \"trainedSlices\": %d, \"holdoutStep\": %d, \"tiles\": %d, ")
				TEXT("\"psnr\": {\"overall\": %.4f, \"rgb\": %.4f, \"sh\": %.4f}, ")
				TEXT("\"maxAbsError\": {\"rgb\": %.6f, \"sh\": %.6f}}%s\n"),
				Result.TexIndex, Result.Width, Result.Height, Result.TimeSlices,
				Result.TrainedSlices, Result.HoldoutStep,
				Result.Tiles, Result.OverallPsnr, Result.RgbPsnr, Result.ShPsnr,
				FMath::Max3(Result.MaxError[0], Result.MaxError[1], Result.MaxError[2]),
				FMath::Max3(Result.MaxError[3], Result.MaxError[4], Result.MaxError[5]),
				Index + 1 < Context->HoldoutResults.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]\n}\n");

		const FString Path = FPaths::ProjectSavedDir()
			/ (Context->bHoldoutBaseline
				? TEXT("NDGI/TemporalHoldoutBaseline.json")
				: TEXT("NDGI/TemporalHoldout.json"));
		if (FFileHelper::SaveStringToFile(Json, *Path))
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateTemporalHoldout: wrote %s."), *Path);
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateTemporalHoldout: could not write %s."), *Path);
		}
	}

	/**
	 * Writes per-tile training PSNR so the pipeline panel can surface the worst
	 * tiles without scraping the log.
	 */
	void WriteTileMetricsJson(const TSharedRef<FTrainAtlasContext>& Context, bool bPassed)
	{
		FString Json = TEXT("{\n");
		Json += FString::Printf(TEXT("  \"level\": \"%s\",\n"), *EscapeHoldoutJson(Context->LevelName));
		Json += FString::Printf(TEXT("  \"tileSize\": %d,\n"), Context->TileSize);
		Json += FString::Printf(TEXT("  \"overlap\": %d,\n"), Context->OverlapTexels);
		Json += FString::Printf(TEXT("  \"trainSteps\": %d,\n"), Context->Steps);
		Json += FString::Printf(TEXT("  \"trainedTiles\": %d,\n"), Context->TileMetrics.Num());
		Json += FString::Printf(TEXT("  \"passed\": %s,\n"), bPassed ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("  \"canceled\": %s,\n"), Context->bCanceled ? TEXT("true") : TEXT("false"));
		Json += FString::Printf(TEXT("  \"generatedAtUtc\": \"%s\",\n"), *FDateTime::UtcNow().ToIso8601());
		Json += TEXT("  \"tiles\": [\n");
		for (int32 Index = 0; Index < Context->TileMetrics.Num(); ++Index)
		{
			const FTileMetric& Metric = Context->TileMetrics[Index];
			Json += FString::Printf(
				TEXT("    {\"texIndex\": %d, \"tileX\": %d, \"tileY\": %d, \"psnr\": %.4f, \"rgb\": %.4f, \"sh\": %.4f}%s\n"),
				Metric.TexIndex, Metric.TileX, Metric.TileY,
				Metric.OverallPsnr, Metric.RgbPsnr, Metric.ShPsnr,
				Index + 1 < Context->TileMetrics.Num() ? TEXT(",") : TEXT(""));
		}
		Json += TEXT("  ]\n}\n");

		const FString Directory = FPaths::ProjectSavedDir() / TEXT("NDGI/Training");
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString Path = Directory / TEXT("TileMetrics.json");
		if (FFileHelper::SaveStringToFile(Json, *Path))
		{
			UE_LOG(LogNDGI, Display, TEXT("TrainAtlas: wrote per-tile metrics for %d tile(s) to %s."),
				Context->TileMetrics.Num(), *Path);
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainAtlas: could not write %s."), *Path);
		}
	}

	void FinishAll(const TSharedRef<FTrainAtlasContext>& Context, bool bSuccess)
	{
		const TCHAR* Label = Context->bHoldout
			? TEXT("ValidateTemporalHoldout")
			: (Context->bBatch ? TEXT("TrainAllAtlases") : TEXT("TrainAtlas"));
		const bool bPassed = !Context->bAborted
			&& Context->FailedAtlases < Context->AtlasIndices.Num()
			&& (!Context->bHoldout || Context->HoldoutTilesEvaluated > 0);

		const bool bHasManifestData = Context->Records.ContainsByPredicate([](const FAtlasRecord& Record)
		{
			return Record.TexturePath.IsValid();
		});

		if (!Context->bHoldout && bHasManifestData)
		{
			UPackage* Package = FPackageName::DoesPackageExist(Context->LevelPackageName) ? LoadPackage(nullptr, *Context->LevelPackageName, LOAD_None) : nullptr;
			UNDGILevelAsset* LevelAsset = Package ? FindObject<UNDGILevelAsset>(Package, *Context->LevelAssetName) : nullptr;
			if (!Package)
			{
				Package = CreatePackage(*Context->LevelPackageName);
			}
			if (!LevelAsset && Package)
			{
				LevelAsset = NewObject<UNDGILevelAsset>(Package, *Context->LevelAssetName, RF_Public | RF_Standalone);
			}

			if (LevelAsset)
			{
				// A batch (or a run starting at atlas 0) starts a fresh manifest so re-bakes
				// that rename built-data textures do not leave stale entries behind.
				const bool bResetManifest = Context->bBatch
					|| (Context->AtlasIndices.Num() > 0 && Context->AtlasIndices[0] == 0);
				if (bResetManifest)
				{
					LevelAsset->Atlases.Reset();
				}

				for (const FAtlasRecord& Record : Context->Records)
				{
					if (!Record.TexturePath.IsValid())
					{
						continue;
					}
					FNDGIAtlasEntry* Entry = nullptr;
					for (FNDGIAtlasEntry& Existing : LevelAsset->Atlases)
					{
						if (Existing.LightmapTexturePath == Record.TexturePath)
						{
							Entry = &Existing;
							break;
						}
					}
					if (!Entry)
					{
						Entry = &LevelAsset->Atlases.AddDefaulted_GetRef();
					}

					Entry->LightmapTexturePath = Record.TexturePath;
					Entry->AtlasSize = Record.AtlasSize;
					Entry->TileSize = Record.TileSize;
					Entry->OverlapTexels = Record.OverlapTexels;
					Entry->TileGrid = Record.TileGrid;
					Entry->Scale0 = Record.Scale0;
					Entry->Add0 = Record.Add0;
					Entry->Scale1 = Record.Scale1;
					Entry->Add1 = Record.Add1;
					Entry->bHighQuality = true;
					Entry->TimeSlices = Record.TimeSlices;
					Entry->Gamma = Record.Gamma;
					Entry->StepMeans = Record.StepMeans;
					Entry->StepMeansSH = Record.StepMeansSH;
					Entry->EncodingRanges = Record.EncodingRanges;
					Entry->EncodingRangeIndices = Record.EncodingRangeIndices;
					Entry->TileBounds = Record.TileBounds;
					if (Record.SourceCells.Num() > 0)
					{
						Entry->SourceCells = Record.SourceCells;
					}
					Entry->TileModelPaths.SetNum(Record.TileGrid.X * Record.TileGrid.Y);
					for (int32 Cell = 0; Cell < Record.ModelPaths.Num(); ++Cell)
					{
						if (!Record.ModelPaths[Cell].IsEmpty())
						{
							Entry->TileModelPaths[Cell] = LoadObject<UNDGIModelAsset>(nullptr, *Record.ModelPaths[Cell]);
						}
					}
#if WITH_EDITORONLY_DATA
					Entry->TileModels.Reset();
#endif
				}

				Package->SetDirtyFlag(true);
				const FString FileName = FPackageName::LongPackageNameToFilename(Context->LevelPackageName, FPackageName::GetAssetPackageExtension());
				FSavePackageArgs SaveArgs;
				SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
				if (UPackage::SavePackage(Package, LevelAsset, *FileName, SaveArgs))
				{
					UE_LOG(LogNDGI, Display,
						TEXT("%s: saved level asset %s.%s (%d atlases). Place an ANDGIActor, assign it, and drag the TimeOfDay slider."),
						Label, *Context->LevelPackageName, *Context->LevelAssetName, LevelAsset->Atlases.Num());
				}
				else
				{
					UE_LOG(LogNDGI, Error, TEXT("%s: failed to save %s."), Label, *FileName);
				}
			}
		}
		else if (!Context->bHoldout && !Context->bAborted && Context->Records.Num() > 0)
		{
			UE_LOG(LogNDGI, Warning,
				TEXT("%s: models exported, but no matching uncompressed HQ lightmap atlas was found in the current world; the level asset was not created. Open the baked level and re-run."),
				Label);
		}

		if (Context->bHoldout)
		{
			WriteHoldoutJson(Context);
			const double TileCount = static_cast<double>(FMath::Max(1, Context->HoldoutTilesEvaluated));
			UE_LOG(LogNDGI, Display,
				TEXT("ValidateTemporalHoldout: %s, %d tile(s) across %d atlas(es), held-out slice PSNR %.2f dB (RGB %.2f dB, SH %.2f dB), max abs RGB %.4f / SH %.4f."),
				Context->bHoldoutBaseline ? TEXT("baseline (all slices)") : TEXT("holdout"),
				Context->HoldoutTilesEvaluated, Context->HoldoutResults.Num(),
				Context->HoldoutOverallPsnrSum / TileCount,
				Context->HoldoutRgbPsnrSum / TileCount,
				Context->HoldoutShPsnrSum / TileCount,
				FMath::Max3(Context->HoldoutMaxError[0], Context->HoldoutMaxError[1], Context->HoldoutMaxError[2]),
				FMath::Max3(Context->HoldoutMaxError[3], Context->HoldoutMaxError[4], Context->HoldoutMaxError[5]));
		}
		else if (Context->TileMetrics.Num() > 0)
		{
			WriteTileMetricsJson(Context, bPassed);
		}

		UE_LOG(LogNDGI, Display,
			TEXT("%s: %s (%d tiles trained across %d atlas(es), %d failed)."),
			Label, bPassed ? TEXT("PASSED") : TEXT("FAILED"),
			Context->bHoldout ? Context->HoldoutTilesEvaluated : Context->TotalTrainedTiles,
			Context->bHoldout ? Context->HoldoutResults.Num() : Context->Records.Num(),
			Context->FailedAtlases);

		NDGI::Editor::Pipeline::Complete(bPassed,
			Context->bCanceled
				? FString::Printf(TEXT("Training canceled after %d tile(s)."), Context->TotalTrainedTiles)
				: FString::Printf(TEXT("%d tile(s) trained across %d atlas(es)%s."),
					Context->TotalTrainedTiles, Context->Records.Num(),
					bPassed ? TEXT("") : TEXT(" — see the log for errors")));

		if (Context->bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void CompleteAtlas(const TSharedRef<FTrainAtlasContext>& Context);
	void TrainNextTile(const TSharedRef<FTrainAtlasContext>& Context);

	void BeginAtlasLoop(const TSharedRef<FTrainAtlasContext>& Context)
	{
		while (Context->AtlasCursor < Context->AtlasIndices.Num())
		{
			if (Context->bAborted)
			{
				FinishAll(Context, false);
				return;
			}
			if (NDGI::Editor::Pipeline::IsCancelRequested())
			{
				Context->bCanceled = true;
				UE_LOG(LogNDGI, Warning, TEXT("%s: canceled before atlas %d/%d."),
					Context->bHoldout ? TEXT("ValidateTemporalHoldout") : TEXT("ndgi.TrainAllAtlases"),
					Context->AtlasCursor + 1, Context->AtlasIndices.Num());
				FinishAll(Context, false);
				return;
			}
			if (StartAtlas(Context, Context->AtlasIndices[Context->AtlasCursor]))
			{
				TrainNextTile(Context);
				return;
			}
			++Context->FailedAtlases;
			++Context->AtlasCursor;
		}
		FinishAll(Context, true);
	}

	void CompleteAtlas(const TSharedRef<FTrainAtlasContext>& Context)
	{
		if (Context->bHoldout)
		{
			FHoldoutAtlasResult Result;
			Result.TexIndex = Context->TexIndex;
			Result.Width = Context->Set.Width;
			Result.Height = Context->Set.Height;
			Result.TimeSlices = Context->Set.Steps.Num();
			Result.TrainedSlices = Context->RefConfig.TimeSlices;
			Result.HoldoutStep = Context->HoldoutStep;
			Result.Tiles = Context->HoldoutAtlasTiles;
			const double TileCount = static_cast<double>(FMath::Max(1, Context->HoldoutAtlasTiles));
			Result.OverallPsnr = Context->HoldoutAtlasOverallPsnrSum / TileCount;
			Result.RgbPsnr = Context->HoldoutAtlasRgbPsnrSum / TileCount;
			Result.ShPsnr = Context->HoldoutAtlasShPsnrSum / TileCount;
			for (int32 Channel = 0; Channel < 6; ++Channel)
			{
				Result.MaxError[Channel] = Context->HoldoutAtlasMaxError[Channel];
			}
			Context->HoldoutResults.Add(Result);

			if (Context->HoldoutAtlasTiles > 0)
			{
				UE_LOG(LogNDGI, Display,
					TEXT("ValidateTemporalHoldout: Tex%02d held-out slice %d/%d (model %d slices): %d tile(s), PSNR %.2f dB (RGB %.2f dB, SH %.2f dB)."),
					Context->TexIndex, Context->HoldoutStep, Context->Set.Steps.Num() - 1,
					Context->RefConfig.TimeSlices, Context->HoldoutAtlasTiles,
					Result.OverallPsnr, Result.RgbPsnr, Result.ShPsnr);
			}
			else
			{
				UE_LOG(LogNDGI, Warning,
					TEXT("ValidateTemporalHoldout: Tex%02d had no trainable tiles."), Context->TexIndex);
			}

			if (Context->MaxTiles > 0 && Context->HoldoutTilesEvaluated >= Context->MaxTiles)
			{
				Context->AtlasCursor = Context->AtlasIndices.Num();
			}
			++Context->AtlasCursor;
			BeginAtlasLoop(Context);
			return;
		}

		FAtlasRecord Record;
		Record.TileSize = Context->ActiveTileSize;
		Record.OverlapTexels = Context->OverlapTexels;
		Record.TileGrid = FIntPoint(Context->GridX, Context->GridY);
		Record.TimeSlices = Context->Set.Steps.Num();
		Record.Gamma = Context->Set.Gamma;
		Record.StepMeans = Context->Set.StepMeans;
		Record.StepMeansSH = Context->Set.StepMeansSH;
		Record.ModelPaths = Context->ModelPaths;
		Record.EncodingRanges = Context->EncodingRanges;
		Record.EncodingRangeIndices = Context->EncodingRangeIndices;
		Record.SourceCells = Context->SourceCells;
		Record.TileBounds = Context->TileBounds;
		if (Context->bHaveAtlas)
		{
			Record.TexturePath = FSoftObjectPath(Context->AtlasInfo.Texture);
			Record.AtlasSize = Context->AtlasInfo.Size;
			Record.Scale0 = Context->AtlasInfo.Scale0;
			Record.Add0 = Context->AtlasInfo.Add0;
			Record.Scale1 = Context->AtlasInfo.Scale1;
			Record.Add1 = Context->AtlasInfo.Add1;
		}
		Context->Records.Add(MoveTemp(Record));
		Context->TotalTrainedTiles += Context->TrainedTiles;
		const int32 AtlasCells = Context->GridX * Context->GridY;
		NDGI::Editor::Pipeline::SetProgress(AtlasCells, AtlasCells,
			FString::Printf(TEXT("Completed atlas %d/%d"), Context->AtlasCursor + 1, Context->AtlasIndices.Num()));

		const double AveragePsnr = Context->AtlasPsnrCount > 0
			? Context->AtlasPsnrSum / static_cast<double>(Context->AtlasPsnrCount)
			: 0.0;
		UE_LOG(LogNDGI, Display,
			TEXT("TrainAtlas: atlas Tex%02d complete: %d tile(s), average PSNR %.2f dB%s."),
			Context->TexIndex, Context->TrainedTiles, AveragePsnr,
			Context->bHaveAtlas ? TEXT("") : TEXT(" (no manifest entry: compressed/high-res mismatch)"));

		++Context->AtlasCursor;
		BeginAtlasLoop(Context);
	}

	bool StartAtlas(const TSharedRef<FTrainAtlasContext>& Context, int32 TexIndex)
	{
		const FString SetPath = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake") / FString::Printf(TEXT("Merged_Tex%02d.ndgi"), TexIndex);
		FLightmapSet Set;
		if (!Set.Load(SetPath))
		{
			UE_LOG(LogNDGI, Error, TEXT("TrainAtlas: merged set %s not found - run ndgi.BakeTOD then ndgi.MergeBakedSets."), *SetPath);
			return false;
		}
		Set.Preprocess(2.2f);

		if (Context->bHoldout)
		{
			const int32 OriginalSlices = Set.Steps.Num();
			if (OriginalSlices < 3)
			{
				UE_LOG(LogNDGI, Error,
					TEXT("ValidateTemporalHoldout: Tex%02d has %d time slice(s); at least 3 are required."),
					TexIndex, OriginalSlices);
				Context->bAborted = true;
				return false;
			}

			if (Context->HoldoutStep < 0)
			{
				Context->HoldoutStep = (OriginalSlices - 1) / 2;
			}
			if (!Set.Steps.IsValidIndex(Context->HoldoutStep))
			{
				UE_LOG(LogNDGI, Error,
					TEXT("ValidateTemporalHoldout: held-out slice %d is outside Tex%02d's 0..%d range."),
					Context->HoldoutStep, TexIndex, OriginalSlices - 1);
				Context->bAborted = true;
				return false;
			}

			// Train a model with one fewer temporal feature slice. The retained
			// slices keep their original normalized times, so the held-out time
			// falls between two trained slices instead of on a dead F_uvt slice.
			const float OriginalDenominator = static_cast<float>(OriginalSlices - 1);
			Context->HoldoutTime = static_cast<float>(Context->HoldoutStep) / OriginalDenominator;
			Context->HoldoutTrainSteps.Reset();
			Context->HoldoutStepTimes.Reset();
			Context->HoldoutTrainSteps.Reserve(OriginalSlices);
			Context->HoldoutStepTimes.Reserve(OriginalSlices);
			for (int32 StepIndex = 0; StepIndex < OriginalSlices; ++StepIndex)
			{
				if (Context->bHoldoutBaseline || StepIndex != Context->HoldoutStep)
				{
					Context->HoldoutTrainSteps.Add(StepIndex);
					Context->HoldoutStepTimes.Add(static_cast<float>(StepIndex) / OriginalDenominator);
				}
			}

			UE_LOG(LogNDGI, Display,
				TEXT("ValidateTemporalHoldout: %s Tex%02d will train %d temporal feature slices on %d baked steps and evaluate step %d at t=%.4f."),
				Context->bHoldoutBaseline ? TEXT("baseline") : TEXT("holdout"),
				TexIndex, Context->HoldoutTrainSteps.Num(), OriginalSlices,
				Context->HoldoutStep, Context->HoldoutTime);
		}

		Context->HoldoutAtlasTiles = 0;
		Context->HoldoutAtlasOverallPsnrSum = 0.0;
		Context->HoldoutAtlasRgbPsnrSum = 0.0;
		Context->HoldoutAtlasShPsnrSum = 0.0;
		for (double& Error : Context->HoldoutAtlasMaxError)
		{
			Error = 0.0;
		}

		int64 ValidMaskTexels = 0;
		if (Set.Steps.Num() > 0)
		{
			for (uint8 MaskValue : Set.Steps[0].Mask)
			{
				ValidMaskTexels += MaskValue ? 1 : 0;
			}
		}
		UE_LOG(LogNDGI, Display, TEXT("TrainAtlas: mask %lld/%d texels valid."),
			ValidMaskTexels, Set.Width * Set.Height);

		// Small lightmap atlases use a smaller tile size so the grid still divides evenly.
		int32 EffectiveTileSize = Context->TileSize;
		while (EffectiveTileSize > 16 && (Set.Width % EffectiveTileSize != 0 || Set.Height % EffectiveTileSize != 0))
		{
			EffectiveTileSize /= 2;
		}

		const int32 TileSize = EffectiveTileSize;
		const int32 NewGridX = Set.Width / TileSize;
		const int32 NewGridY = Set.Height / TileSize;
		if (NewGridX <= 0 || NewGridY <= 0
			|| NewGridX * TileSize != Set.Width
			|| NewGridY * TileSize != Set.Height)
		{
			UE_LOG(LogNDGI, Error,
				TEXT("TrainAtlas: tile size %d does not divide Tex%02d (%dx%d)."),
				TileSize, TexIndex, Set.Width, Set.Height);
			return false;
		}

		Context->TexIndex = TexIndex;
		Context->SourceCells = Context->SourceCellsByAtlas.FindRef(TexIndex);
		Context->Set = MoveTemp(Set);
		Context->ActiveTileSize = TileSize;
		Context->GridX = NewGridX;
		Context->GridY = NewGridY;
		Context->NextCell = 0;
		Context->TrainedTiles = 0;
		Context->AtlasPsnrSum = 0.0;
		Context->AtlasPsnrCount = 0;
		Context->ModelPaths.Reset();
		Context->ModelPaths.SetNum(Context->GridX * Context->GridY);
		Context->TileBounds.Reset();

		// Overlapping models cover the tile plus a halo on every side. The halo
		// gives adjacent models shared context and provides the cross-fade band
		// used by the runtime decoder.
		const int32 Extent = TileSize + 2 * Context->OverlapTexels;
		Context->RefConfig = Reference::FTileConfig();
		Context->RefConfig.UVSize = Extent;
		Context->RefConfig.UVTSize = Extent / 4;
		Context->RefConfig.UTSize = Extent / 2;
		Context->RefConfig.VTSize = Extent / 2;
		Context->RefConfig.TimeSlices = (Context->bHoldout && !Context->bHoldoutBaseline)
			? Context->Set.Steps.Num() - 1
			: Context->Set.Steps.Num();
		Context->RefConfig.bSimulateBC = true;

		Context->GPUConfig = Train::FTileTrainConfig();
		Context->GPUConfig.UVSize = Context->RefConfig.UVSize;
		Context->GPUConfig.UVTSize = Context->RefConfig.UVTSize;
		Context->GPUConfig.UTSize = Context->RefConfig.UTSize;
		Context->GPUConfig.VTSize = Context->RefConfig.VTSize;
		Context->GPUConfig.TimeSlices = Context->RefConfig.TimeSlices;
		Context->GPUConfig.NumTiles = 1;
		Context->GPUConfig.BatchSize = 64;

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		Context->bHaveAtlas = ResolveLightmapAtlas(World, TexIndex, Context->AtlasInfo)
			&& Context->AtlasInfo.Texture != nullptr;
		if (Context->bHaveAtlas)
		{
			const bool bFormatOk = Context->AtlasInfo.Texture->GetPlatformData()
				&& (Context->AtlasInfo.Texture->GetPlatformData()->PixelFormat == PF_FloatRGBA
					|| Context->AtlasInfo.Texture->GetPlatformData()->PixelFormat == PF_B8G8R8A8)
				&& Context->AtlasInfo.Size == FIntPoint(Context->Set.Width, Context->Set.Height * 2);
			if (!bFormatOk)
			{
				UE_LOG(LogNDGI, Warning,
					TEXT("TrainAtlas: atlas '%s' is not an uncompressed lightmap of the expected size; runtime write-back will not work. Re-bake with Compress Lightmaps off."),
					*Context->AtlasInfo.Texture->GetName());
				Context->bHaveAtlas = false;
			}
		}
		if (Context->bHaveAtlas)
		{
			BuildTileBounds(
				World,
				Context->AtlasInfo.Texture,
				Context->Set.Width,
				Context->Set.Height,
				TileSize,
				Context->GridX,
				Context->GridY,
				Context->TileBounds);
			int32 ValidBounds = 0;
			for (const FBox& Box : Context->TileBounds)
			{
				ValidBounds += Box.IsValid ? 1 : 0;
			}
			UE_LOG(LogNDGI, Display, TEXT("TrainAtlas: %d/%d tile(s) have world bounds for VT page streaming."),
				ValidBounds, Context->TileBounds.Num());
		}

		Context->EncodingRanges.Reset();
		Context->EncodingRangeIndices.Reset();
		const int32 AtlasTexelCount = Context->Set.Width * Context->Set.Height;
		if (Context->Set.EncodingRanges.Num() > 0 && Context->Set.EncodingRangeIndices.Num() == AtlasTexelCount)
		{
			// Use the exact map the bake exported with; recomputing it could pick a
			// different range order and silently mismatch the SH/RGB targets.
			Context->EncodingRanges = Context->Set.EncodingRanges;
			Context->EncodingRangeIndices = Context->Set.EncodingRangeIndices;
			UE_LOG(LogNDGI, Display,
				TEXT("TrainAtlas: using %d encoding range(s) from the bake export."),
				Context->EncodingRanges.Num());
		}
		else
		{
			// A pre-v3 merged set was decoded with one atlas-wide range. Recomputing
			// a per-texel map here would make the training targets inconsistent with
			// the runtime encode, so keep the legacy path and ask for a re-bake.
			UE_LOG(LogNDGI, Warning,
				TEXT("TrainAtlas: merged set has no encoding range map (%s). Re-run ndgi.BakeTOD with this version for per-component colors/SH; training with the atlas-wide fallback."),
				*SetPath);
		}

		NDGI::Editor::Pipeline::SetProgress(0, Context->GridX * Context->GridY,
			FString::Printf(TEXT("Atlas %d/%d — %dx%d, tile %d"), Context->AtlasCursor + 1,
				Context->AtlasIndices.Num(), Context->Set.Width, Context->Set.Height, TileSize));

		UE_LOG(LogNDGI, Display,
			TEXT("TrainAtlas: [%d/%d] %s, atlas %dx%d, tile %d (+%d halo = %d), grid %dx%d, %d steps/tile, %d time slices, %s."),
			Context->AtlasCursor + 1, Context->AtlasIndices.Num(), *SetPath,
			Context->Set.Width, Context->Set.Height, TileSize, Context->OverlapTexels, Extent,
			Context->GridX, Context->GridY, Context->Steps,
			Context->Set.Steps.Num(),
			Context->bHaveAtlas ? TEXT("level asset entry will be built") : TEXT("no uncompressed atlas found (models only)"));
		return true;
	}

	void TrainNextTile(const TSharedRef<FTrainAtlasContext>& Context)
	{
		const int32 NumCells = Context->GridX * Context->GridY;
		while (Context->NextCell < NumCells)
		{
			if (NDGI::Editor::Pipeline::IsCancelRequested())
			{
				Context->bCanceled = true;
				Context->bAborted = true;
				UE_LOG(LogNDGI, Warning, TEXT("%s: canceled at tile %d/%d of Tex%02d."),
					Context->bHoldout ? TEXT("ValidateTemporalHoldout") : TEXT("ndgi.TrainAllAtlases"),
					Context->NextCell, NumCells, Context->TexIndex);
				FinishAll(Context, false);
				return;
			}
			if (Context->bHoldout && Context->MaxTiles > 0
				&& Context->HoldoutTilesEvaluated >= Context->MaxTiles)
			{
				break;
			}

			const int32 Cell = Context->NextCell;
			const int32 TileX = Cell % Context->GridX;
			const int32 TileY = Cell / Context->GridX;
			const int32 TileSeed = 1337 + Cell * 7919 + Context->TexIndex * 104729;

			NDGI::Editor::Pipeline::SetProgress(Cell, NumCells,
				FString::Printf(TEXT("Atlas %d/%d · tile (%d,%d) of %dx%d"),
					Context->AtlasCursor + 1, Context->AtlasIndices.Num(), TileX, TileY,
					Context->GridX, Context->GridY));

			const int32 TileSize = Context->ActiveTileSize;
			const int32 Overlap = Context->OverlapTexels;
			const int32 Extent = TileSize + 2 * Overlap;
			const int32 ModelX = TileX * TileSize - Overlap;
			const int32 ModelY = TileY * TileSize - Overlap;

			TArray<int32> EmptySteps;
			TArray<float> EmptyTimes;
			const TArray<int32>& TrainStepFilter = Context->bHoldout
				? Context->HoldoutTrainSteps
				: EmptySteps;
			const TArray<float>& TrainStepTimes = Context->bHoldout
				? Context->HoldoutStepTimes
				: EmptyTimes;
			auto GenerateRegion = [&](int32 RegionX, int32 RegionY, int32 SizeX, int32 SizeY,
				int32 Count, int32 SeedOffset, TArray<FVector>& OutCoords, TArray<float>& OutTargets)
			{
				Context->Set.GenerateRegionSamples(
					RegionX, RegionY, SizeX, SizeY,
					ModelX, ModelY, Extent,
					Count, TileSeed ^ SeedOffset, OutCoords, OutTargets, TrainStepFilter, TrainStepTimes);
			};

			// Full model extent (core + halo), plus extra samples concentrated on
			// the four core-edge/halo bands. The mix is interleaved per Adam step
			// below so every step sees both distributions.
			TArray<FVector> FullCoords;
			TArray<float> FullTargets;
			GenerateRegion(ModelX, ModelY, Extent, Extent,
				Context->Steps * 64, 0, FullCoords, FullTargets);
			if (FullCoords.Num() == 0)
			{
				UE_LOG(LogNDGI, Display,
					TEXT("TrainAtlas: tile (%d,%d) contains only atlas padding, skipped."), TileX, TileY);
				++Context->NextCell;
				continue;
			}

			TArray<FVector> EdgeCoords;
			TArray<float> EdgeTargets;
			if (Overlap > 0)
			{
				const int32 Band = FMath::Max(1, Overlap);
				const int32 BandThickness = Overlap + Band; // halo + core edge
				const int32 CoreX = TileX * TileSize;
				const int32 CoreY = TileY * TileSize;
				const int32 EdgeCountPerBand = Context->Steps * 8;

				GenerateRegion(ModelX, ModelY, Extent, BandThickness, EdgeCountPerBand, 0x2222, EdgeCoords, EdgeTargets);
				TArray<FVector> BandCoords;
				TArray<float> BandTargets;
				GenerateRegion(ModelX, CoreY + TileSize - Band, Extent, BandThickness, EdgeCountPerBand, 0x3333, BandCoords, BandTargets);
				EdgeCoords.Append(BandCoords);
				EdgeTargets.Append(BandTargets);
				GenerateRegion(ModelX, ModelY, BandThickness, Extent, EdgeCountPerBand, 0x4444, BandCoords, BandTargets);
				EdgeCoords.Append(BandCoords);
				EdgeTargets.Append(BandTargets);
				GenerateRegion(CoreX + TileSize - Band, ModelY, BandThickness, Extent, EdgeCountPerBand, 0x5555, BandCoords, BandTargets);
				EdgeCoords.Append(BandCoords);
				EdgeTargets.Append(BandTargets);
			}

			TArray<FVector> TrainCoords;
			TArray<float> TrainTargets;
			if (Overlap == 0)
			{
				TrainCoords = MoveTemp(FullCoords);
				TrainTargets = MoveTemp(FullTargets);
			}
			else
			{
				TrainCoords.Reserve(Context->Steps * 64);
				TrainTargets.Reserve(Context->Steps * 64 * 6);
				const int32 FullMixPerStep = 32;
				const int32 EdgeMixPerStep = 32;
				const int32 FullFillerBase = Context->Steps * 32;
				for (int32 Step = 0; Step < Context->Steps; ++Step)
				{
					for (int32 Sample = 0; Sample < FullMixPerStep; ++Sample)
					{
						const int32 Index = Step * FullMixPerStep + Sample;
						TrainCoords.Add(FullCoords[Index]);
						for (int32 Channel = 0; Channel < 6; ++Channel)
						{
							TrainTargets.Add(FullTargets[Index * 6 + Channel]);
						}
					}
					for (int32 Sample = 0; Sample < EdgeMixPerStep; ++Sample)
					{
						const int32 EdgeIndex = Step * EdgeMixPerStep + Sample;
						if (EdgeIndex < EdgeCoords.Num())
						{
							TrainCoords.Add(EdgeCoords[EdgeIndex]);
							for (int32 Channel = 0; Channel < 6; ++Channel)
							{
								TrainTargets.Add(EdgeTargets[EdgeIndex * 6 + Channel]);
							}
						}
						else
						{
							// Sparse/empty edge bands: fill with the full-region samples.
							const int32 Filler = FMath::Min(FullFillerBase + EdgeIndex, FullCoords.Num() - 1);
							TrainCoords.Add(FullCoords[Filler]);
							for (int32 Channel = 0; Channel < 6; ++Channel)
							{
								TrainTargets.Add(FullTargets[Filler * 6 + Channel]);
							}
						}
					}
				}
			}
			++Context->NextCell;

			if (TrainCoords.Num() < 64)
			{
				UE_LOG(LogNDGI, Display, TEXT("TrainAtlas: tile (%d,%d) is empty, skipped."), TileX, TileY);
				continue;
			}

			// Evaluate on the core only; the halo is training context, not output.
			// Held-out validation evaluates the excluded baked slice at its true
			// normalized time through a model that has one fewer temporal slice.
			TArray<FVector> EvalCoords;
			TArray<float> EvalTargets;
			if (Context->bHoldout)
			{
				Context->Set.GenerateRegionSamplesAtStep(
					TileX * Context->ActiveTileSize, TileY * Context->ActiveTileSize,
					Context->ActiveTileSize, Context->ActiveTileSize,
					ModelX, ModelY, Extent,
					Context->HoldoutStep, Context->HoldoutTime,
					2048, TileSeed ^ 0x5bd1e995, EvalCoords, EvalTargets);
			}
			else
			{
				Context->Set.GenerateRegionSamples(
					TileX * Context->ActiveTileSize, TileY * Context->ActiveTileSize,
					Context->ActiveTileSize, Context->ActiveTileSize,
					ModelX, ModelY, Extent,
					2048, TileSeed ^ 0x5bd1e995, EvalCoords, EvalTargets);
			}

			TArray<FVector> EvalCoordsCopy = EvalCoords;
			TArray<float> EvalTargetsCopy = EvalTargets;

			Reference::FTileReference InitialTile(Context->RefConfig, TileSeed);
			TArray<double> InitialFlat;
			InitialTile.ExportFlatParameters(InitialFlat);
			TArray<float> Parameters;
			Parameters.SetNumUninitialized(InitialFlat.Num());
			for (int32 Index = 0; Index < InitialFlat.Num(); ++Index)
			{
				Parameters[Index] = static_cast<float>(InitialFlat[Index]);
			}

			TArray<float> CoordsFlat;
			CoordsFlat.Reserve(TrainCoords.Num() * 3);
			for (const FVector& Coord : TrainCoords)
			{
				CoordsFlat.Add(static_cast<float>(Coord.X));
				CoordsFlat.Add(static_cast<float>(Coord.Y));
				CoordsFlat.Add(static_cast<float>(Coord.Z));
			}

			TArray<float> Noise;
			Noise.SetNumUninitialized(TrainCoords.Num() * 12);
			FRandomStream NoiseStream(TileSeed ^ 0x9e3779b9);
			const float Alpha = static_cast<float>(Context->RefConfig.NoiseAlpha);
			for (float& Value : Noise)
			{
				Value = (NoiseStream.FRand() - 0.5f) * Alpha;
			}

			const FString PackageName = FString::Printf(TEXT("/Game/NDGI/Trained/%s_Tex%02d_%d_%d"), *Context->LevelName, Context->TexIndex, TileX, TileY);
			const FString AssetName = FString::Printf(TEXT("%s_Tex%02d_%d_%d"), *Context->LevelName, Context->TexIndex, TileX, TileY);

			UE_LOG(LogNDGI, Display, TEXT("TrainAtlas: training tile (%d,%d), %d steps..."), TileX, TileY, Context->Steps);

			Train::RunTrainingSteps(
				Context->GPUConfig, Parameters, CoordsFlat, TrainTargets, Noise, Context->Steps,
				1e-3f, 0.9f, 0.999f, 1e-8f, 0,
				[Context, Cell, TileX, TileY, EvalCoordsCopy, EvalTargetsCopy, PackageName, AssetName](bool bSuccess, TArray<float>&& GpuParameters)
				{
					if (!bSuccess || GpuParameters.Num() == 0)
					{
						UE_LOG(LogNDGI, Error, TEXT("TrainAtlas: GPU training failed for tile (%d,%d) of Tex%02d."), TileX, TileY, Context->TexIndex);
						Context->bAborted = true;
						FinishAll(Context, false);
						return;
					}

					TArray<double> Flat;
					Flat.SetNumUninitialized(GpuParameters.Num());
					for (int32 Index = 0; Index < GpuParameters.Num(); ++Index)
					{
						Flat[Index] = static_cast<double>(GpuParameters[Index]);
					}

					Reference::FTileReference Tile(Context->RefConfig, 0);
					Tile.ImportFlatParameters(Flat);

					TArray<Reference::FTrainSample> EvalSamples;
					EvalSamples.SetNumUninitialized(EvalCoordsCopy.Num());
					for (int32 Index = 0; Index < EvalCoordsCopy.Num(); ++Index)
					{
						Reference::FTrainSample& Sample = EvalSamples[Index];
						Sample.U = EvalCoordsCopy[Index].X;
						Sample.V = EvalCoordsCopy[Index].Y;
						Sample.Time = EvalCoordsCopy[Index].Z;
						for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
						{
							Sample.Target[Channel] = EvalTargetsCopy[Index * Reference::OutputDim + Channel];
						}
					}

					double Mse = 0.0;
					double Psnr = 0.0;
					Tile.Evaluate(EvalSamples, Mse, Psnr);
					double PerOutputPsnr[Reference::OutputDim];
					Tile.EvaluatePerOutput(EvalSamples, PerOutputPsnr);
					const double RgbPsnr = (PerOutputPsnr[0] + PerOutputPsnr[1] + PerOutputPsnr[2]) / 3.0;
					const double SHPsnr = (PerOutputPsnr[3] + PerOutputPsnr[4] + PerOutputPsnr[5]) / 3.0;
					Context->AtlasPsnrSum += Psnr;
					++Context->AtlasPsnrCount;

					if (Context->bHoldout)
					{
						if (EvalCoordsCopy.Num() == 0)
						{
							UE_LOG(LogNDGI, Display,
								TEXT("ValidateTemporalHoldout: Tex%02d tile (%d,%d) has no covered texels, skipped."),
								Context->TexIndex, TileX, TileY);
							TrainNextTile(Context);
							return;
						}

						TArray<float> Outputs;
						Tile.EvaluatePoints(EvalCoordsCopy, Outputs);
						for (int32 Index = 0; Index < EvalCoordsCopy.Num(); ++Index)
						{
							for (int32 Channel = 0; Channel < Reference::OutputDim; ++Channel)
							{
								const double Diff = FMath::Abs(
									static_cast<double>(Outputs[Index * Reference::OutputDim + Channel])
									- static_cast<double>(EvalTargetsCopy[Index * Reference::OutputDim + Channel]));
								Context->HoldoutAtlasMaxError[Channel] =
									FMath::Max(Context->HoldoutAtlasMaxError[Channel], Diff);
								Context->HoldoutMaxError[Channel] =
									FMath::Max(Context->HoldoutMaxError[Channel], Diff);
							}
						}

						++Context->HoldoutAtlasTiles;
						Context->HoldoutAtlasOverallPsnrSum += Psnr;
						Context->HoldoutAtlasRgbPsnrSum += RgbPsnr;
						Context->HoldoutAtlasShPsnrSum += SHPsnr;
						++Context->HoldoutTilesEvaluated;
						Context->HoldoutOverallPsnrSum += Psnr;
						Context->HoldoutRgbPsnrSum += RgbPsnr;
						Context->HoldoutShPsnrSum += SHPsnr;

						UE_LOG(LogNDGI, Display,
							TEXT("ValidateTemporalHoldout: Tex%02d tile (%d,%d), held-out slice %d: PSNR %.2f dB (RGB %.2f dB, SH %.2f dB)."),
							Context->TexIndex, TileX, TileY, Context->HoldoutStep, Psnr, RgbPsnr, SHPsnr);
						TrainNextTile(Context);
						return;
					}

					UE_LOG(LogNDGI, Display,
						TEXT("TrainAtlas: Tex%02d tile (%d,%d) PSNR %.2f dB (RGB %.2f dB, SH %.2f dB)."),
						Context->TexIndex, TileX, TileY, Psnr, RgbPsnr, SHPsnr);

					FString Error;
					if (!EditorTools::WriteModelAsset(PackageName, AssetName, Tile, Context->RefConfig,
						Context->Set.StepMeans, Context->Set.StepMeansSH, Context->Set.Gamma, Error,
						Context->OverlapTexels))
					{
						UE_LOG(LogNDGI, Error, TEXT("TrainAtlas: model export failed for tile (%d,%d) of Tex%02d: %s"), TileX, TileY, Context->TexIndex, *Error);
						Context->bAborted = true;
						FinishAll(Context, false);
						return;
					}

					Context->ModelPaths[Cell] = PackageName + TEXT(".") + AssetName;
					++Context->TrainedTiles;

					FTileMetric Metric;
					Metric.TexIndex = Context->TexIndex;
					Metric.TileX = TileX;
					Metric.TileY = TileY;
					Metric.OverallPsnr = Psnr;
					Metric.RgbPsnr = RgbPsnr;
					Metric.ShPsnr = SHPsnr;
					Context->TileMetrics.Add(Metric);

					TrainNextTile(Context);
				});
			return;
		}

		CompleteAtlas(Context);
	}

	void ConfigureContext(const TSharedRef<FTrainAtlasContext>& Context, int32 TileSize, int32 OverlapTexels, int32 Steps, bool bQuitWhenDone)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		Context->LevelName = World ? World->GetName() : TEXT("Level");
		Context->LevelPackageName = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s"), *Context->LevelName);
		Context->LevelAssetName = FString::Printf(TEXT("NDGI_%s"), *Context->LevelName);
		Context->TileSize = TileSize;
		Context->OverlapTexels = OverlapTexels;
		Context->Steps = Steps;
		Context->bQuitWhenDone = bQuitWhenDone;
	}

	/** Copies per-atlas streaming-cell ownership from the current level's bake manifest. */
	void LoadSourceCellsFromManifest(const TSharedRef<FTrainAtlasContext>& Context)
	{
		BakeManifest::FManifest Manifest;
		if (!BakeManifest::Load(Manifest))
		{
			return;
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString WorldPackage = World ? World->GetOutermost()->GetName() : FString();
		if (!WorldPackage.IsEmpty() && Manifest.LevelPackage != WorldPackage)
		{
			UE_LOG(LogNDGI, Warning,
				TEXT("TrainAtlas: bake manifest belongs to '%s', not '%s'; training without cell ownership metadata."),
				*Manifest.LevelPackage, *WorldPackage);
			return;
		}

		for (const BakeManifest::FAtlasEntry& Atlas : Manifest.Atlases)
		{
			if (Atlas.Cells.Num() > 0)
			{
				Context->SourceCellsByAtlas.Add(Atlas.TexIndex, Atlas.Cells);
			}
		}
	}

	/**
	 * Resolves the merged atlases to process for the current level. Uses the
	 * bake manifest when it matches the world and falls back to a folder scan
	 * for legacy bake folders.
	 */
	bool CollectMergedAtlasIndices(const TCHAR* CommandName, TArray<int32>& OutIndices, FString& OutError)
	{
		OutIndices.Reset();
		OutError.Reset();

		const FString Directory = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *(Directory / TEXT("Merged_Tex*.ndgi")), true, false);

		BakeManifest::FManifest Manifest;
		if (BakeManifest::Load(Manifest))
		{
			UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
			const FString WorldPackage = World ? World->GetOutermost()->GetName() : FString();
			if (!WorldPackage.IsEmpty() && Manifest.LevelPackage != WorldPackage)
			{
				OutError = FString::Printf(
					TEXT("%s: the bake manifest belongs to a different level. Re-bake the current level."),
					CommandName);
				return false;
			}

			TSet<int32> ManifestIndices;
			for (const BakeManifest::FAtlasEntry& Atlas : Manifest.Atlases)
			{
				ManifestIndices.Add(Atlas.TexIndex);
			}
			for (const FString& File : Files)
			{
				const FString BaseName = FPaths::GetBaseFilename(File);
				const int32 AtlasIndex = FCString::Atoi(*BaseName.RightChop(10));
				if (!ManifestIndices.Contains(AtlasIndex))
				{
					continue;
				}

				const FString MergedPath = Directory / File;
				const FDateTime MergedTime = IFileManager::Get().GetTimeStamp(*MergedPath);
				if (Manifest.SavedAtTicks > 0 && MergedTime.GetTicks() < Manifest.SavedAtTicks)
				{
					UE_LOG(LogNDGI, Warning,
						TEXT("%s: %s predates the current bake; run ndgi.MergeAllBakedSets to refresh it."),
						CommandName, *File);
					continue;
				}
				OutIndices.AddUnique(AtlasIndex);
			}
			const int32 MissingMerged = ManifestIndices.Num() - OutIndices.Num();
			if (MissingMerged > 0)
			{
				UE_LOG(LogNDGI, Warning,
					TEXT("%s: %d manifest atlas(es) have no merged file; run ndgi.MergeAllBakedSets first."),
					CommandName, MissingMerged);
			}
			UE_LOG(LogNDGI, Display,
				TEXT("%s: using bake manifest for '%s' (%d atlas(es))."),
				CommandName, *Manifest.LevelPackage, OutIndices.Num());
		}
		else
		{
			for (const FString& File : Files)
			{
				const FString BaseName = FPaths::GetBaseFilename(File);
				OutIndices.AddUnique(FCString::Atoi(*BaseName.RightChop(10)));
			}
			UE_LOG(LogNDGI, Warning,
				TEXT("%s: no bake manifest; scanning all Merged_Tex*.ndgi files. Re-bake for level-scoped training."),
				CommandName);
		}
		OutIndices.Sort();

		if (OutIndices.Num() == 0)
		{
			OutError = FString::Printf(
				TEXT("%s: no merged atlases found - run ndgi.BakeTOD then ndgi.MergeBakedSets."),
				CommandName);
			return false;
		}
		return true;
	}

	void RunTrainAtlas(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		int32 TexIndex = 0;
		int32 TileSize = 128;
		int32 OverlapTexels = 8;
		int32 Steps = 500;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				if (NumericArgIndex == 0) { TexIndex = FMath::Max(0, Value); }
				else if (NumericArgIndex == 1) { TileSize = FMath::Max(16, Value); }
				else if (NumericArgIndex == 2) { Steps = FMath::Max(1, Value); }
				else if (NumericArgIndex == 3) { OverlapTexels = FMath::Max(0, Value); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Training,
			FString::Printf(TEXT("Training atlas %d"), TexIndex), 1, TEXT("Loading merged set..."));

		auto Fail = [bQuitWhenDone](const TCHAR* Message)
		{
			UE_LOG(LogNDGI, Error, TEXT("%s"), Message);
			NDGI::Editor::Pipeline::Fail(Message);
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		if (TileSize % 16 != 0)
		{
			Fail(TEXT("ndgi.TrainAtlas: tile size must be a multiple of 16."));
			return;
		}
		if (OverlapTexels % 8 != 0)
		{
			Fail(TEXT("ndgi.TrainAtlas: overlap must be a multiple of 8 texels."));
			return;
		}

		TSharedRef<FTrainAtlasContext> Context = MakeShared<FTrainAtlasContext>();
		ConfigureContext(Context, TileSize, OverlapTexels, Steps, bQuitWhenDone);
		LoadSourceCellsFromManifest(Context);
		Context->bBatch = false;
		Context->AtlasIndices.Add(TexIndex);
		BeginAtlasLoop(Context);
	}

	void RunTrainAllAtlases(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		int32 TileSize = 128;
		int32 OverlapTexels = 8;
		int32 Steps = 500;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				if (NumericArgIndex == 0) { TileSize = FMath::Max(16, Value); }
				else if (NumericArgIndex == 1) { Steps = FMath::Max(1, Value); }
				else if (NumericArgIndex == 2) { OverlapTexels = FMath::Max(0, Value); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Training,
			TEXT("Training all atlases"), 0, TEXT("Scanning merged sets..."));

		auto Fail = [bQuitWhenDone](const TCHAR* Message)
		{
			UE_LOG(LogNDGI, Error, TEXT("%s"), Message);
			NDGI::Editor::Pipeline::Fail(Message);
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		if (TileSize % 16 != 0)
		{
			Fail(TEXT("ndgi.TrainAllAtlases: tile size must be a multiple of 16."));
			return;
		}
		if (OverlapTexels % 8 != 0)
		{
			Fail(TEXT("ndgi.TrainAllAtlases: overlap must be a multiple of 8 texels."));
			return;
		}

		TArray<int32> AtlasIndices;
		FString CollectError;
		if (!CollectMergedAtlasIndices(TEXT("ndgi.TrainAllAtlases"), AtlasIndices, CollectError))
		{
			Fail(*CollectError);
			return;
		}

		TSharedRef<FTrainAtlasContext> Context = MakeShared<FTrainAtlasContext>();
		ConfigureContext(Context, TileSize, OverlapTexels, Steps, bQuitWhenDone);
		LoadSourceCellsFromManifest(Context);
		Context->bBatch = true;
		Context->AtlasIndices = MoveTemp(AtlasIndices);

		UE_LOG(LogNDGI, Display, TEXT("ndgi.TrainAllAtlases: %d atlas(es), %d steps/tile, tile %d (+%d halo = %d)."),
			Context->AtlasIndices.Num(), Steps, TileSize, OverlapTexels, TileSize + 2 * OverlapTexels);
		NDGI::Editor::Pipeline::SetProgress(0, Context->AtlasIndices.Num(),
			FString::Printf(TEXT("%d atlas(es) queued"), Context->AtlasIndices.Num()));
		BeginAtlasLoop(Context);
	}

	void RunValidateTemporalHoldout(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		int32 TileSize = 128;
		int32 Steps = 800;
		int32 OverlapTexels = 8;
		int32 HoldoutStep = -1;
		int32 MaxTiles = 0;
		bool bBaseline = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (Arg.Equals(TEXT("baseline"), ESearchCase::IgnoreCase))
			{
				bBaseline = true;
			}
			else
			{
				const int32 Value = FCString::Atoi(*Arg);
				if (NumericArgIndex == 0) { TileSize = FMath::Max(16, Value); }
				else if (NumericArgIndex == 1) { Steps = FMath::Max(1, Value); }
				else if (NumericArgIndex == 2) { OverlapTexels = FMath::Max(0, Value); }
				else if (NumericArgIndex == 3) { HoldoutStep = Value; }
				else if (NumericArgIndex == 4) { MaxTiles = FMath::Max(0, Value); }
				++NumericArgIndex;
			}
		}

		auto Fail = [bQuitWhenDone](const TCHAR* Message)
		{
			UE_LOG(LogNDGI, Error, TEXT("%s"), Message);
			NDGI::Editor::Pipeline::Fail(Message);
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		if (TileSize % 16 != 0)
		{
			Fail(TEXT("ndgi.ValidateTemporalHoldout: tile size must be a multiple of 16."));
			return;
		}
		if (OverlapTexels % 8 != 0)
		{
			Fail(TEXT("ndgi.ValidateTemporalHoldout: overlap must be a multiple of 8 texels."));
			return;
		}
		if (HoldoutStep < -1)
		{
			Fail(TEXT("ndgi.ValidateTemporalHoldout: held-out step must be -1 (middle slice) or a non-negative slice index."));
			return;
		}

		TArray<int32> AtlasIndices;
		FString CollectError;
		if (!CollectMergedAtlasIndices(TEXT("ndgi.ValidateTemporalHoldout"), AtlasIndices, CollectError))
		{
			Fail(*CollectError);
			return;
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Training,
			TEXT("Validating temporal holdout"), 0, TEXT("Loading merged sets..."));

		TSharedRef<FTrainAtlasContext> Context = MakeShared<FTrainAtlasContext>();
		ConfigureContext(Context, TileSize, OverlapTexels, Steps, bQuitWhenDone);
		Context->bBatch = true;
		Context->bHoldout = true;
		Context->bHoldoutBaseline = bBaseline;
		Context->HoldoutStep = HoldoutStep;
		Context->MaxTiles = MaxTiles;
		Context->AtlasIndices = MoveTemp(AtlasIndices);

		const FString HoldoutLabel = HoldoutStep < 0 ? TEXT("middle") : FString::FromInt(HoldoutStep);
		const FString TileLimitLabel = MaxTiles > 0
			? FString::Printf(TEXT("max %d tile(s)"), MaxTiles)
			: FString(TEXT("all tiles"));
		UE_LOG(LogNDGI, Display,
			TEXT("ndgi.ValidateTemporalHoldout: %s, %d atlas(es), tile %d (+%d halo), %d train steps/tile, held-out slice %s, %s."),
			bBaseline ? TEXT("baseline (all slices)") : TEXT("holdout"),
			Context->AtlasIndices.Num(), TileSize, OverlapTexels, Steps,
			*HoldoutLabel, *TileLimitLabel);
		BeginAtlasLoop(Context);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITrainAtlasCommand(
	TEXT("ndgi.TrainAtlas"),
	TEXT("Trains every tile of a merged lightmap atlas, exports per-tile models and builds the runtime level asset. Usage: ndgi.TrainAtlas <texIndex> <tileSize> <stepsPerTile> [overlapTexels] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTrainAtlas));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITrainAllAtlasesCommand(
	TEXT("ndgi.TrainAllAtlases"),
	TEXT("Trains all merged atlases in Saved/NDGI/Bake, exports per-tile models and builds the runtime level asset. Usage: ndgi.TrainAllAtlases <tileSize> <stepsPerTile> [overlapTexels] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTrainAllAtlases));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIValidateTemporalHoldoutCommand(
	TEXT("ndgi.ValidateTemporalHoldout"),
	TEXT("Trains each tile without one baked time slice, evaluates the excluded slice, and writes Saved/NDGI/TemporalHoldout.json. Add 'baseline' to train all slices and evaluate the same time. Usage: ndgi.ValidateTemporalHoldout <tileSize> <trainSteps> <overlap> <holdoutStep|-1> [maxTiles] [baseline] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunValidateTemporalHoldout));

namespace
{
	/** Small bakeable level used to validate the bake -> merge -> train -> runtime loop quickly. */
	void RunCreateBakeTestLevel(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		const bool bQuitWhenDone = Args.ContainsByPredicate([](const FString& Arg)
		{
			return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		});

		auto Fail = [bQuitWhenDone](const TCHAR* Message)
		{
			UE_LOG(LogNDGI, Error, TEXT("%s"), Message);
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		if (!World)
		{
			Fail(TEXT("ndgi.CreateBakeTestLevel: could not create a new level."));
			return;
		}

		AWorldSettings* Settings = World->GetWorldSettings();
		Settings->bForceNoPrecomputedLighting = false;
		Settings->LightmassSettings.bCompressLightmaps = false;

		UStaticMesh* CubeMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		if (!CubeMesh || !PlaneMesh)
		{
			Fail(TEXT("ndgi.CreateBakeTestLevel: could not load the engine basic shapes."));
			return;
		}

		auto SpawnMesh = [World](UStaticMesh* Mesh, const FVector& Location, const FVector& Scale, const FRotator& Rotation, int32 LightmapRes)
		{
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, Rotation);
			UStaticMeshComponent* Component = Actor->GetStaticMeshComponent();
			Component->SetMobility(EComponentMobility::Static);
			Component->SetStaticMesh(Mesh);
			Component->bOverrideLightMapRes = true;
			Component->OverriddenLightMapRes = LightmapRes;
			Actor->SetActorScale3D(Scale);
		};

		// The floor deliberately exceeds one 64-texel model tile so the seam
		// validation has real continuous boundaries to measure.
		SpawnMesh(PlaneMesh, FVector(0.0, 0.0, 0.0), FVector(12.0, 12.0, 1.0), FRotator::ZeroRotator, 256);
		SpawnMesh(CubeMesh, FVector(-200.0, 0.0, 100.0), FVector(2.0, 2.0, 2.0), FRotator(0.0f, 20.0f, 0.0f), 64);
		SpawnMesh(CubeMesh, FVector(150.0, 120.0, 75.0), FVector(1.5, 1.5, 1.5), FRotator(0.0f, -35.0f, 0.0f), 64);
		SpawnMesh(CubeMesh, FVector(250.0, -180.0, 50.0), FVector(1.0, 1.0, 1.0), FRotator(0.0f, 10.0f, 0.0f), 64);

		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 900.0), FRotator(-35.0f, -40.0f, 0.0f));
		Sun->GetLightComponent()->SetMobility(EComponentMobility::Static);

		ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0.0, 0.0, 500.0), FRotator::ZeroRotator);
		USkyLightComponent* SkyComponent = Sky->GetLightComponent();
		SkyComponent->SetMobility(EComponentMobility::Stationary);
		SkyComponent->bRealTimeCapture = false;

		const FString PackageName = TEXT("/Game/NDGI/NDGITestBakeMap");
		if (!UEditorLoadingAndSavingUtils::SaveMap(World, PackageName))
		{
			Fail(TEXT("ndgi.CreateBakeTestLevel: failed to save the level."));
			return;
		}

		UE_LOG(LogNDGI, Display,
			TEXT("ndgi.CreateBakeTestLevel: saved %s (static floor + 3 cubes, static sun, stationary sky, uncompressed lightmaps)."),
			*PackageName);
		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGICreateBakeTestLevelCommand(
	TEXT("ndgi.CreateBakeTestLevel"),
	TEXT("Creates /Game/NDGI/NDGITestBakeMap for a fast bake -> merge -> train validation. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunCreateBakeTestLevel));

namespace
{
	bool ReadbackLightmap(UTexture2D* Texture, TArray<FColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FTextureResource* Resource = Texture ? Texture->GetResource() : nullptr;
		FTexturePlatformData* PlatformData = Texture ? Texture->GetPlatformData() : nullptr;
		if (!Resource || !PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		OutWidth = PlatformData->Mips[0].SizeX;
		OutHeight = PlatformData->Mips[0].SizeY;
		if (OutWidth <= 0 || OutHeight <= 0)
		{
			return false;
		}

		TSharedRef<TArray<FColor>> Pixels = MakeShared<TArray<FColor>>();
		const FTextureRHIRef TextureRHI = Resource->GetTextureRHI();
		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGIValidateReadback)(
			[Pixels, TextureRHI, Width, Height](FRHICommandListImmediate& RHICmdList)
			{
				Pixels->SetNumUninitialized(Width * Height);
				RHICmdList.ReadSurfaceData(TextureRHI, FIntRect(0, 0, Width, Height), *Pixels, FReadSurfaceDataFlags(RCM_UNorm));
			});
		FlushRenderingCommands();
		OutPixels = MoveTemp(*Pixels);
		return OutPixels.Num() == Width * Height;
	}

	void RunValidateLevelAsset(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		float Time = 0.0f;
		bool bQuitWhenDone = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0) { Time = FCString::Atof(*Arg); }
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Validating,
			TEXT("Validating level asset"), 1, TEXT("Building cache and decoding..."));

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateLevelAsset: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			NDGI::Editor::Pipeline::Complete(bPassed,
				bPassed ? TEXT("Level asset validation passed.") : TEXT("Level asset validation failed — see the log."));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		const FString LevelName = World ? World->GetName() : TEXT("Level");
		const FString PackageName = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s"), *LevelName);
		const FString AssetName = FString::Printf(TEXT("NDGI_%s"), *LevelName);
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *(PackageName + TEXT(".") + AssetName));
		if (!LevelAsset)
		{
			UE_LOG(LogNDGI, Error, TEXT("ValidateLevelAsset: %s not found - run ndgi.TrainAtlas first."), *PackageName);
			Finish(false);
			return;
		}

		int32 ModelCount = 0;
		int32 ChangedTexels = 0;
		UTexture2D* FirstTexture = nullptr;
		for (const FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
		{
			for (int32 Cell = 0; Cell < Entry.TileModelPaths.Num(); ++Cell)
			{
				ModelCount += Entry.GetTileModelPath(Cell).IsValid() ? 1 : 0;
			}
			if (!FirstTexture)
			{
				FirstTexture = Cast<UTexture2D>(Entry.LightmapTexturePath.TryLoad());
			}
		}

		TArray<FColor> Before;
		int32 BeforeWidth = 0;
		int32 BeforeHeight = 0;
		if (FirstTexture)
		{
			FTextureResource* Resource = FirstTexture->GetResource();
			FRHITexture* RHI = Resource ? Resource->GetTextureRHI() : nullptr;
			FTexturePlatformData* Data = FirstTexture->GetPlatformData();
			const int32 FullWidth = (Data && Data->Mips.Num() > 0) ? Data->Mips[0].SizeX : 0;
			if (!RHI || static_cast<int32>(RHI->GetSizeX()) < FullWidth)
			{
				FirstTexture->NeverStream = true;
				FirstTexture->UpdateResource();
				FlushRenderingCommands();
			}
		}
		const bool bHaveBefore = FirstTexture && ReadbackLightmap(FirstTexture, Before, BeforeWidth, BeforeHeight);

		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> Cache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		const bool bCacheBuilt = NDGI::Runtime::BuildLevelDecodeCache(LevelAsset, Cache, &BuildMs, &SkippedModels);
		const bool bDecoded = bCacheBuilt && NDGI::Runtime::DecodeCachedLevel(Cache, Time);
		FlushRenderingCommands();
		GDynamicRHI->RHIBlockUntilGPUIdle();
		if (bCacheBuilt)
		{
			UE_LOG(LogNDGI, Display, TEXT("ValidateLevelAsset: cache %d atlas(es), %d model(s), %.1f ms, %d skipped."),
				Cache->Entries.Num(), Cache->Tiles, BuildMs, SkippedModels);
		}

		if (bHaveBefore && FirstTexture)
		{
			TArray<FColor> After;
			int32 AfterWidth = 0;
			int32 AfterHeight = 0;
			if (ReadbackLightmap(FirstTexture, After, AfterWidth, AfterHeight)
				&& After.Num() == Before.Num())
			{
				double TotalDiff = 0.0;
				for (int32 Index = 0; Index < After.Num(); ++Index)
				{
					// Alpha carries the HQ luminance; include it so a pure brightness
					// update (chroma kept from the bake) still counts as a change.
					const int32 Diff = FMath::Abs(After[Index].R - Before[Index].R)
						+ FMath::Abs(After[Index].G - Before[Index].G)
						+ FMath::Abs(After[Index].B - Before[Index].B)
						+ FMath::Abs(After[Index].A - Before[Index].A);
					TotalDiff += Diff;
					ChangedTexels += (Diff != 0) ? 1 : 0;
				}
				UE_LOG(LogNDGI, Display, TEXT("ValidateLevelAsset: %d/%d texels changed, mean |diff| %.2f/1020."),
					ChangedTexels, After.Num(), After.Num() > 0 ? TotalDiff / After.Num() : 0.0);
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("ValidateLevelAsset: %d atlas(es), %d model(s), time %.2f, decode %s."),
			LevelAsset->Atlases.Num(), ModelCount, Time, bDecoded ? TEXT("ok") : TEXT("failed"));
		Finish(bDecoded && ModelCount > 0 && ChangedTexels > 0);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIValidateLevelAssetCommand(
	TEXT("ndgi.ValidateLevelAsset"),
	TEXT("Decodes the trained level asset for the current editor world at the given time. Usage: ndgi.ValidateLevelAsset <time> [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunValidateLevelAsset));

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "NDGIBakeManifest.h"
#include "NDGIEncodingMap.h"
#include "NDGILightmapSet.h"
#include "NDGILevelActor.h"
#include "NDGIPipelineStatus.h"
#include "NDGITimeOfDayProfile.h"
#include "NDGI.h"

#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "Editor.h"
#include "EditorBuildUtils.h"
#include "FileHelpers.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/Texture2D.h"
#include "GameFramework/WorldSettings.h"
#include "GPULightmassSettings.h"
#include "LightMap.h"
#include "Rendering/StaticLightingSystemInterface.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "StaticMeshComponentLODInfo.h"
#include "UnrealEdGlobals.h"
#include "Editor/UnrealEdEngine.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/SkyLightComponent.h"
#include "Engine/TextureRenderTarget2D.h"
#include "CanvasTypes.h"
#include "CanvasItem.h"
#include "BatchedElements.h"
#include "GameTime.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/WorldPartitionEditorPerProjectUserSettings.h"
#include "WorldPartition/StaticLightingData/MapBuildDataActor.h"
#include "WorldPartition/WorldPartitionEditorLoaderAdapter.h"
#include "WorldPartition/LoaderAdapter/LoaderAdapterShape.h"

namespace
{
	using namespace NDGI;
	namespace BakeManifest = NDGI::Editor::BakeManifest;

	struct FTextureReadbackResult
	{
		TArray<FLinearColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		bool bSuccess = false;
	};

	/** One unique HQ/LQ lightmap texture plus the per-atlas scale/add needed to decode it. */
	struct FLightmapExportSource
	{
		UTexture2D* Texture = nullptr;
		bool bHighQuality = true;
		FVector4f Scale = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		FVector4f Add = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
		/** Coefficient 1 (directional SH) range for HQ lightmaps. */
		FVector4f ScaleSH = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
		FVector4f AddSH = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
	};

	/**
	 * Renders the texture into a transient float render target so compressed lightmap formats
	 * (DXT5/BC6H) are hardware-decoded, then reads the target back as linear colors.
	 */
	bool ReadbackTexture(UTexture2D* Texture, FTextureReadbackResult& OutResult)
	{
		if (!Texture || !Texture->GetResource())
		{
			return false;
		}
		FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		if (!PlatformData || PlatformData->Mips.Num() == 0)
		{
			return false;
		}
		const int32 Width = PlatformData->Mips[0].SizeX;
		const int32 Height = PlatformData->Mips[0].SizeY;
		if (Width <= 0 || Height <= 0)
		{
			return false;
		}

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage());
		RenderTarget->RenderTargetFormat = RTF_RGBA16f;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitCustomFormat(Width, Height, PF_FloatRGBA, false);
		RenderTarget->UpdateResourceImmediate(true);
		FTextureRenderTargetResource* Resource = RenderTarget->GameThread_GetRenderTargetResource();
		if (!Resource)
		{
			return false;
		}

		{
			FCanvas Canvas(Resource, nullptr, FGameTime::GetTimeSinceAppStart(), GMaxRHIFeatureLevel);
			Canvas.Clear(FLinearColor::Black);
			FCanvasTileItem Tile(FVector2D::ZeroVector, Texture->GetResource(), FVector2D(Width, Height), FLinearColor::White);
			Tile.BlendMode = SE_BLEND_Opaque;
			Canvas.DrawItem(Tile);
			Canvas.Flush_GameThread();
		}
		FlushRenderingCommands();

		OutResult.Width = Width;
		OutResult.Height = Height;
		TSharedRef<FTextureReadbackResult> Result = MakeShared<FTextureReadbackResult>();
		Result->Width = Width;
		Result->Height = Height;

		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.LightmapReadback"));
		FTextureRHIRef TargetRHI = Resource->GetRenderTargetTexture();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueueLightmapCopy)(
			[Readback, TargetRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TargetRHI);
			});
		FlushRenderingCommands();

		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			UE_LOG(LogNDGI, Warning, TEXT("Bake export: timed out waiting for texture readback."));
			ENQUEUE_RENDER_COMMAND(NDGIDeleteLightmapReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		ENQUEUE_RENDER_COMMAND(NDGILockLightmapReadback)(
			[Readback, Result](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitchInPixels = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitchInPixels, &BufferHeight);
				if (Data)
				{
					const int32 Height = FMath::Min(BufferHeight, Result->Height);
					Result->Pixels.SetNumUninitialized(Result->Width * Result->Height);
					for (int32 Y = 0; Y < Height; ++Y)
					{
						const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
							static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitchInPixels * sizeof(FFloat16Color));
						for (int32 X = 0; X < Result->Width; ++X)
						{
							Result->Pixels[Y * Result->Width + X] = FLinearColor(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
						}
					}
					Result->bSuccess = true;
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();

		OutResult = MoveTemp(*Result);
		return OutResult.bSuccess;
	}

	/**
	 * M0 World Partition spike: force every cell in the editor world's bounds to load
	 * so the bake scan can see all static primitives and their built-data packages.
	 */
	bool ForceLoadAllWorldPartitionCells(UWorld* World)
	{
		UWorldPartition* WorldPartition = World ? World->GetWorldPartition() : nullptr;
		if (!WorldPartition)
		{
			return false;
		}

		const FBox Bounds = WorldPartition->GetEditorWorldBounds();
		UE_LOG(LogNDGI, Display,
			TEXT("WP spike: world partition '%s', streaming %d, editor bounds %s."),
			*WorldPartition->GetName(),
			WorldPartition->IsStreamingEnabled() ? 1 : 0,
			*Bounds.ToString());

		if (!WorldPartition->IsStreamingEnabled() || !Bounds.IsValid)
		{
			return true;
		}

		UWorldPartitionEditorPerProjectUserSettings* Settings = GetMutableDefault<UWorldPartitionEditorPerProjectUserSettings>();
		Settings->SetEditorLoadedRegions(World, { Bounds });

		// Create the loader adapter ourselves so we can wait on IsLoaded() rather than
		// relying on the editor's normal per-frame World Partition tick.
		UWorldPartitionEditorLoaderAdapter* EditorAdapter =
			WorldPartition->CreateEditorLoaderAdapter<FLoaderAdapterShape>(World, Bounds, TEXT("NDGI WP Spike"));
		IWorldPartitionActorLoaderInterface::ILoaderAdapter* Loader = EditorAdapter ? EditorAdapter->GetLoaderAdapter() : nullptr;
		if (Loader)
		{
			Loader->SetUserCreated(true);
			Loader->Load();
		}

		UWorldPartitionSubsystem* Subsystem = World->GetSubsystem<UWorldPartitionSubsystem>();
		const double StartSeconds = FPlatformTime::Seconds();
		while (Loader && !Loader->IsLoaded() && FPlatformTime::Seconds() - StartSeconds < 120.0)
		{
			GEditor->Tick(0.0f, true);
			FPlatformProcess::Sleep(0.01f);
		}
		World->BlockTillLevelStreamingCompleted();
		while (Subsystem && !Subsystem->IsAllStreamingCompleted() && FPlatformTime::Seconds() - StartSeconds < 120.0)
		{
			GEditor->Tick(0.0f, true);
			FPlatformProcess::Sleep(0.01f);
		}

		int32 LoadedLevels = 0;
		int32 LoadedComponents = 0;
		for (ULevel* Level : World->GetLevels())
		{
			if (!Level)
			{
				continue;
			}
			++LoadedLevels;
			for (AActor* Actor : Level->Actors)
			{
				if (Actor)
				{
					LoadedComponents += Actor->GetComponents().Num();
				}
			}
		}
		UE_LOG(LogNDGI, Display, TEXT("WP spike: streaming %s, %d level(s), %d component(s) loaded."),
			(Subsystem && Subsystem->IsAllStreamingCompleted()) ? TEXT("complete") : TEXT("TIMED OUT"),
			LoadedLevels, LoadedComponents);
		return true;
	}

	int32 LogWorldPartitionBuildData(UWorld* World)
	{
		if (!World || !World->GetWorldPartition())
		{
			return 0;
		}

		int32 Levels = 0;
		int32 LevelsWithBuildData = 0;
		for (ULevel* Level : World->GetLevels())
		{
			if (!Level)
			{
				continue;
			}
			++Levels;
			UMapBuildDataRegistry* Registry = Level->MapBuildData;
			const UPackage* LevelPackage = Level->GetOutermost();
			const UPackage* RegistryPackage = Registry ? Registry->GetOutermost() : nullptr;
			if (RegistryPackage)
			{
				++LevelsWithBuildData;
				UE_LOG(LogNDGI, Display, TEXT("WP spike: level %s -> build data %s (exists=%d)."),
					LevelPackage ? *LevelPackage->GetName() : TEXT("<none>"),
					*RegistryPackage->GetName(),
					FPackageName::DoesPackageExist(RegistryPackage->GetName()) ? 1 : 0);
			}
			else
			{
				UE_LOG(LogNDGI, Display, TEXT("WP spike: level %s -> no build data."),
					LevelPackage ? *LevelPackage->GetName() : TEXT("<none>"));
			}
		}

		int32 BuildDataActors = 0;
		for (TActorIterator<AMapBuildDataActor> It(World); It; ++It)
		{
			++BuildDataActors;
			UMapBuildDataRegistry* Registry = It->GetBuildData();
			UE_LOG(LogNDGI, Display, TEXT("WP spike: build-data actor %s -> %s."),
				*It->GetActorNameOrLabel(),
				Registry && Registry->GetOutermost() ? *Registry->GetOutermost()->GetName() : TEXT("<none>"));
		}

		UE_LOG(LogNDGI, Display, TEXT("WP spike: %d level(s), %d with build data, %d build-data actor(s)."),
			Levels, LevelsWithBuildData, BuildDataActors);
		return LevelsWithBuildData;
	}

	/** Collects the unique lightmap atlases of all static primitives in the world. */
	void CollectLightmapTextures(UWorld* World, TArray<FLightmapExportSource>& OutSources, int32& OutVirtualTextureCount)
	{
		OutVirtualTextureCount = 0;
		TMap<UTexture2D*, FLightmapExportSource> UniqueSources;
		int32 NumComponents = 0;
		int32 NumStaticMobility = 0;
		int32 NumWithLODData = 0;
		int32 NumWithBuildData = 0;
		int32 NumWithLightmap = 0;
		int32 NumValidBasis = 0;

		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World)
			{
				continue;
			}
			++NumComponents;
			if (Component->Mobility != EComponentMobility::Static)
			{
				continue;
			}
			++NumStaticMobility;
			if (Component->LODData.Num() == 0)
			{
				continue;
			}
			++NumWithLODData;
			// Read the map build data registry directly: FStaticLightingSystemInterface always prefers
			// the GPULightmass implementation when the plugin is enabled, which returns nothing for
			// levels built with the standard (CPU) Lightmass.
			const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
			if (!BuildData)
			{
				continue;
			}
			++NumWithBuildData;
			if (!BuildData->LightMap)
			{
				continue;
			}
			++NumWithLightmap;
			const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
			if (!LightMap2D)
			{
				continue;
			}

			// Prefer the HQ coefficient textures (basis 0), fall back to LQ (basis 1).
			const bool bHighQuality = LightMap2D->IsValid(0);
			const int32 BasisIndex = bHighQuality ? 0 : 1;
			if (!LightMap2D->IsValid(BasisIndex))
			{
				if (LightMap2D->IsVirtualTextureValid())
				{
					++OutVirtualTextureCount;
				}
				continue;
			}
			++NumValidBasis;
			if (UTexture2D* Texture = const_cast<UTexture2D*>(LightMap2D->GetTexture(BasisIndex)))
			{
				if (!UniqueSources.Contains(Texture))
				{
					FLightmapExportSource Source;
					Source.Texture = Texture;
					Source.bHighQuality = bHighQuality;
					const int32 CoefIndex = bHighQuality ? 0 : LQ_LIGHTMAP_COEF_INDEX;
					Source.Scale = LightMap2D->ScaleVectors[CoefIndex];
					Source.Add = LightMap2D->AddVectors[CoefIndex];
					Source.ScaleSH = LightMap2D->ScaleVectors[1];
					Source.AddSH = LightMap2D->AddVectors[1];
					UniqueSources.Add(Texture, Source);
				}
			}
		}

		UniqueSources.GenerateValueArray(OutSources);
		// Stable ordering so the same lightmap gets the same Tex index across bake steps,
		// which is what allows merging the per-step files into one temporal set.
		OutSources.Sort([](const FLightmapExportSource& A, const FLightmapExportSource& B)
		{
			return A.Texture->GetPathName() < B.Texture->GetPathName();
		});

		UE_LOG(LogNDGI, Display,
			TEXT("Bake export: scan %d mesh components, %d static, %d with LOD data, %d with build data, %d with lightmap, %d valid basis, %d unique textures, %d virtual."),
			NumComponents, NumStaticMobility, NumWithLODData, NumWithBuildData, NumWithLightmap, NumValidBasis,
			OutSources.Num(), OutVirtualTextureCount);
		if (NumComponents > 0 && NumStaticMobility == 0)
		{
			UE_LOG(LogNDGI, Warning, TEXT("Bake export: no Static-mobility mesh components found; set the meshes to Static mobility to receive lightmaps."));
		}
		else if (NumWithLightmap == 0 && NumStaticMobility > 0)
		{
			UE_LOG(LogNDGI, Warning, TEXT("Bake export: static meshes have no lightmap data. Ensure the level has a Stationary/Static light and the lightmap build actually ran."));
		}
	}

	bool ExportCurrentLightmaps(UWorld* World, int32 BakeStep, int32* OutExportedTextures = nullptr, int32 TotalSteps = 0)
	{
		if (OutExportedTextures)
		{
			*OutExportedTextures = 0;
		}
		TArray<FLightmapExportSource> Sources;
		int32 VirtualTextureCount = 0;
		CollectLightmapTextures(World, Sources, VirtualTextureCount);
		if (VirtualTextureCount > 0)
		{
			UE_LOG(LogNDGI, Warning, TEXT("Bake export: %d primitive(s) use virtual-texture lightmaps, which are not exported. Disable 'Support Virtual Texture Lightmaps' in World Settings for NDGI."), VirtualTextureCount);
		}
		if (Sources.Num() == 0)
		{
			UE_LOG(LogNDGI, Warning, TEXT("Bake export: no baked lightmap textures found at step %d."), BakeStep);
			return false;
		}

		const FString Directory = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
		IFileManager::Get().MakeDirectory(*Directory, true);

		BakeManifest::FManifest Manifest;
		Manifest.LevelPackage = World ? World->GetOutermost()->GetName() : FString();
		Manifest.Steps = TotalSteps;

		int32 ExportedCount = 0;
		for (int32 TextureIndex = 0; TextureIndex < Sources.Num(); ++TextureIndex)
		{
			const FLightmapExportSource& Source = Sources[TextureIndex];
			UE_LOG(LogNDGI, Display, TEXT("Bake export: Tex%02d = %s (%s)"), TextureIndex, *Source.Texture->GetPathName(),
				Source.bHighQuality ? TEXT("HQ") : TEXT("LQ"));

			FTextureReadbackResult Readback;
			if (!ReadbackTexture(Source.Texture, Readback))
			{
				continue;
			}

			// Lightmap atlases pack coefficient 0 in the top half and coefficient 1 (SH) in the
			// bottom half (LightmapUV1 = LightmapUV0 + float2(0, 0.5)). Export coefficient 0.
			const int32 Width = Readback.Width;
			const int32 HalfHeight = Readback.Height / 2;
			if (HalfHeight <= 0)
			{
				continue;
			}

			// Different components sharing this atlas have their own HQ ranges.
			// Decode each texel with the range that owns it, not the first component's.
			Editor::FEncodingRangeMap RangeMap;
			FString RangeError;
			const bool bHasRangeMap = Editor::BuildEncodingRangeMap(
				World, Source.Texture, Width, HalfHeight, RangeMap, &RangeError);
			if (bHasRangeMap)
			{
				UE_LOG(LogNDGI, Display,
					TEXT("Bake export: Tex%02d encoding ranges: %d distinct, %d/%d lit texels, %d range-owned, %d conflict(s)."),
					TextureIndex, RangeMap.Ranges.Num(), RangeMap.Covered, Width * HalfHeight, RangeMap.RangeCovered, RangeMap.Conflicts);
			}
			else
			{
				UE_LOG(LogNDGI, Warning,
					TEXT("Bake export: Tex%02d has no per-component range map (%s); using the first component's range."),
					TextureIndex, *RangeError);
			}

			FLightmapSet Set;
			Set.Width = Width;
			Set.Height = HalfHeight;
			Set.EncodingRanges = RangeMap.Ranges;
			Set.EncodingRangeIndices = RangeMap.Indices;
			Set.Steps.SetNum(1);
			FLightmapStep& Step = Set.Steps[0];
			Step.RGB.SetNumUninitialized(Width * HalfHeight * 3);
			Step.SH.SetNumUninitialized(Width * HalfHeight * 3);
			Step.Mask.SetNumUninitialized(Width * HalfHeight);
			for (int32 Y = 0; Y < HalfHeight; ++Y)
			{
				for (int32 X = 0; X < Width; ++X)
				{
					const FLinearColor Top = Readback.Pixels[Y * Width + X];
					const FLinearColor Bottom = Readback.Pixels[(Y + HalfHeight) * Width + X];
					const FNDGIEncodingRange* Range = bHasRangeMap ? &RangeMap.GetRange(X, Y, Width) : nullptr;
					const FVector4f Scale0 = Range ? Range->Scale0 : Source.Scale;
					const FVector4f Add0 = Range ? Range->Add0 : Source.Add;
					const FVector4f Scale1 = Range ? Range->Scale1 : Source.ScaleSH;
					const FVector4f Add1 = Range ? Range->Add1 : Source.AddSH;
					const FVector3f ScaleRGB(Scale0.X, Scale0.Y, Scale0.Z);
					const FVector3f AddRGB(Add0.X, Add0.Y, Add0.Z);

					FVector3f Color = FVector3f::ZeroVector;
					FVector3f SH = FVector3f::ZeroVector;
					if (Source.bHighQuality)
					{
						float LogL = Top.A + Bottom.A * (1.0f / 255.0f) - (0.5f / 255.0f);
						LogL = LogL * Scale0.W + Add0.W;
						const float L = FMath::Exp2(LogL) - 0.01858136f;
						const FVector3f UVW = FVector3f(Top.R * Top.R, Top.G * Top.G, Top.B * Top.B) * ScaleRGB + AddRGB;
						Color = UVW * FMath::Max(L, 0.0f);

						// Coefficient 1 holds the directional SH. The engine samples it as
						// SH = raw * Scale[1] + Add[1] and dots it with float4(N.yzx, 1).
						SH = FVector3f(Bottom.R, Bottom.G, Bottom.B) * FVector3f(Scale1.X, Scale1.Y, Scale1.Z)
							+ FVector3f(Add1.X, Add1.Y, Add1.Z);
					}
					else
					{
						const FVector3f LogRGB = FVector3f(Top.R, Top.G, Top.B) * ScaleRGB + AddRGB;
						const float LogL = 0.3f * LogRGB.X + 0.59f * LogRGB.Y + 0.11f * LogRGB.Z;
						const float L = FMath::Exp2(LogL * 16.0f - 8.0f) - (1.0f / 256.0f);
						Color = LogRGB * (FMath::Max(L, 0.0f) / FMath::Max(LogL, 1e-5f));
					}

					const int32 Texel = Y * Width + X;
					Step.RGB[Texel * 3 + 0] = FMath::Max(Color.X, 0.0f);
					Step.RGB[Texel * 3 + 1] = FMath::Max(Color.Y, 0.0f);
					Step.RGB[Texel * 3 + 2] = FMath::Max(Color.Z, 0.0f);
					Step.SH[Texel * 3 + 0] = SH.X;
					Step.SH[Texel * 3 + 1] = SH.Y;
					Step.SH[Texel * 3 + 2] = SH.Z;
					// Texels outside every component triangle are atlas padding.
					// The runtime preserves them, so they must not be trained on
					// or counted by seam/SH validation. Gutter texels still get a
					// range (above) so the decoder can re-encode them correctly.
					Step.Mask[Texel] = (!bHasRangeMap || RangeMap.Coverage[Texel] != 0) ? 1 : 0;
				}
			}

			const FString FileName = FString::Printf(TEXT("Step%02d_Tex%02d.ndgi"), BakeStep, TextureIndex);
			if (Set.Save(Directory / FileName))
			{
				++ExportedCount;
				BakeManifest::FAtlasEntry& Atlas = Manifest.Atlases.AddDefaulted_GetRef();
				Atlas.TexIndex = TextureIndex;
				Atlas.TexturePath = Source.Texture->GetPathName();
				Atlas.Width = Width;
				Atlas.Height = HalfHeight * 2;
				Atlas.Cells = RangeMap.SourceCells;
			}
		}

		if (BakeStep == 0 && TotalSteps > 0 && Manifest.Atlases.Num() > 0)
		{
			if (BakeManifest::Save(Manifest))
			{
				UE_LOG(LogNDGI, Display, TEXT("Bake export: wrote level-scoped bake manifest %s (%d atlas(es), %d steps)."),
					*BakeManifest::GetPath(), Manifest.Atlases.Num(), Manifest.Steps);
			}
			else
			{
				UE_LOG(LogNDGI, Warning, TEXT("Bake export: could not write the bake manifest; merge/train will fall back to scanning the bake folder."));
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("Bake export: wrote %d/%d lightmap texture(s) for step %d to %s."),
			ExportedCount, Sources.Num(), BakeStep, *Directory);
		if (OutExportedTextures)
		{
			*OutExportedTextures = ExportedCount;
		}
		return ExportedCount > 0;
	}

	struct FNDGIBakeJob
	{
		UWorld* World = nullptr;
		TArray<TWeakObjectPtr<ADirectionalLight>> Suns;
		int32 NumSteps = 8;
		int32 CurrentStep = 0;
		bool bUseGPU = false;
		bool bQuitWhenDone = false;
		bool bWaiting = false;
		bool bGPUFinished = false;
		bool bFinished = false;
		bool bHadFailure = false;
		bool bCanceled = false;
		TWeakObjectPtr<UNDGITimeOfDayProfile> Profile;
		int32 TotalExported = 0;
		int32 ExportedTextures = 0;
		double StepStartSeconds = 0.0;
		FDelegateHandle GPUDelegateHandle;
	};

	void FinishBakeJob(const TSharedRef<FNDGIBakeJob>& Job)
	{
		Job->bFinished = true;
		if (Job->bCanceled)
		{
			UE_LOG(LogNDGI, Warning, TEXT("ndgi.BakeTOD: canceled after %d/%d step(s)."),
				Job->CurrentStep, Job->NumSteps);
			NDGI::Editor::Pipeline::Complete(false,
				FString::Printf(TEXT("Bake canceled after %d/%d slice(s)."), Job->CurrentStep, Job->NumSteps));
			if (Job->bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
			return;
		}
		if (Job->TotalExported == 0)
		{
			UE_LOG(LogNDGI, Error,
				TEXT("ndgi.BakeTOD: no lightmaps were exported. Check that 'Force No Precomputed Lighting' is off, ")
				TEXT("the level has Static meshes with valid lightmap UVs, a Directional Light exists, and the bake produced lightmaps."));
		}
		if (Job->TotalExported > 0)
		{
			const bool bSaved = UEditorLoadingAndSavingUtils::SaveDirtyPackages(true, true);
			UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: level + lightmap save %s."), bSaved ? TEXT("ok") : TEXT("FAILED"));
			Job->bHadFailure |= !bSaved;
			const int32 LevelsWithBuildData = LogWorldPartitionBuildData(Job->World);
			if (Job->World && Job->World->GetWorldPartition())
			{
				const bool bWPVerified = bSaved && Job->ExportedTextures > 0 && LevelsWithBuildData > 0;
				UE_LOG(LogNDGI, Display, TEXT("WP spike: %s - %d atlas texture(s), %d level(s) with saved build data."),
					bWPVerified ? TEXT("PASSED") : TEXT("FAILED"), Job->ExportedTextures, LevelsWithBuildData);
				Job->bHadFailure |= !bWPVerified;
			}
		}
		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: %s."),
			Job->bHadFailure ? TEXT("completed with errors (see warnings above)") : TEXT("all steps exported"));
		NDGI::Editor::Pipeline::Complete(!Job->bHadFailure,
			Job->bHadFailure ? TEXT("Bake completed with errors — see the log.") : TEXT("Bake completed."));
		if (Job->bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void StartBakeStep(const TSharedRef<FNDGIBakeJob>& Job)
	{
		if (!Job->World || Job->CurrentStep >= Job->NumSteps)
		{
			FinishBakeJob(Job);
			return;
		}

		if (NDGI::Editor::Pipeline::IsCancelRequested())
		{
			Job->bCanceled = true;
			FinishBakeJob(Job);
			return;
		}

		// Simple sun arc: low in the morning to low at night, peaking at noon.
		const float Alpha = static_cast<float>(Job->CurrentStep) / static_cast<float>(FMath::Max(1, Job->NumSteps - 1));
		const bool bUseProfile = Job->Profile.IsValid();
		FNDGITimeOfDayKey Key;
		if (bUseProfile)
		{
			Key = Job->Profile->Evaluate(Alpha);
		}
		else
		{
			Key.SunPitch = 10.0f + 70.0f * FMath::Sin(PI * Alpha);
		}

		for (TWeakObjectPtr<ADirectionalLight>& WeakSun : Job->Suns)
		{
			if (ADirectionalLight* Sun = WeakSun.Get())
			{
				FRotator Rotation = Sun->GetActorRotation();
				// Profile pitch is sun elevation above the horizon. UE's light
				// forward vector points from the sun toward the ground, so the
				// actor rotation pitch is the negated elevation.
				Rotation.Pitch = -Key.SunPitch;
				if (bUseProfile)
				{
					Rotation.Yaw = Key.SunYaw;
				}
				Sun->SetActorRotation(Rotation);

				if (bUseProfile)
				{
					ULightComponent* Light = Sun->GetLightComponent();
					if (Light && Light->Mobility != EComponentMobility::Static)
					{
						Light->SetLightColor(Key.SunColor, false);
						Light->SetIntensity(Key.SunIntensity);
					}
				}
			}
		}

		if (bUseProfile)
		{
			for (TActorIterator<ASkyLight> It(Job->World); It; ++It)
			{
				USkyLightComponent* Sky = It->GetLightComponent();
				if (Sky && Sky->Mobility != EComponentMobility::Static)
				{
					Sky->SetLightColor(Key.SkyColor);
					Sky->SetIntensity(Key.SkyIntensity);
					break;
				}
			}
		}

		Job->bWaiting = true;
		Job->bGPUFinished = false;
		Job->StepStartSeconds = FPlatformTime::Seconds();

		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: step %d/%d starting (%s)..."),
			Job->CurrentStep + 1, Job->NumSteps, Job->bUseGPU ? TEXT("GPULightmass") : TEXT("CPU Lightmass"));
		NDGI::Editor::Pipeline::SetProgress(Job->CurrentStep, Job->NumSteps,
			FString::Printf(TEXT("Baking step %d/%d (%s)"), Job->CurrentStep + 1, Job->NumSteps,
				Job->bUseGPU ? TEXT("GPULightmass") : TEXT("CPU Lightmass")));

		if (Job->bUseGPU)
		{
			UGPULightmassSubsystem* Subsystem = Job->World->GetSubsystem<UGPULightmassSubsystem>();
			if (!Subsystem)
			{
				UE_LOG(LogNDGI, Error, TEXT("ndgi.BakeTOD: GPULightmass subsystem unavailable (is the plugin enabled?)."));
				Job->bHadFailure = true;
				FinishBakeJob(Job);
				return;
			}
			if (Subsystem->IsRunning())
			{
				Subsystem->Save();
				Subsystem->Stop();
			}
			Job->GPUDelegateHandle = Subsystem->OnLightBuildEnded().AddLambda([Job]()
			{
				Job->bGPUFinished = true;
			});
			Subsystem->Launch();
			if (!Subsystem->IsRunning())
			{
				Subsystem->OnLightBuildEnded().Remove(Job->GPUDelegateHandle);
				UE_LOG(LogNDGI, Error, TEXT("ndgi.BakeTOD: GPULightmass failed to start (requires DXR; check lightmap UVs and the plugin)."));
				Job->bHadFailure = true;
				FinishBakeJob(Job);
				return;
			}
		}
		else
		{
			const bool bBuilt = FEditorBuildUtils::EditorBuild(Job->World, FBuildOptions::BuildLighting, false);
			Job->bHadFailure |= !bBuilt;
		}
	}

	void CompleteBakeStep(const TSharedRef<FNDGIBakeJob>& Job)
	{
		FlushRenderingCommands();
		const bool bExported = ExportCurrentLightmaps(Job->World, Job->CurrentStep, &Job->ExportedTextures, Job->NumSteps);
		Job->TotalExported += bExported ? 1 : 0;
		Job->bHadFailure |= !bExported;
		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: step %d/%d - export %s."),
			Job->CurrentStep + 1, Job->NumSteps, bExported ? TEXT("ok") : TEXT("no textures"));
		Job->bWaiting = false;
		++Job->CurrentStep;
		NDGI::Editor::Pipeline::SetProgress(Job->CurrentStep, Job->NumSteps,
			FString::Printf(TEXT("Exported step %d/%d"), Job->CurrentStep, Job->NumSteps));
		if (Job->CurrentStep >= Job->NumSteps)
		{
			FinishBakeJob(Job);
		}
		else
		{
			StartBakeStep(Job);
		}
	}

	void RunBakeTOD(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		int32 NumSteps = 8;
		bool bQuitWhenDone = false;
		bool bUseGPU = false;
		bool bNumStepsSpecified = false;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (Arg.Equals(TEXT("gpu"), ESearchCase::IgnoreCase))
			{
				bUseGPU = true;
			}
			else
			{
				if (NumericArgIndex == 0)
				{
					NumSteps = FMath::Clamp(FCString::Atoi(*Arg), 2, 64);
					bNumStepsSpecified = true;
				}
				++NumericArgIndex;
			}
		}

		NDGI::Editor::Pipeline::Begin(NDGI::Editor::Pipeline::EStage::Baking,
			TEXT("Baking time-of-day"), NumSteps, TEXT("Preparing bake..."));

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogNDGI, Error, TEXT("ndgi.BakeTOD requires an open editor world."));
			NDGI::Editor::Pipeline::Fail(TEXT("No editor world is open."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		// World Partition: force-load every cell so the bake scan sees all static
		// primitives and their per-cell built-data packages.
		ForceLoadAllWorldPartitionCells(World);

		// Shared time-of-day profile: an ANDGIActor's profile wins; otherwise use
		// the project default. The profile drives sun/sky keyframes and defaults.
		UNDGITimeOfDayProfile* TODProfile = nullptr;
		for (TActorIterator<ANDGIActor> It(World); It; ++It)
		{
			if (It->Profile)
			{
				TODProfile = It->Profile;
				break;
			}
		}
		if (!TODProfile)
		{
			TODProfile = UNDGITimeOfDayProfile::LoadDefaultProfile();
		}
		if (TODProfile && !bNumStepsSpecified)
		{
			NumSteps = FMath::Clamp(TODProfile->BakeSteps, 2, 64);
			NDGI::Editor::Pipeline::SetProgress(0, NumSteps,
				FString::Printf(TEXT("TOD profile: %d slices"), NumSteps));
		}
		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: %s."),
			TODProfile
				? *FString::Printf(TEXT("using TOD profile '%s' (%d steps)"), *TODProfile->GetName(), NumSteps)
				: TEXT("no TOD profile; using the legacy sun arc"));

		TArray<ADirectionalLight*> Suns;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			Suns.Add(*It);
		}
		if (Suns.Num() == 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("ndgi.BakeTOD: no directional light found; add a sun to the level first."));
			NDGI::Editor::Pipeline::Fail(TEXT("No directional light found in the level."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: baking %d time-of-day steps (%s)..."),
			NumSteps, bUseGPU ? TEXT("GPULightmass") : TEXT("CPU Lightmass"));

		// Stationary keeps direct+indirect lighting on static geometry in the lightmap that
		// NDGI updates, while still casting real-time directional shadows on movable objects
		// (the same shadows a Movable sun would cast). Lightmaps include only Static/Stationary lights.
		for (ADirectionalLight* Sun : Suns)
		{
			ULightComponent* LightComponent = Sun->GetLightComponent();
			if (LightComponent && LightComponent->Mobility != EComponentMobility::Stationary)
			{
				UE_LOG(LogNDGI, Warning, TEXT("ndgi.BakeTOD: sun '%s' is %s; switching it to Stationary so its direct lighting is baked for static geometry and movable objects keep dynamic shadows."),
					*Sun->GetActorNameOrLabel(),
					LightComponent->Mobility == EComponentMobility::Static ? TEXT("Static") : TEXT("Movable"));
				LightComponent->Modify();
				LightComponent->SetMobility(EComponentMobility::Stationary);
			}
		}
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			USkyLightComponent* SkyComponent = It->GetLightComponent();
			if (SkyComponent && SkyComponent->Mobility == EComponentMobility::Movable)
			{
				UE_LOG(LogNDGI, Warning, TEXT("ndgi.BakeTOD: SkyLight '%s' is Movable; switching it to Stationary so it contributes to baked lighting."),
					*It->GetActorNameOrLabel());
				SkyComponent->Modify();
				SkyComponent->SetMobility(EComponentMobility::Stationary);
				SkyComponent->SetCaptureIsDirty();
			}
		}
		World->UpdateAllSkyCaptures();

		// Diagnose the exact world/WorldSettings the bake will use.
		AWorldSettings* WorldSettings = World->GetWorldSettings();
		IConsoleVariable* AllowStaticLighting = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AllowStaticLighting"));
		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: world '%s', WorldSettings '%s', bForceNoPrecomputedLighting=%d, r.AllowStaticLighting=%d."),
			*World->GetName(),
			WorldSettings ? *WorldSettings->GetName() : TEXT("none"),
			WorldSettings ? static_cast<int32>(WorldSettings->bForceNoPrecomputedLighting) : -1,
			AllowStaticLighting ? AllowStaticLighting->GetInt() : -1);

		if (AllowStaticLighting && AllowStaticLighting->GetInt() == 0)
		{
			UE_LOG(LogNDGI, Error,
				TEXT("ndgi.BakeTOD: static lighting is disabled (r.AllowStaticLighting=0). Enable Project Settings > Rendering > Allow Static Lighting and restart the editor."));
			NDGI::Editor::Pipeline::Fail(TEXT("Static lighting is disabled (r.AllowStaticLighting=0)."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		// Static lighting builds are skipped entirely if the level opts out of precomputed lighting.
		if (WorldSettings && WorldSettings->bForceNoPrecomputedLighting)
		{
			UE_LOG(LogNDGI, Warning,
				TEXT("ndgi.BakeTOD: 'Force No Precomputed Lighting' was enabled on WorldSettings '%s'; disabling it so lightmaps can be baked. Leave it off for levels that use NDGI."),
				*WorldSettings->GetName());
			WorldSettings->Modify();
			WorldSettings->bForceNoPrecomputedLighting = false;
			World->MarkPackageDirty();
		}

		UE_LOG(LogNDGI, Display, TEXT("ndgi.BakeTOD: bForceNoPrecomputedLighting is now %d."),
			WorldSettings ? static_cast<int32>(WorldSettings->bForceNoPrecomputedLighting) : -1);

		// NDGI runtime write-back requires uncompressed HQ lightmaps (RGBA16F).
		if (WorldSettings && WorldSettings->LightmassSettings.bCompressLightmaps)
		{
			UE_LOG(LogNDGI, Warning,
				TEXT("ndgi.BakeTOD: 'Compress Lightmaps' was enabled on WorldSettings '%s'; disabling it so HQ lightmaps are uncompressed RGBA16F for runtime NDGI updates."),
				*WorldSettings->GetName());
			WorldSettings->Modify();
			WorldSettings->LightmassSettings.bCompressLightmaps = false;
			World->MarkPackageDirty();
		}

		TSharedRef<FNDGIBakeJob> Job = MakeShared<FNDGIBakeJob>();
		Job->World = World;
		Job->NumSteps = NumSteps;
		Job->Profile = TODProfile;
		Job->bUseGPU = bUseGPU;
		Job->bQuitWhenDone = bQuitWhenDone;
		for (ADirectionalLight* Sun : Suns)
		{
			Job->Suns.Add(Sun);
		}

		// Drive the job from the editor ticker: CPU Lightmass and GPULightmass both advance on
		// editor ticks, so blocking the game thread here would stall the import forever.
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Job](float DeltaSeconds) -> bool
		{
			if (Job->bFinished)
			{
				return false;
			}
			if (!Job->bWaiting)
			{
				return true;
			}

			const double Now = FPlatformTime::Seconds();
			bool bStepDone = false;

			if (Job->bUseGPU)
			{
				UGPULightmassSubsystem* Subsystem = Job->World ? Job->World->GetSubsystem<UGPULightmassSubsystem>() : nullptr;
				if (!Subsystem)
				{
					Job->bHadFailure = true;
					FinishBakeJob(Job);
					return false;
				}
				if (Job->bGPUFinished)
				{
					// In FullBake mode GPULightmass already applied the finished lightmaps before
					// broadcasting completion; calling Save() again would re-apply after tile
					// eviction and write empty (null) lightmaps. Only BakeWhatYouSee needs Save().
					Subsystem->OnLightBuildEnded().Remove(Job->GPUDelegateHandle);
					if (UGPULightmassSettings* Settings = Subsystem->GetSettings())
					{
						if (Settings->Mode == EGPULightmassMode::BakeWhatYouSee)
						{
							Subsystem->Save();
						}
					}
					Subsystem->Stop();
					bStepDone = true;
				}
				else if (!Subsystem->IsRunning())
				{
					// Cancelled or failed before reporting completion.
					Subsystem->OnLightBuildEnded().Remove(Job->GPUDelegateHandle);
					Job->bHadFailure = true;
					bStepDone = true;
				}
			}
			else
			{
				bStepDone = GUnrealEd ? !GUnrealEd->IsLightingBuildCurrentlyRunning() : true;
			}

			if (!bStepDone)
			{
				if (Now - Job->StepStartSeconds > 900.0)
				{
					UE_LOG(LogNDGI, Error, TEXT("ndgi.BakeTOD: timed out waiting for the lighting build at step %d."), Job->CurrentStep + 1);
					Job->bHadFailure = true;
					if (Job->bUseGPU)
					{
						if (UGPULightmassSubsystem* Subsystem = Job->World->GetSubsystem<UGPULightmassSubsystem>())
						{
							Subsystem->Stop();
						}
					}
					FinishBakeJob(Job);
					return false;
				}
				return true;
			}

			CompleteBakeStep(Job);
			return !Job->bFinished;
		}), 0.0f);

		StartBakeStep(Job);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIBakeTODCommand(
	TEXT("ndgi.BakeTOD"),
	TEXT("Bakes the current level at N time-of-day states and exports the lightmaps to Saved/NDGI/Bake. Usage: ndgi.BakeTOD <steps> [gpu] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunBakeTOD));

// Force rebuild marker: bake world-settings diagnostics.

// Copyright Epic Games, Inc. All Rights Reserved.

#include "Components/LightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Engine.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/SkyLight.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GameFramework/PlayerStart.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "LightMap.h"
#include "Misc/PackageName.h"
#include "NDGI.h"
#include "NDGILevelAsset.h"
#include "StaticMeshComponentLODInfo.h"
#include "UObject/UObjectIterator.h"

namespace
{
	const TCHAR* MobilityToString(EComponentMobility::Type Mobility)
	{
		switch (Mobility)
		{
		case EComponentMobility::Static: return TEXT("Static");
		case EComponentMobility::Stationary: return TEXT("Stationary");
		case EComponentMobility::Movable: return TEXT("Movable");
		default: return TEXT("Unknown");
		}
	}

	void FinishSetupCommand(bool bQuitWhenDone)
	{
		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void RunInspectBenchmarkScene(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogNDGI, Error, TEXT("InspectBenchmarkScene: no editor world."));
			return;
		}

		AWorldSettings* Settings = World->GetWorldSettings();
		UE_LOG(LogNDGI, Display, TEXT("Inspect: world '%s' (package '%s')."), *World->GetName(), *World->GetOutermost()->GetName());
		UE_LOG(LogNDGI, Display, TEXT("Inspect: bForceNoPrecomputedLighting=%d, CompressLightmaps=%d."),
			Settings ? static_cast<int32>(Settings->bForceNoPrecomputedLighting) : -1,
			Settings ? static_cast<int32>(Settings->LightmassSettings.bCompressLightmaps) : -1);

		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			const ULightComponent* Light = It->GetLightComponent();
			const FRotator Rotation = It->GetActorRotation();
			UE_LOG(LogNDGI, Display, TEXT("Inspect: DirectionalLight '%s' mobility=%s rotation=(%.1f, %.1f, %.1f)."),
				*It->GetActorNameOrLabel(), MobilityToString(Light ? Light->Mobility.GetValue() : EComponentMobility::Movable),
				Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
		}
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			const USkyLightComponent* Sky = It->GetLightComponent();
			UE_LOG(LogNDGI, Display, TEXT("Inspect: SkyLight '%s' mobility=%s."),
				*It->GetActorNameOrLabel(), MobilityToString(Sky ? Sky->Mobility.GetValue() : EComponentMobility::Movable));
		}

		int32 MeshCount = 0;
		int32 WithLODData = 0;
		int32 WithBuildData = 0;
		int32 WithLightmap = 0;
		TSet<UTexture2D*> LightmapTextures;
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World)
			{
				continue;
			}
			++MeshCount;
			WithLODData += Component->LODData.Num() > 0 ? 1 : 0;
			const FMeshMapBuildData* BuildData = Component->LODData.Num() > 0
				? Component->GetMeshMapBuildData(Component->LODData[0])
				: nullptr;
			if (BuildData)
			{
				++WithBuildData;
				const FLightMap2D* LightMap2D = BuildData->LightMap ? BuildData->LightMap->GetLightMap2D() : nullptr;
				if (LightMap2D && LightMap2D->IsValid(0))
				{
					++WithLightmap;
					if (UTexture2D* Texture = const_cast<UTexture2D*>(LightMap2D->GetTexture(0)))
					{
						LightmapTextures.Add(Texture);
					}
				}
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("Inspect: %d static mesh component(s), %d with LODData, %d with build data, %d with an HQ lightmap."),
			MeshCount, WithLODData, WithBuildData, WithLightmap);

		TArray<UTexture2D*> SortedTextures = LightmapTextures.Array();
		SortedTextures.Sort([](const UTexture2D& A, const UTexture2D& B)
		{
			return A.GetPathName() < B.GetPathName();
		});
		for (int32 Index = 0; Index < SortedTextures.Num(); ++Index)
		{
			UTexture2D* Texture = SortedTextures[Index];
			FTexturePlatformData* PlatformData = Texture->GetPlatformData();
			const TCHAR* FormatName = PlatformData ? GPixelFormats[PlatformData->PixelFormat].Name : TEXT("none");
			const int32 SizeX = (PlatformData && PlatformData->Mips.Num() > 0) ? PlatformData->Mips[0].SizeX : 0;
			const int32 SizeY = (PlatformData && PlatformData->Mips.Num() > 0) ? PlatformData->Mips[0].SizeY : 0;
			UE_LOG(LogNDGI, Display, TEXT("Inspect: lightmap[%d] '%s' %dx%d %s."), Index, *Texture->GetName(), SizeX, SizeY, FormatName);
		}

		int32 PlayerStartCount = 0;
		for (TActorIterator<APlayerStart> It(World); It; ++It)
		{
			if (PlayerStartCount == 0)
			{
				const FVector Location = It->GetActorLocation();
				const FRotator Rotation = It->GetActorRotation();
				UE_LOG(LogNDGI, Display, TEXT("Inspect: PlayerStart '%s' location=(%.1f, %.1f, %.1f) rotation=(%.1f, %.1f, %.1f)."),
					*It->GetActorNameOrLabel(), Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
			}
			++PlayerStartCount;
		}
		UE_LOG(LogNDGI, Display, TEXT("Inspect: %d PlayerStart(s)."), PlayerStartCount);

		const FString LevelName = UWorld::RemovePIEPrefix(World->GetName());
		const FString LevelAssetPackage = FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s"), *LevelName);
		UE_LOG(LogNDGI, Display, TEXT("Inspect: level asset %s %s."),
			*LevelAssetPackage, FPackageName::DoesPackageExist(LevelAssetPackage) ? TEXT("exists") : TEXT("missing"));
	}

	void ConfigureDynamicWorld(UWorld* World)
	{
		int32 Suns = 0;
		for (TActorIterator<ADirectionalLight> It(World); It; ++It)
		{
			ULightComponent* Light = It->GetLightComponent();
			if (Light)
			{
				Light->Modify();
				Light->SetMobility(EComponentMobility::Movable);
				++Suns;
			}
		}

		int32 Skies = 0;
		for (TActorIterator<ASkyLight> It(World); It; ++It)
		{
			USkyLightComponent* Sky = It->GetLightComponent();
			if (Sky)
			{
				Sky->Modify();
				Sky->SetMobility(EComponentMobility::Movable);
				Sky->bRealTimeCapture = true;
				Sky->SetCaptureIsDirty();
				++Skies;
			}
		}

		int32 Cleared = 0;
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* Component = *It;
			if (!Component || Component->GetWorld() != World)
			{
				continue;
			}
			if (Component->LODData.Num() > 0)
			{
				Component->Modify();
				Component->SetLODDataCount(0, 0);
				Component->MarkRenderStateDirty();
				++Cleared;
			}
		}

		if (AWorldSettings* Settings = World->GetWorldSettings())
		{
			Settings->Modify();
			Settings->bForceNoPrecomputedLighting = true;
		}
		World->MarkPackageDirty();

		UE_LOG(LogNDGI, Display,
			TEXT("SetupBenchmarkMaps: dynamic copy: %d sun(s) movable, %d sky(s) movable, cleared lightmap data on %d component(s), precomputed lighting disabled."),
			Suns, Skies, Cleared);
	}

	void RunSetupBenchmarkMaps(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		const bool bQuitWhenDone = Args.ContainsByPredicate([](const FString& Arg)
		{
			return Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase);
		});

		auto Fail = [bQuitWhenDone](const TCHAR* Message)
		{
			UE_LOG(LogNDGI, Error, TEXT("%s"), Message);
			FinishSetupCommand(bQuitWhenDone);
		};

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: no editor world."));
			return;
		}

		const FString SourcePackage = World->GetOutermost()->GetName();
		const FString DynamicPackage = TEXT("/Game/NDGI/Bench/Lvl_Bench_Dynamic");
		const FString NDGIPackage = TEXT("/Game/NDGI/Bench/Lvl_Bench_NDGI");

		if (SourcePackage == DynamicPackage || SourcePackage == NDGIPackage)
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: open the source level (for example /Game/Maps/Lvl_ThirdPerson1), not a benchmark copy."));
			return;
		}

		// SaveMap does not overwrite in unattended mode; remove previous benchmark copies.
		for (const FString& Package : { DynamicPackage, NDGIPackage })
		{
			const FString MapFile = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension());
			const FString BuiltDataFile = FPackageName::LongPackageNameToFilename(Package + TEXT("_BuiltData"), FPackageName::GetAssetPackageExtension());
			IFileManager::Get().Delete(*MapFile, false, true, true);
			IFileManager::Get().Delete(*BuiltDataFile, false, true, true);
		}

		const FString SourceFilename = FPackageName::LongPackageNameToFilename(SourcePackage, FPackageName::GetMapPackageExtension());

		UWorld* DynamicWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(SourceFilename, false);
		if (!DynamicWorld)
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: failed to load the source level as a template."));
			return;
		}
		ConfigureDynamicWorld(DynamicWorld);
		if (!UEditorLoadingAndSavingUtils::SaveMap(DynamicWorld, DynamicPackage))
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: failed to save the dynamic benchmark copy."));
			return;
		}
		UE_LOG(LogNDGI, Display, TEXT("SetupBenchmarkMaps: created %s (movable sun + Lumen, no baked lightmaps)."), *DynamicPackage);

		UWorld* NDGIWorld = UEditorLoadingAndSavingUtils::NewMapFromTemplate(SourceFilename, false);
		if (!NDGIWorld)
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: failed to create the NDGI copy."));
			return;
		}
		if (AWorldSettings* Settings = NDGIWorld->GetWorldSettings())
		{
			Settings->Modify();
			Settings->bForceNoPrecomputedLighting = false;
		}
		NDGIWorld->MarkPackageDirty();
		if (!UEditorLoadingAndSavingUtils::SaveMap(NDGIWorld, NDGIPackage))
		{
			Fail(TEXT("ndgi.SetupBenchmarkMaps: failed to save the NDGI benchmark copy."));
			return;
		}
		UE_LOG(LogNDGI, Display, TEXT("SetupBenchmarkMaps: created %s (static lighting, NDGI target)."), *NDGIPackage);
		UE_LOG(LogNDGI, Display,
			TEXT("SetupBenchmarkMaps: next, bake/train the NDGI copy and run Tools/RunNDGIBenchmark.ps1."));

		FinishSetupCommand(bQuitWhenDone);
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIInspectBenchmarkSceneCommand(
	TEXT("ndgi.InspectBenchmarkScene"),
	TEXT("Logs lighting/lightmap/mobility state of the current editor level (benchmark setup diagnostics)."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunInspectBenchmarkScene));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGISetupBenchmarkMapsCommand(
	TEXT("ndgi.SetupBenchmarkMaps"),
	TEXT("Copies the current level into /Game/NDGI/Bench (dynamic + NDGI variants). Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunSetupBenchmarkMaps));

// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGILevelActor.h"

#include "Components/LightComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SkyLightComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Level.h"
#include "Engine/SkyLight.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "NDGIAtlasDecoder.h"
#include "NDGILevelAsset.h"
#include "NDGIRuntime.h"
#include "NDGITimeOfDayProfile.h"
#include "Net/UnrealNetwork.h"

namespace
{
	/** Stable per-streaming-level ID shared with the bake/export cell metadata. */
	TSet<FString> GatherLoadedCellPackages(UWorld* World)
	{
		TSet<FString> Cells;
		if (!World)
		{
			return Cells;
		}
		for (ULevel* Level : World->GetLevels())
		{
			if (Level)
			{
				Cells.Add(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
			}
		}
		return Cells;
	}

	FVector GetStreamingCameraLocation(UWorld* World, const AActor* Fallback)
	{
		if (World)
		{
			if (APlayerController* PlayerController = World->GetFirstPlayerController())
			{
				if (APlayerCameraManager* CameraManager = PlayerController->PlayerCameraManager)
				{
					return CameraManager->GetCameraLocation();
				}
				if (const APawn* Pawn = PlayerController->GetPawn())
				{
					return Pawn->GetActorLocation();
				}
			}
		}
		return Fallback ? Fallback->GetActorLocation() : FVector::ZeroVector;
	}
}

ANDGIActor::ANDGIActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.0f;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	bReplicates = true;
	NetUpdateFrequency = 10.0f;
}

void ANDGIActor::SetTimeOfDay(float NewTime)
{
	NewTime = FMath::Clamp(NewTime, 0.0f, 1.0f);

	// Clients drive the shared time through the server so all peers see the
	// same keyframe. Standalone/listen-server calls apply locally.
	if (!HasAuthority() && GetNetMode() != NM_Standalone)
	{
		ServerSetTimeOfDay(NewTime);
		return;
	}

	TimeOfDay = NewTime;
	ApplyTimeOfDay();
}

void ANDGIActor::ServerSetTimeOfDay_Implementation(float NewTime)
{
	SetTimeOfDay(NewTime);
}

void ANDGIActor::OnRep_TimeOfDay()
{
	ApplyTimeOfDay();
}

void ANDGIActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME(ANDGIActor, TimeOfDay);
	DOREPLIFETIME(ANDGIActor, bAutoCycle);
	DOREPLIFETIME(ANDGIActor, Profile);
}

void ANDGIActor::UpdateSunRotation()
{
	if (!bDriveSunRotation)
	{
		return;
	}

	ADirectionalLight* Sun = SunLight;
	if (!Sun && GetWorld())
	{
		for (TActorIterator<ADirectionalLight> It(GetWorld()); It; ++It)
		{
			Sun = *It;
			break;
		}
	}
	if (!Sun)
	{
		return;
	}

	const bool bUseProfile = Profile != nullptr;
	FNDGITimeOfDayKey Key;
	if (bUseProfile)
	{
		Key = Profile->Evaluate(TimeOfDay);
	}
	else
	{
		Key.SunPitch = 10.0f + 70.0f * FMath::Sin(PI * TimeOfDay);
		Key.SunYaw = Sun->GetActorRotation().Yaw;
	}

	ULightComponent* Light = Sun->GetLightComponent();
	if (!Light || Light->Mobility == EComponentMobility::Static)
	{
		return;
	}

	FRotator Rotation = Sun->GetActorRotation();
	// Profile pitch is sun elevation above the horizon. UE's light forward
	// vector points from the sun toward the ground, so the actor rotation
	// pitch is the negated elevation.
	Rotation.Pitch = -Key.SunPitch;
	Rotation.Yaw = Key.SunYaw;
	Sun->SetActorRotation(Rotation);

	if (bUseProfile)
	{
		Light->SetLightColor(Key.SunColor, false);
		Light->SetIntensity(Key.SunIntensity);

		if (GetWorld())
		{
			for (TActorIterator<ASkyLight> It(GetWorld()); It; ++It)
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
	}
}

void ANDGIActor::StartDecode()
{
	if (bDecodeInFlight || !DecodeCache.IsValid())
	{
		return;
	}
	if (DecodeCache->ActiveAtlases == 0)
	{
		bDecodePending = false;
		return;
	}

	bDecodePending = false;
	bDecodeInFlight = true;
	LastDecodeSeconds = FPlatformTime::Seconds();

	const float RequestedTime = TimeOfDay;
	TWeakObjectPtr<ANDGIActor> WeakThis(this);
	const bool bStarted = NDGI::Runtime::DecodeCachedLevel(DecodeCache, RequestedTime, nullptr,
		[WeakThis, RequestedTime]()
		{
			if (ANDGIActor* Actor = WeakThis.Get())
			{
				Actor->bDecodeInFlight = false;
				++Actor->CompletedDecodes;
				if (!FMath::IsNearlyEqual(Actor->TimeOfDay, RequestedTime, KINDA_SMALL_NUMBER))
				{
					Actor->bDecodePending = true;
				}
			}
		});
	if (!bStarted)
	{
		bDecodeInFlight = false;
		UE_LOG(LogNDGIRuntime, Warning, TEXT("ANDGIActor '%s': cached decode failed to enqueue."), *GetName());
	}
}

void ANDGIActor::ApplyTimeOfDay()
{
	if (!Profile)
	{
		Profile = UNDGITimeOfDayProfile::LoadDefaultProfile();
	}
	UpdateSunRotation();
	if (!LevelAsset)
	{
		UE_LOG(LogNDGIRuntime, Warning, TEXT("ANDGIActor '%s' has no LevelAsset assigned."), *GetName());
		return;
	}
	EnsureDecodeCache();
	bDecodePending = true;

	// A chunked build is in progress; decode once it finishes (Tick).
	if (CacheBuild.IsValid())
	{
		return;
	}

	if (!DecodeCache.IsValid())
	{
		// Cache build failed; keep the legacy path working synchronously.
		if (!NDGI::Runtime::DecodeLevelAsset(LevelAsset, TimeOfDay))
		{
			UE_LOG(LogNDGIRuntime, Warning, TEXT("ANDGIActor '%s': no atlas was decoded (check the level asset)."), *GetName());
		}
		return;
	}

	if (!bDecodeInFlight
		&& FPlatformTime::Seconds() - LastDecodeSeconds >= static_cast<double>(DecodeIntervalSeconds))
	{
		StartDecode();
	}
}

void ANDGIActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	if (bAutoCycle && HasAuthority())
	{
		const float Duration = CycleDurationSeconds > 0.0f
			? CycleDurationSeconds
			: (Profile ? Profile->CycleDurationSeconds : 120.0f);
		if (Duration > KINDA_SMALL_NUMBER && DeltaSeconds > 0.0f)
		{
			float NewTime = TimeOfDay;
			if (Profile && Profile->bPingPong)
			{
				NewTime += CycleDirection * DeltaSeconds / Duration;
				while (NewTime > 1.0f || NewTime < 0.0f)
				{
					if (NewTime > 1.0f)
					{
						NewTime = 2.0f - NewTime;
						CycleDirection = -1;
					}
					else
					{
						NewTime = -NewTime;
						CycleDirection = 1;
					}
				}
			}
			else
			{
				NewTime = FMath::Fmod(NewTime + DeltaSeconds / Duration, 1.0f);
				if (NewTime < 0.0f)
				{
					NewTime += 1.0f;
				}
			}
			SetTimeOfDay(NewTime);
		}
	}
	else if (!bAutoCycle)
	{
		UpdateSunRotation();
	}

	if (CacheBuild.IsValid())
	{
		TSharedPtr<NDGI::Runtime::FLevelDecodeCache> NewCache;
		double BuildMs = 0.0;
		int32 SkippedModels = 0;
		if (NDGI::Runtime::TickBuildLevelDecodeCacheAsync(CacheBuild, NewCache, &BuildMs, &SkippedModels))
		{
			const FString BuildError = CacheBuild.IsValid() ? CacheBuild->Error : FString();
			if (NewCache.IsValid())
			{
				DecodeCache = NewCache;
				CachedLevelAsset = LevelAsset.Get();
				DecodeCache->UpdateStreaming(GetStreamingCameraLocation(GetWorld(), this));
				UE_LOG(LogNDGIRuntime, Display,
					TEXT("ANDGIActor '%s': built decode cache in %.1f ms (%d atlas(es), %d active, %d model(s), %.1f MB params%s)."),
					*GetName(), BuildMs, DecodeCache->Entries.Num(), DecodeCache->ActiveAtlases, DecodeCache->Tiles,
					DecodeCache->ParameterBytes / (1024.0 * 1024.0),
					SkippedModels > 0 ? *FString::Printf(TEXT(", %d skipped"), SkippedModels) : TEXT(""));
			}
			else
			{
				UE_LOG(LogNDGIRuntime, Warning,
					TEXT("ANDGIActor '%s': could not build the decode cache (%s); falling back to per-decode packing."),
					*GetName(), BuildError.IsEmpty() ? TEXT("unknown error") : *BuildError);
			}
			CacheBuild.Reset();
			bDecodePending = true;
		}
	}

	// World Partition: activate atlases as their streaming levels load and release
	// GPU resources when no loaded cell references them.
	const double Now = FPlatformTime::Seconds();
	if (DecodeCache.IsValid() && !CacheBuild.IsValid() && !bDecodeInFlight
		&& Now - LastStreamingCheckSeconds >= 0.5)
	{
		LastStreamingCheckSeconds = Now;
		const TSet<FString> LoadedCells = GatherLoadedCellPackages(GetWorld());
		const bool bCellsDiffer = DecodeCache->ActiveCells.Num() != LoadedCells.Num()
			|| !DecodeCache->ActiveCells.Includes(LoadedCells);
		if (LoadedCells.Num() > 0 && bCellsDiffer)
		{
			if (DecodeCache->UpdateActiveCells(LoadedCells))
			{
				bDecodePending = true;
				UE_LOG(LogNDGIRuntime, Display,
					TEXT("ANDGIActor '%s': streaming changed; %d/%d atlas(es) active, GPU %.1f MB, %d loaded level(s)."),
					*GetName(), DecodeCache->ActiveAtlases, DecodeCache->Entries.Num(),
					DecodeCache->ActiveGPUBytes / (1024.0 * 1024.0), LoadedCells.Num());
			}
		}
	}

	// Virtual-texture page cache: keep the nearest model pages resident in a
	// bounded physical BC7 pool. Updates are throttled and flushed as a group.
	if (DecodeCache.IsValid() && !CacheBuild.IsValid() && !bDecodeInFlight
		&& Now - DecodeCache->LastStreamingUpdateSeconds >= NDGI::Runtime::GetStreamingUpdateIntervalSeconds())
	{
		const FVector CameraLocation = GetStreamingCameraLocation(GetWorld(), this);
		if (DecodeCache->UpdateStreaming(CameraLocation))
		{
			bDecodePending = true;
			UE_LOG(LogNDGIRuntime, Display,
				TEXT("ANDGIActor '%s': VT page cache updated at (%.0f %.0f %.0f); %d resident model page(s), %d upload(s), GPU %.1f MB."),
				*GetName(), CameraLocation.X, CameraLocation.Y, CameraLocation.Z,
				DecodeCache->ResidentModels, DecodeCache->StreamingUploads,
				DecodeCache->ActiveGPUBytes / (1024.0 * 1024.0));
		}
	}

	if (bDecodePending && !bDecodeInFlight
		&& FPlatformTime::Seconds() - LastDecodeSeconds >= static_cast<double>(DecodeIntervalSeconds))
	{
		StartDecode();
	}
}

void ANDGIActor::EnsureDecodeCache()
{
	if (!LevelAsset)
	{
		DecodeCache.Reset();
		CachedLevelAsset.Reset();
		CacheBuild.Reset();
		return;
	}
	if (DecodeCache.IsValid() && CachedLevelAsset.Get() == LevelAsset.Get())
	{
		return;
	}
	if (CacheBuild.IsValid() && CacheBuild->LevelAsset.Get() == LevelAsset.Get())
	{
		return;
	}

	DecodeCache.Reset();
	CachedLevelAsset.Reset();
	CacheBuild = NDGI::Runtime::BeginBuildLevelDecodeCacheAsync(LevelAsset, GatherLoadedCellPackages(GetWorld()));
	if (!CacheBuild.IsValid())
	{
		UE_LOG(LogNDGIRuntime, Warning,
			TEXT("ANDGIActor '%s': could not start the decode-cache build; falling back to per-decode packing."), *GetName());
	}
}

void ANDGIActor::BeginPlay()
{
	Super::BeginPlay();
	if (!Profile)
	{
		Profile = UNDGITimeOfDayProfile::LoadDefaultProfile();
	}
	ApplyTimeOfDay();
}

#if WITH_EDITOR
void ANDGIActor::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	const FName PropertyName = PropertyChangedEvent.GetPropertyName();
	if (PropertyName == GET_MEMBER_NAME_CHECKED(ANDGIActor, TimeOfDay)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ANDGIActor, LevelAsset)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ANDGIActor, Profile)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ANDGIActor, bAutoCycle)
		|| PropertyName == GET_MEMBER_NAME_CHECKED(ANDGIActor, CycleDurationSeconds))
	{
		ApplyTimeOfDay();
	}
}
#endif

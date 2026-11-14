// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "NDGILevelActor.generated.h"

class UNDGILevelAsset;
class UNDGITimeOfDayProfile;
class ADirectionalLight;

namespace NDGI
{
	namespace Runtime
	{
		struct FLevelDecodeCache;
		struct FDecodeCacheBuildState;
	}
}

/**
 * Place this actor in a persistent level to drive NDGI lightmap decoding.
 * Move the TimeOfDay slider (Details panel or Blueprint) to update the level's
 * decoded lightmaps; the slider is the manual time source.
 */
UCLASS()
class NDGIRUNTIME_API ANDGIActor : public AActor
{
	GENERATED_BODY()

public:
	ANDGIActor();

	UPROPERTY(EditAnywhere, Category = "NDGI")
	TObjectPtr<UNDGILevelAsset> LevelAsset = nullptr;

	/** Shared stylized keyframes used by both the bake and runtime. */
	UPROPERTY(Replicated, EditAnywhere, Category = "NDGI")
	TObjectPtr<UNDGITimeOfDayProfile> Profile = nullptr;

	UPROPERTY(ReplicatedUsing = OnRep_TimeOfDay, EditAnywhere, Interp, Category = "NDGI", meta = (UIMin = "0", UIMax = "1", ClampMin = "0", ClampMax = "1"))
	float TimeOfDay = 0.0f;

	/** Server-driven auto-cycle. Clients receive TimeOfDay through replication. */
	UPROPERTY(Replicated, EditAnywhere, Category = "NDGI")
	bool bAutoCycle = false;

	/**
	 * Seconds for one full cycle. 0 = use the profile's CycleDurationSeconds
	 * (fallback 120 s when no profile is assigned).
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI", meta = (ClampMin = "0"))
	float CycleDurationSeconds = 0.0f;

	/** Optional. When null, the first DirectionalLight in the world is used. */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	TObjectPtr<ADirectionalLight> SunLight = nullptr;

	/**
	 * Rotates a Movable/Stationary sun with the same arc used by ndgi.BakeTOD so its
	 * dynamic shadows on movable objects match the decoded time slice.
	 */
	UPROPERTY(EditAnywhere, Category = "NDGI")
	bool bDriveSunRotation = true;

	/** Minimum seconds between lightmap decodes (10-20 Hz). The sun still rotates every frame. */
	UPROPERTY(EditAnywhere, Category = "NDGI", meta = (ClampMin = "0.05", ClampMax = "0.1"))
	float DecodeIntervalSeconds = 0.05f;

	UFUNCTION(BlueprintCallable, Category = "NDGI")
	void SetTimeOfDay(float NewTime);

	UFUNCTION(BlueprintCallable, Category = "NDGI")
	void ApplyTimeOfDay();

	/** Number of lightmap decodes that have completed. Useful for cadence tests. */
	UFUNCTION(BlueprintPure, Category = "NDGI")
	int32 GetCompletedDecodeCount() const { return CompletedDecodes; }

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

#if WITH_EDITOR
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

private:
	void EnsureDecodeCache();
	void StartDecode();
	void UpdateSunRotation();

	UFUNCTION()
	void OnRep_TimeOfDay();

	UFUNCTION(Server, Reliable)
	void ServerSetTimeOfDay(float NewTime);

	TSharedPtr<NDGI::Runtime::FLevelDecodeCache> DecodeCache;
	TSharedPtr<NDGI::Runtime::FDecodeCacheBuildState> CacheBuild;
	TWeakObjectPtr<UNDGILevelAsset> CachedLevelAsset;
	bool bDecodeInFlight = false;
	bool bDecodePending = false;
	double LastDecodeSeconds = 0.0;
	double LastStreamingCheckSeconds = 0.0;
	int32 CompletedDecodes = 0;
	int32 CycleDirection = 1;
};

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "NDGITimeOfDayProfile.generated.h"

/** One stylized time-of-day keyframe shared by the bake and the runtime actor. */
USTRUCT(BlueprintType)
struct FNDGITimeOfDayKey
{
	GENERATED_BODY()

	/** Normalized time in [0,1]. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI", meta = (ClampMin = "0", ClampMax = "1"))
	float Time = 0.0f;

	/** Sun elevation above the horizon in degrees. Positive is above the horizon. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI")
	float SunPitch = 10.0f;

	/** Sun azimuth in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI")
	float SunYaw = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI")
	FLinearColor SunColor = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI", meta = (ClampMin = "0"))
	float SunIntensity = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI")
	FLinearColor SkyColor = FLinearColor(0.35f, 0.45f, 0.6f, 1.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "NDGI", meta = (ClampMin = "0"))
	float SkyIntensity = 1.0f;
};

/**
 * Stylized time-of-day profile. Used by both ndgi.BakeTOD (sun/sky at every
 * time slice) and ANDGIActor (sun/sky at runtime), so the baked and dynamic
 * lighting follow the same keyframes.
 */
UCLASS(BlueprintType)
class NDGIRUNTIME_API UNDGITimeOfDayProfile : public UDataAsset
{
	GENERATED_BODY()

public:
	/** Keyframes sorted by Time. Missing endpoints are wrapped. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI")
	TArray<FNDGITimeOfDayKey> Keyframes;

	/** Seconds for one full 0..1 cycle when auto-cycle is enabled. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI", meta = (ClampMin = "0.1"))
	float CycleDurationSeconds = 120.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI")
	bool bPingPong = false;

	/** Defaults used by ndgi.BakeTOD and the pipeline panel. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI", meta = (ClampMin = "2", ClampMax = "64"))
	int32 BakeSteps = 8;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI", meta = (ClampMin = "16", ClampMax = "512"))
	int32 TileSize = 128;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI", meta = (ClampMin = "1"))
	int32 TrainSteps = 3000;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "NDGI", meta = (ClampMin = "0", ClampMax = "64"))
	int32 OverlapTexels = 8;

	/** Returns the interpolated keyframe at NormalizedTime (wraps at 0/1). */
	UFUNCTION(BlueprintPure, Category = "NDGI")
	FNDGITimeOfDayKey Evaluate(float NormalizedTime) const;

	/** Loads the project's default profile (/Game/NDGI/TOD/NDGITODProfile), if present. */
	static UNDGITimeOfDayProfile* LoadDefaultProfile();
};

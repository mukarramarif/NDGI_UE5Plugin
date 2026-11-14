// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGITimeOfDayProfile.h"

namespace
{
	FNDGITimeOfDayKey LerpKey(const FNDGITimeOfDayKey& A, const FNDGITimeOfDayKey& B, float Alpha)
	{
		Alpha = FMath::Clamp(Alpha, 0.0f, 1.0f);
		FNDGITimeOfDayKey Out;
		Out.Time = FMath::Lerp(A.Time, B.Time, Alpha);
		Out.SunPitch = FMath::Lerp(A.SunPitch, B.SunPitch, Alpha);
		Out.SunYaw = FMath::Lerp(A.SunYaw, B.SunYaw, Alpha);
		Out.SunColor = A.SunColor * (1.0f - Alpha) + B.SunColor * Alpha;
		Out.SunIntensity = FMath::Lerp(A.SunIntensity, B.SunIntensity, Alpha);
		Out.SkyColor = A.SkyColor * (1.0f - Alpha) + B.SkyColor * Alpha;
		Out.SkyIntensity = FMath::Lerp(A.SkyIntensity, B.SkyIntensity, Alpha);
		return Out;
	}
}

FNDGITimeOfDayKey UNDGITimeOfDayProfile::Evaluate(float NormalizedTime) const
{
	const float Time = FMath::Clamp(NormalizedTime, 0.0f, 1.0f);
	if (Keyframes.Num() == 0)
	{
		// Legacy proof-of-concept arc: low in the morning/evening, high at noon.
		FNDGITimeOfDayKey Out;
		Out.Time = Time;
		Out.SunPitch = 10.0f + 70.0f * FMath::Sin(PI * Time);
		Out.SunYaw = 0.0f;
		Out.SunColor = FLinearColor::White;
		Out.SunIntensity = 5.0f;
		Out.SkyColor = FLinearColor(0.35f, 0.45f, 0.6f, 1.0f);
		Out.SkyIntensity = 1.0f;
		return Out;
	}

	TArray<FNDGITimeOfDayKey> Sorted = Keyframes;
	Sorted.Sort([](const FNDGITimeOfDayKey& A, const FNDGITimeOfDayKey& B)
	{
		return A.Time < B.Time;
	});

	if (Sorted.Num() == 1)
	{
		FNDGITimeOfDayKey Out = Sorted[0];
		Out.Time = Time;
		return Out;
	}

	const FNDGITimeOfDayKey& First = Sorted[0];
	const FNDGITimeOfDayKey& Last = Sorted.Last();
	if (Time <= First.Time)
	{
		if (First.Time <= KINDA_SMALL_NUMBER)
		{
			return First;
		}
		const float Span = (1.0f - Last.Time) + First.Time;
		const float Alpha = Span > KINDA_SMALL_NUMBER ? (Time + 1.0f - Last.Time) / Span : 1.0f;
		return LerpKey(Last, First, Alpha);
	}
	if (Time >= Last.Time)
	{
		// Interpolate across the wrap from the last key back to the first.
		const float Span = (1.0f - Last.Time) + First.Time;
		const float Alpha = Span > KINDA_SMALL_NUMBER ? (Time - Last.Time) / Span : 0.0f;
		return LerpKey(Last, First, Alpha);
	}

	for (int32 Index = 0; Index + 1 < Sorted.Num(); ++Index)
	{
		const FNDGITimeOfDayKey& A = Sorted[Index];
		const FNDGITimeOfDayKey& B = Sorted[Index + 1];
		if (Time >= A.Time && Time <= B.Time)
		{
			const float Span = B.Time - A.Time;
			const float Alpha = Span > KINDA_SMALL_NUMBER ? (Time - A.Time) / Span : 0.0f;
			return LerpKey(A, B, Alpha);
		}
	}

	return Last;
}

UNDGITimeOfDayProfile* UNDGITimeOfDayProfile::LoadDefaultProfile()
{
	return LoadObject<UNDGITimeOfDayProfile>(nullptr, TEXT("/Game/NDGI/TOD/NDGITODProfile.NDGITODProfile"));
}

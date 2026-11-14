// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

NDGI_API DECLARE_LOG_CATEGORY_EXTERN(LogNDGI, Log, All);

class FNDGIModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

namespace NDGI
{
	/**
	 * Phase 0 smoke test: dispatches the smoke-test compute shader which writes
	 * Values[i] = i * 2 + Offset, then reads the result back from the GPU.
	 *
	 * OnComplete is always called on the game thread once the readback is ready
	 * (or immediately with bSuccess == false if the dispatch could not be queued).
	 */
	NDGI_API void DispatchSmokeTest(uint32 Count, uint32 Offset, TFunction<void(bool bSuccess, TArray<uint32>&& Values)> OnComplete);
}

// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

NDGIRUNTIME_API DECLARE_LOG_CATEGORY_EXTERN(LogNDGIRuntime, Log, All);

class FNDGIRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};

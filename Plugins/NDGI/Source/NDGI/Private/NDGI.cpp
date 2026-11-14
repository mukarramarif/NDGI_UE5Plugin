// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGI.h"

#define LOCTEXT_NAMESPACE "FNDGIModule"

DEFINE_LOG_CATEGORY(LogNDGI);

void FNDGIModule::StartupModule()
{
	// The /Plugin/NDGI shader directory is mapped by the NDGIRuntime module so packaged
	// builds can resolve shader paths as well (see FNDGIRuntimeModule::StartupModule).
}

void FNDGIModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FNDGIModule, NDGI)

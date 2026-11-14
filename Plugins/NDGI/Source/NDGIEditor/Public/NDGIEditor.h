// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FNDGIEditorModule : public IModuleInterface
{
public:

	/** IModuleInterface implementation */
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:

	/** Console command ndgi.SmokeTest: dispatches and validates the GPU smoke-test kernel. */
	static void SmokeTestConsoleCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice);
};

/** Registers/unregisters the NDGI preview tab. */
void RegisterNDGIPreviewTab();
void UnregisterNDGIPreviewTab();
void OpenNDGIPreviewTab();

/** Registers/unregisters the NDGI Pipeline tab. */
void RegisterNDGIPipelineTab();
void UnregisterNDGIPipelineTab();
void OpenNDGIPipelineTab();

/** Registers/unregisters the NDGI Lightmap Inspector tab. */
void RegisterNDGILightmapInspectorTab();
void UnregisterNDGILightmapInspectorTab();
void OpenNDGILightmapInspectorTab(int32 TexIndex = INDEX_NONE, int32 Slice = INDEX_NONE);

/** Points the preview tab at a model asset (e.g. "/Game/NDGI/Trained/Tile_0.Tile_0"). */
void SetNDGIPreviewModel(const FString& PackageObjectPath);

// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIEditor.h"

#include "Engine/Engine.h"
#include "HAL/IConsoleManager.h"
#include "NDGI.h"
#include "NDGIPipelineStatus.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"

#define LOCTEXT_NAMESPACE "FNDGIEditorModule"

IMPLEMENT_MODULE(FNDGIEditorModule, NDGIEditor)

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGISmokeTestCommand(
	TEXT("ndgi.SmokeTest"),
	TEXT("Dispatches the NDGI smoke-test compute shader and validates the GPU readback. Optional arg: element count (default 4096)."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&FNDGIEditorModule::SmokeTestConsoleCommand));

namespace
{
	void RegisterNDGIMenu()
	{
		if (UToolMenus* Menus = UToolMenus::Get())
		{
			if (UToolMenu* Menu = Menus->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools")))
			{
				FToolMenuSection& Section = Menu->AddSection("NDGI", LOCTEXT("NDGISection", "NDGI"));
				Section.AddMenuEntry(
					"NDGIOpenPipeline",
					LOCTEXT("NDGIOpenPipeline", "NDGI Pipeline"),
					LOCTEXT("NDGIOpenPipelineTooltip", "Bake, merge, train, and validate NDGI lightmaps for the current level"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateStatic(&OpenNDGIPipelineTab)));
				Section.AddMenuEntry(
					"NDGIOpenPreview",
					LOCTEXT("NDGIOpenPreview", "NDGI Preview"),
					LOCTEXT("NDGIOpenPreviewTooltip", "Open the NDGI model preview window"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateStatic(&OpenNDGIPreviewTab)));
				Section.AddMenuEntry(
					"NDGIOpenLightmapInspector",
					LOCTEXT("NDGIOpenLightmapInspector", "NDGI Lightmap Inspector"),
					LOCTEXT("NDGIOpenLightmapInspectorTooltip", "Inspect CPU Lightmass atlases and merged bake sets"),
					FSlateIcon(),
					FUIAction(FExecuteAction::CreateLambda([]() { OpenNDGILightmapInspectorTab(); })));
			}
		}
	}

	void UnregisterNDGIMenu()
	{
		if (UToolMenus* Menus = UToolMenus::Get())
		{
			Menus->RemoveSection(TEXT("LevelEditor.MainMenu.Tools"), TEXT("NDGI"));
		}
	}
}

void FNDGIEditorModule::StartupModule()
{
	NDGI::Editor::Pipeline::RegisterLogCapture();
	RegisterNDGIPipelineTab();
	RegisterNDGIPreviewTab();
	RegisterNDGILightmapInspectorTab();
	RegisterNDGIMenu();
}

void FNDGIEditorModule::ShutdownModule()
{
	NDGI::Editor::Pipeline::StopAutoRun();
	UnregisterNDGIMenu();
	UnregisterNDGILightmapInspectorTab();
	UnregisterNDGIPreviewTab();
	UnregisterNDGIPipelineTab();
	NDGI::Editor::Pipeline::UnregisterLogCapture();
}

void FNDGIEditorModule::SmokeTestConsoleCommand(const TArray<FString>& Args, FOutputDevice& OutputDevice)
{
	uint32 Count = 4096u;
	bool bQuitWhenDone = false;
	for (const FString& Arg : Args)
	{
		if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
		{
			bQuitWhenDone = true;
		}
		else
		{
			Count = static_cast<uint32>(FMath::Max(1, FCString::Atoi(*Arg)));
		}
	}

	const uint32 Offset = 7u;

	UE_LOG(LogNDGI, Display, TEXT("Smoke test: dispatching %u elements with offset %u..."), Count, Offset);

	NDGI::DispatchSmokeTest(Count, Offset, [Count, Offset, bQuitWhenDone](bool bSuccess, TArray<uint32>&& Values)
	{
		if (!bSuccess || Values.Num() != static_cast<int32>(Count))
		{
			UE_LOG(LogNDGI, Error, TEXT("Smoke test FAILED: readback unavailable (%d values)."), Values.Num());
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
			return;
		}

		uint32 MismatchCount = 0;
		for (uint32 Index = 0; Index < Count; ++Index)
		{
			const uint32 Expected = Index * 2u + Offset;
			if (Values[Index] != Expected)
			{
				if (MismatchCount < 8)
				{
					UE_LOG(LogNDGI, Error, TEXT("  mismatch at %u: got %u, expected %u"), Index, Values[Index], Expected);
				}
				++MismatchCount;
			}
		}

		if (MismatchCount == 0)
		{
			UE_LOG(LogNDGI, Display, TEXT("Smoke test PASSED: %u elements verified."), Count);
		}
		else
		{
			UE_LOG(LogNDGI, Error, TEXT("Smoke test FAILED: %u/%u mismatches."), MismatchCount, Count);
		}

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	});
}

#undef LOCTEXT_NAMESPACE

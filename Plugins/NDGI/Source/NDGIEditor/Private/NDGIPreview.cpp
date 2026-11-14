// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIEditor.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/TextureRenderTarget2D.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Misc/PackageName.h"
#include "NDGI.h"
#include "NDGILightmapSet.h"
#include "NDGIModelAsset.h"
#include "NDGIRenderPreview.h"
#include "NDGIReference.h"
#include "NDGITileReference.h"
#include "NDGITileTrain.h"
#include "RHIGPUReadback.h"
#include "RenderingThread.h"
#include "Styling/SlateBrush.h"
#include "TextureResource.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UnrealClient.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NDGIPreview"

/** Declared here so the internal preview command can open the global tab. */
void OpenNDGIPreviewTab();

namespace
{
	using namespace NDGI;

	constexpr int32 PreviewResolution = 512;
	const TCHAR* DefaultModelAssetPath = TEXT("/Game/NDGI/TestModel.TestModel");
	const TCHAR* TestLevelPackage = TEXT("/Game/NDGI/NDGITestMap");
	const TCHAR* TestRTPackage = TEXT("/Game/NDGI/NDGITestRT");
	const TCHAR* DisplayMaterialPackage = TEXT("/Game/NDGI/NDGIDisplayMaterial");
	const TCHAR* DisplayMaterialInstancePackage = TEXT("/Game/NDGI/NDGIDisplayMI");

	FName PreviewTabName(TEXT("NDGIPreview"));

	/** Computes the flat layout offsets (mirrors NDGITile.ush). */
	void ComputeTileLayout(const Train::FTileTrainConfig& Config, int32& OutUTOffset, int32& OutVTOffset)
	{
		const int32 UVBlocks = (Config.UVSize / 4) * (Config.UVSize / 4);
		const int32 UVTBlocksTotal = (Config.UVTSize / 4) * (Config.UVTSize / 4) * Config.TimeSlices;
		OutUTOffset = Train::MLPParameterCount + UVBlocks * 24 + UVTBlocksTotal * 24;
		OutVTOffset = OutUTOffset + Config.UTSize * Config.TimeSlices * 2;
	}

	/** Encodes RGBA8 texels into BC-simulation endpoints/weights at the given flat offset. */
	void EncodeTexelsToBC(const TArray<uint8>& Texels, int32 SizeX, int32 SizeY,
		int32 E1Offset, int32 E2Offset, int32 WOffset, TArray<float>& OutParameters)
	{
		const int32 BlocksX = SizeX / 4;
		const int32 BlocksY = SizeY / 4;
		for (int32 BlockY = 0; BlockY < BlocksY; ++BlockY)
		{
			for (int32 BlockX = 0; BlockX < BlocksX; ++BlockX)
			{
				const int32 Block = BlockY * BlocksX + BlockX;
				for (int32 Channel = 0; Channel < 4; ++Channel)
				{
					float MinValue = 1.0f;
					float MaxValue = 0.0f;
					for (int32 P = 0; P < 16; ++P)
					{
						const int32 X = BlockX * 4 + (P % 4);
						const int32 Y = BlockY * 4 + (P / 4);
						const float Value = Texels[(Y * SizeX + X) * 4 + Channel] / 255.0f;
						MinValue = FMath::Min(MinValue, Value);
						MaxValue = FMath::Max(MaxValue, Value);
					}
					OutParameters[E1Offset + Block * 4 + Channel] = MinValue;
					OutParameters[E2Offset + Block * 4 + Channel] = MaxValue;
					const float Range = MaxValue - MinValue;
					for (int32 P = 0; P < 16; ++P)
					{
						const int32 X = BlockX * 4 + (P % 4);
						const int32 Y = BlockY * 4 + (P / 4);
						const float Value = Texels[(Y * SizeX + X) * 4 + Channel] / 255.0f;
						OutParameters[WOffset + Block * 16 + P] = (Range > 1e-6f)
							? FMath::Clamp((Value - MinValue) / Range, 0.0f, 1.0f)
							: 0.0f;
					}
				}
			}
		}
	}

	/** Builds the flat decode parameter buffer from a model asset (encodes texels back to endpoints/weights). */
	bool BuildModelDecodeParameters(const UNDGIModelAsset& Model, Train::FTileTrainConfig& OutConfig, TArray<float>& OutParameters)
	{
		if (Model.TileSize.X <= 0 || Model.TileSize.X % 4 != 0
			|| Model.FeatureUVTSize.X <= 0 || Model.FeatureUVTSize.X % 4 != 0
			|| Model.TimeSlices <= 0
			|| Model.FeatureUV.Num() != Model.TileSize.X * Model.TileSize.Y * 4
			|| Model.FeatureUVT.Num() != Model.TimeSlices * Model.FeatureUVTSize.X * Model.FeatureUVTSize.Y * 4
			|| Model.MLPParameters.Num() != Train::MLPParameterCount)
		{
			return false;
		}

		OutConfig = Train::FTileTrainConfig();
		OutConfig.UVSize = Model.TileSize.X;
		OutConfig.UVTSize = Model.FeatureUVTSize.X;
		OutConfig.UTSize = Model.UTSize;
		OutConfig.VTSize = Model.VTSize;
		OutConfig.TimeSlices = Model.TimeSlices;
		OutConfig.NumTiles = 1;

		const int32 TotalParams = Train::ComputeTotalParameters(OutConfig);
		OutParameters.SetNumZeroed(TotalParams);

		// MLP
		for (int32 Index = 0; Index < Train::MLPParameterCount; ++Index)
		{
			OutParameters[Index] = Model.MLPParameters[Index];
		}

		// Layout offsets.
		const int32 UVBlocks = (OutConfig.UVSize / 4) * (OutConfig.UVSize / 4);
		const int32 UVTBlocksTotal = (OutConfig.UVTSize / 4) * (OutConfig.UVTSize / 4) * OutConfig.TimeSlices;
		const int32 UVE1 = Train::MLPParameterCount;
		const int32 UVE2 = UVE1 + UVBlocks * 4;
		const int32 UVW = UVE2 + UVBlocks * 4;
		const int32 UVTE1 = UVW + UVBlocks * 16;
		const int32 UVTE2 = UVTE1 + UVTBlocksTotal * 4;
		const int32 UVTW = UVTE2 + UVTBlocksTotal * 4;
		int32 UTOffset = 0;
		int32 VTOffset = 0;
		ComputeTileLayout(OutConfig, UTOffset, VTOffset);

		EncodeTexelsToBC(Model.FeatureUV, OutConfig.UVSize, OutConfig.UVSize, UVE1, UVE2, UVW, OutParameters);

		const int32 SliceTexels = OutConfig.UVTSize * OutConfig.UVTSize * 4;
		for (int32 Slice = 0; Slice < OutConfig.TimeSlices; ++Slice)
		{
			TArray<uint8> SliceData;
			SliceData.SetNumUninitialized(SliceTexels);
			FMemory::Memcpy(SliceData.GetData(), Model.FeatureUVT.GetData() + static_cast<int64>(Slice) * SliceTexels, SliceTexels);
			const int32 SliceBlocks = (OutConfig.UVTSize / 4) * (OutConfig.UVTSize / 4);
			EncodeTexelsToBC(SliceData, OutConfig.UVTSize, OutConfig.UVTSize,
				UVTE1 + Slice * SliceBlocks * 4,
				UVTE2 + Slice * SliceBlocks * 4,
				UVTW + Slice * SliceBlocks * 16,
				OutParameters);
		}

		for (int32 Index = 0; Index < Model.UT.Num(); ++Index)
		{
			OutParameters[UTOffset + Index] = Model.UT[Index];
		}
		for (int32 Index = 0; Index < Model.VT.Num(); ++Index)
		{
			OutParameters[VTOffset + Index] = Model.VT[Index];
		}
		return true;
	}

	FVector3f GetMeanForTime(const UNDGIModelAsset& Model, float Time)
	{
		const int32 Step = FMath::Clamp(FMath::RoundToInt(Time * FMath::Max(1, Model.TimeSlices - 1)), 0, Model.TimeSlices - 1);
		if (Model.StepMeans.Num() >= (Step + 1) * 3)
		{
			return FVector3f(Model.StepMeans[Step * 3 + 0], Model.StepMeans[Step * 3 + 1], Model.StepMeans[Step * 3 + 2]);
		}
		return FVector3f::ZeroVector;
	}

	/** Shared preview state: model, flat parameters, time, and the render target. */
	struct FNDGIPreviewManager
	{
		static FNDGIPreviewManager& Get()
		{
			static FNDGIPreviewManager Instance;
			return Instance;
		}

		void EnsureInitialized()
		{
			if (!RenderTarget)
			{
				// Prefer the persistent test render target so the preview tab and the test level
				// (whose material instance references that asset) show the same decoded image.
				RenderTarget = LoadObject<UTextureRenderTarget2D>(nullptr, TEXT("/Game/NDGI/NDGITestRT.NDGITestRT"));
				if (!RenderTarget)
				{
					RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), TEXT("NDGIPreviewRT"));
					RenderTarget->RenderTargetFormat = RTF_RGBA16f;
					RenderTarget->bSupportsUAV = true;
					RenderTarget->ClearColor = FLinearColor::Black;
					RenderTarget->bAutoGenerateMips = false;
					RenderTarget->InitCustomFormat(PreviewResolution, PreviewResolution, PF_FloatRGBA, false);
					RenderTarget->UpdateResourceImmediate(true);
				}
				ReloadModel();
			}
		}

		UTextureRenderTarget2D* GetRenderTarget()
		{
			EnsureInitialized();
			return RenderTarget;
		}

		UNDGIModelAsset* GetModelAsset()
		{
			EnsureInitialized();
			return Model.Get();
		}

		/** Switches the preview to another model asset and refreshes. */
		void SetModelPath(const FString& InModelAssetPath)
		{
			ModelAssetPath = InModelAssetPath;
			ReloadAndRefresh();
		}

		float GetTime() const { return Time; }

		void SetTime(float NewTime)
		{
			Time = FMath::Clamp(NewTime, 0.0f, 1.0f);
			Refresh();
		}

		void ReloadAndRefresh()
		{
			ReloadModel();
			Refresh();
		}

		const FString& GetStatus() const { return Status; }

	private:
		void ReloadModel()
		{
			Model = LoadObject<UNDGIModelAsset>(nullptr, *ModelAssetPath);
			bHasParameters = false;
			if (!Model.IsValid())
			{
				Status = TEXT("Model not found: run ndgi.ExportTile first.");
				return;
			}
			bHasParameters = BuildModelDecodeParameters(*Model, Config, Parameters);
			Status = bHasParameters
				? FString::Printf(TEXT("Model %dx%d tile, %d time slices."), Config.UVSize, Config.UVSize, Config.TimeSlices)
				: TEXT("Model asset is invalid.");
			Refresh();
		}

		void Refresh()
		{
			if (!bHasParameters || !Model.IsValid() || !RenderTarget)
			{
				return;
			}
			Decode::DecodeTileToRenderTarget(Config, Parameters, 0, Time, GetMeanForTime(*Model, Time), Model->Gamma, RenderTarget);
		}

		UTextureRenderTarget2D* RenderTarget = nullptr;
		TWeakObjectPtr<UNDGIModelAsset> Model;
		FString ModelAssetPath = DefaultModelAssetPath;
		Train::FTileTrainConfig Config;
		TArray<float> Parameters;
		float Time = 0.0f;
		bool bHasParameters = false;
		FString Status;
	};

	class SNDGIPreviewWidget : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNDGIPreviewWidget) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			FNDGIPreviewManager& Manager = FNDGIPreviewManager::Get();
			Brush.SetResourceObject(Manager.GetRenderTarget());

			ChildSlot
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(8)
				[
					SNew(STextBlock).Text(LOCTEXT("NDGITitle", "NDGI Preview (use ndgi.PreviewModel or ndgi.TrainLightmapSet to select a tile)"))
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(8)
				[
					SNew(SBox).WidthOverride(PreviewResolution).HeightOverride(PreviewResolution)
					[
						SNew(SImage).Image(&Brush)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Text(LOCTEXT("NDGITimeLabel", "Time"))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(8.0f, 0.0f)
					[
						SNew(SSlider)
						.Value(this, &SNDGIPreviewWidget::GetTimeValue)
						.OnValueChanged(this, &SNDGIPreviewWidget::OnTimeChanged)
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						SNew(SButton)
						.Text(LOCTEXT("NDGIRefresh", "Reload Model"))
						.OnClicked(this, &SNDGIPreviewWidget::OnRefreshClicked)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(8)
				[
					SNew(STextBlock).Text(this, &SNDGIPreviewWidget::GetStatusText)
				]
			];
		}

	private:
		float GetTimeValue() const { return FNDGIPreviewManager::Get().GetTime(); }
		void OnTimeChanged(float NewValue) { FNDGIPreviewManager::Get().SetTime(NewValue); }
		FReply OnRefreshClicked() { FNDGIPreviewManager::Get().ReloadAndRefresh(); return FReply::Handled(); }
		FText GetStatusText() const { return FText::FromString(FNDGIPreviewManager::Get().GetStatus()); }

		FSlateBrush Brush;
	};

	TSharedRef<SDockTab> SpawnNDGIPreviewTab(const FSpawnTabArgs& Args)
	{
		return SNew(SDockTab)
			.TabRole(ETabRole::NomadTab)
			[
				SNew(SNDGIPreviewWidget)
			];
	}

	/** Saves an asset package to /Game. */
	bool SaveAssetPackage(UPackage* Package, UObject* Asset, const FString& LongPackageName)
	{
		const FString FileName = FPackageName::LongPackageNameToFilename(LongPackageName, FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		return UPackage::SavePackage(Package, Asset, *FileName, SaveArgs);
	}

	UTextureRenderTarget2D* GetOrCreateTestRenderTarget()
	{
		UTextureRenderTarget2D* Existing = LoadObject<UTextureRenderTarget2D>(nullptr, TEXT("/Game/NDGI/NDGITestRT.NDGITestRT"));
		if (Existing)
		{
			return Existing;
		}
		UPackage* Package = CreatePackage(TestRTPackage);
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(Package, TEXT("NDGITestRT"), RF_Public | RF_Standalone);
		RenderTarget->RenderTargetFormat = RTF_RGBA16f;
				RenderTarget->bSupportsUAV = true;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitCustomFormat(PreviewResolution, PreviewResolution, PF_FloatRGBA, false);
		RenderTarget->UpdateResourceImmediate(true);
		Package->SetDirtyFlag(true);
		SaveAssetPackage(Package, RenderTarget, TestRTPackage);
		return RenderTarget;
	}

	UMaterialInstanceConstant* GetOrCreateDisplayMaterialInstance(UTextureRenderTarget2D* RenderTarget)
	{
		UMaterialInstanceConstant* Existing = LoadObject<UMaterialInstanceConstant>(nullptr, TEXT("/Game/NDGI/NDGIDisplayMI.NDGIDisplayMI"));
		if (Existing)
		{
			Existing->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("NDGITexture"))), RenderTarget);
			return Existing;
		}

		UPackage* MaterialPackage = CreatePackage(DisplayMaterialPackage);
		UMaterial* Material = NewObject<UMaterial>(MaterialPackage, TEXT("NDGIDisplayMaterial"), RF_Public | RF_Standalone);
		Material->MaterialDomain = MD_Surface;
		Material->BlendMode = BLEND_Opaque;
		Material->TwoSided = true;
		Material->SetShadingModel(MSM_Unlit);

		UMaterialExpressionTextureSampleParameter2D* TextureNode = Cast<UMaterialExpressionTextureSampleParameter2D>(
			UMaterialEditingLibrary::CreateMaterialExpression(Material, UMaterialExpressionTextureSampleParameter2D::StaticClass(), -300, 0));
		if (TextureNode)
		{
			TextureNode->ParameterName = TEXT("NDGITexture");
			TextureNode->SamplerType = SAMPLERTYPE_Color;
			UMaterialEditingLibrary::ConnectMaterialProperty(TextureNode, TEXT("RGB"), MP_EmissiveColor);
		}
		Material->PostEditChange();
		MaterialPackage->SetDirtyFlag(true);
		SaveAssetPackage(MaterialPackage, Material, DisplayMaterialPackage);

		UPackage* InstancePackage = CreatePackage(DisplayMaterialInstancePackage);
		UMaterialInstanceConstant* Instance = NewObject<UMaterialInstanceConstant>(InstancePackage, TEXT("NDGIDisplayMI"), RF_Public | RF_Standalone);
		Instance->SetParentEditorOnly(Material);
		Instance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(FName(TEXT("NDGITexture"))), RenderTarget);
		Instance->PostEditChange();
		InstancePackage->SetDirtyFlag(true);
		SaveAssetPackage(InstancePackage, Instance, DisplayMaterialInstancePackage);
		return Instance;
	}

	bool ReadbackRenderTarget(UTextureRenderTarget2D* RenderTarget, TArray<FLinearColor>& OutPixels, int32& OutWidth, int32& OutHeight)
	{
		FTextureRenderTargetResource* Resource = RenderTarget ? RenderTarget->GameThread_GetRenderTargetResource() : nullptr;
		if (!Resource)
		{
			return false;
		}
		OutWidth = RenderTarget->SizeX;
		OutHeight = RenderTarget->SizeY;

		TSharedRef<TArray<FLinearColor>> Pixels = MakeShared<TArray<FLinearColor>>();
		FRHIGPUTextureReadback* Readback = new FRHIGPUTextureReadback(TEXT("NDGI.PreviewReadback"));
		FTextureRHIRef TextureRHI = Resource->GetRenderTargetTexture();
		ENQUEUE_RENDER_COMMAND(NDGIEnqueuePreviewCopy)(
			[Readback, TextureRHI](FRHICommandListImmediate& RHICmdList)
			{
				Readback->EnqueueCopy(RHICmdList, TextureRHI);
			});
		FlushRenderingCommands();
		for (int32 Spin = 0; Spin < 10000 && !Readback->IsReady(); ++Spin)
		{
			FPlatformProcess::Sleep(0.001f);
		}
		if (!Readback->IsReady())
		{
			ENQUEUE_RENDER_COMMAND(NDGIDeletePreviewReadback)([Readback](FRHICommandListImmediate&) { delete Readback; });
			FlushRenderingCommands();
			return false;
		}

		const int32 Width = OutWidth;
		const int32 Height = OutHeight;
		ENQUEUE_RENDER_COMMAND(NDGILockPreviewReadback)(
			[Readback, Pixels, Width, Height](FRHICommandListImmediate& RHICmdList)
			{
				int32 RowPitch = 0;
				int32 BufferHeight = 0;
				const void* Data = Readback->Lock(RowPitch, &BufferHeight);
				if (Data)
				{
					Pixels->SetNumUninitialized(Width * Height);
					for (int32 Y = 0; Y < FMath::Min(BufferHeight, Height); ++Y)
					{
						const FFloat16Color* Row = reinterpret_cast<const FFloat16Color*>(
							static_cast<const uint8*>(Data) + static_cast<int64>(Y) * RowPitch * sizeof(FFloat16Color));
						for (int32 X = 0; X < Width; ++X)
						{
							(*Pixels)[Y * Width + X] = FLinearColor(Row[X].R, Row[X].G, Row[X].B, Row[X].A);
						}
					}
				}
				Readback->Unlock();
				delete Readback;
			});
		FlushRenderingCommands();
		OutPixels = MoveTemp(*Pixels);
		return OutPixels.Num() == Width * Height;
	}

	void RunCreateTestLevel(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
		}

		auto Finish = [bQuitWhenDone]()
		{
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		UNDGIModelAsset* Model = FNDGIPreviewManager::Get().GetModelAsset();
		if (!Model)
		{
			UE_LOG(LogNDGI, Error, TEXT("CreateTestLevel: model not found - run ndgi.ExportTile or ndgi.TrainLightmapSet first."));
			Finish();
			return;
		}

		Train::FTileTrainConfig Config;
		TArray<float> Parameters;
		if (!BuildModelDecodeParameters(*Model, Config, Parameters))
		{
			UE_LOG(LogNDGI, Error, TEXT("CreateTestLevel: model asset data is invalid."));
			Finish();
			return;
		}

		UTextureRenderTarget2D* RenderTarget = GetOrCreateTestRenderTarget();
		UMaterialInstanceConstant* DisplayMaterial = GetOrCreateDisplayMaterialInstance(RenderTarget);
		if (!RenderTarget || !DisplayMaterial)
		{
			UE_LOG(LogNDGI, Error, TEXT("CreateTestLevel: could not create display assets."));
			Finish();
			return;
		}

		UWorld* World = GEditor ? GEditor->NewMap(false) : nullptr;
		if (!World)
		{
			UE_LOG(LogNDGI, Error, TEXT("CreateTestLevel: could not create a new level."));
			Finish();
			return;
		}
		World = GEditor->GetEditorWorldContext().World();

		// Display plane.
		UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
		AStaticMeshActor* PlaneActor = World->SpawnActor<AStaticMeshActor>(FVector(0.0, 0.0, 100.0), FRotator::ZeroRotator);
		if (PlaneActor)
		{
			PlaneActor->SetActorLabel(TEXT("NDGI_Display"));
			PlaneActor->SetActorScale3D(FVector(5.0f, 5.0f, 5.0f));
			if (UStaticMeshComponent* MeshComponent = PlaneActor->GetStaticMeshComponent())
			{
				MeshComponent->SetStaticMesh(PlaneMesh);
				MeshComponent->SetMaterial(0, DisplayMaterial);
				MeshComponent->SetMobility(EComponentMobility::Static);
			}
		}

		// Sun for context.
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0.0, 0.0, 500.0), FRotator(-45.0f, -30.0f, 0.0f));
		if (Sun)
		{
			Sun->SetActorLabel(TEXT("NDGI_TestSun"));
		}

		// Decode the model into the display texture.
		Decode::DecodeTileToRenderTarget(Config, Parameters, 0, 0.0f, GetMeanForTime(*Model, 0.0f), Model->Gamma, RenderTarget);

		const FString MapFileName = FPackageName::LongPackageNameToFilename(TestLevelPackage, FPackageName::GetMapPackageExtension());
		FString SavedFileName;
		const bool bSaved = FEditorFileUtils::SaveLevel(World->PersistentLevel, *MapFileName, &SavedFileName);
		UE_LOG(LogNDGI, Display, TEXT("CreateTestLevel: %s (%s)."),
			bSaved ? TEXT("saved test level") : TEXT("FAILED to save test level"),
			bSaved ? *SavedFileName : *MapFileName);

		if (bSaved)
		{
			UE_LOG(LogNDGI, Display, TEXT("CreateTestLevel: open /Game/NDGI/NDGITestMap, then use Window > NDGI Preview or 'ndgi.Preview' to scrub time."));
		}
		Finish();
	}

	void RunPreview(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		float RequestedTime = -1.0f;
		int32 NumericArgIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else
			{
				if (NumericArgIndex == 0)
				{
					RequestedTime = FCString::Atof(*Arg);
				}
				++NumericArgIndex;
			}
		}

		OpenNDGIPreviewTab();
		if (RequestedTime >= 0.0f)
		{
			FNDGIPreviewManager::Get().SetTime(RequestedTime);
		}
		UE_LOG(LogNDGI, Display, TEXT("NDGI Preview: %s"), *FNDGIPreviewManager::Get().GetStatus());

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}

	void RunTestPreviewRT(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		FString RequestedModel;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (!Arg.IsNumeric())
			{
				RequestedModel = Arg;
			}
		}

		auto Finish = [bQuitWhenDone](bool bPassed)
		{
			UE_LOG(LogNDGI, Display, TEXT("Preview render target test: %s."), bPassed ? TEXT("PASSED") : TEXT("FAILED"));
			if (bQuitWhenDone && GEngine)
			{
				GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
			}
		};

		FNDGIPreviewManager& Manager = FNDGIPreviewManager::Get();
		if (!RequestedModel.IsEmpty())
		{
			Manager.SetModelPath(RequestedModel);
		}
		UNDGIModelAsset* Model = Manager.GetModelAsset();
		Train::FTileTrainConfig Config;
		TArray<float> Parameters;
		if (!Model || !BuildModelDecodeParameters(*Model, Config, Parameters))
		{
			UE_LOG(LogNDGI, Error, TEXT("TestPreviewRT: no valid model (run ndgi.ExportTile or ndgi.TrainLightmapSet first)."));
			Finish(false);
			return;
		}

		const float Time = 0.37f;
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), TEXT("NDGIPreviewTestRT"));
		RenderTarget->RenderTargetFormat = RTF_RGBA16f;
				RenderTarget->bSupportsUAV = true;
		RenderTarget->ClearColor = FLinearColor::Black;
		RenderTarget->bAutoGenerateMips = false;
		RenderTarget->InitCustomFormat(64, 64, PF_FloatRGBA, false);
		RenderTarget->UpdateResourceImmediate(true);

		const FVector3f Mean = GetMeanForTime(*Model, Time);
		Decode::DecodeTileToRenderTarget(Config, Parameters, 0, Time, Mean, Model->Gamma, RenderTarget);
		FlushRenderingCommands();

		TArray<FLinearColor> Pixels;
		int32 Width = 0;
		int32 Height = 0;
		if (!ReadbackRenderTarget(RenderTarget, Pixels, Width, Height))
		{
			Finish(false);
			return;
		}

		// CPU reference from the same flat parameters.
		Reference::FTileConfig ReferenceConfig;
		ReferenceConfig.UVSize = Config.UVSize;
		ReferenceConfig.UVTSize = Config.UVTSize;
		ReferenceConfig.UTSize = Config.UTSize;
		ReferenceConfig.VTSize = Config.VTSize;
		ReferenceConfig.TimeSlices = Config.TimeSlices;
		ReferenceConfig.bSimulateBC = true;
		Reference::FTileReference Tile(ReferenceConfig, 0);
		TArray<double> FlatDoubles;
		FlatDoubles.SetNumUninitialized(Parameters.Num());
		for (int32 Index = 0; Index < Parameters.Num(); ++Index)
		{
			FlatDoubles[Index] = static_cast<double>(Parameters[Index]);
		}
		Tile.ImportFlatParameters(FlatDoubles);

		TArray<FVector> Points;
		Points.SetNumUninitialized(Width * Height);
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				Points[Y * Width + X] = FVector(
					(static_cast<double>(X) + 0.5) / Width,
					(static_cast<double>(Y) + 0.5) / Height,
					static_cast<double>(Time));
			}
		}
		TArray<float> CpuNormalized;
		Tile.EvaluatePoints(Points, CpuNormalized);

		double MaxAbsError = 0.0;
		int32 Violations = 0;
		for (int32 Index = 0; Index < Pixels.Num(); ++Index)
		{
			for (int32 Channel = 0; Channel < 3; ++Channel)
			{
				const double Cpu = FMath::Pow(FMath::Max(static_cast<double>(CpuNormalized[Index * Reference::OutputDim + Channel]) + static_cast<double>(Mean[Channel]), 0.0), Model->Gamma);
				const double Gpu = Channel == 0 ? Pixels[Index].R : (Channel == 1 ? Pixels[Index].G : Pixels[Index].B);
				const double AbsError = FMath::Abs(Cpu - Gpu);
				MaxAbsError = FMath::Max(MaxAbsError, AbsError);
				if (AbsError > 2e-3 + 5e-3 * FMath::Abs(Cpu))
				{
					++Violations;
				}
			}
		}

		UE_LOG(LogNDGI, Display, TEXT("Preview RT vs CPU: %d values, %d violations, max abs err %.3e."),
			Pixels.Num() * 3, Violations, MaxAbsError);
		Finish(Violations == 0);
	}
}

void OpenNDGIPreviewTab()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(PreviewTabName));
	FNDGIPreviewManager::Get().ReloadAndRefresh();
}

void RegisterNDGIPreviewTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PreviewTabName, FOnSpawnTab::CreateStatic(&SpawnNDGIPreviewTab))
		.SetDisplayName(LOCTEXT("NDGIPreviewTabTitle", "NDGI Preview"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

}

void UnregisterNDGIPreviewTab()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PreviewTabName);
}

void SetNDGIPreviewModel(const FString& PackageObjectPath)
{
	FNDGIPreviewManager::Get().SetModelPath(PackageObjectPath);
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGIPreviewCommand(
	TEXT("ndgi.Preview"),
	TEXT("Opens the NDGI preview tab; optional arg: time (0..1)."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunPreview));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGICreateTestLevelCommand(
	TEXT("ndgi.CreateTestLevel"),
	TEXT("Creates /Game/NDGI/NDGITestMap with a display plane showing the NDGI model. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunCreateTestLevel));

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestPreviewRTCommand(
	TEXT("ndgi.TestPreviewRT"),
	TEXT("Decodes the model into a render target and validates the pixels against the CPU reference. Optional arg: quit."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestPreviewRT));

#undef LOCTEXT_NAMESPACE

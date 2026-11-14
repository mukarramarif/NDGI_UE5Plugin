// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIEditor.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "NDGI.h"
#include "NDGIBakeManifest.h"
#include "NDGIEncodingMap.h"
#include "NDGILevelAsset.h"
#include "NDGILightmapSet.h"
#include "NDGILightmapSources.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSpacer.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NDGILightmapInspector"

namespace
{
	using namespace NDGI;
	namespace BakeManifest = NDGI::Editor::BakeManifest;
	namespace LightmapSources = NDGI::Editor::LightmapSources;

	FName LightmapInspectorTabName(TEXT("NDGILightmapInspector"));

	const FLinearColor InspectorMutedText(0.58f, 0.58f, 0.58f, 1.0f);
	const FLinearColor InspectorWarningText(1.0f, 0.68f, 0.18f, 1.0f);
	const FLinearColor InspectorErrorText(0.95f, 0.28f, 0.24f, 1.0f);

	DECLARE_DELEGATE_TwoParams(FInspectorMouseMove, const FGeometry&, const FPointerEvent&);

	/** SImage with hover notification for the pixel readout. */
	class SNDGILightmapImage : public SImage
	{
	public:
		SLATE_BEGIN_ARGS(SNDGILightmapImage)
			: _Image(FCoreStyle::Get().GetDefaultBrush())
		{}
			SLATE_ATTRIBUTE(const FSlateBrush*, Image)
			SLATE_EVENT(FInspectorMouseMove, OnHover)
			SLATE_EVENT(FSimpleDelegate, OnLeave)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs)
		{
			SImage::FArguments Base;
			Base._Image = InArgs._Image;
			SImage::Construct(Base);
			HoverDelegate = InArgs._OnHover;
			LeaveDelegate = InArgs._OnLeave;
		}

		virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override
		{
			HoverDelegate.ExecuteIfBound(MyGeometry, MouseEvent);
			return FReply::Handled();
		}

		virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override
		{
			SImage::OnMouseLeave(MouseEvent);
			LeaveDelegate.ExecuteIfBound();
		}

	private:
		FInspectorMouseMove HoverDelegate;
		FSimpleDelegate LeaveDelegate;
	};

	enum class EInspectorSource : uint8
	{
		LiveAtlas,
		MergedSet
	};

	enum class EInspectorView : uint8
	{
		FinalColor,
		RGB,
		Red,
		Green,
		Blue,
		Luminance,
		Alpha,
		SH,
		SHW,
		RangeIndex,
		Coverage,
		Difference
	};

	struct FInspectorAtlas
	{
		int32 TexIndex = 0;
		FString TexturePath;
		FString MergedPath;
		int32 Width = 0;
		int32 Height = 0;
		TArray<FString> Cells;
		bool bMergedExists = false;
		FDateTime MergedTime;
	};

	struct FInspectorLiveData
	{
		bool bLoaded = false;
		FString Error;
		int32 Width = 0;
		int32 Height = 0;
		int32 RawWidth = 0;
		int32 RawHeight = 0;
		bool bHighQuality = true;
		TArray<FLinearColor> RawPixels;
		TArray<FVector3f> RGB;
		TArray<FVector3f> SH;
		TArray<uint8> Coverage;
		TArray<uint8> RangeIndices;
		TArray<FNDGIEncodingRange> Ranges;
	};

	struct FInspectorMergedData
	{
		bool bLoaded = false;
		FString Error;
		FString Path;
		FLightmapSet Set;
	};

	FString GetLevelAssetPathForWorld(UWorld* World)
	{
		if (!World)
		{
			return FString();
		}
		const FString MapName = UWorld::RemovePIEPrefix(World->GetName());
		return FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *MapName, *MapName);
	}

	UNDGILevelAsset* LoadLevelAssetForWorld(UWorld* World)
	{
		const FString AssetPath = GetLevelAssetPathForWorld(World);
		if (AssetPath.IsEmpty())
		{
			return nullptr;
		}
		const FString PackageName = FPackageName::ObjectPathToPackageName(AssetPath);
		if (!FPackageName::DoesPackageExist(PackageName))
		{
			return nullptr;
		}
		return LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
	}

	const FNDGIAtlasEntry* FindLevelEntry(const UNDGILevelAsset* LevelAsset, const FInspectorAtlas& Atlas)
	{
		if (!LevelAsset)
		{
			return nullptr;
		}
		const FSoftObjectPath AtlasPath(Atlas.TexturePath);
		for (const FNDGIAtlasEntry& Entry : LevelAsset->Atlases)
		{
			if (FSoftObjectPath(Entry.LightmapTexturePath) == AtlasPath)
			{
				return &Entry;
			}
		}
		return LevelAsset->Atlases.IsValidIndex(Atlas.TexIndex) ? &LevelAsset->Atlases[Atlas.TexIndex] : nullptr;
	}

	FString ViewToString(EInspectorView View)
	{
		switch (View)
		{
		case EInspectorView::FinalColor: return TEXT("Final color");
		case EInspectorView::RGB: return TEXT("RGB");
		case EInspectorView::Red: return TEXT("Red");
		case EInspectorView::Green: return TEXT("Green");
		case EInspectorView::Blue: return TEXT("Blue");
		case EInspectorView::Luminance: return TEXT("Luminance");
		case EInspectorView::Alpha: return TEXT("Stored alpha (live only)");
		case EInspectorView::SH: return TEXT("SH xyz");
		case EInspectorView::SHW: return TEXT("SH.w (live only)");
		case EInspectorView::RangeIndex: return TEXT("Encoding range");
		case EInspectorView::Coverage: return TEXT("Coverage");
		case EInspectorView::Difference: return TEXT("Live vs merged");
		default: return TEXT("View");
		}
	}

	FString SourceToString(EInspectorSource Source)
	{
		return Source == EInspectorSource::LiveAtlas ? TEXT("Live CPU Lightmass") : TEXT("Merged .ndgi");
	}

	FColor PaletteColor(uint8 Index)
	{
		const uint8 R = static_cast<uint8>((Index * 79 + 37) & 0xFF);
		const uint8 G = static_cast<uint8>((Index * 157 + 91) & 0xFF);
		const uint8 B = static_cast<uint8>((Index * 211 + 13) & 0xFF);
		return FColor(R, G, B, 255);
	}

	FColor LinearToDisplayColor(const FLinearColor& Color)
	{
		return Color.ToFColor(true);
	}

	FColor HeatColor(float Value)
	{
		Value = FMath::Clamp(Value, 0.0f, 1.0f);
		return FLinearColor(
			FMath::Clamp(Value * 2.0f, 0.0f, 1.0f),
			FMath::Clamp(1.0f - FMath::Abs(Value * 2.0f - 1.0f), 0.0f, 1.0f),
			FMath::Clamp((1.0f - Value) * 2.0f, 0.0f, 1.0f),
			1.0f).ToFColor(true);
	}

	class SNDGILightmapInspectorPanel final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNDGILightmapInspectorPanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs);
		virtual ~SNDGILightmapInspectorPanel() override = default;

		void ApplyPendingSelection(bool bRefresh);

		/** Saves the current view to Saved/NDGI/Inspector. */
		void ExportPng();

	private:
		EActiveTimerReturnType Tick(double CurrentTime, float DeltaTime);
		void RefreshAtlasList();
		void RefreshSource();
		void LoadLiveData();
		void LoadMergedData();
		void RebuildDisplay();
		void UpdateDisplayTexture();
		void UpdateInfoText();
		void UpdateStatsText();
		void UpdateReadoutText();
		void UpdateRangeText();

		UWorld* GetEditorWorld() const;
		UNDGILevelAsset* GetLevelAsset() const;
		const FInspectorAtlas* GetSelectedAtlas() const;
		const FNDGIAtlasEntry* GetSelectedLevelEntry() const;
		int32 GetSliceCount() const;
		int32 GetDisplayWidth() const;
		int32 GetDisplayHeight() const;

		void SelectAtlasByOffset(int32 Offset);
		void SetSource(EInspectorSource NewSource);
		void SetView(EInspectorView NewView);
		void SetSlice(int32 NewSlice);
		void SetZoom(float NewZoom);
		void OnImageMouseMove(const FGeometry& Geometry, const FPointerEvent& Event);
		void OnImageMouseLeave();
		void OpenTextureEditor();
		TSharedRef<SWidget> MakeViewMenu();
		TSharedRef<SWidget> MakeAtlasMenu();
		FText GetAtlasLabel() const;
		FText GetSliceLabel() const;
		FText GetSourceLabel() const;
		FText GetStatusText() const;
		FSlateColor GetStatusColor() const;

		TArray<FInspectorAtlas> Atlases;
		int32 SelectedAtlas = 0;
		EInspectorSource Source = EInspectorSource::LiveAtlas;
		EInspectorView View = EInspectorView::FinalColor;
		int32 Slice = 0;
		float Exposure = 0.0f;
		bool bShowCoverage = true;
		bool bShowTileGrid = true;
		float Zoom = 0.5f;

		FInspectorLiveData LiveData;
		FInspectorMergedData MergedData;
		FString ManifestSummary;
		FString LoadError;
		int32 HoverX = INDEX_NONE;
		int32 HoverY = INDEX_NONE;

		TStrongObjectPtr<UTexture2D> DisplayTexture;
		TArray<FColor> DisplayPixels;
		FSlateBrush Brush;

		TSharedPtr<STextBlock> HeaderText;
		TSharedPtr<STextBlock> StatusText;
		TSharedPtr<STextBlock> AtlasText;
		TSharedPtr<STextBlock> SliceText;
		TSharedPtr<STextBlock> InfoText;
		TSharedPtr<STextBlock> ReadoutText;
		TSharedPtr<STextBlock> StatsText;
		TSharedPtr<STextBlock> RangeText;
		TSharedPtr<SBox> ImageBox;
		TSharedPtr<SNDGILightmapImage> ImageWidget;

		double LastRefreshSeconds = 0.0;
		bool bPendingInitialLoad = true;
	};

	TWeakPtr<SNDGILightmapInspectorPanel> GInspectorPanel;
	int32 GInspectorPendingTexIndex = INDEX_NONE;
	int32 GInspectorPendingSlice = INDEX_NONE;

	void SNDGILightmapInspectorPanel::Construct(const FArguments&)
	{
		UE_LOG(LogNDGI, Display, TEXT("NDGI Lightmap Inspector: constructing tab."));
		RefreshAtlasList();

		const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 17);
		const FSlateFontInfo SectionFont = FCoreStyle::GetDefaultFontStyle("Bold", 11);
		const FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);

		auto MakeSourceButton = [this](EInspectorSource InSource) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.Text_Lambda([this, InSource]()
				{
					return FText::FromString((Source == InSource ? TEXT("● ") : TEXT("")) + SourceToString(InSource));
				})
				.ToolTipText(InSource == EInspectorSource::LiveAtlas
					? LOCTEXT("LiveSourceTip", "Read the current CPU Lightmass lightmap atlas from map build data.")
					: LOCTEXT("MergedSourceTip", "Read the level-scoped Merged_TexNN.ndgi bake set."))
				.OnClicked_Lambda([this, InSource]() { SetSource(InSource); return FReply::Handled(); });
		};

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							SNew(STextBlock)
							.Text(LOCTEXT("Title", "NDGI Lightmap Inspector"))
							.Font(TitleFont)
						]
						+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
						[
							SAssignNew(HeaderText, STextBlock)
							.Font(SmallFont)
							.ColorAndOpacity(FSlateColor(InspectorMutedText))
							.AutoWrapText(true)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
					[
						SAssignNew(StatusText, STextBlock)
						.Font(SectionFont)
						.ColorAndOpacity(FSlateColor(InspectorMutedText))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
					[
						MakeSourceButton(EInspectorSource::LiveAtlas)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 14, 0)
					[
						MakeSourceButton(EInspectorSource::MergedSet)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("PrevAtlas", "<"))
						.ToolTipText(LOCTEXT("PrevAtlasTip", "Previous lightmap atlas."))
						.OnClicked_Lambda([this]() { SelectAtlasByOffset(-1); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SComboButton)
						.OnGetMenuContent(this, &SNDGILightmapInspectorPanel::MakeAtlasMenu)
						.ButtonContent()
						[
							SAssignNew(AtlasText, STextBlock)
							.Font(BodyFont)
							.Text(this, &SNDGILightmapInspectorPanel::GetAtlasLabel)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 14, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("NextAtlas", ">"))
						.ToolTipText(LOCTEXT("NextAtlasTip", "Next lightmap atlas."))
						.OnClicked_Lambda([this]() { SelectAtlasByOffset(1); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 6, 0)
					[
						SNew(SComboButton)
						.OnGetMenuContent(this, &SNDGILightmapInspectorPanel::MakeViewMenu)
						.ButtonContent()
						[
							SNew(STextBlock)
							.Font(BodyFont)
							.Text_Lambda([this]() { return FText::FromString(ViewToString(View)); })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("SliceLabel", "Slice"))
						.Font(SmallFont)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SSpinBox<int32>)
						.MinValue(0)
						.MaxValue_Lambda([this]() { return FMath::Max(0, GetSliceCount() - 1); })
						.Delta(1)
						.Value_Lambda([this]() { return Slice; })
						.OnValueChanged_Lambda([this](int32 NewValue) { SetSlice(NewValue); })
						.IsEnabled_Lambda([this]() { return Source == EInspectorSource::MergedSet || View == EInspectorView::Difference; })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SAssignNew(SliceText, STextBlock)
						.Font(SmallFont)
						.ColorAndOpacity(FSlateColor(InspectorMutedText))
						.Text(this, &SNDGILightmapInspectorPanel::GetSliceLabel)
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ExposureLabel", "Exposure"))
						.Font(SmallFont)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SSpinBox<float>)
						.MinValue(-10.0f)
						.MaxValue(10.0f)
						.Delta(0.25f)
						.MinFractionalDigits(2)
						.MaxFractionalDigits(2)
						.Value_Lambda([this]() { return Exposure; })
						.OnValueChanged_Lambda([this](float NewValue) { Exposure = NewValue; RebuildDisplay(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bShowCoverage ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowCoverage = State == ECheckBoxState::Checked; RebuildDisplay(); })
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ShowCoverage", "Coverage"))
							.Font(SmallFont)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]() { return bShowTileGrid ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State) { bShowTileGrid = State == ECheckBoxState::Checked; RebuildDisplay(); })
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ShowTileGrid", "Tile grid"))
							.Font(SmallFont)
						]
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f)
					[
						SNew(SSpacer)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("ZoomOut", "−"))
						.ToolTipText(LOCTEXT("ZoomOutTip", "Zoom out."))
						.OnClicked_Lambda([this]() { SetZoom(Zoom * 0.5f); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Font(SmallFont)
						.ColorAndOpacity(FSlateColor(InspectorMutedText))
						.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("%.0f%%"), Zoom * 100.0f)); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 8, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("ZoomIn", "+"))
						.ToolTipText(LOCTEXT("ZoomInTip", "Zoom in."))
						.OnClicked_Lambda([this]() { SetZoom(Zoom * 2.0f); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("Refresh", "Refresh"))
						.ToolTipText(LOCTEXT("RefreshTip", "Reload the manifest, atlas list, and selected source."))
						.OnClicked_Lambda([this]()
						{
							LastRefreshSeconds = 0.0;
							RefreshAtlasList();
							RefreshSource();
							return FReply::Handled();
						})
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 4, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("ExportPng", "Export PNG"))
						.ToolTipText(LOCTEXT("ExportPngTip", "Save the current view to Saved/NDGI/Inspector."))
						.OnClicked_Lambda([this]() { ExportPng(); return FReply::Handled(); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(SButton)
						.Text(LOCTEXT("OpenTexture", "Open Texture"))
						.ToolTipText(LOCTEXT("OpenTextureTip", "Open the selected live lightmap texture in the texture editor."))
						.OnClicked_Lambda([this]() { OpenTextureEditor(); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 10, 0, 0)
				[
					SNew(SSplitter)
					.Orientation(Orient_Horizontal)
					+ SSplitter::Slot().Value(0.74f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(4.0f))
						[
							SNew(SScrollBox)
							+ SScrollBox::Slot()
							[
								SAssignNew(ImageBox, SBox)
								.WidthOverride_Lambda([this]() { return FMath::Max(1.0f, GetDisplayWidth() * Zoom); })
								.HeightOverride_Lambda([this]() { return FMath::Max(1.0f, GetDisplayHeight() * Zoom); })
								[
									SAssignNew(ImageWidget, SNDGILightmapImage)
									.Image(&Brush)
									.OnHover(this, &SNDGILightmapInspectorPanel::OnImageMouseMove)
									.OnLeave(this, &SNDGILightmapInspectorPanel::OnImageMouseLeave)
								]
							]
						]
					]
					+ SSplitter::Slot().Value(0.26f)
					[
						SNew(SBorder)
						.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
						.Padding(FMargin(10.0f, 8.0f))
						[
							SNew(SVerticalBox)
							+ SVerticalBox::Slot().AutoHeight()
							[
								SNew(STextBlock)
								.Text(LOCTEXT("ReadoutTitle", "Pixel / atlas"))
								.Font(SectionFont)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 5, 0, 0)
							[
								SAssignNew(InfoText, STextBlock)
								.Font(SmallFont)
								.ColorAndOpacity(FSlateColor(InspectorMutedText))
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SAssignNew(ReadoutText, STextBlock)
								.Font(SmallFont)
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(SSeparator)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("StatsTitle", "Statistics"))
								.Font(SectionFont)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
							[
								SAssignNew(StatsText, STextBlock)
								.Font(SmallFont)
								.ColorAndOpacity(FSlateColor(InspectorMutedText))
								.AutoWrapText(true)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(SSeparator)
							]
							+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
							[
								SNew(STextBlock)
								.Text(LOCTEXT("RangesTitle", "Encoding ranges"))
								.Font(SectionFont)
							]
							+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 4, 0, 0)
							[
								SNew(SScrollBox)
								+ SScrollBox::Slot()
								[
									SAssignNew(RangeText, STextBlock)
									.Font(SmallFont)
									.ColorAndOpacity(FSlateColor(InspectorMutedText))
									.AutoWrapText(true)
								]
							]
						]
					]
				]
			]
		];

		if (HeaderText.IsValid())
		{
			HeaderText->SetText(FText::FromString(ManifestSummary));
		}
		GInspectorPanel = SharedThis(this);
		ApplyPendingSelection(false);
		UpdateInfoText();
		RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SNDGILightmapInspectorPanel::Tick));
	}

	void SNDGILightmapInspectorPanel::ApplyPendingSelection(bool bRefresh)
	{
		bool bChanged = false;
		if (GInspectorPendingTexIndex != INDEX_NONE)
		{
			for (int32 Index = 0; Index < Atlases.Num(); ++Index)
			{
				if (Atlases[Index].TexIndex == GInspectorPendingTexIndex)
				{
					SelectedAtlas = Index;
					bChanged = true;
					break;
				}
			}
			GInspectorPendingTexIndex = INDEX_NONE;
		}
		if (GInspectorPendingSlice != INDEX_NONE)
		{
			Source = EInspectorSource::MergedSet;
			Slice = FMath::Max(0, GInspectorPendingSlice);
			bChanged = true;
			GInspectorPendingSlice = INDEX_NONE;
		}
		if (bRefresh && bChanged)
		{
			bPendingInitialLoad = false;
			LiveData = FInspectorLiveData();
			MergedData = FInspectorMergedData();
			RefreshSource();
			UE_LOG(LogNDGI, Display, TEXT("NDGI Lightmap Inspector: focused on %s."), *GetStatusText().ToString());
		}
	}

	EActiveTimerReturnType SNDGILightmapInspectorPanel::Tick(double, float)
	{
		if (bPendingInitialLoad)
		{
			// Readback and merged-set loading are intentionally deferred until
			// the editor has finished constructing the tab.
			bPendingInitialLoad = false;
			RefreshSource();
			UE_LOG(LogNDGI, Display, TEXT("NDGI Lightmap Inspector: initial load — %s."), *GetStatusText().ToString());
			return EActiveTimerReturnType::Continue;
		}

		// Keep the header/status in sync with the manifest and selected atlas
		// without repeatedly reading the large lightmap texture.
		const double Now = FPlatformTime::Seconds();
		if (Now - LastRefreshSeconds > 2.0)
		{
			LastRefreshSeconds = Now;
			UpdateInfoText();
			UpdateStatsText();
		}
		return EActiveTimerReturnType::Continue;
	}

	UWorld* SNDGILightmapInspectorPanel::GetEditorWorld() const
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	UNDGILevelAsset* SNDGILightmapInspectorPanel::GetLevelAsset() const
	{
		return LoadLevelAssetForWorld(GetEditorWorld());
	}

	const FInspectorAtlas* SNDGILightmapInspectorPanel::GetSelectedAtlas() const
	{
		return Atlases.IsValidIndex(SelectedAtlas) ? &Atlases[SelectedAtlas] : nullptr;
	}

	const FNDGIAtlasEntry* SNDGILightmapInspectorPanel::GetSelectedLevelEntry() const
	{
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		return Atlas ? FindLevelEntry(GetLevelAsset(), *Atlas) : nullptr;
	}

	int32 SNDGILightmapInspectorPanel::GetSliceCount() const
	{
		return MergedData.bLoaded ? MergedData.Set.Steps.Num() : 0;
	}

	int32 SNDGILightmapInspectorPanel::GetDisplayWidth() const
	{
		return Source == EInspectorSource::LiveAtlas ? LiveData.Width : MergedData.Set.Width;
	}

	int32 SNDGILightmapInspectorPanel::GetDisplayHeight() const
	{
		return Source == EInspectorSource::LiveAtlas ? LiveData.Height : MergedData.Set.Height;
	}

	FText SNDGILightmapInspectorPanel::GetAtlasLabel() const
	{
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		if (!Atlas)
		{
			return LOCTEXT("NoAtlas", "No atlas");
		}
		return FText::FromString(FString::Printf(TEXT("Tex%02d  %dx%d"), Atlas->TexIndex, Atlas->Width, Atlas->Height));
	}

	FText SNDGILightmapInspectorPanel::GetSliceLabel() const
	{
		if (!MergedData.bLoaded || MergedData.Set.Steps.Num() == 0)
		{
			return FText::GetEmpty();
		}
		const int32 Count = MergedData.Set.Steps.Num();
		const float Time = Count > 1 ? static_cast<float>(Slice) / static_cast<float>(Count - 1) : 0.0f;
		return FText::FromString(FString::Printf(TEXT("Step %d/%d  t=%.3f"), Slice + 1, Count, Time));
	}

	FText SNDGILightmapInspectorPanel::GetSourceLabel() const
	{
		return FText::FromString(SourceToString(Source));
	}

	FText SNDGILightmapInspectorPanel::GetStatusText() const
	{
		if (!LoadError.IsEmpty())
		{
			return FText::FromString(LoadError);
		}
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		if (!Atlas)
		{
			return LOCTEXT("NoAtlasStatus", "No lightmap atlas found.");
		}
		if (Source == EInspectorSource::LiveAtlas)
		{
			return LiveData.bLoaded
				? FText::FromString(FString::Printf(TEXT("Live CPU Lightmass · %dx%d"), LiveData.Width, LiveData.Height))
				: LOCTEXT("LiveNotLoaded", "Live atlas not loaded.");
		}
		return MergedData.bLoaded
			? FText::FromString(FString::Printf(TEXT("Merged .ndgi · %dx%d · %d slices"), MergedData.Set.Width, MergedData.Set.Height, MergedData.Set.Steps.Num()))
			: LOCTEXT("MergedNotLoaded", "Merged set not loaded.");
	}

	FSlateColor SNDGILightmapInspectorPanel::GetStatusColor() const
	{
		if (!LoadError.IsEmpty())
		{
			return FSlateColor(InspectorErrorText);
		}
		return FSlateColor(InspectorMutedText);
	}

	void SNDGILightmapInspectorPanel::RefreshAtlasList()
	{
		Atlases.Reset();
		LiveData = FInspectorLiveData();
		MergedData = FInspectorMergedData();
		ManifestSummary.Reset();
		LoadError.Reset();

		UWorld* World = GetEditorWorld();
		BakeManifest::FManifest Manifest;
		const bool bHasManifest = BakeManifest::Load(Manifest);
		UNDGILevelAsset* LevelAsset = GetLevelAsset();
		const FString WorldPackage = World ? World->GetOutermost()->GetName() : FString();
		const bool bManifestMatches = bHasManifest && Manifest.LevelPackage == WorldPackage;

		if (bHasManifest)
		{
			ManifestSummary = FString::Printf(TEXT("%s · %d slices · %d atlas(es) · %s"),
				*Manifest.LevelPackage, Manifest.Steps, Manifest.Atlases.Num(),
				bManifestMatches ? TEXT("matches level") : TEXT("DIFFERENT LEVEL"));
			const FString BakeDir = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
			for (const BakeManifest::FAtlasEntry& ManifestAtlas : Manifest.Atlases)
			{
				FInspectorAtlas& Atlas = Atlases.AddDefaulted_GetRef();
				Atlas.TexIndex = ManifestAtlas.TexIndex;
				Atlas.TexturePath = ManifestAtlas.TexturePath;
				Atlas.Width = ManifestAtlas.Width;
				Atlas.Height = ManifestAtlas.Height;
				Atlas.Cells = ManifestAtlas.Cells;
				Atlas.MergedPath = BakeDir / FString::Printf(TEXT("Merged_Tex%02d.ndgi"), Atlas.TexIndex);
				Atlas.bMergedExists = IFileManager::Get().FileExists(*Atlas.MergedPath);
				if (Atlas.bMergedExists)
				{
					Atlas.MergedTime = IFileManager::Get().GetTimeStamp(*Atlas.MergedPath);
				}
			}
		}
		else if (LevelAsset)
		{
			ManifestSummary = FString::Printf(TEXT("No bake manifest · using level asset with %d atlas(es)"), LevelAsset->Atlases.Num());
			const FString BakeDir = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
			for (int32 Index = 0; Index < LevelAsset->Atlases.Num(); ++Index)
			{
				const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[Index];
				FInspectorAtlas& Atlas = Atlases.AddDefaulted_GetRef();
				Atlas.TexIndex = Index;
				Atlas.TexturePath = Entry.LightmapTexturePath.ToString();
				Atlas.Width = Entry.AtlasSize.X;
				Atlas.Height = Entry.AtlasSize.Y;
				Atlas.Cells = Entry.SourceCells;
				Atlas.MergedPath = BakeDir / FString::Printf(TEXT("Merged_Tex%02d.ndgi"), Index);
				Atlas.bMergedExists = IFileManager::Get().FileExists(*Atlas.MergedPath);
				if (Atlas.bMergedExists)
				{
					Atlas.MergedTime = IFileManager::Get().GetTimeStamp(*Atlas.MergedPath);
				}
			}
		}
		else
		{
			TArray<LightmapSources::FAtlasSource> Sources;
			int32 VirtualTextureCount = 0;
			LightmapSources::CollectLightmapTextures(World, Sources, VirtualTextureCount);
			ManifestSummary = FString::Printf(TEXT("No manifest or level asset · %d world atlas(es)"), Sources.Num());
			for (int32 Index = 0; Index < Sources.Num(); ++Index)
			{
				FInspectorAtlas& Atlas = Atlases.AddDefaulted_GetRef();
				Atlas.TexIndex = Index;
				Atlas.TexturePath = Sources[Index].Texture ? Sources[Index].Texture->GetPathName() : FString();
				if (Sources[Index].Texture)
				{
					const FTexturePlatformData* PlatformData = Sources[Index].Texture->GetPlatformData();
					Atlas.Width = PlatformData && PlatformData->Mips.Num() > 0 ? PlatformData->Mips[0].SizeX : 0;
					Atlas.Height = PlatformData && PlatformData->Mips.Num() > 0 ? PlatformData->Mips[0].SizeY : 0;
				}
			}
		}

		Atlases.Sort([](const FInspectorAtlas& A, const FInspectorAtlas& B) { return A.TexIndex < B.TexIndex; });
		SelectedAtlas = Atlases.Num() > 0 ? FMath::Clamp(SelectedAtlas, 0, Atlases.Num() - 1) : 0;
		Slice = FMath::Clamp(Slice, 0, FMath::Max(0, GetSliceCount() - 1));
		if (HeaderText.IsValid())
		{
			HeaderText->SetText(FText::FromString(ManifestSummary));
		}
		UpdateInfoText();
		UpdateRangeText();
	}

	void SNDGILightmapInspectorPanel::RefreshSource()
	{
		LoadError.Reset();
		if (Source == EInspectorSource::LiveAtlas)
		{
			LoadLiveData();
		}
		else
		{
			LoadMergedData();
		}
		RebuildDisplay();
		UpdateInfoText();
		UpdateStatsText();
		UpdateRangeText();
	}

	void SNDGILightmapInspectorPanel::LoadLiveData()
	{
		LiveData = FInspectorLiveData();
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		UWorld* World = GetEditorWorld();
		if (!Atlas || Atlas->TexturePath.IsEmpty())
		{
			LoadError = TEXT("No live atlas selected.");
			return;
		}

		TArray<LightmapSources::FAtlasSource> Sources;
		int32 VirtualTextureCount = 0;
		LightmapSources::CollectLightmapTextures(World, Sources, VirtualTextureCount);

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *Atlas->TexturePath);
		if (!Texture)
		{
			Texture = Cast<UTexture2D>(FSoftObjectPath(Atlas->TexturePath).TryLoad());
		}
		if (!Texture)
		{
			// Fall back to the world scan so manifest paths that reference an
			// unloaded built-data subobject still resolve by atlas index/name.
			for (const LightmapSources::FAtlasSource& AtlasSource : Sources)
			{
				if (AtlasSource.Texture && (AtlasSource.Texture->GetPathName() == Atlas->TexturePath
					|| (Atlas->TexIndex >= 0 && Sources.IsValidIndex(Atlas->TexIndex) && AtlasSource.Texture == Sources[Atlas->TexIndex].Texture)))
				{
					Texture = AtlasSource.Texture;
					break;
				}
			}
		}
		if (!Texture)
		{
			LoadError = FString::Printf(TEXT("Could not load live atlas '%s'."), *Atlas->TexturePath);
			return;
		}

		const LightmapSources::FAtlasSource* SourceInfo = nullptr;
		for (const LightmapSources::FAtlasSource& AtlasSource : Sources)
		{
			if (AtlasSource.Texture == Texture)
			{
				SourceInfo = &AtlasSource;
				break;
			}
		}

		FTextureResource* Resource = Texture->GetResource();
		FTexturePlatformData* PlatformData = Texture->GetPlatformData();
		const FRHITexture* RHI = Resource ? Resource->GetTextureRHI() : nullptr;
		if (!PlatformData || PlatformData->Mips.Num() == 0)
		{
			LoadError = TEXT("Live atlas has no platform data.");
			return;
		}
		const int32 FullWidth = PlatformData->Mips[0].SizeX;
		const int32 FullHeight = PlatformData->Mips[0].SizeY;
		const bool bForcedResident = !RHI
			|| static_cast<int32>(RHI->GetSizeX()) < FullWidth
			|| static_cast<int32>(RHI->GetSizeY()) < FullHeight;
		const bool bWasNeverStream = Texture->NeverStream;
		if (bForcedResident)
		{
			Texture->NeverStream = true;
			Texture->UpdateResource();
			FlushRenderingCommands();
		}

		LightmapSources::FReadback Readback;
		const bool bReadbackOk = LightmapSources::ReadbackTexture(Texture, Readback);
		if (bForcedResident)
		{
			Texture->NeverStream = bWasNeverStream;
			Texture->UpdateResource();
			FlushRenderingCommands();
		}
		if (!bReadbackOk)
		{
			LoadError = FString::Printf(TEXT("Readback failed for '%s'."), *Texture->GetName());
			return;
		}

		LiveData.bLoaded = true;
		LiveData.RawWidth = Readback.Width;
		LiveData.RawHeight = Readback.Height;
		LiveData.Width = Readback.Width;
		LiveData.Height = Readback.Height / 2;
		LiveData.RawPixels = MoveTemp(Readback.Pixels);
		if (LiveData.Width <= 0 || LiveData.Height <= 0 || LiveData.RawHeight < LiveData.Height * 2)
		{
			LiveData.bLoaded = false;
			LoadError = TEXT("Live atlas has an invalid size.");
			return;
		}

		const FNDGIAtlasEntry* LevelEntry = GetSelectedLevelEntry();
		LiveData.bHighQuality = LevelEntry ? LevelEntry->bHighQuality : (SourceInfo ? SourceInfo->bHighQuality : true);

		NDGI::Editor::FEncodingRangeMap RangeMap;
		FString RangeError;
		bool bHaveRanges = World && NDGI::Editor::BuildEncodingRangeMap(World, Texture, LiveData.Width, LiveData.Height, RangeMap, &RangeError);
		if (bHaveRanges)
		{
			LiveData.Ranges = RangeMap.Ranges;
			LiveData.RangeIndices = RangeMap.Indices;
			LiveData.Coverage = RangeMap.Coverage;
		}
		else if (LevelEntry
			&& LevelEntry->EncodingRanges.Num() > 0
			&& LevelEntry->EncodingRangeIndices.Num() == LiveData.Width * LiveData.Height)
		{
			LiveData.Ranges = LevelEntry->EncodingRanges;
			LiveData.RangeIndices = LevelEntry->EncodingRangeIndices;
			LiveData.Coverage.SetNumUninitialized(LiveData.Width * LiveData.Height);
			for (int32 Texel = 0; Texel < LiveData.Coverage.Num(); ++Texel)
			{
				LiveData.Coverage[Texel] = LiveData.RangeIndices[Texel] != 255 ? 1 : 0;
			}
		}
		else
		{
			LiveData.Ranges.SetNum(1);
			FNDGIEncodingRange& DefaultRange = LiveData.Ranges[0];
			DefaultRange.Scale0 = SourceInfo ? SourceInfo->Scale : FVector4f(1, 1, 1, 1);
			DefaultRange.Add0 = SourceInfo ? SourceInfo->Add : FVector4f(0, 0, 0, 0);
			DefaultRange.Scale1 = SourceInfo ? SourceInfo->ScaleSH : FVector4f(1, 1, 1, 1);
			DefaultRange.Add1 = SourceInfo ? SourceInfo->AddSH : FVector4f(0, 0, 0, 0);
			LiveData.RangeIndices.Init(0, LiveData.Width * LiveData.Height);
			LiveData.Coverage.Init(1, LiveData.Width * LiveData.Height);
			UE_LOG(LogNDGI, Warning, TEXT("Lightmap inspector: no encoding range map for '%s' (%s); using the atlas-wide fallback."),
				*Texture->GetName(), RangeError.IsEmpty() ? TEXT("no covered components") : *RangeError);
		}

		LiveData.RGB.SetNumUninitialized(LiveData.Width * LiveData.Height);
		LiveData.SH.SetNumUninitialized(LiveData.Width * LiveData.Height);
		for (int32 Y = 0; Y < LiveData.Height; ++Y)
		{
			for (int32 X = 0; X < LiveData.Width; ++X)
			{
				const int32 Texel = Y * LiveData.Width + X;
				const int32 RangeIndex = LiveData.RangeIndices.IsValidIndex(Texel) ? LiveData.RangeIndices[Texel] : 0;
				const FNDGIEncodingRange& Range = LiveData.Ranges[FMath::Clamp<int32>(RangeIndex, 0, LiveData.Ranges.Num() - 1)];
				const FLinearColor Top = LiveData.RawPixels[Y * LiveData.RawWidth + X];
				const FLinearColor Bottom = LiveData.RawPixels[(Y + LiveData.Height) * LiveData.RawWidth + X];
				if (LiveData.bHighQuality)
				{
					LightmapSources::DecodeHQTexel(Top, Bottom, Range.Scale0, Range.Add0, Range.Scale1, Range.Add1,
						LiveData.RGB[Texel], LiveData.SH[Texel]);
				}
				else
				{
					LightmapSources::DecodeLQTexel(Top, Range.Scale0, Range.Add0, LiveData.RGB[Texel], LiveData.SH[Texel]);
				}
			}
		}
	}

	void SNDGILightmapInspectorPanel::LoadMergedData()
	{
		MergedData = FInspectorMergedData();
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		if (!Atlas || Atlas->MergedPath.IsEmpty())
		{
			LoadError = TEXT("No merged set selected.");
			return;
		}
		MergedData.Path = Atlas->MergedPath;
		if (!IFileManager::Get().FileExists(*MergedData.Path))
		{
			LoadError = FString::Printf(TEXT("Merged set not found: %s"), *MergedData.Path);
			return;
		}
		if (!MergedData.Set.Load(MergedData.Path) || MergedData.Set.Steps.Num() == 0)
		{
			LoadError = FString::Printf(TEXT("Could not load merged set: %s"), *MergedData.Path);
			return;
		}
		MergedData.bLoaded = true;
		Slice = FMath::Clamp(Slice, 0, MergedData.Set.Steps.Num() - 1);
	}

	void SNDGILightmapInspectorPanel::SetSource(EInspectorSource NewSource)
	{
		if (Source == NewSource)
		{
			return;
		}
		Source = NewSource;
		if (Source == EInspectorSource::LiveAtlas && View != EInspectorView::Difference)
		{
			MergedData = FInspectorMergedData();
		}
		RefreshSource();
	}

	void SNDGILightmapInspectorPanel::SetView(EInspectorView NewView)
	{
		View = NewView;
		LoadError.Reset();
		if (View == EInspectorView::Difference && !MergedData.bLoaded)
		{
			LoadMergedData();
		}
		if (View == EInspectorView::Difference && !LiveData.bLoaded)
		{
			LoadLiveData();
		}
		RebuildDisplay();
	}

	void SNDGILightmapInspectorPanel::SetSlice(int32 NewSlice)
	{
		Slice = FMath::Clamp(NewSlice, 0, FMath::Max(0, GetSliceCount() - 1));
		RebuildDisplay();
		UpdateReadoutText();
	}

	void SNDGILightmapInspectorPanel::SetZoom(float NewZoom)
	{
		Zoom = FMath::Clamp(NewZoom, 0.05f, 8.0f);
		Invalidate(EInvalidateWidget::Layout);
	}

	void SNDGILightmapInspectorPanel::SelectAtlasByOffset(int32 Offset)
	{
		if (Atlases.Num() == 0)
		{
			return;
		}
		SelectedAtlas = (SelectedAtlas + Offset + Atlases.Num()) % Atlases.Num();
		Slice = 0;
		LiveData = FInspectorLiveData();
		MergedData = FInspectorMergedData();
		RefreshSource();
	}

	void SNDGILightmapInspectorPanel::RebuildDisplay()
	{
		const bool bNeedLive = Source == EInspectorSource::LiveAtlas || View == EInspectorView::Difference;
		const bool bNeedMerged = Source == EInspectorSource::MergedSet || View == EInspectorView::Difference;
		if (bNeedLive && !LiveData.bLoaded)
		{
			LoadLiveData();
		}
		if (bNeedMerged && !MergedData.bLoaded)
		{
			LoadMergedData();
		}
		if (View == EInspectorView::Difference && (!LiveData.bLoaded || !MergedData.bLoaded))
		{
			DisplayPixels.Reset();
			Brush.SetResourceObject(nullptr);
			LoadError = TEXT("Difference view needs both the live atlas and a merged set.");
			Invalidate(EInvalidateWidget::Layout);
			return;
		}

		const int32 Width = Source == EInspectorSource::LiveAtlas ? LiveData.Width : MergedData.Set.Width;
		const int32 Height = Source == EInspectorSource::LiveAtlas ? LiveData.Height : MergedData.Set.Height;
		if (Width <= 0 || Height <= 0)
		{
			DisplayPixels.Reset();
			Brush.SetResourceObject(nullptr);
			Invalidate(EInvalidateWidget::Layout);
			return;
		}

		DisplayPixels.SetNumUninitialized(Width * Height);
		const int32 TileSize = [this]()
		{
			const FNDGIAtlasEntry* Entry = GetSelectedLevelEntry();
			return Entry ? Entry->TileSize : 0;
		}();

		const FLightmapStep* MergedStep = MergedData.Set.Steps.IsValidIndex(Slice) ? &MergedData.Set.Steps[Slice] : nullptr;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Texel = Y * Width + X;
				const bool bValid = Source == EInspectorSource::LiveAtlas
					? (LiveData.Coverage.IsValidIndex(Texel) ? LiveData.Coverage[Texel] != 0 : true)
					: (MergedStep && MergedStep->Mask.IsValidIndex(Texel) ? MergedStep->Mask[Texel] != 0 : true);

				FVector3f RGB = FVector3f::ZeroVector;
				FVector3f SH = FVector3f::ZeroVector;
				if (Source == EInspectorSource::LiveAtlas)
				{
					if (LiveData.RGB.IsValidIndex(Texel)) { RGB = LiveData.RGB[Texel]; }
					if (LiveData.SH.IsValidIndex(Texel)) { SH = LiveData.SH[Texel]; }
				}
				else if (MergedStep)
				{
					if (MergedStep->RGB.IsValidIndex(Texel * 3 + 2))
					{
						RGB = FVector3f(MergedStep->RGB[Texel * 3 + 0], MergedStep->RGB[Texel * 3 + 1], MergedStep->RGB[Texel * 3 + 2]);
					}
					if (MergedStep->SH.IsValidIndex(Texel * 3 + 2))
					{
						SH = FVector3f(MergedStep->SH[Texel * 3 + 0], MergedStep->SH[Texel * 3 + 1], MergedStep->SH[Texel * 3 + 2]);
					}
				}

				FColor Color(0, 0, 0, 255);
				const FLinearColor ExposureColor = FLinearColor(RGB.X, RGB.Y, RGB.Z, 1.0f) * FMath::Pow(2.0f, Exposure);
				switch (View)
				{
				case EInspectorView::FinalColor:
					Color = LinearToDisplayColor(ExposureColor);
					break;
				case EInspectorView::RGB:
					Color = LinearToDisplayColor(ExposureColor);
					break;
				case EInspectorView::Red:
					Color = LinearToDisplayColor(FLinearColor(ExposureColor.R, ExposureColor.R, ExposureColor.R, 1.0f));
					break;
				case EInspectorView::Green:
					Color = LinearToDisplayColor(FLinearColor(ExposureColor.G, ExposureColor.G, ExposureColor.G, 1.0f));
					break;
				case EInspectorView::Blue:
					Color = LinearToDisplayColor(FLinearColor(ExposureColor.B, ExposureColor.B, ExposureColor.B, 1.0f));
					break;
				case EInspectorView::Luminance:
				{
					const float L = FMath::Max(0.0f, 0.3f * RGB.X + 0.59f * RGB.Y + 0.11f * RGB.Z);
					const float LogL = FMath::Clamp((FMath::Log2(L + 1e-5f) + 10.0f) / 10.0f + Exposure / 10.0f, 0.0f, 1.0f);
					Color = FLinearColor(LogL, LogL, LogL, 1.0f).ToFColor(true);
					break;
				}
				case EInspectorView::Alpha:
					if (Source == EInspectorSource::LiveAtlas && LiveData.RawPixels.IsValidIndex(Y * LiveData.RawWidth + X))
					{
						const float A = LiveData.RawPixels[Y * LiveData.RawWidth + X].A;
						Color = FLinearColor(A, A, A, 1.0f).ToFColor(true);
					}
					else
					{
						Color = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f).ToFColor(true);
					}
					break;
				case EInspectorView::SH:
					Color = FLinearColor(
						FMath::Clamp(0.5f + SH.X * 0.5f, 0.0f, 1.0f),
						FMath::Clamp(0.5f + SH.Y * 0.5f, 0.0f, 1.0f),
						FMath::Clamp(0.5f + SH.Z * 0.5f, 0.0f, 1.0f),
						1.0f).ToFColor(true);
					break;
				case EInspectorView::SHW:
					if (Source == EInspectorSource::LiveAtlas && LiveData.RawPixels.IsValidIndex((Y + LiveData.Height) * LiveData.RawWidth + X))
					{
						const float W = LiveData.RawPixels[(Y + LiveData.Height) * LiveData.RawWidth + X].A;
						Color = FLinearColor(W, W, W, 1.0f).ToFColor(true);
					}
					else
					{
						Color = FLinearColor(0.1f, 0.1f, 0.1f, 1.0f).ToFColor(true);
					}
					break;
				case EInspectorView::RangeIndex:
				{
					const uint8 RangeIndex = Source == EInspectorSource::LiveAtlas
						? (LiveData.RangeIndices.IsValidIndex(Texel) ? LiveData.RangeIndices[Texel] : 0)
						: (MergedData.Set.EncodingRangeIndices.IsValidIndex(Texel) ? MergedData.Set.EncodingRangeIndices[Texel] : 0);
					Color = RangeIndex == 255 ? FColor(16, 16, 16, 255) : PaletteColor(RangeIndex);
					break;
				}
				case EInspectorView::Coverage:
					Color = bValid ? FColor(220, 220, 220, 255) : FColor(24, 24, 24, 255);
					break;
				case EInspectorView::Difference:
				{
					FVector3f LiveRGB = FVector3f::ZeroVector;
					FVector3f MergedRGB = FVector3f::ZeroVector;
					if (LiveData.RGB.IsValidIndex(Texel)) { LiveRGB = LiveData.RGB[Texel]; }
					if (MergedStep && MergedStep->RGB.IsValidIndex(Texel * 3 + 2))
					{
						MergedRGB = FVector3f(MergedStep->RGB[Texel * 3 + 0], MergedStep->RGB[Texel * 3 + 1], MergedStep->RGB[Texel * 3 + 2]);
					}
					const FVector3f Diff = (LiveRGB - MergedRGB).GetAbs();
					const float Value = FMath::Max3(Diff.X, Diff.Y, Diff.Z) * FMath::Pow(2.0f, Exposure);
					Color = HeatColor(Value * 8.0f);
					break;
				}
				default:
					break;
				}

				if (bShowCoverage && !bValid && View != EInspectorView::Coverage && View != EInspectorView::RangeIndex)
				{
					Color = FColor(
						static_cast<uint8>(Color.R * 0.2f),
						static_cast<uint8>(Color.G * 0.2f + 18),
						static_cast<uint8>(Color.B * 0.2f + 40),
						255);
				}
				if (bShowTileGrid && TileSize > 0 && (X % TileSize == 0 || Y % TileSize == 0))
				{
					Color = FColor(255, 190, 30, 255);
				}
				DisplayPixels[Texel] = Color;
			}
		}

		UpdateDisplayTexture();
		UpdateStatsText();
		UpdateRangeText();
		Invalidate(EInvalidateWidget::Layout);
	}

	void SNDGILightmapInspectorPanel::UpdateDisplayTexture()
	{
		const int32 Width = GetDisplayWidth();
		const int32 Height = GetDisplayHeight();
		if (Width <= 0 || Height <= 0 || DisplayPixels.Num() != Width * Height)
		{
			Brush.SetResourceObject(nullptr);
			return;
		}

		if (!DisplayTexture.IsValid()
			|| DisplayTexture->GetPlatformData() == nullptr
			|| DisplayTexture->GetPlatformData()->SizeX != Width
			|| DisplayTexture->GetPlatformData()->SizeY != Height)
		{
			DisplayTexture.Reset();
			UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8);
			if (!Texture)
			{
				return;
			}
			Texture->SRGB = false;
			Texture->Filter = TF_Nearest;
			Texture->NeverStream = true;
			Texture->UpdateResource();
			FlushRenderingCommands();
			DisplayTexture = TStrongObjectPtr<UTexture2D>(Texture);
		}

		TArray<FColor>* UploadPixels = new TArray<FColor>(DisplayPixels);
		FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Width, Height);
		DisplayTexture->UpdateTextureRegions(
			0, 1, Region, Width * sizeof(FColor), sizeof(FColor),
			reinterpret_cast<uint8*>(UploadPixels->GetData()),
			[UploadPixels, Region](uint8*, const FUpdateTextureRegion2D*)
			{
				delete UploadPixels;
				delete Region;
			});

		Brush.SetResourceObject(DisplayTexture.Get());
		Brush.ImageSize = FVector2D(Width, Height);
		if (ImageWidget.IsValid())
		{
			ImageWidget->InvalidateImage();
		}
	}

	void SNDGILightmapInspectorPanel::UpdateInfoText()
	{
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		const FNDGIAtlasEntry* LevelEntry = GetSelectedLevelEntry();
		if (!Atlas)
		{
			if (InfoText.IsValid())
			{
				InfoText->SetText(FText::FromString(TEXT("No lightmap atlas available. Bake the level or train it to create a level asset.")));
			}
			if (StatusText.IsValid())
			{
				StatusText->SetText(GetStatusText());
				StatusText->SetColorAndOpacity(GetStatusColor());
			}
			return;
		}

		FString Info;
		Info += FString::Printf(TEXT("Source: %s\n"), *SourceToString(Source));
		Info += FString::Printf(TEXT("Atlas: Tex%02d  %dx%d\n"), Atlas->TexIndex, Atlas->Width, Atlas->Height);
		Info += FString::Printf(TEXT("Path: %s\n"), *Atlas->TexturePath);
		if (Atlas->Cells.Num() > 0)
		{
			Info += FString::Printf(TEXT("Cells: %s\n"), *FString::Join(Atlas->Cells, TEXT(", ")));
		}
		if (Source == EInspectorSource::MergedSet)
		{
			Info += FString::Printf(TEXT("Merged: %s\n"), *Atlas->MergedPath);
		}
		if (LevelEntry)
		{
			Info += FString::Printf(TEXT("Tile: %d · Grid %dx%d · Halo %d\n"),
				LevelEntry->TileSize, LevelEntry->TileGrid.X, LevelEntry->TileGrid.Y, LevelEntry->OverlapTexels);
		}
		else
		{
			Info += TEXT("Tile grid: no matching level asset entry\n");
		}
		if (Source == EInspectorSource::LiveAtlas)
		{
			Info += FString::Printf(TEXT("Format: %s\n"), LiveData.bHighQuality ? TEXT("HQ") : TEXT("LQ"));
		}
		else if (MergedData.bLoaded)
		{
			Info += FString::Printf(TEXT("Steps: %d · Range map: %s\n"), MergedData.Set.Steps.Num(),
				MergedData.Set.EncodingRanges.Num() > 0 ? TEXT("yes") : TEXT("no"));
		}

		if (InfoText.IsValid())
		{
			InfoText->SetText(FText::FromString(Info));
		}
		if (StatusText.IsValid())
		{
			StatusText->SetText(GetStatusText());
			StatusText->SetColorAndOpacity(GetStatusColor());
		}
	}

	void SNDGILightmapInspectorPanel::UpdateStatsText()
	{
		const int32 Width = GetDisplayWidth();
		const int32 Height = GetDisplayHeight();
		if (!StatsText.IsValid())
		{
			return;
		}
		if (Width <= 0 || Height <= 0)
		{
			StatsText->SetText(FText::GetEmpty());
			return;
		}

		int32 ValidCount = 0;
		double SumLum = 0.0;
		double MaxLum = 0.0;
		const FLightmapStep* MergedStep = MergedData.Set.Steps.IsValidIndex(Slice) ? &MergedData.Set.Steps[Slice] : nullptr;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Texel = Y * Width + X;
				const bool bValid = Source == EInspectorSource::LiveAtlas
					? (LiveData.Coverage.IsValidIndex(Texel) ? LiveData.Coverage[Texel] != 0 : true)
					: (MergedStep && MergedStep->Mask.IsValidIndex(Texel) ? MergedStep->Mask[Texel] != 0 : true);
				if (!bValid)
				{
					continue;
				}
				++ValidCount;
				FVector3f RGB = FVector3f::ZeroVector;
				if (Source == EInspectorSource::LiveAtlas && LiveData.RGB.IsValidIndex(Texel))
				{
					RGB = LiveData.RGB[Texel];
				}
				else if (MergedStep && MergedStep->RGB.IsValidIndex(Texel * 3 + 2))
				{
					RGB = FVector3f(MergedStep->RGB[Texel * 3 + 0], MergedStep->RGB[Texel * 3 + 1], MergedStep->RGB[Texel * 3 + 2]);
				}
				const double L = 0.3 * RGB.X + 0.59 * RGB.Y + 0.11 * RGB.Z;
				SumLum += L;
				MaxLum = FMath::Max(MaxLum, L);
			}
		}

		const TArray<FNDGIEncodingRange>& Ranges = Source == EInspectorSource::LiveAtlas ? LiveData.Ranges : MergedData.Set.EncodingRanges;
		const double Coverage = Width * Height > 0 ? 100.0 * static_cast<double>(ValidCount) / static_cast<double>(Width * Height) : 0.0;
		const double MeanLum = ValidCount > 0 ? SumLum / static_cast<double>(ValidCount) : 0.0;
		const FString Text = FString::Printf(
			TEXT("Valid: %.2f%%  (%d/%d)\nMean luminance: %.5f\nMax luminance: %.5f\nRanges: %d\nSlice: %d / %d"),
			Coverage, ValidCount, Width * Height, MeanLum, MaxLum, Ranges.Num(),
			Slice + 1, FMath::Max(1, GetSliceCount()));
		StatsText->SetText(FText::FromString(Text));
	}

	void SNDGILightmapInspectorPanel::UpdateReadoutText()
	{
		if (!ReadoutText.IsValid())
		{
			return;
		}
		const int32 Width = GetDisplayWidth();
		const int32 Height = GetDisplayHeight();
		if (HoverX < 0 || HoverY < 0 || HoverX >= Width || HoverY >= Height)
		{
			ReadoutText->SetText(FText::FromString(TEXT("Move over the atlas to inspect a texel.")));
			return;
		}

		const int32 Texel = HoverY * Width + HoverX;
		FString Text = FString::Printf(TEXT("Texel: (%d, %d)\n"), HoverX, HoverY);
		if (Source == EInspectorSource::LiveAtlas && LiveData.bLoaded)
		{
			if (LiveData.RawPixels.IsValidIndex(HoverY * LiveData.RawWidth + HoverX))
			{
				const FLinearColor& Top = LiveData.RawPixels[HoverY * LiveData.RawWidth + HoverX];
				const FLinearColor& Bottom = LiveData.RawPixels[(HoverY + LiveData.Height) * LiveData.RawWidth + HoverX];
				Text += FString::Printf(TEXT("Raw top:   %.4f %.4f %.4f %.4f\n"), Top.R, Top.G, Top.B, Top.A);
				Text += FString::Printf(TEXT("Raw bottom: %.4f %.4f %.4f %.4f\n"), Bottom.R, Bottom.G, Bottom.B, Bottom.A);
			}
			if (LiveData.RGB.IsValidIndex(Texel))
			{
				const FVector3f& RGB = LiveData.RGB[Texel];
				const FVector3f& SH = LiveData.SH[Texel];
				Text += FString::Printf(TEXT("Linear RGB: %.5f %.5f %.5f\n"), RGB.X, RGB.Y, RGB.Z);
				Text += FString::Printf(TEXT("SH.xyz:     %.5f %.5f %.5f\n"), SH.X, SH.Y, SH.Z);
			}
			Text += FString::Printf(TEXT("Coverage: %s\n"), LiveData.Coverage.IsValidIndex(Texel) && LiveData.Coverage[Texel] ? TEXT("valid") : TEXT("padding"));
			const uint8 RangeIndex = LiveData.RangeIndices.IsValidIndex(Texel) ? LiveData.RangeIndices[Texel] : 0;
			Text += FString::Printf(TEXT("Range: %d / %d\n"), RangeIndex, LiveData.Ranges.Num());
			if (LiveData.Ranges.IsValidIndex(RangeIndex))
			{
				const FNDGIEncodingRange& Range = LiveData.Ranges[RangeIndex];
				Text += FString::Printf(TEXT("Scale0: %.5f %.5f %.5f %.5f\n"), Range.Scale0.X, Range.Scale0.Y, Range.Scale0.Z, Range.Scale0.W);
				Text += FString::Printf(TEXT("Add0:   %.5f %.5f %.5f %.5f\n"), Range.Add0.X, Range.Add0.Y, Range.Add0.Z, Range.Add0.W);
			}
		}
		else if (MergedData.bLoaded && MergedData.Set.Steps.IsValidIndex(Slice))
		{
			const FLightmapStep& Step = MergedData.Set.Steps[Slice];
			if (Step.RGB.IsValidIndex(Texel * 3 + 2))
			{
				Text += FString::Printf(TEXT("Linear RGB: %.5f %.5f %.5f\n"),
					Step.RGB[Texel * 3 + 0], Step.RGB[Texel * 3 + 1], Step.RGB[Texel * 3 + 2]);
			}
			if (Step.SH.IsValidIndex(Texel * 3 + 2))
			{
				Text += FString::Printf(TEXT("SH.xyz:     %.5f %.5f %.5f\n"),
					Step.SH[Texel * 3 + 0], Step.SH[Texel * 3 + 1], Step.SH[Texel * 3 + 2]);
			}
			Text += FString::Printf(TEXT("Coverage: %s\n"), Step.Mask.IsValidIndex(Texel) && Step.Mask[Texel] ? TEXT("valid") : TEXT("padding"));
			const uint8 RangeIndex = MergedData.Set.EncodingRangeIndices.IsValidIndex(Texel)
				? MergedData.Set.EncodingRangeIndices[Texel] : 0;
			Text += FString::Printf(TEXT("Range: %d / %d\n"), RangeIndex, MergedData.Set.EncodingRanges.Num());
			if (MergedData.Set.EncodingRanges.IsValidIndex(RangeIndex))
			{
				const FNDGIEncodingRange& Range = MergedData.Set.EncodingRanges[RangeIndex];
				Text += FString::Printf(TEXT("Scale0: %.5f %.5f %.5f %.5f\n"), Range.Scale0.X, Range.Scale0.Y, Range.Scale0.Z, Range.Scale0.W);
				Text += FString::Printf(TEXT("Add0:   %.5f %.5f %.5f %.5f\n"), Range.Add0.X, Range.Add0.Y, Range.Add0.Z, Range.Add0.W);
			}
		}

		const FNDGIAtlasEntry* Entry = GetSelectedLevelEntry();
		if (Entry && Entry->TileSize > 0)
		{
			Text += FString::Printf(TEXT("Tile: (%d, %d) of %dx%d\n"),
				HoverX / Entry->TileSize, HoverY / Entry->TileSize,
				Entry->TileGrid.X, Entry->TileGrid.Y);
		}
		ReadoutText->SetText(FText::FromString(Text));
	}

	void SNDGILightmapInspectorPanel::UpdateRangeText()
	{
		if (!RangeText.IsValid())
		{
			return;
		}
		const TArray<FNDGIEncodingRange>& Ranges = Source == EInspectorSource::LiveAtlas
			? LiveData.Ranges : MergedData.Set.EncodingRanges;
		if (Ranges.Num() == 0)
		{
			RangeText->SetText(FText::FromString(TEXT("No encoding ranges available.")));
			return;
		}
		FString Text;
		for (int32 Index = 0; Index < Ranges.Num(); ++Index)
		{
			const FNDGIEncodingRange& Range = Ranges[Index];
			Text += FString::Printf(TEXT("[%d] S0 %.4f %.4f %.4f %.4f · A0 %.4f %.4f %.4f %.4f\n"),
				Index, Range.Scale0.X, Range.Scale0.Y, Range.Scale0.Z, Range.Scale0.W,
				Range.Add0.X, Range.Add0.Y, Range.Add0.Z, Range.Add0.W);
		}
		RangeText->SetText(FText::FromString(Text));
	}

	void SNDGILightmapInspectorPanel::OnImageMouseMove(const FGeometry& Geometry, const FPointerEvent& Event)
	{
		const FVector2D Local = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
		const FVector2D Size = Geometry.GetLocalSize();
		const int32 Width = GetDisplayWidth();
		const int32 Height = GetDisplayHeight();
		if (Width <= 0 || Height <= 0 || Size.X <= 1.0 || Size.Y <= 1.0)
		{
			OnImageMouseLeave();
			return;
		}
		const int32 X = FMath::Clamp(FMath::FloorToInt(Local.X / Size.X * Width), 0, Width - 1);
		const int32 Y = FMath::Clamp(FMath::FloorToInt(Local.Y / Size.Y * Height), 0, Height - 1);
		if (X != HoverX || Y != HoverY)
		{
			HoverX = X;
			HoverY = Y;
			UpdateReadoutText();
		}
	}

	void SNDGILightmapInspectorPanel::OnImageMouseLeave()
	{
		HoverX = INDEX_NONE;
		HoverY = INDEX_NONE;
		UpdateReadoutText();
	}

	void SNDGILightmapInspectorPanel::ExportPng()
	{
		const int32 Width = GetDisplayWidth();
		const int32 Height = GetDisplayHeight();
		if (DisplayPixels.Num() != Width * Height || Width <= 0 || Height <= 0)
		{
			LoadError = TEXT("Nothing to export; load an atlas first.");
			return;
		}
		TArray64<uint8> Compressed;
		FImageUtils::PNGCompressImageArray(Width, Height, TArrayView64<const FColor>(DisplayPixels.GetData(), DisplayPixels.Num()), Compressed);
		const FString Directory = FPaths::ProjectSavedDir() / TEXT("NDGI/Inspector");
		IFileManager::Get().MakeDirectory(*Directory, true);
		const FString MapName = GetEditorWorld() ? UWorld::RemovePIEPrefix(GetEditorWorld()->GetName()) : TEXT("Level");
		const FString SourceName = Source == EInspectorSource::LiveAtlas ? TEXT("Live") : TEXT("Merged");
		const FString ViewName = ViewToString(View).Replace(TEXT(" "), TEXT(""));
		const FString SliceSuffix = Source == EInspectorSource::MergedSet
			? FString::Printf(TEXT("_Step%02d"), Slice) : FString();
		const FString FileName = FString::Printf(TEXT("%s_Tex%02d_%s_%s%s.png"),
			*MapName, GetSelectedAtlas() ? GetSelectedAtlas()->TexIndex : 0, *SourceName, *ViewName, *SliceSuffix);
		const FString Path = Directory / FileName;
		if (FFileHelper::SaveArrayToFile(Compressed, *Path))
		{
			UE_LOG(LogNDGI, Display, TEXT("Lightmap inspector: exported %s."), *Path);
		}
		else
		{
			UE_LOG(LogNDGI, Warning, TEXT("Lightmap inspector: failed to export %s."), *Path);
		}
	}

	void SNDGILightmapInspectorPanel::OpenTextureEditor()
	{
		const FInspectorAtlas* Atlas = GetSelectedAtlas();
		if (!Atlas || Source != EInspectorSource::LiveAtlas)
		{
			return;
		}
		UTexture2D* Texture = Cast<UTexture2D>(FSoftObjectPath(Atlas->TexturePath).TryLoad());
		if (Texture && GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditor->OpenEditorForAsset(Texture);
			}
		}
	}

	TSharedRef<SWidget> SNDGILightmapInspectorPanel::MakeViewMenu()
	{
		FMenuBuilder Menu(true, nullptr);
		const EInspectorView Views[] =
		{
			EInspectorView::FinalColor,
			EInspectorView::RGB,
			EInspectorView::Red,
			EInspectorView::Green,
			EInspectorView::Blue,
			EInspectorView::Luminance,
			EInspectorView::Alpha,
			EInspectorView::SH,
			EInspectorView::SHW,
			EInspectorView::RangeIndex,
			EInspectorView::Coverage,
			EInspectorView::Difference
		};
		for (EInspectorView InView : Views)
		{
			Menu.AddMenuEntry(
				FText::FromString(ViewToString(InView)),
				FText::GetEmpty(),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, InView]() { SetView(InView); })));
		}
		return Menu.MakeWidget();
	}

	TSharedRef<SWidget> SNDGILightmapInspectorPanel::MakeAtlasMenu()
	{
		FMenuBuilder Menu(true, nullptr);
		for (int32 Index = 0; Index < Atlases.Num(); ++Index)
		{
			const FInspectorAtlas& Atlas = Atlases[Index];
			Menu.AddMenuEntry(
				FText::FromString(FString::Printf(TEXT("Tex%02d  %dx%d%s"), Atlas.TexIndex, Atlas.Width, Atlas.Height,
					Atlas.bMergedExists ? TEXT("  · merged") : TEXT(""))),
				FText::FromString(Atlas.TexturePath),
				FSlateIcon(),
				FUIAction(FExecuteAction::CreateLambda([this, Index]()
				{
					SelectedAtlas = Index;
					Slice = 0;
					LiveData = FInspectorLiveData();
					MergedData = FInspectorMergedData();
					RefreshSource();
				})));
		}
		return Menu.MakeWidget();
	}
}

void RegisterNDGILightmapInspectorTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(LightmapInspectorTabName,
		FOnSpawnTab::CreateStatic([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab)
				.TabRole(ETabRole::NomadTab)
				[
					SNew(SBox).MinDesiredWidth(960.0f).MinDesiredHeight(640.0f)
					[
						SNew(SNDGILightmapInspectorPanel)
					]
				];
		}))
		.SetDisplayName(LOCTEXT("TabTitle", "NDGI Lightmap Inspector"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void UnregisterNDGILightmapInspectorTab()
{
	GInspectorPanel.Reset();
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(LightmapInspectorTabName);
}

void OpenNDGILightmapInspectorTab(int32 TexIndex, int32 Slice)
{
	GInspectorPendingTexIndex = TexIndex;
	GInspectorPendingSlice = Slice;
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(LightmapInspectorTabName));
	if (TSharedPtr<SNDGILightmapInspectorPanel> Panel = GInspectorPanel.Pin())
	{
		Panel->ApplyPendingSelection(true);
	}
}

static FAutoConsoleCommand GNDGILightmapInspectorCommand(
	TEXT("ndgi.Inspector"),
	TEXT("Opens the NDGI Lightmap Inspector for the current level's CPU Lightmass atlases and merged bake sets. Optional: atlas index and merged slice."),
	FConsoleCommandWithArgsDelegate::CreateStatic([](const TArray<FString>& Args)
	{
		const int32 TexIndex = Args.Num() > 0 ? FCString::Atoi(*Args[0]) : INDEX_NONE;
		const int32 Slice = Args.Num() > 1 ? FCString::Atoi(*Args[1]) : INDEX_NONE;
		OpenNDGILightmapInspectorTab(TexIndex, Slice);
	}));

static FAutoConsoleCommand GNDGIInspectorExportCommand(
	TEXT("ndgi.InspectorExport"),
	TEXT("Exports the current NDGI Lightmap Inspector view to Saved/NDGI/Inspector as a PNG."),
	FConsoleCommandDelegate::CreateStatic([]()
	{
		if (TSharedPtr<SNDGILightmapInspectorPanel> Panel = GInspectorPanel.Pin())
		{
			Panel->ExportPng();
		}
		else
		{
			UE_LOG(LogNDGI, Warning, TEXT("NDGI InspectorExport: the Lightmap Inspector tab is not open."));
		}
	}));

namespace
{
	void RunTestLightmapInspector(const TArray<FString>& Args, FOutputDevice& OutputDevice)
	{
		bool bQuitWhenDone = false;
		int32 TexIndex = 0;
		for (const FString& Arg : Args)
		{
			if (Arg.Equals(TEXT("quit"), ESearchCase::IgnoreCase))
			{
				bQuitWhenDone = true;
			}
			else if (Arg.IsNumeric())
			{
				TexIndex = FMath::Max(0, FCString::Atoi(*Arg));
			}
		}

		UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
		if (!World)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: no editor world."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		BakeManifest::FManifest Manifest;
		if (!BakeManifest::Load(Manifest))
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: no bake manifest."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		const BakeManifest::FAtlasEntry* ManifestAtlas = nullptr;
		for (const BakeManifest::FAtlasEntry& Atlas : Manifest.Atlases)
		{
			if (Atlas.TexIndex == TexIndex)
			{
				ManifestAtlas = &Atlas;
				break;
			}
		}
		if (!ManifestAtlas)
		{
			ManifestAtlas = Manifest.Atlases.Num() > 0 ? &Manifest.Atlases[0] : nullptr;
		}
		if (!ManifestAtlas)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: manifest has no atlases."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		UTexture2D* Texture = LoadObject<UTexture2D>(nullptr, *ManifestAtlas->TexturePath);
		if (!Texture)
		{
			Texture = Cast<UTexture2D>(FSoftObjectPath(ManifestAtlas->TexturePath).TryLoad());
		}
		if (!Texture)
		{
			TArray<LightmapSources::FAtlasSource> Sources;
			int32 VirtualTextureCount = 0;
			LightmapSources::CollectLightmapTextures(World, Sources, VirtualTextureCount);
			if (Sources.IsValidIndex(TexIndex) && Sources[TexIndex].Texture)
			{
				Texture = Sources[TexIndex].Texture;
			}
		}
		if (!Texture)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: could not load live atlas '%s'."), *ManifestAtlas->TexturePath);
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		LightmapSources::FReadback Readback;
		if (!LightmapSources::ReadbackTexture(Texture, Readback))
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: live atlas readback failed."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}
		const int32 Width = Readback.Width;
		const int32 Height = Readback.Height / 2;
		if (Width <= 0 || Height <= 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: invalid live atlas size."));
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}

		NDGI::Editor::FEncodingRangeMap RangeMap;
		FString RangeError;
		const bool bHaveRanges = NDGI::Editor::BuildEncodingRangeMap(World, Texture, Width, Height, RangeMap, &RangeError);
		if (!bHaveRanges)
		{
			UE_LOG(LogNDGI, Warning, TEXT("TestLightmapInspector: no coverage/range map (%s); using linear fallback."), *RangeError);
		}

		const FString MergedPath = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake") / FString::Printf(TEXT("Merged_Tex%02d.ndgi"), ManifestAtlas->TexIndex);
		FLightmapSet Merged;
		if (!Merged.Load(MergedPath) || Merged.Steps.Num() == 0)
		{
			UE_LOG(LogNDGI, Error, TEXT("TestLightmapInspector: could not load '%s'."), *MergedPath);
			if (bQuitWhenDone && GEngine) { GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR")); }
			return;
		}
		const int32 StepIndex = Merged.Steps.Num() - 1;
		const FLightmapStep& Step = Merged.Steps[StepIndex];

		int32 Covered = 0;
		double RGBErrorSum = 0.0;
		double SHErrorSum = 0.0;
		int32 Compared = 0;
		for (int32 Y = 0; Y < Height; ++Y)
		{
			for (int32 X = 0; X < Width; ++X)
			{
				const int32 Texel = Y * Width + X;
				const bool bCovered = bHaveRanges
					? (RangeMap.Coverage.IsValidIndex(Texel) && RangeMap.Coverage[Texel] != 0)
					: (Step.Mask.IsValidIndex(Texel) && Step.Mask[Texel] != 0);
				if (!bCovered)
				{
					continue;
				}
				++Covered;
				const int32 RangeIndex = bHaveRanges && RangeMap.Indices.IsValidIndex(Texel) ? RangeMap.Indices[Texel] : 0;
				const FNDGIEncodingRange& Range = bHaveRanges && RangeMap.Ranges.Num() > 0
					? RangeMap.Ranges[FMath::Clamp<int32>(RangeIndex, 0, RangeMap.Ranges.Num() - 1)]
					: FNDGIEncodingRange();
				const FLinearColor Top = Readback.Pixels[Y * Width + X];
				const FLinearColor Bottom = Readback.Pixels[(Y + Height) * Width + X];
				FVector3f LiveRGB;
				FVector3f LiveSH;
				LightmapSources::DecodeHQTexel(Top, Bottom, Range.Scale0, Range.Add0, Range.Scale1, Range.Add1, LiveRGB, LiveSH);
				if (Step.RGB.IsValidIndex(Texel * 3 + 2) && Step.SH.IsValidIndex(Texel * 3 + 2))
				{
					const FVector3f MergedRGB(Step.RGB[Texel * 3 + 0], Step.RGB[Texel * 3 + 1], Step.RGB[Texel * 3 + 2]);
					const FVector3f MergedSH(Step.SH[Texel * 3 + 0], Step.SH[Texel * 3 + 1], Step.SH[Texel * 3 + 2]);
					RGBErrorSum += FMath::Abs(LiveRGB.X - MergedRGB.X) + FMath::Abs(LiveRGB.Y - MergedRGB.Y) + FMath::Abs(LiveRGB.Z - MergedRGB.Z);
					SHErrorSum += FMath::Abs(LiveSH.X - MergedSH.X) + FMath::Abs(LiveSH.Y - MergedSH.Y) + FMath::Abs(LiveSH.Z - MergedSH.Z);
					++Compared;
				}
			}
		}

		const double RGBError = Compared > 0 ? RGBErrorSum / (3.0 * Compared) : 0.0;
		const double SHError = Compared > 0 ? SHErrorSum / (3.0 * Compared) : 0.0;
		const bool bPassed = Covered > 0 && Compared > 0 && RGBError < 0.05 && SHError < 0.10;
		UE_LOG(LogNDGI, Display,
			TEXT("TestLightmapInspector: %s - Tex%02d %dx%d, %d covered, %d compared, mean |RGB| %.5f, mean |SH| %.5f, live vs merged step %d."),
			bPassed ? TEXT("PASSED") : TEXT("FAILED"), ManifestAtlas->TexIndex, Width, Height,
			Covered, Compared, RGBError, SHError, StepIndex);

		if (bQuitWhenDone && GEngine)
		{
			GEngine->DeferredCommands.Add(TEXT("QUIT_EDITOR"));
		}
	}
}

static FAutoConsoleCommandWithArgsAndOutputDevice GNDGITestLightmapInspectorCommand(
	TEXT("ndgi.TestLightmapInspector"),
	TEXT("Compares the live CPU Lightmass atlas with the last merged .ndgi step and validates the inspector decode path. Usage: ndgi.TestLightmapInspector [texIndex] [quit]."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(&RunTestLightmapInspector));

#undef LOCTEXT_NAMESPACE

// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIEditor.h"

#include "Editor.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "HAL/PlatformProcess.h"
#include "Internationalization/Regex.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Timespan.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "NDGI.h"
#include "NDGIAtlasDecoder.h"
#include "NDGIBakeManifest.h"
#include "NDGILevelAsset.h"
#include "NDGIPipelineStatus.h"
#include "NDGITimeOfDayProfile.h"
#include "NDGIRuntime.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSearchableComboBox.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Input/SSpinBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SSplitter.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "NDGIPipeline"

namespace
{
	namespace BakeManifest = NDGI::Editor::BakeManifest;

	FName PipelineTabName(TEXT("NDGIPipeline"));

	const FLinearColor MutedText(0.58f, 0.58f, 0.58f, 1.0f);
	const FLinearColor RunningColor(1.0f, 0.68f, 0.18f, 1.0f);
	const FLinearColor PassedColor(0.28f, 0.82f, 0.42f, 1.0f);
	const FLinearColor FailedColor(0.95f, 0.28f, 0.24f, 1.0f);
	const FLinearColor IdleColor(0.72f, 0.72f, 0.72f, 1.0f);
	const FLinearColor WarningColor(1.0f, 0.72f, 0.24f, 1.0f);

	/** One atlas row in the artist-facing health panel. */
	struct FAtlasUiRow
	{
		int32 TexIndex = 0;
		FString Resolution;
		FString Grid;
		int32 ModelCount = 0;
		int32 LegacyCount = 0;
		int32 EncodingRanges = 0;
		int64 Bytes = 0;
	};

	/** One per-tile training result read from TileMetrics.json. */
	struct FTileMetricUi
	{
		int32 TexIndex = 0;
		int32 TileX = 0;
		int32 TileY = 0;
		double Psnr = 0.0;
		double RgbPsnr = 0.0;
		double ShPsnr = 0.0;
	};

	class SNDGIPipelinePanel final : public SCompoundWidget
	{
	public:
		SLATE_BEGIN_ARGS(SNDGIPipelinePanel) {}
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs);
		virtual ~SNDGIPipelinePanel() override = default;

	private:
		EActiveTimerReturnType Tick(double CurrentTime, float DeltaTime);
		void RefreshUi();
		void RefreshSceneInfo(bool bDeepScan = false);

		bool IsRunning() const;
		bool CanStart() const;
		bool HasEditorWorld() const;
		UWorld* GetEditorWorld() const;

		void StartBake();
		void StartMerge();
		void StartTrain();
		void StartValidateLevel();
		void StartValidateSH();
		void StartValidateSeams();
		void StartValidateDirectionality();
		void StartValidateHoldout();
		void StartInspectRanges();
		void StartFullPipeline();
		void ExecuteConsoleCommand(const FString& Command, bool bAssetsMayChange = false);
		void ApplyProfileDefaults();

		/** Time-of-day profile helpers used by the TOD card. */
		UNDGITimeOfDayProfile* GetTODProfile();
		void RefreshTodCard();
		void OpenTODProfile();
		void CreateTODProfileAsset();
		void SaveTODProfile();

		/** Per-tile training metrics shown as the worst-tile list. */
		void RefreshQualityCard();
		void PreviewTile(int32 TexIndex, int32 TileX, int32 TileY);

		/** Runs `Command [Args]` through the same exec path as the pipeline buttons. */
		void RunCommand(const FString& Command, const FString& Args = FString(), bool bAssetsMayChange = false);
		void RunSelectedCommand();
		void RefreshCommandList();
		void UpdateCommandHelp();

		FText GetStatusText() const;
		FSlateColor GetStatusColor() const;
		TOptional<float> GetProgressPercent() const;
		FString GetLevelName() const;
		FString GetLevelAssetPath() const;
		FString BuildLogText() const;

		TSharedRef<SWidget> MakeLabeledSpin(
			const FText& Label, int32 SNDGIPipelinePanel::*Member,
			int32 Min, int32 Max, int32 Delta, const FText& Tooltip);

		// Settings.
		int32 BakeSteps = 8;
		int32 TrainTileSize = 128;
		int32 TrainSteps = 3000;
		int32 TrainOverlap = 8;
		int32 HoldoutSteps = 3000;
		int32 HoldoutTiles = 3;
		bool bHoldoutBaseline = false;
		float ValidateTime = 1.0f;
		bool bValidateAfterTrain = true;

		// Diagnostics card settings.
		int32 CadenceSeconds = 5;
		int32 BenchmarkIterations = 50;
		FString CommandArgs;

		// Time-of-day card.
		float TodPreviewTime = 0.3f;
		FLinearColor TodPreviewSunColor = FLinearColor::White;
		FLinearColor TodPreviewSkyColor = FLinearColor(0.35f, 0.45f, 0.6f, 1.0f);
		TWeakObjectPtr<UNDGITimeOfDayProfile> TodProfile;

		// Per-tile metrics cache (re-parsed only when the report file changes).
		TArray<FTileMetricUi> CachedTileMetrics;
		FDateTime CachedTileMetricsStamp;
		int64 CachedTileMetricsSize = -1;

		// Every registered `ndgi.*` console command, discovered at panel construction.
		TArray<TSharedPtr<FString>> CommandOptions;
		TSharedPtr<FString> SelectedCommand;

		// Widgets that need runtime updates.
		TSharedPtr<STextBlock> StatusText;
		TSharedPtr<STextBlock> DetailText;
		TSharedPtr<STextBlock> ElapsedText;
		TSharedPtr<STextBlock> ResultText;
		TSharedPtr<STextBlock> HealthSummaryText;
		TSharedPtr<SVerticalBox> HealthListBox;
		TSharedPtr<SVerticalBox> AtlasListBox;
		TSharedPtr<SVerticalBox> QualityListBox;
		TSharedPtr<STextBlock> QualitySummaryText;
		TSharedPtr<STextBlock> TodSummaryText;
		TSharedPtr<SVerticalBox> TodKeyframeList;
		TSharedPtr<STextBlock> TodPreviewText;
		TSharedPtr<STextBlock> HoldoutResultText;
		TSharedPtr<STextBlock> CommandHelpText;
		TSharedPtr<STextBlock> LogTextBlock;
		TSharedPtr<SScrollBox> LogScrollBox;
		TSharedPtr<SProgressBar> ProgressBar;

		TSharedPtr<SButton> BakeButton;
		TSharedPtr<SButton> MergeButton;
		TSharedPtr<SButton> TrainButton;
		TSharedPtr<SButton> AutoButton;
		TSharedPtr<SButton> ValidateLevelButton;
		TSharedPtr<SButton> ValidateSHButton;
		TSharedPtr<SButton> ValidateSeamsButton;
		TSharedPtr<SButton> ValidateDirectionalityButton;

		double LastSceneRefreshSeconds = 0.0;
		uint64 LastLogVersion = 0;
		bool bAutoScrollLog = true;
		bool bLastRunning = false;
		bool bLastAutoRunning = false;
		bool bForceDeepScan = true;
		FString ScannedAssetPath;
		TArray<FAtlasUiRow> CachedAtlasRows;
		NDGI::Editor::Pipeline::EStage LastStage = NDGI::Editor::Pipeline::EStage::Idle;
	};

	void SNDGIPipelinePanel::Construct(const FArguments& InArgs)
	{
		UE_LOG(LogNDGI, Display, TEXT("NDGI Pipeline: panel constructed for '%s'."), *GetLevelName());

		const FSlateFontInfo TitleFont = FCoreStyle::GetDefaultFontStyle("Bold", 18);
		const FSlateFontInfo SectionFont = FCoreStyle::GetDefaultFontStyle("Bold", 11);
		const FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);
		const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);

		auto MakeCard = [](TSharedRef<SWidget> Content) -> TSharedRef<SWidget>
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(12.0f, 10.0f))
				[
					Content
				];
		};

		auto MakeSectionTitle = [SectionFont](const FText& Text) -> TSharedRef<SWidget>
		{
			return SNew(STextBlock)
				.Text(Text)
				.Font(SectionFont);
		};

		auto MakeHint = [SmallFont](const FText& Text) -> TSharedRef<SWidget>
		{
			return SNew(STextBlock)
				.Text(Text)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true);
		};

		auto MakePill = [](const FText& Text) -> TSharedRef<SWidget>
		{
			return SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(8.0f, 3.0f))
				[
					SNew(STextBlock)
					.Text(Text)
					.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
					.ColorAndOpacity(FSlateColor(MutedText))
				];
		};

		auto MakeCommandButton = [this](const FText& Label, const FText& Tooltip, const FString& Command, bool bAssetsMayChange = false) -> TSharedRef<SWidget>
		{
			return SNew(SButton)
				.Text(Label)
				.ToolTipText(Tooltip)
				.IsEnabled_Lambda([this]() { return CanStart(); })
				.OnClicked_Lambda([this, Command, bAssetsMayChange]()
				{
					RunCommand(Command, FString(), bAssetsMayChange);
					return FReply::Handled();
				});
		};

		// Discover commands once so the browser always reflects this module's
		// registrations (editor + runtime diagnostics).
		RefreshCommandList();

		// ------------------------------------------------------------------
		// Header
		// ------------------------------------------------------------------
		TSharedRef<SWidget> Header = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("Title", "NDGI Pipeline"))
						.Font(TitleFont)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						MakeHint(LOCTEXT("Subtitle", "Bake multi-step lighting, merge the slices, train per-tile models, and verify the result for the current level."))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SNew(SBorder)
					.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
					.Padding(FMargin(10.0f, 5.0f))
					[
						SAssignNew(StatusText, STextBlock)
						.Font(SectionFont)
						.Text(LOCTEXT("StatusIdle", "Ready"))
					]
				]
			];

		// ------------------------------------------------------------------
		// Project health card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> HealthCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeSectionTitle(LOCTEXT("HealthTitle", "Project health"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("RescanAssets", "Rescan"))
					.ToolTipText(LOCTEXT("RescanAssetsTip", "Reload the level asset, model BC7 status, bake manifest, merged sets, and TOD profile."))
					.OnClicked_Lambda([this]()
					{
						LastSceneRefreshSeconds = 0.0;
						bForceDeepScan = true;
						RefreshSceneInfo(true);
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(HealthSummaryText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(HealthListBox, SVerticalBox)
			]);

		// ------------------------------------------------------------------
		// Atlas breakdown card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> AtlasCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeSectionTitle(LOCTEXT("AtlasTitle", "Atlases"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakePill(LOCTEXT("AtlasPill", "runtime data"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				MakeHint(LOCTEXT("AtlasHint", "Model count, BC7 readiness, encoding ranges, and projected GPU bytes per atlas. Rescan after training."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(AtlasListBox, SVerticalBox)
			]);

		// ------------------------------------------------------------------
		// Model quality card (worst tiles from the last training run)
		// ------------------------------------------------------------------
		TSharedRef<SWidget> QualityCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeSectionTitle(LOCTEXT("QualityTitle", "Model quality"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakePill(LOCTEXT("QualityPill", "per-tile PSNR"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				MakeHint(LOCTEXT("QualityHint", "Worst tiles from the last training run, read from Saved/NDGI/Training/TileMetrics.json. Use Preview to open the tile's model, or the tile coordinates with the Lightmap Inspector to find it on the atlas."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(QualitySummaryText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SAssignNew(QualityListBox, SVerticalBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenMetricsFolder", "Open Metrics Folder"))
					.ToolTipText(LOCTEXT("OpenMetricsFolderTip", "Open Saved/NDGI/Training in the file browser."))
					.OnClicked_Lambda([]()
					{
						FPlatformProcess::ExploreFolder(*(FPaths::ProjectSavedDir() / TEXT("NDGI/Training")));
						return FReply::Handled();
					})
				]
			]);

		// ------------------------------------------------------------------
		// Time-of-day profile card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> TodCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeSectionTitle(LOCTEXT("TodTitle", "Time of day"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakePill(LOCTEXT("TodPill", "bake + runtime profile"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				MakeHint(LOCTEXT("TodHint", "The default profile drives both the bake and the runtime sun/sky. Preview any time below, then open the asset to edit keyframes."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(TodSummaryText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TodKeyHeader", "Keyframes"))
				.Font(BodyFont)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("TodKeyColumns", "Time    elevation / azimuth      sun color × intensity      sky color × intensity"))
				.Font(MonoFont)
				.ColorAndOpacity(FSlateColor(MutedText))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SAssignNew(TodKeyframeList, SVerticalBox)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TodPreviewLabel", "Preview"))
					.Font(BodyFont)
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center).Padding(12, 0, 0, 0)
				[
					SNew(SSlider)
					.MinValue(0.0f)
					.MaxValue(1.0f)
					.Value_Lambda([this]() { return TodPreviewTime; })
					.OnValueChanged_Lambda([this](float Value)
					{
						TodPreviewTime = FMath::Clamp(Value, 0.0f, 1.0f);
						RefreshTodCard();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return FText::FromString(FString::Printf(TEXT("%.2f"), TodPreviewTime)); })
					.Font(MonoFont)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SAssignNew(TodPreviewText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TodPreviewSun", "Sun"))
					.Font(SmallFont)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 8, 0)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this]() { return FSlateColor(TodPreviewSunColor); })
					.Padding(FMargin(0.0f))
					[
						SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("TodPreviewSky", "Sky"))
					.Font(SmallFont)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 8, 0)
				[
					SNew(SBorder)
					.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
					.BorderBackgroundColor_Lambda([this]() { return FSlateColor(TodPreviewSkyColor); })
					.Padding(FMargin(0.0f))
					[
						SNew(SBox).WidthOverride(14.0f).HeightOverride(14.0f)
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TodCycle", "Cycle seconds"))
						.Font(SmallFont)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						SNew(SBox).WidthOverride(110.0f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.1f)
							.MaxValue(86400.0f)
							.Delta(5.0f)
							.MinFractionalDigits(1)
							.MaxFractionalDigits(2)
							.Value_Lambda([this]()
							{
								const UNDGITimeOfDayProfile* Profile = TodProfile.Get();
								return Profile ? Profile->CycleDurationSeconds : 120.0f;
							})
							.IsEnabled_Lambda([this]() { return TodProfile.IsValid(); })
							.OnValueChanged_Lambda([this](float Value)
							{
								if (UNDGITimeOfDayProfile* Profile = TodProfile.Get())
								{
									Profile->CycleDurationSeconds = FMath::Clamp(Value, 0.1f, 86400.0f);
									Profile->GetOutermost()->MarkPackageDirty();
								}
							})
							.OnValueCommitted_Lambda([this](float, ETextCommit::Type)
							{
								if (UNDGITimeOfDayProfile* Profile = TodProfile.Get())
								{
									Profile->PostEditChange();
								}
							})
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						const UNDGITimeOfDayProfile* Profile = TodProfile.Get();
						return Profile && Profile->bPingPong ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.IsEnabled_Lambda([this]() { return TodProfile.IsValid(); })
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						if (UNDGITimeOfDayProfile* Profile = TodProfile.Get())
						{
							Profile->Modify();
							Profile->bPingPong = State == ECheckBoxState::Checked;
							Profile->PostEditChange();
							Profile->GetOutermost()->MarkPackageDirty();
						}
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TodPingPong", "Ping-pong"))
						.Font(SmallFont)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("TodSave", "Save Profile"))
					.ToolTipText(LOCTEXT("TodSaveTip", "Save the default TOD profile after editing cycle duration or ping-pong."))
					.IsEnabled_Lambda([this]()
					{
						const UNDGITimeOfDayProfile* Profile = TodProfile.Get();
						return Profile && Profile->GetOutermost()->IsDirty();
					})
					.OnClicked_Lambda([this]()
					{
						SaveTODProfile();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("TodOpen", "Open Profile"))
					.ToolTipText(LOCTEXT("TodOpenTip", "Open the default TOD profile asset to edit keyframes, colors, and defaults."))
					.IsEnabled_Lambda([this]() { return TodProfile.IsValid(); })
					.OnClicked_Lambda([this]()
					{
						OpenTODProfile();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text_Lambda([this]()
					{
						return TodProfile.IsValid()
							? LOCTEXT("TodReset", "Reset Default")
							: LOCTEXT("TodCreate", "Create Default");
					})
					.ToolTipText(LOCTEXT("TodCreateTip", "Create or reset /Game/NDGI/TOD/NDGITODProfile with the built-in dawn/noon/dusk/night keyframes and default bake/training settings."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						CreateTODProfileAsset();
						return FReply::Handled();
					})
				]
			]);

		// ------------------------------------------------------------------
		// Pipeline card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> PipelineCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					MakeSectionTitle(LOCTEXT("PipelineTitle", "Pipeline"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					MakePill(LOCTEXT("PipelineHint", "CPU Lightmass"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				MakeSectionTitle(LOCTEXT("BakeTitle", "1 · Bake time-of-day"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
				[
					MakeLabeledSpin(
						LOCTEXT("BakeSlices", "Time slices"),
						&SNDGIPipelinePanel::BakeSteps, 2, 32, 1,
						LOCTEXT("BakeSlicesTip", "Number of baked time-of-day slices. 8 is the production default; at least 3 are required."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(16, 0, 0, 0)
				[
					SAssignNew(BakeButton, SButton)
					.Text(LOCTEXT("Bake", "Bake Time-of-Day"))
					.ToolTipText(LOCTEXT("BakeTip", "Bake the current level at the selected number of slices, export every lightmap atlas to Saved/NDGI/Bake, and save the level."))
					.HAlign(HAlign_Center)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartBake(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				MakeHint(LOCTEXT("BakeHint", "Stationary sun and sky, uncompressed lightmaps. The bake saves the level and build data."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 10)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("MergeTitle", "2 · Merge slices"))
						.Font(BodyFont)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						MakeHint(LOCTEXT("MergeHint", "Combines every complete StepNN_TexNN set into Merged_TexNN.ndgi using the bake slice count above."))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SAssignNew(MergeButton, SButton)
					.Text(LOCTEXT("Merge", "Merge Steps"))
					.ToolTipText(LOCTEXT("MergeTip", "Merge the exported time slices for every atlas that has a complete step set."))
					.HAlign(HAlign_Center)
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartMerge(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 10)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				MakeSectionTitle(LOCTEXT("TrainTitle", "3 · Train models"))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
				[
					MakeLabeledSpin(
						LOCTEXT("TileSize", "Tile size"),
						&SNDGIPipelinePanel::TrainTileSize, 16, 512, 16,
						LOCTEXT("TileSizeTip", "Model tile size in lightmap texels. Must be a multiple of 16; 128 is the default."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
				[
					MakeLabeledSpin(
						LOCTEXT("StepsPerTile", "Steps / tile"),
						&SNDGIPipelinePanel::TrainSteps, 1, 200000, 100,
						LOCTEXT("StepsPerTileTip", "Adam steps per tile. 800 is a quick pass; 3000 is the quality default."))
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
				[
					MakeLabeledSpin(
						LOCTEXT("Overlap", "Halo"),
						&SNDGIPipelinePanel::TrainOverlap, 0, 64, 8,
						LOCTEXT("OverlapTip", "Texels of overlap on every side. 8 is the default; 0 disables overlapping inference."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(16, 0, 0, 0)
				[
					SAssignNew(TrainButton, SButton)
					.Text(LOCTEXT("Train", "Train All Atlases"))
					.ToolTipText(LOCTEXT("TrainTip", "Train every merged atlas, export one model per tile, and build the level asset for the current level."))
					.HAlign(HAlign_Center)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartTrain(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeHint(LOCTEXT("TrainHint", "Models are written to /Game/NDGI/Trained and the level manifest to /Game/NDGI/Levels."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("QuickPreset", "Quick (800)"))
					.ToolTipText(LOCTEXT("QuickPresetTip", "Set 800 steps per tile for a fast preview pass."))
					.OnClicked_Lambda([this]() { TrainSteps = 800; return FReply::Handled(); })
					.IsEnabled_Lambda([this]() { return CanStart(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("QualityPreset", "Quality (3000)"))
					.ToolTipText(LOCTEXT("QualityPresetTip", "Set 3000 steps per tile for production-quality models."))
					.OnClicked_Lambda([this]() { TrainSteps = 3000; return FReply::Handled(); })
					.IsEnabled_Lambda([this]() { return CanStart(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("TODProfileDefaults", "TOD Defaults"))
					.ToolTipText(LOCTEXT("TODProfileDefaultsTip", "Load bake slice count, tile size, training steps, and halo from /Game/NDGI/TOD/NDGITODProfile."))
					.OnClicked_Lambda([this]() { ApplyProfileDefaults(); return FReply::Handled(); })
					.IsEnabled_Lambda([this]() { return CanStart(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 10)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]()
						{
							return bValidateAfterTrain ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
						{
							bValidateAfterTrain = State == ECheckBoxState::Checked;
						})
						.IsEnabled_Lambda([this]() { return CanStart(); })
						[
							SNew(STextBlock)
							.Text(LOCTEXT("ValidateAfterTrain", "Validate level after training"))
							.Font(BodyFont)
						]
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						MakeHint(LOCTEXT("ValidateAfterTrainHint", "Full pipeline: bake → merge → train → validate level asset."))
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(16, 0, 0, 0)
				[
					SAssignNew(AutoButton, SButton)
					.Text(LOCTEXT("RunFull", "Run Full Pipeline"))
					.ToolTipText(LOCTEXT("RunFullTip", "Run bake, merge, training, and the selected validation in one go. Settings above are used for each stage."))
					.HAlign(HAlign_Center)
					.ButtonStyle(FAppStyle::Get(), "PrimaryButton")
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartFullPipeline(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("StopAuto", "Stop Auto"))
					.ToolTipText(LOCTEXT("StopAutoTip", "Stop chaining after the currently running stage. The active bake or training job will finish."))
					.IsEnabled_Lambda([]() { return NDGI::Editor::Pipeline::IsAutoRunning(); })
					.OnClicked_Lambda([]()
					{
						NDGI::Editor::Pipeline::StopAutoRun();
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CancelJob", "Cancel Job"))
					.ToolTipText(LOCTEXT("CancelJobTip", "Stop the active bake or training job after its current slice/tile and stop the auto-run chain."))
					.IsEnabled_Lambda([]() { return NDGI::Editor::Pipeline::GetState().bRunning; })
					.OnClicked_Lambda([]()
					{
						NDGI::Editor::Pipeline::StopAutoRun();
						NDGI::Editor::Pipeline::RequestCancel();
						return FReply::Handled();
					})
				]
			]);

		// ------------------------------------------------------------------
		// Validation card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> ValidationCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f)
				[
					MakeSectionTitle(LOCTEXT("ValidateTitle", "4 · Validate"))
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("InspectRanges", "Inspect Ranges"))
					.ToolTipText(LOCTEXT("InspectRangesTip", "List the distinct HQ encoding ranges per atlas. Use this when colors or SH look wrong on a shared atlas."))
					.OnClicked_Lambda([this]() { StartInspectRanges(); return FReply::Handled(); })
					.IsEnabled_Lambda([this]() { return CanStart(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("ValidateTime", "Time of day"))
						.Font(SmallFont)
					]
					+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
					[
						SNew(SHorizontalBox)
						+ SHorizontalBox::Slot().FillWidth(1.0f)
						[
							SNew(SSpinBox<float>)
							.MinValue(0.0f)
							.MaxValue(1.0f)
							.Delta(0.01f)
							.MinFractionalDigits(2)
							.MaxFractionalDigits(3)
							.Value_Lambda([this]() { return ValidateTime; })
							.OnValueChanged_Lambda([this](float Value) { ValidateTime = FMath::Clamp(Value, 0.0f, 1.0f); })
							.IsEnabled_Lambda([this]() { return CanStart(); })
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("LastStep", "1.0"))
							.ToolTipText(LOCTEXT("LastStepTip", "Use the last baked step (1.0)."))
							.OnClicked_Lambda([this]() { ValidateTime = 1.0f; return FReply::Handled(); })
							.IsEnabled_Lambda([this]() { return CanStart(); })
						]
						+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
						[
							SNew(SButton)
							.Text(LOCTEXT("Midday", "0.5"))
							.ToolTipText(LOCTEXT("MiddayTip", "Use the mid-day slice (0.5)."))
							.OnClicked_Lambda([this]() { ValidateTime = 0.5f; return FReply::Handled(); })
							.IsEnabled_Lambda([this]() { return CanStart(); })
						]
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(16, 0, 0, 0)
				[
					SAssignNew(ValidateLevelButton, SButton)
					.Text(LOCTEXT("ValidateLevel", "Validate Level"))
					.ToolTipText(LOCTEXT("ValidateLevelTip", "Decode the level asset and report changed lightmap texels."))
					.HAlign(HAlign_Center)
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartValidateLevel(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SAssignNew(ValidateSHButton, SButton)
					.Text(LOCTEXT("ValidateSH", "Validate SH"))
					.ToolTipText(LOCTEXT("ValidateSHTip", "Report directional SH PSNR against the lightmap's current Lightmap1 contents."))
					.HAlign(HAlign_Center)
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartValidateSH(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeHint(LOCTEXT("ValidateHint", "Run on a freshly loaded level: validation decodes the lightmaps, so a second validation would compare against the previous decode."))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(12, 0, 0, 0)
				[
					SAssignNew(ValidateSeamsButton, SButton)
					.Text(LOCTEXT("ValidateSeams", "Validate Seams"))
					.ToolTipText(LOCTEXT("ValidateSeamsTip", "Report raw and decoded discontinuity across tile boundaries. Decoded metrics use each texel's own HQ range."))
					.HAlign(HAlign_Center)
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartValidateSeams(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(6, 0, 0, 0)
				[
					SAssignNew(ValidateDirectionalityButton, SButton)
					.Text(LOCTEXT("ValidateDirectionality", "Directionality"))
					.ToolTipText(LOCTEXT("ValidateDirectionalityTip", "Compare engine and decoded SH directionality, including Lightmap1 alpha/SH.w."))
					.HAlign(HAlign_Center)
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]() { StartValidateDirectionality(); return FReply::Handled(); })
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text(LOCTEXT("HoldoutTitle", "Temporal holdout (artist preview)"))
					.Font(BodyFont)
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
				[
					MakeHint(LOCTEXT("HoldoutHint", "Trains a small preview model without one baked slice and reports how well it interpolates that missing time. Baseline trains every slice for comparison. Results: Saved/NDGI/TemporalHoldout*.json."))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
					[
						MakeLabeledSpin(
							LOCTEXT("HoldoutSteps", "Holdout steps"),
							&SNDGIPipelinePanel::HoldoutSteps, 100, 200000, 100,
							LOCTEXT("HoldoutStepsTip", "Adam steps per tile for the holdout preview. 3000 matches the quality default."))
					]
					+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
					[
						MakeLabeledSpin(
							LOCTEXT("HoldoutTileCount", "Tiles to test"),
							&SNDGIPipelinePanel::HoldoutTiles, 1, 200, 1,
							LOCTEXT("HoldoutTileCountTip", "Stop after this many tiles so the preview stays fast. Use the console command for a full-map run."))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
					[
						SNew(SCheckBox)
						.IsChecked_Lambda([this]()
						{
							return bHoldoutBaseline ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
						})
						.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
						{
							bHoldoutBaseline = State == ECheckBoxState::Checked;
						})
						.IsEnabled_Lambda([this]() { return CanStart(); })
						[
							SNew(STextBlock)
							.Text(LOCTEXT("HoldoutBaseline", "Baseline"))
							.Font(SmallFont)
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(12, 0, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("RunHoldout", "Run Holdout"))
						.ToolTipText(LOCTEXT("RunHoldoutTip", "Validate temporal interpolation on the current level's merged bake. Does not modify the trained level asset."))
						.HAlign(HAlign_Center)
						.IsEnabled_Lambda([this]() { return CanStart(); })
						.OnClicked_Lambda([this]() { StartValidateHoldout(); return FReply::Handled(); })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SAssignNew(HoldoutResultText, STextBlock)
					.Font(SmallFont)
					.ColorAndOpacity(FSlateColor(MutedText))
					.AutoWrapText(true)
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenPreview", "Open Preview"))
					.ToolTipText(LOCTEXT("OpenPreviewTip", "Open the NDGI model preview tab."))
					.OnClicked_Lambda([]() { OpenNDGIPreviewTab(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenLightmapInspector", "Inspect Lightmaps"))
					.ToolTipText(LOCTEXT("OpenLightmapInspectorTip", "Open the CPU Lightmass atlas and merged bake inspector."))
					.OnClicked_Lambda([]() { OpenNDGILightmapInspectorTab(); return FReply::Handled(); })
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenBakeFolder", "Bake Folder"))
					.ToolTipText(LOCTEXT("OpenBakeFolderTip", "Open Saved/NDGI/Bake in the file browser."))
					.OnClicked_Lambda([]()
					{
						const FString Folder = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
						FPlatformProcess::ExploreFolder(*Folder);
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("Refresh", "Refresh"))
					.ToolTipText(LOCTEXT("RefreshTip", "Refresh the level and bake-file summary."))
					.OnClicked_Lambda([this]()
					{
						LastSceneRefreshSeconds = 0.0;
						RefreshSceneInfo(true);
						return FReply::Handled();
					})
				]
			]);

		// ------------------------------------------------------------------
		// Reports & diagnostics card
		// ------------------------------------------------------------------
		TSharedRef<SWidget> DiagnosticsCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					MakeSectionTitle(LOCTEXT("DiagnosticsTitle", "5 · Reports & diagnostics"))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					MakePill(LOCTEXT("DiagnosticsPill", "console commands"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				MakeHint(LOCTEXT("DiagnosticsHint", "Run the NDGI reporting and diagnostic commands without leaving the panel. Output is captured in the Log; long commands block the editor until they finish."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SWrapBox)
				.UseAllottedSize(true)
				.InnerSlotPadding(FVector2D(6.0f, 6.0f))
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("ReportStorage", "Report Storage"),
						LOCTEXT("ReportStorageTip", "ndgi.ReportStorage: projected BC7/raw bytes, compression ratios, BPP, and the pre-BC7 stale-asset check."),
						TEXT("ndgi.ReportStorage"))
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("VerifyDeps", "Verify Deps"),
						LOCTEXT("VerifyDepsTip", "ndgi.VerifyStreamingDeps: checks that every model path resolves to a package and reports cook dependency edges."),
						TEXT("ndgi.VerifyStreamingDeps"))
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("ResaveLevelAsset", "Resave Level Asset"),
						LOCTEXT("ResaveLevelAssetTip", "ndgi.ResaveLevelAsset: persists migrated soft model paths and cook dependency data."),
						TEXT("ndgi.ResaveLevelAsset"), true)
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("InspectBenchScene", "Inspect Bench Scene"),
						LOCTEXT("InspectBenchSceneTip", "ndgi.InspectBenchmarkScene: logs sun/sky mobility, lightmap formats, build data, and whether the level asset exists."),
						TEXT("ndgi.InspectBenchmarkScene"))
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("TestBC7", "Test BC7"),
						LOCTEXT("TestBC7Tip", "ndgi.TestBC7: round-trips a known pattern through the BC7 encoder and GPU readback."),
						TEXT("ndgi.TestBC7"))
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("TestTOD", "Test TOD Profile"),
						LOCTEXT("TestTODTip", "ndgi.TestTODProfile: evaluates the default time-of-day profile at keyframes and midpoints."),
						TEXT("ndgi.TestTODProfile"))
				]
				+ SWrapBox::Slot()
				[
					MakeCommandButton(
						LOCTEXT("TestAsyncCache", "Async Cache"),
						LOCTEXT("TestAsyncCacheTip", "ndgi.TestAsyncCache: builds the decode cache one atlas per tick and reports worst tick, wall time, and CPU/GPU bytes."),
						TEXT("ndgi.TestAsyncCache"))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
				[
					MakeLabeledSpin(
						LOCTEXT("CadenceSeconds", "Cadence seconds"),
						&SNDGIPipelinePanel::CadenceSeconds, 1, 120, 1,
						LOCTEXT("CadenceSecondsTip", "Duration used by ndgi.TestDecodeCadence. The gate expects a 10-20 Hz cadence."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunCadence", "Test Cadence"))
					.ToolTipText(LOCTEXT("RunCadenceTip", "ndgi.TestDecodeCadence <seconds>: changes time-of-day every frame and reports completed decodes."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						RunCommand(TEXT("ndgi.TestDecodeCadence"), FString::FromInt(CadenceSeconds));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom).Padding(16, 0, 0, 0)
				[
					MakeLabeledSpin(
						LOCTEXT("BenchmarkIterations", "Benchmark iterations"),
						&SNDGIPipelinePanel::BenchmarkIterations, 1, 10000, 10,
						LOCTEXT("BenchmarkIterationsTip", "Iteration count used by the decode benchmarks."))
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("BenchmarkDecode", "Benchmark Decode"))
					.ToolTipText(LOCTEXT("BenchmarkDecodeTip", "ndgi.BenchmarkDecode <iterations>: cold/warm decode latency including parameter packing and readback."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						RunCommand(TEXT("ndgi.BenchmarkDecode"), FString::FromInt(BenchmarkIterations));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("BenchmarkDecodeWarm", "Warm Benchmark"))
					.ToolTipText(LOCTEXT("BenchmarkDecodeWarmTip", "ndgi.BenchmarkDecodeWarm <iterations>: builds the persistent cache once, then measures warm decode into FP16, BGRA8 UAV, and live engine lightmaps."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						RunCommand(TEXT("ndgi.BenchmarkDecodeWarm"), FString::FromInt(BenchmarkIterations));
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("TestInspector", "Inspector Test"))
					.ToolTipText(LOCTEXT("TestInspectorTip", "ndgi.TestLightmapInspector 0: compares the live CPU Lightmass atlas against the last merged bake step."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						RunCommand(TEXT("ndgi.TestLightmapInspector"), TEXT("0"));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ExportInspector", "Export Inspector PNG"))
					.ToolTipText(LOCTEXT("ExportInspectorTip", "ndgi.InspectorExport: exports the open Lightmap Inspector view to Saved/NDGI/Inspector."))
					.IsEnabled_Lambda([this]() { return CanStart(); })
					.OnClicked_Lambda([this]()
					{
						RunCommand(TEXT("ndgi.InspectorExport"));
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 6, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("OpenSavedFolder", "Saved/NDGI Folder"))
					.ToolTipText(LOCTEXT("OpenSavedFolderTip", "Open the report, bake, benchmark, and inspector output folder."))
					.OnClicked_Lambda([]()
					{
						FPlatformProcess::ExploreFolder(*(FPaths::ProjectSavedDir() / TEXT("NDGI")));
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 10, 0, 0)
			[
				SNew(SSeparator)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("CommandBrowserTitle", "Run any NDGI command"))
				.Font(BodyFont)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(SSearchableComboBox)
					.InitiallySelectedItem(SelectedCommand)
					.OptionsSource(&CommandOptions)
					.OnGenerateWidget_Lambda([](TSharedPtr<FString> Item) -> TSharedRef<SWidget>
					{
						FText Tooltip;
						if (Item.IsValid())
						{
							if (const IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(**Item))
							{
								Tooltip = FText::FromString(ConsoleObject->GetHelp());
							}
						}
						return SNew(STextBlock)
							.Text(Item.IsValid() ? FText::FromString(*Item) : FText::GetEmpty())
							.ToolTipText(Tooltip);
					})
					.OnSelectionChanged_Lambda([this](TSharedPtr<FString> Item, ESelectInfo::Type)
					{
						SelectedCommand = Item;
						UpdateCommandHelp();
					})
					[
						SNew(STextBlock)
						.Text_Lambda([this]()
						{
							return SelectedCommand.IsValid()
								? FText::FromString(*SelectedCommand)
								: LOCTEXT("NoCommandSelected", "Choose a command…");
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SBox).WidthOverride(190.0f)
					[
						SNew(SEditableTextBox)
						.HintText(LOCTEXT("CommandArgsHint", "Arguments (optional)"))
						.ToolTipText(LOCTEXT("CommandArgsTip", "Arguments appended to the selected command, for example: 50 or 128 800 8."))
						.Text(FText::FromString(CommandArgs))
						.OnTextChanged_Lambda([this](const FText& Text) { CommandArgs = Text.ToString(); })
						.OnTextCommitted_Lambda([this](const FText& Text, ETextCommit::Type CommitType)
						{
							CommandArgs = Text.ToString();
							if (CommitType == ETextCommit::OnEnter)
							{
								RunSelectedCommand();
							}
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(6, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("RunCommand", "Run"))
					.ToolTipText(LOCTEXT("RunCommandTip", "Execute the selected command with the arguments above. Press Enter in the arguments box to run."))
					.IsEnabled_Lambda([this]() { return CanStart() && SelectedCommand.IsValid(); })
					.OnClicked_Lambda([this]()
					{
						RunSelectedCommand();
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
			[
				SAssignNew(CommandHelpText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]);

		// ------------------------------------------------------------------
		// Status + log
		// ------------------------------------------------------------------
		TSharedRef<SWidget> StatusCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("StatusTitle", "Status"))
					.Font(SectionFont)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
				[
					SAssignNew(ElapsedText, STextBlock)
					.Font(SmallFont)
					.ColorAndOpacity(FSlateColor(MutedText))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(DetailText, STextBlock)
				.Font(BodyFont)
				.AutoWrapText(true)
				.Text(LOCTEXT("StatusReadyDetail", "Choose a pipeline step or run the full pipeline."))
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 8, 0, 0)
			[
				SNew(SBox).HeightOverride(8.0f)
				[
					SAssignNew(ProgressBar, SProgressBar)
					.Percent(0.0f)
					.FillColorAndOpacity(FSlateColor(IdleColor))
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
			[
				SAssignNew(ResultText, STextBlock)
				.Font(SmallFont)
				.ColorAndOpacity(FSlateColor(MutedText))
				.AutoWrapText(true)
			]);

		TSharedRef<SWidget> LogCard = MakeCard(
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("LogTitle", "Log"))
					.Font(SectionFont)
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([this]()
					{
						return bAutoScrollLog ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
					})
					.OnCheckStateChanged_Lambda([this](ECheckBoxState State)
					{
						bAutoScrollLog = State == ECheckBoxState::Checked;
					})
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AutoScroll", "Auto-scroll"))
						.Font(SmallFont)
					]
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("CopyLog", "Copy"))
					.ToolTipText(LOCTEXT("CopyLogTip", "Copy the current log to the clipboard."))
					.OnClicked_Lambda([this]()
					{
						FPlatformApplicationMisc::ClipboardCopy(*BuildLogText());
						return FReply::Handled();
					})
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(4, 0, 0, 0)
				[
					SNew(SButton)
					.Text(LOCTEXT("ClearLog", "Clear"))
					.ToolTipText(LOCTEXT("ClearLogTip", "Clear the captured log."))
					.OnClicked_Lambda([this]()
					{
						NDGI::Editor::Pipeline::ClearLog();
						LastLogVersion = 0;
						return FReply::Handled();
					})
				]
			]
			+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 6, 0, 0)
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(FMargin(6.0f, 4.0f))
				[
					SNew(SBox).MinDesiredHeight(110.0f)
					[
						SAssignNew(LogScrollBox, SScrollBox)
						+ SScrollBox::Slot()
						[
							SAssignNew(LogTextBlock, STextBlock)
							.Font(MonoFont)
							.ColorAndOpacity(FSlateColor(FLinearColor(0.78f, 0.78f, 0.78f, 1.0f)))
							.AutoWrapText(true)
							.Text(FText::GetEmpty())
						]
					]
				]
			]);

		ChildSlot
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(FMargin(14.0f, 12.0f))
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					Header
				]
				+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 10, 0, 0)
				[
					SNew(SSplitter)
					.Orientation(Orient_Vertical)
					+ SSplitter::Slot().Value(0.62f)
					[
						SNew(SScrollBox)
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							HealthCard
						]
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							AtlasCard
						]
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							TodCard
						]
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							PipelineCard
						]
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							QualityCard
						]
						+ SScrollBox::Slot().Padding(0, 0, 0, 8)
						[
							ValidationCard
						]
						+ SScrollBox::Slot()
						[
							DiagnosticsCard
						]
					]
					+ SSplitter::Slot().Value(0.38f)
					[
						SNew(SVerticalBox)
						+ SVerticalBox::Slot().AutoHeight()
						[
							StatusCard
						]
						+ SVerticalBox::Slot().FillHeight(1.0f).Padding(0, 8, 0, 0)
						[
							LogCard
						]
					]
				]
			]
		];

		LastSceneRefreshSeconds = 0.0;
		LastLogVersion = 0;
		RefreshSceneInfo(true);
		RefreshUi();
		UpdateCommandHelp();

		RegisterActiveTimer(0.1f, FWidgetActiveTimerDelegate::CreateSP(this, &SNDGIPipelinePanel::Tick));
	}

	TSharedRef<SWidget> SNDGIPipelinePanel::MakeLabeledSpin(
		const FText& Label, int32 SNDGIPipelinePanel::*Member,
		int32 Min, int32 Max, int32 Delta, const FText& Tooltip)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(STextBlock)
				.Text(Label)
				.Font(FCoreStyle::GetDefaultFontStyle("Regular", 8))
				.ToolTipText(Tooltip)
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0, 2, 0, 0)
			[
				SNew(SSpinBox<int32>)
				.MinValue(Min)
				.MaxValue(Max)
				.Delta(Delta)
				.Value_Lambda([this, Member]() { return this->*Member; })
				.OnValueChanged_Lambda([this, Member, Min, Max, Delta](int32 Value)
				{
					// Keep the structural 16-texel / 8-texel grids valid when the
					// user types a free-form value into the spin box.
					if (Member == &SNDGIPipelinePanel::TrainTileSize)
					{
						Value = FMath::RoundToInt(static_cast<float>(Value) / 16.0f) * 16;
					}
					else if (Member == &SNDGIPipelinePanel::TrainOverlap)
					{
						Value = FMath::RoundToInt(static_cast<float>(Value) / 8.0f) * 8;
					}
					this->*Member = FMath::Clamp(Value, Min, Max);
				})
				.IsEnabled_Lambda([this]() { return CanStart(); })
			];
	}

	EActiveTimerReturnType SNDGIPipelinePanel::Tick(double, float)
	{
		RefreshUi();
		return EActiveTimerReturnType::Continue;
	}

	bool SNDGIPipelinePanel::IsRunning() const
	{
		return NDGI::Editor::Pipeline::GetState().bRunning;
	}

	bool SNDGIPipelinePanel::CanStart() const
	{
		return !IsRunning() && !NDGI::Editor::Pipeline::IsAutoRunning() && HasEditorWorld();
	}

	bool SNDGIPipelinePanel::HasEditorWorld() const
	{
		return GEditor && GEditor->GetEditorWorldContext().World() != nullptr;
	}

	UWorld* SNDGIPipelinePanel::GetEditorWorld() const
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	FString SNDGIPipelinePanel::GetLevelName() const
	{
		UWorld* World = GetEditorWorld();
		return World ? World->GetName() : TEXT("Level");
	}

	FString SNDGIPipelinePanel::GetLevelAssetPath() const
	{
		const FString LevelName = GetLevelName();
		return FString::Printf(TEXT("/Game/NDGI/Levels/NDGI_%s.NDGI_%s"), *LevelName, *LevelName);
	}

	FText SNDGIPipelinePanel::GetStatusText() const
	{
		const NDGI::Editor::Pipeline::FState State = NDGI::Editor::Pipeline::GetState();
		if (State.bRunning)
		{
			return FText::FromString(NDGI::Editor::Pipeline::StageToString(State.Stage) + TEXT("…"));
		}
		switch (State.Stage)
		{
		case NDGI::Editor::Pipeline::EStage::Completed:
			return LOCTEXT("StatusCompleted", "Completed");
		case NDGI::Editor::Pipeline::EStage::Failed:
			return LOCTEXT("StatusFailed", "Failed");
		default:
			return LOCTEXT("StatusIdle", "Ready");
		}
	}

	FSlateColor SNDGIPipelinePanel::GetStatusColor() const
	{
		const NDGI::Editor::Pipeline::FState State = NDGI::Editor::Pipeline::GetState();
		if (State.bRunning)
		{
			return FSlateColor(RunningColor);
		}
		if (State.Stage == NDGI::Editor::Pipeline::EStage::Completed)
		{
			return FSlateColor(PassedColor);
		}
		if (State.Stage == NDGI::Editor::Pipeline::EStage::Failed)
		{
			return FSlateColor(FailedColor);
		}
		return FSlateColor(IdleColor);
	}

	TOptional<float> SNDGIPipelinePanel::GetProgressPercent() const
	{
		const NDGI::Editor::Pipeline::FState State = NDGI::Editor::Pipeline::GetState();
		if (State.Total <= 0)
		{
			return TOptional<float>();
		}
		return FMath::Clamp(static_cast<float>(State.Current) / static_cast<float>(State.Total), 0.0f, 1.0f);
	}

	void SNDGIPipelinePanel::RefreshSceneInfo(bool bDeepScan)
	{
		const double Now = FPlatformTime::Seconds();
		if (!bDeepScan && Now - LastSceneRefreshSeconds < 2.0)
		{
			return;
		}
		LastSceneRefreshSeconds = Now;
		bDeepScan = bDeepScan || bForceDeepScan;
		bForceDeepScan = false;

		const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);
		const FSlateFontInfo BodyFont = FCoreStyle::GetDefaultFontStyle("Regular", 9);
		const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);

		auto FormatBytes = [](int64 Bytes) -> FString
		{
			const double MB = static_cast<double>(Bytes) / (1024.0 * 1024.0);
			return MB >= 1.0
				? FString::Printf(TEXT("%.2f MB"), MB)
				: FString::Printf(TEXT("%.0f KB"), static_cast<double>(Bytes) / 1024.0);
		};

		TArray<FString> HealthLines;
		TArray<FLinearColor> HealthColors;
		auto AddHealth = [&HealthLines, &HealthColors](const FString& Text, const FLinearColor& Color)
		{
			HealthLines.Add(Text);
			HealthColors.Add(Color);
		};

		FString Summary;
		FString NextStep = TEXT("Open a level to start.");
		FLinearColor NextColor = IdleColor;
		UWorld* World = GetEditorWorld();
		if (!World)
		{
			Summary = TEXT("No editor level is open.");
			AddHealth(TEXT("Open a level before running the pipeline."), WarningColor);
		}
		else
		{
			Summary = FString::Printf(TEXT("Level: %s"), *World->GetName());

			const FString AssetPath = GetLevelAssetPath();
			FString PackageName = AssetPath;
			int32 DotIndex = INDEX_NONE;
			if (PackageName.FindChar(TEXT('.'), DotIndex))
			{
				PackageName.LeftInline(DotIndex);
			}

			UNDGILevelAsset* LevelAsset = nullptr;
			if (FPackageName::DoesPackageExist(PackageName))
			{
				LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
			}

			const bool bAssetChanged = ScannedAssetPath != AssetPath;
			if (!LevelAsset && bAssetChanged)
			{
				CachedAtlasRows.Reset();
				ScannedAssetPath = AssetPath;
			}
			if (LevelAsset && (bDeepScan || bAssetChanged || CachedAtlasRows.Num() == 0))
			{
				TArray<FAtlasUiRow> Rows;
				for (int32 AtlasIndex = 0; AtlasIndex < LevelAsset->Atlases.Num(); ++AtlasIndex)
				{
					const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[AtlasIndex];
					FAtlasUiRow Row;
					Row.TexIndex = AtlasIndex;
					Row.Resolution = FString::Printf(TEXT("%dx%d"), Entry.AtlasSize.X, Entry.AtlasSize.Y);
					Row.Grid = FString::Printf(TEXT("%dx%d"), Entry.TileGrid.X, Entry.TileGrid.Y);
					Row.EncodingRanges = Entry.EncodingRanges.Num();

					const int32 CellCount = Entry.TileGrid.X > 0 && Entry.TileGrid.Y > 0
						? Entry.TileGrid.X * Entry.TileGrid.Y
						: Entry.TileModelPaths.Num();
					int64 AtlasBytes = 0;
					for (int32 Cell = 0; Cell < CellCount; ++Cell)
					{
						const FSoftObjectPath ModelPath = Entry.GetTileModelPath(Cell);
						if (!ModelPath.IsValid())
						{
							continue;
						}
						++Row.ModelCount;

						const UNDGIModelAsset* Model = Cast<UNDGIModelAsset>(ModelPath.TryLoad());
						if (!Model)
						{
							continue;
						}
						if (!Model->HasBC7Features())
						{
							++Row.LegacyCount;
						}

						NDGI::Train::FTileTrainConfig Config;
						Config.UVSize = Model->TileSize.X;
						Config.UVTSize = Model->FeatureUVTSize.X;
						Config.UTSize = Model->UTSize;
						Config.VTSize = Model->VTSize;
						Config.TimeSlices = Model->TimeSlices;
						const int64 CompactBytes =
							static_cast<int64>(NDGI::Runtime::ComputeCompactTileLayout(Config).TotalParams) * sizeof(float);
						AtlasBytes += Model->FeatureUVBC7.Num() + Model->FeatureUVTBC7.Num() + CompactBytes;
					}
					Row.Bytes = AtlasBytes;
					Rows.Add(Row);
				}
				CachedAtlasRows = MoveTemp(Rows);
				ScannedAssetPath = AssetPath;
			}

			int32 TotalModels = 0;
			int32 TotalLegacy = 0;
			int64 TotalBytes = 0;
			for (const FAtlasUiRow& Row : CachedAtlasRows)
			{
				TotalModels += Row.ModelCount;
				TotalLegacy += Row.LegacyCount;
				TotalBytes += Row.Bytes;
			}

			if (LevelAsset)
			{
				Summary += FString::Printf(TEXT("\nLevel asset: %d atlas(es) · %d model(s) · %s projected"),
					LevelAsset->Atlases.Num(), TotalModels, *FormatBytes(TotalBytes));
				if (TotalLegacy > 0)
				{
					AddHealth(FString::Printf(
						TEXT("%d of %d model(s) are pre-BC7. Retrain before packaging or the runtime skips them."),
						TotalLegacy, TotalModels), FailedColor);
					NextStep = TEXT("Next: Retrain models to add BC7 features (step 3).");
					NextColor = FailedColor;
				}
				else if (TotalModels > 0)
				{
					AddHealth(TEXT("All models have BC7 features."), PassedColor);
				}
				else
				{
					AddHealth(TEXT("The level asset has no model paths. Run Train All Atlases."), WarningColor);
				}
			}
			else
			{
				Summary += TEXT("\nLevel asset: not trained");
				AddHealth(TEXT("No trained level asset. Run Train All Atlases to create one."), WarningColor);
				NextStep = TEXT("Next: Train models after baking and merging (step 3).");
				NextColor = WarningColor;
			}

			const FString BakeDir = FPaths::ProjectSavedDir() / TEXT("NDGI/Bake");
			TArray<FString> MergedFiles;
			IFileManager::Get().FindFiles(MergedFiles, *(BakeDir / TEXT("Merged_Tex*.ndgi")), true, false);
			FDateTime NewestMerged;
			for (const FString& File : MergedFiles)
			{
				const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*(BakeDir / File));
				if (Stamp > NewestMerged)
				{
					NewestMerged = Stamp;
				}
			}

			BakeManifest::FManifest Manifest;
			const bool bHaveManifest = BakeManifest::Load(Manifest);
			const FString WorldPackage = World->GetOutermost()->GetName();
			const bool bManifestMatches = bHaveManifest && Manifest.LevelPackage == WorldPackage;
			if (bHaveManifest)
			{
				const double AgeDays = Manifest.SavedAtTicks > 0
					? FMath::Max(0.0, static_cast<double>(FDateTime::UtcNow().GetTicks() - Manifest.SavedAtTicks)
						/ static_cast<double>(ETimespan::TicksPerDay))
					: 0.0;
				Summary += FString::Printf(TEXT("\nBake: %d slice(s) · %d atlas(es) · %.1f day(s) old"),
					Manifest.Steps, Manifest.Atlases.Num(), AgeDays);
				if (!bManifestMatches)
				{
					AddHealth(FString::Printf(
						TEXT("Bake manifest belongs to '%s'. Re-bake the current level."),
						*Manifest.LevelPackage), FailedColor);
					NextStep = TEXT("Next: Bake the current level (step 1).");
					NextColor = FailedColor;
				}
				else
				{
					if (Manifest.Steps != BakeSteps)
					{
						AddHealth(FString::Printf(
							TEXT("Panel has %d slices but the bake has %d. Bake with %d or use TOD Defaults."),
							BakeSteps, Manifest.Steps, Manifest.Steps), WarningColor);
					}
					if (AgeDays > 14.0)
					{
						AddHealth(TEXT("Bake is more than two weeks old. Re-bake after lighting or TOD changes."), WarningColor);
					}
				}
			}
			else
			{
				Summary += TEXT("\nBake: no manifest");
				AddHealth(TEXT("No bake manifest. Merge/Train fall back to scanning the shared bake folder."), WarningColor);
				NextStep = TEXT("Next: Bake time-of-day (step 1).");
				NextColor = WarningColor;
			}

			Summary += FString::Printf(TEXT("\nMerged sets: %d file(s)"), MergedFiles.Num());
			if (MergedFiles.Num() == 0)
			{
				AddHealth(TEXT("No merged sets. Run Merge Steps."), WarningColor);
				if (!bHaveManifest || bManifestMatches)
				{
					NextStep = TEXT("Next: Merge slices (step 2).");
					NextColor = WarningColor;
				}
			}
			else if (bManifestMatches && Manifest.SavedAtTicks > 0 && NewestMerged.GetTicks() < Manifest.SavedAtTicks)
			{
				AddHealth(TEXT("Merged sets are older than the current bake. Run Merge Steps."), WarningColor);
				NextStep = TEXT("Next: Merge slices (step 2).");
				NextColor = WarningColor;
			}
			else
			{
				AddHealth(FString::Printf(TEXT("Merged sets: %d file(s)."), MergedFiles.Num()), PassedColor);
			}

			if (const UNDGITimeOfDayProfile* Profile = UNDGITimeOfDayProfile::LoadDefaultProfile())
			{
				Summary += FString::Printf(TEXT("\nTOD: %d keyframe(s) · cycle %.0f s"),
					Profile->Keyframes.Num(), Profile->CycleDurationSeconds);
				if (Profile->Keyframes.Num() < 2)
				{
					AddHealth(TEXT("TOD profile has fewer than two keyframes."), WarningColor);
				}
			}
			else
			{
				Summary += TEXT("\nTOD: no profile (legacy sun arc)");
				AddHealth(TEXT("No TOD profile. Bake and runtime use the legacy sun arc."), WarningColor);
			}

			if (NextStep.IsEmpty())
			{
				NextStep = TEXT("Next: Validate the level or run a temporal holdout (step 4).");
				NextColor = PassedColor;
			}
		}

		if (HealthSummaryText.IsValid())
		{
			HealthSummaryText->SetText(FText::FromString(Summary));
		}

		if (HealthListBox.IsValid())
		{
			HealthListBox->ClearChildren();
			for (int32 Index = 0; Index < HealthLines.Num(); ++Index)
			{
				HealthListBox->AddSlot()
					.AutoHeight()
					.Padding(FMargin(0.0f, 2.0f))
					[
						SNew(STextBlock)
						.Text(FText::FromString(TEXT("• ") + HealthLines[Index]))
						.Font(SmallFont)
						.ColorAndOpacity(FSlateColor(HealthColors[Index]))
						.AutoWrapText(true)
					];
			}
			HealthListBox->AddSlot()
				.AutoHeight()
				.Padding(FMargin(0.0f, 6.0f, 0.0f, 0.0f))
				[
					SNew(STextBlock)
					.Text(FText::FromString(NextStep))
					.Font(BodyFont)
					.ColorAndOpacity(FSlateColor(NextColor))
					.AutoWrapText(true)
				];
		}

		if (AtlasListBox.IsValid())
		{
			AtlasListBox->ClearChildren();
			if (CachedAtlasRows.Num() == 0)
			{
				AtlasListBox->AddSlot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("NoAtlases", "No trained atlases yet. Bake, merge, and train to populate this list."))
						.Font(SmallFont)
						.ColorAndOpacity(FSlateColor(MutedText))
						.AutoWrapText(true)
					];
			}
			else
			{
				AtlasListBox->AddSlot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.Text(LOCTEXT("AtlasHeader", "Idx   Resolution   Grid   Models   BC7   Ranges   Projected"))
						.Font(MonoFont)
						.ColorAndOpacity(FSlateColor(MutedText))
					];
				for (const FAtlasUiRow& Row : CachedAtlasRows)
				{
					const FString Bc7Label = Row.LegacyCount == 0
						? FString(TEXT("yes"))
						: FString::Printf(TEXT("%d old"), Row.LegacyCount);
					const FString Line = FString::Printf(
						TEXT("%02d    %-11s  %-5s  %5d   %-6s  %6d   %s"),
						Row.TexIndex, *Row.Resolution, *Row.Grid, Row.ModelCount,
						*Bc7Label, Row.EncodingRanges, *FormatBytes(Row.Bytes));

					AtlasListBox->AddSlot()
						.AutoHeight()
						.Padding(FMargin(0.0f, 2.0f))
						[
							SNew(STextBlock)
							.Text(FText::FromString(Line))
							.Font(MonoFont)
							.ColorAndOpacity(FSlateColor(Row.LegacyCount == 0 ? IdleColor : WarningColor))
							.AutoWrapText(true)
						];
				}
			}
		}

		// Holdout result files are small; re-read them on the slow refresh.
		if (HoldoutResultText.IsValid())
		{
			auto ReadHoldout = [](const FString& Path, double& OutPsnr, int32& OutTiles) -> bool
			{
				FString Contents;
				if (!FFileHelper::LoadFileToString(Contents, *Path))
				{
					return false;
				}
				FRegexPattern PsnrPattern(TEXT("\"overall\"\\s*:\\s*([\\d.]+)"));
				FRegexMatcher PsnrMatcher(PsnrPattern, Contents);
				FRegexPattern TilesPattern(TEXT("\"tilesEvaluated\"\\s*:\\s*([\\d]+)"));
				FRegexMatcher TilesMatcher(TilesPattern, Contents);
				if (!PsnrMatcher.FindNext())
				{
					return false;
				}
				OutPsnr = FCString::Atod(*PsnrMatcher.GetCaptureGroup(1));
				OutTiles = TilesMatcher.FindNext() ? FCString::Atoi(*TilesMatcher.GetCaptureGroup(1)) : 0;
				return true;
			};

			const FString HoldoutPath = FPaths::ProjectSavedDir() / TEXT("NDGI/TemporalHoldout.json");
			const FString BaselinePath = FPaths::ProjectSavedDir() / TEXT("NDGI/TemporalHoldoutBaseline.json");
			double HoldoutPsnr = 0.0;
			double BaselinePsnr = 0.0;
			int32 HoldoutTileCount = 0;
			int32 BaselineTileCount = 0;
			const bool bHaveHoldout = ReadHoldout(HoldoutPath, HoldoutPsnr, HoldoutTileCount);
			const bool bHaveBaseline = ReadHoldout(BaselinePath, BaselinePsnr, BaselineTileCount);
			if (bHaveHoldout || bHaveBaseline)
			{
				FString Text;
				if (bHaveHoldout)
				{
					Text = FString::Printf(TEXT("Holdout: %.2f dB over %d tile(s)"), HoldoutPsnr, HoldoutTileCount);
				}
				if (bHaveBaseline)
				{
					Text += Text.IsEmpty()
						? FString::Printf(TEXT("Baseline: %.2f dB over %d tile(s)"), BaselinePsnr, BaselineTileCount)
						: FString::Printf(TEXT("  ·  Baseline: %.2f dB over %d tile(s)"), BaselinePsnr, BaselineTileCount);
				}
				if (bHaveHoldout && bHaveBaseline)
				{
					Text += FString::Printf(TEXT("  ·  gap %.2f dB"), BaselinePsnr - HoldoutPsnr);
				}
				HoldoutResultText->SetText(FText::FromString(Text));
				HoldoutResultText->SetColorAndOpacity(FSlateColor(
					bHaveHoldout && HoldoutPsnr < 10.0 ? WarningColor : MutedText));
			}
			else
			{
				HoldoutResultText->SetText(LOCTEXT("NoHoldout", "No holdout result yet. Run a preview above."));
				HoldoutResultText->SetColorAndOpacity(FSlateColor(MutedText));
			}
		}

		RefreshQualityCard();
		RefreshTodCard();
	}

	void SNDGIPipelinePanel::ApplyProfileDefaults()
	{
		if (UNDGITimeOfDayProfile* Profile = UNDGITimeOfDayProfile::LoadDefaultProfile())
		{
			BakeSteps = FMath::Clamp(Profile->BakeSteps, 2, 32);
			TrainTileSize = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Profile->TileSize) / 16.0f) * 16, 16, 512);
			TrainSteps = FMath::Clamp(Profile->TrainSteps, 1, 200000);
			TrainOverlap = FMath::Clamp(FMath::RoundToInt(static_cast<float>(Profile->OverlapTexels) / 8.0f) * 8, 0, 64);
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(
				TEXT("Loaded TOD profile defaults: %d slices, tile %d, %d steps, halo %d."),
				BakeSteps, TrainTileSize, TrainSteps, TrainOverlap));
		}
		else
		{
			NDGI::Editor::Pipeline::AddLogLine(TEXT("No TOD profile found; run ndgi.CreateTODProfile first."));
		}
	}

	UNDGITimeOfDayProfile* SNDGIPipelinePanel::GetTODProfile()
	{
		return UNDGITimeOfDayProfile::LoadDefaultProfile();
	}

	void SNDGIPipelinePanel::RefreshTodCard()
	{
		if (!TodSummaryText.IsValid() || !TodKeyframeList.IsValid())
		{
			return;
		}

		const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);
		const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);

		UNDGITimeOfDayProfile* Profile = GetTODProfile();
		TodProfile = Profile;

		TodKeyframeList->ClearChildren();

		if (!Profile)
		{
			TodPreviewSunColor = FLinearColor::Black;
			TodPreviewSkyColor = FLinearColor::Black;
			TodSummaryText->SetText(LOCTEXT("NoTodProfile",
				"No default profile. Create one, or the bake and runtime fall back to the legacy sun arc."));
			TodSummaryText->SetColorAndOpacity(FSlateColor(WarningColor));
			if (TodPreviewText.IsValid())
			{
				TodPreviewText->SetText(LOCTEXT("NoTodPreview", "Create the default profile to preview keyframes."));
			}
			return;
		}

		const bool bDirty = Profile->GetOutermost()->IsDirty();
		TodSummaryText->SetText(FText::FromString(FString::Printf(
			TEXT("%s · %d keyframe(s) · cycle %.1f s · %s%s\nDefaults: %d slices · tile %d · %d steps · halo %d"),
			*Profile->GetPathName(), Profile->Keyframes.Num(), Profile->CycleDurationSeconds,
			Profile->bPingPong ? TEXT("ping-pong") : TEXT("loop"),
			bDirty ? TEXT(" · UNSAVED") : TEXT(""),
			Profile->BakeSteps, Profile->TileSize, Profile->TrainSteps, Profile->OverlapTexels)));
		TodSummaryText->SetColorAndOpacity(FSlateColor(bDirty ? WarningColor : MutedText));

		const FNDGITimeOfDayKey Preview = Profile->Evaluate(TodPreviewTime);
		TodPreviewSunColor = Preview.SunColor;
		TodPreviewSkyColor = Preview.SkyColor;
		if (TodPreviewText.IsValid())
		{
			TodPreviewText->SetText(FText::FromString(FString::Printf(
				TEXT("t %.2f   sun elevation %.1f° · azimuth %.1f° · intensity %.2f   sky intensity %.2f"),
				Preview.Time, Preview.SunPitch, Preview.SunYaw, Preview.SunIntensity, Preview.SkyIntensity)));
		}

		auto MakeSwatch = [](const FLinearColor& Color) -> TSharedRef<SWidget>
		{
			return SNew(SBorder)
				.BorderImage(FCoreStyle::Get().GetBrush("WhiteBrush"))
				.BorderBackgroundColor(FSlateColor(Color))
				.Padding(FMargin(0.0f))
				[
					SNew(SBox).WidthOverride(12.0f).HeightOverride(12.0f)
				];
		};

		TArray<FNDGITimeOfDayKey> Sorted = Profile->Keyframes;
		Sorted.Sort([](const FNDGITimeOfDayKey& A, const FNDGITimeOfDayKey& B)
		{
			return A.Time < B.Time;
		});

		for (const FNDGITimeOfDayKey& Key : Sorted)
		{
			const float KeyTime = Key.Time;
			TodKeyframeList->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 2.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("t %.2f   ele %5.1f°   az %5.1f°   "),
							Key.Time, Key.SunPitch, Key.SunYaw)))
						.Font(MonoFont)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						MakeSwatch(Key.SunColor)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("×%.1f"), Key.SunIntensity)))
						.Font(MonoFont)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(12, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text(LOCTEXT("TodSkyLabel", "sky"))
						.Font(SmallFont)
						.ColorAndOpacity(FSlateColor(MutedText))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
					[
						MakeSwatch(Key.SkyColor)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(4, 0, 0, 0)
					[
						SNew(STextBlock)
						.Text(FText::FromString(FString::Printf(TEXT("×%.1f"), Key.SkyIntensity)))
						.Font(MonoFont)
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("TodPreviewKey", "Preview"))
						.ToolTipText(LOCTEXT("TodPreviewKeyTip", "Move the preview slider to this keyframe's time."))
						.OnClicked_Lambda([this, KeyTime]()
						{
							TodPreviewTime = FMath::Clamp(KeyTime, 0.0f, 1.0f);
							RefreshTodCard();
							return FReply::Handled();
						})
					]
				];
		}
	}

	void SNDGIPipelinePanel::OpenTODProfile()
	{
		UNDGITimeOfDayProfile* Profile = GetTODProfile();
		if (!Profile)
		{
			NDGI::Editor::Pipeline::AddLogLine(TEXT("No TOD profile to open. Create the default profile first."));
			return;
		}

		if (GEditor)
		{
			if (UAssetEditorSubsystem* AssetEditor = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>())
			{
				AssetEditor->OpenEditorForAsset(Profile);
			}
		}
	}

	void SNDGIPipelinePanel::CreateTODProfileAsset()
	{
		RunCommand(TEXT("ndgi.CreateTODProfile"), FString(), true);
		LastSceneRefreshSeconds = 0.0;
		bForceDeepScan = true;
		RefreshSceneInfo(true);
	}

	void SNDGIPipelinePanel::SaveTODProfile()
	{
		UNDGITimeOfDayProfile* Profile = TodProfile.Get();
		if (!Profile)
		{
			return;
		}

		UPackage* Package = Profile->GetOutermost();
		Package->MarkPackageDirty();
		const FString FileName = FPackageName::LongPackageNameToFilename(
			Package->GetName(), FPackageName::GetAssetPackageExtension());
		FSavePackageArgs SaveArgs;
		SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
		if (UPackage::SavePackage(Package, Profile, *FileName, SaveArgs))
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(TEXT("Saved TOD profile %s."), *FileName));
		}
		else
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(TEXT("Failed to save TOD profile %s."), *FileName));
		}
		RefreshTodCard();
	}

	void SNDGIPipelinePanel::RefreshQualityCard()
	{
		if (!QualitySummaryText.IsValid() || !QualityListBox.IsValid())
		{
			return;
		}

		const FString MetricsPath = FPaths::ProjectSavedDir() / TEXT("NDGI/Training/TileMetrics.json");
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*MetricsPath);
		const int64 Size = IFileManager::Get().FileSize(*MetricsPath);
		if (Stamp != CachedTileMetricsStamp || Size != CachedTileMetricsSize)
		{
			CachedTileMetricsStamp = Stamp;
			CachedTileMetricsSize = Size;
			CachedTileMetrics.Reset();

			FString Contents;
			if (Size > 0 && FFileHelper::LoadFileToString(Contents, *MetricsPath))
			{
				FRegexPattern TilePattern(TEXT(
					"\\{\"texIndex\"\\s*:\\s*(\\d+),\\s*\"tileX\"\\s*:\\s*(\\d+),\\s*\"tileY\"\\s*:\\s*(\\d+),"
					"\\s*\"psnr\"\\s*:\\s*(-?[\\d.]+),\\s*\"rgb\"\\s*:\\s*(-?[\\d.]+),\\s*\"sh\"\\s*:\\s*(-?[\\d.]+)\\}"));
				FRegexMatcher Matcher(TilePattern, Contents);
				while (Matcher.FindNext())
				{
					FTileMetricUi Metric;
					Metric.TexIndex = FCString::Atoi(*Matcher.GetCaptureGroup(1));
					Metric.TileX = FCString::Atoi(*Matcher.GetCaptureGroup(2));
					Metric.TileY = FCString::Atoi(*Matcher.GetCaptureGroup(3));
					Metric.Psnr = FCString::Atod(*Matcher.GetCaptureGroup(4));
					Metric.RgbPsnr = FCString::Atod(*Matcher.GetCaptureGroup(5));
					Metric.ShPsnr = FCString::Atod(*Matcher.GetCaptureGroup(6));
					CachedTileMetrics.Add(Metric);
				}
			}

			CachedTileMetrics.Sort([](const FTileMetricUi& A, const FTileMetricUi& B)
			{
				return A.Psnr < B.Psnr;
			});

			UE_LOG(LogNDGI, Display, TEXT("NDGI Pipeline: loaded %d per-tile metric(s) from %s."),
				CachedTileMetrics.Num(), *MetricsPath);
		}

		QualityListBox->ClearChildren();
		const FSlateFontInfo SmallFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);
		const FSlateFontInfo MonoFont = FCoreStyle::GetDefaultFontStyle("Regular", 8);

		if (CachedTileMetrics.Num() == 0)
		{
			QualitySummaryText->SetText(LOCTEXT("NoTileMetrics",
				"No per-tile metrics yet. Train models to write Saved/NDGI/Training/TileMetrics.json."));
			QualitySummaryText->SetColorAndOpacity(FSlateColor(MutedText));
			return;
		}

		double PsnrSum = 0.0;
		for (const FTileMetricUi& Metric : CachedTileMetrics)
		{
			PsnrSum += Metric.Psnr;
		}
		const double Average = PsnrSum / static_cast<double>(CachedTileMetrics.Num());
		const double Worst = CachedTileMetrics[0].Psnr;

		QualitySummaryText->SetText(FText::FromString(FString::Printf(
			TEXT("Last training: %d tile(s) · average %.2f dB · worst %.2f dB"),
			CachedTileMetrics.Num(), Average, Worst)));
		QualitySummaryText->SetColorAndOpacity(FSlateColor(
			Worst < 15.0 ? FailedColor : (Worst < 20.0 ? WarningColor : MutedText)));

		const int32 MaxRows = 5;
		const int32 RowCount = FMath::Min(CachedTileMetrics.Num(), MaxRows);
		for (int32 Index = 0; Index < RowCount; ++Index)
		{
			const FTileMetricUi& Metric = CachedTileMetrics[Index];
			const FString Line = FString::Printf(
				TEXT("Tex%02d  tile (%d,%d)   %.2f dB   RGB %.2f   SH %.2f"),
				Metric.TexIndex, Metric.TileX, Metric.TileY,
				Metric.Psnr, Metric.RgbPsnr, Metric.ShPsnr);
			const FLinearColor Color = Metric.Psnr < 15.0
				? FailedColor
				: (Metric.Psnr < 20.0 ? WarningColor : IdleColor);

			QualityListBox->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 2.0f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.Text(FText::FromString(Line))
						.Font(MonoFont)
						.ColorAndOpacity(FSlateColor(Color))
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8, 0, 0, 0)
					[
						SNew(SButton)
						.Text(LOCTEXT("PreviewWorstTile", "Preview"))
						.ToolTipText(LOCTEXT("PreviewWorstTileTip", "Open the model preview tab for this tile."))
						.OnClicked_Lambda([this, Metric]()
						{
							PreviewTile(Metric.TexIndex, Metric.TileX, Metric.TileY);
							return FReply::Handled();
						})
					]
				];
		}

		if (CachedTileMetrics.Num() > RowCount)
		{
			QualityListBox->AddSlot()
				.AutoHeight()
				.Padding(0.0f, 2.0f)
				[
					SNew(STextBlock)
					.Text(FText::FromString(FString::Printf(
						TEXT("…and %d more. Open the JSON for the full list."),
						CachedTileMetrics.Num() - RowCount)))
					.Font(SmallFont)
					.ColorAndOpacity(FSlateColor(MutedText))
				];
		}
	}

	void SNDGIPipelinePanel::PreviewTile(int32 TexIndex, int32 TileX, int32 TileY)
	{
		const FString AssetPath = GetLevelAssetPath();
		UNDGILevelAsset* LevelAsset = LoadObject<UNDGILevelAsset>(nullptr, *AssetPath);
		if (!LevelAsset || !LevelAsset->Atlases.IsValidIndex(TexIndex))
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(
				TEXT("No trained level asset entry for Tex%02d. Train and rescan first."), TexIndex));
			return;
		}

		const FNDGIAtlasEntry& Entry = LevelAsset->Atlases[TexIndex];
		if (Entry.TileGrid.X <= 0 || Entry.TileGrid.Y <= 0
			|| TileX < 0 || TileY < 0 || TileX >= Entry.TileGrid.X || TileY >= Entry.TileGrid.Y)
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(
				TEXT("Tile (%d,%d) is outside Tex%02d's %dx%d grid."),
				TileX, TileY, TexIndex, Entry.TileGrid.X, Entry.TileGrid.Y));
			return;
		}

		const FSoftObjectPath ModelPath = Entry.GetTileModelPath(TileY * Entry.TileGrid.X + TileX);
		if (!ModelPath.IsValid())
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(
				TEXT("Tex%02d tile (%d,%d) has no model path. Retrain the atlas."), TexIndex, TileX, TileY));
			return;
		}

		SetNDGIPreviewModel(ModelPath.ToString());
		OpenNDGIPreviewTab();
	}

	void SNDGIPipelinePanel::RefreshUi()
	{
		using namespace NDGI::Editor;

		const Pipeline::FState State = Pipeline::GetState();

		if (StatusText.IsValid())
		{
			StatusText->SetText(GetStatusText());
			StatusText->SetColorAndOpacity(GetStatusColor());
		}
		if (DetailText.IsValid())
		{
			FString Detail;
			if (State.bRunning)
			{
				Detail = State.Operation;
				if (!State.Detail.IsEmpty())
				{
					Detail += FString::Printf(TEXT("  ·  %s"), *State.Detail);
				}
			}
			else if (State.Stage == Pipeline::EStage::Completed || State.Stage == Pipeline::EStage::Failed)
			{
				Detail = State.Detail.IsEmpty() ? State.Operation : State.Detail;
			}
			else
			{
				Detail = TEXT("Choose a pipeline step or run the full pipeline.");
			}
			DetailText->SetText(FText::FromString(Detail));
		}
		if (ElapsedText.IsValid())
		{
			const double Elapsed = Pipeline::GetElapsedSeconds(State);
			FString Text = State.bRunning || Elapsed > 0.0
				? FString::Printf(TEXT("%.1f s"), Elapsed)
				: FString();
			if (State.Total > 0)
			{
				Text += FString::Printf(TEXT("   ·   %d / %d"), State.Current, State.Total);
				if (State.bRunning && State.Current > 0 && State.Current < State.Total)
				{
					const double Remaining = Elapsed * static_cast<double>(State.Total - State.Current)
						/ static_cast<double>(State.Current);
					Text += Remaining >= 60.0
						? FString::Printf(TEXT("   ·   ~%.0f min left"), Remaining / 60.0)
						: FString::Printf(TEXT("   ·   ~%.0f s left"), Remaining);
				}
			}
			ElapsedText->SetText(FText::FromString(Text));
		}
		if (ProgressBar.IsValid())
		{
			const TOptional<float> Percent = GetProgressPercent();
			ProgressBar->SetPercent(Percent);
			ProgressBar->SetFillColorAndOpacity(GetStatusColor());
		}
		if (ResultText.IsValid())
		{
			FString Result;
			if (Pipeline::IsAutoRunning())
			{
				Result = TEXT("Full pipeline active — closing this tab will not stop it.");
			}
			else if (!State.bRunning && State.Stage == Pipeline::EStage::Completed)
			{
				Result = TEXT("Last run passed.");
			}
			else if (!State.bRunning && State.Stage == Pipeline::EStage::Failed)
			{
				Result = TEXT("Last run failed — see the log for details.");
			}
			ResultText->SetText(FText::FromString(Result));
		}

		// A finished job may have written models, level assets, or bake files.
		if (bLastRunning && !State.bRunning)
		{
			bForceDeepScan = true;
			LastSceneRefreshSeconds = 0.0;
		}

		// Re-evaluate IsEnabled attributes when the running or auto-run state changes.
		const bool bAutoRunning = Pipeline::IsAutoRunning();
		if (State.bRunning != bLastRunning || State.Stage != LastStage || bAutoRunning != bLastAutoRunning)
		{
			bLastRunning = State.bRunning;
			LastStage = State.Stage;
			bLastAutoRunning = bAutoRunning;
			Invalidate(EInvalidateWidget::Layout);
		}

		// Log.
		const uint64 LogVersion = Pipeline::GetLogVersion();
		if (LogVersion != LastLogVersion)
		{
			LastLogVersion = LogVersion;
			if (LogTextBlock.IsValid())
			{
				LogTextBlock->SetText(FText::FromString(BuildLogText()));
				if (bAutoScrollLog && LogScrollBox.IsValid())
				{
					LogScrollBox->ScrollToEnd();
				}
			}
		}

		RefreshSceneInfo();
	}

	void SNDGIPipelinePanel::ExecuteConsoleCommand(const FString& Command, bool bAssetsMayChange)
	{
		if (!GEditor)
		{
			return;
		}

		NDGI::Editor::Pipeline::AddLogLine(FString::Printf(TEXT("> %s"), *Command));
		FStringOutputDevice Output;
		const bool bExecuted = GEditor->Exec(GetEditorWorld(), *Command, Output);
		if (!bExecuted)
		{
			NDGI::Editor::Pipeline::AddLogLine(FString::Printf(TEXT("Command failed to execute: %s"), *Command));
		}
		else if (Output.Len() > 0)
		{
			NDGI::Editor::Pipeline::AddLogLine(Output);
		}

		LastSceneRefreshSeconds = 0.0;
		bForceDeepScan = bForceDeepScan || bAssetsMayChange;
	}

	void SNDGIPipelinePanel::RunCommand(const FString& Command, const FString& Args, bool bAssetsMayChange)
	{
		if (!CanStart() || Command.IsEmpty())
		{
			return;
		}

		const FString TrimmedArgs = Args.TrimStartAndEnd();
		ExecuteConsoleCommand(
			TrimmedArgs.IsEmpty() ? Command : FString::Printf(TEXT("%s %s"), *Command, *TrimmedArgs),
			bAssetsMayChange);
	}

	void SNDGIPipelinePanel::RunSelectedCommand()
	{
		if (SelectedCommand.IsValid())
		{
			RunCommand(*SelectedCommand, CommandArgs.TrimStartAndEnd());
		}
	}

	void SNDGIPipelinePanel::RefreshCommandList()
	{
		CommandOptions.Reset();
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
			FConsoleObjectVisitor::CreateLambda([this](const TCHAR* Name, IConsoleObject* ConsoleObject)
			{
				if (Name && ConsoleObject)
				{
					CommandOptions.Add(MakeShared<FString>(Name));
				}
			}),
			TEXT("ndgi."));

		CommandOptions.Sort([](const TSharedPtr<FString>& A, const TSharedPtr<FString>& B)
		{
			return A->Compare(*B) < 0;
		});

		// Prefer the storage report so the browser opens on a useful command.
		SelectedCommand.Reset();
		for (const TSharedPtr<FString>& Option : CommandOptions)
		{
			if (Option.IsValid() && *Option == TEXT("ndgi.ReportStorage"))
			{
				SelectedCommand = Option;
				break;
			}
		}
		if (!SelectedCommand.IsValid() && CommandOptions.Num() > 0)
		{
			SelectedCommand = CommandOptions[0];
		}
	}

	void SNDGIPipelinePanel::UpdateCommandHelp()
	{
		if (!CommandHelpText.IsValid())
		{
			return;
		}

		FString Help;
		if (SelectedCommand.IsValid())
		{
			if (const IConsoleObject* ConsoleObject = IConsoleManager::Get().FindConsoleObject(**SelectedCommand))
			{
				Help = ConsoleObject->GetHelp();
			}
		}

		CommandHelpText->SetText(Help.IsEmpty()
			? LOCTEXT("CommandHelpEmpty", "Select a command to see its help text.")
			: FText::FromString(Help));
	}

	void SNDGIPipelinePanel::StartBake()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.BakeTOD %d"), BakeSteps), true);
	}

	void SNDGIPipelinePanel::StartMerge()
	{
		if (!CanStart())
		{
			return;
		}

		// Prefer the slice count recorded by the last bake; the user may have
		// changed the panel value since then.
		int32 MergeSteps = BakeSteps;
		BakeManifest::FManifest Manifest;
		UWorld* World = GetEditorWorld();
		if (World && BakeManifest::Load(Manifest)
			&& Manifest.LevelPackage == World->GetOutermost()->GetName()
			&& Manifest.Steps > 0)
		{
			MergeSteps = Manifest.Steps;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.MergeAllBakedSets %d"), MergeSteps), true);
	}

	void SNDGIPipelinePanel::StartTrain()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.TrainAllAtlases %d %d %d"),
			TrainTileSize, TrainSteps, TrainOverlap), true);
	}

	void SNDGIPipelinePanel::StartValidateLevel()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.ValidateLevelAsset %.3f"), ValidateTime));
	}

	void SNDGIPipelinePanel::StartValidateSH()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.ValidateSH %.3f"), ValidateTime));
	}

	void SNDGIPipelinePanel::StartValidateSeams()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.ValidateSeams %.3f"), ValidateTime));
	}

	void SNDGIPipelinePanel::StartValidateDirectionality()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.ValidateDirectionality %.3f"), ValidateTime));
	}

	void SNDGIPipelinePanel::StartValidateHoldout()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(FString::Printf(TEXT("ndgi.ValidateTemporalHoldout %d %d %d -1 %d%s"),
			TrainTileSize, HoldoutSteps, TrainOverlap, HoldoutTiles,
			bHoldoutBaseline ? TEXT(" baseline") : TEXT("")));
	}

	void SNDGIPipelinePanel::StartInspectRanges()
	{
		if (!CanStart())
		{
			return;
		}
		ExecuteConsoleCommand(TEXT("ndgi.InspectLightmapScales"));
	}

	void SNDGIPipelinePanel::StartFullPipeline()
	{
		if (!CanStart())
		{
			return;
		}
		NDGI::Editor::Pipeline::FAutoRunOptions Options;
		Options.BakeSteps = BakeSteps;
		Options.TileSize = TrainTileSize;
		Options.TrainSteps = TrainSteps;
		Options.Overlap = TrainOverlap;
		Options.bValidateAfterTrain = bValidateAfterTrain;
		NDGI::Editor::Pipeline::StartAutoRun(Options);
	}

	FString SNDGIPipelinePanel::BuildLogText() const
	{
		const TArray<FString> Lines = NDGI::Editor::Pipeline::GetLogLines();
		return FString::Join(Lines, TEXT("\n"));
	}
}

void OpenNDGIPipelineTab()
{
	UE_LOG(LogNDGI, Display, TEXT("NDGI Pipeline: opening panel."));
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(PipelineTabName));
}

static TSharedRef<SDockTab> SpawnNDGIPipelineTab(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SBox).MinDesiredWidth(680.0f)
			[
				SNew(SNDGIPipelinePanel)
			]
		];
}

void RegisterNDGIPipelineTab()
{
	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(PipelineTabName, FOnSpawnTab::CreateStatic(&SpawnNDGIPipelineTab))
		.SetDisplayName(LOCTEXT("NDGIPipelineTabTitle", "NDGI Pipeline"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);
}

void UnregisterNDGIPipelineTab()
{
	FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(PipelineTabName);
}

static FAutoConsoleCommand GNDGIPipelineCommand(
	TEXT("ndgi.Pipeline"),
	TEXT("Opens the NDGI Pipeline tab for baking, merging, training, and validation."),
	FConsoleCommandDelegate::CreateStatic(&OpenNDGIPipelineTab));

#undef LOCTEXT_NAMESPACE

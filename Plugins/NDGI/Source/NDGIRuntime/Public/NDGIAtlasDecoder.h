// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NDGILevelAsset.h"
#include "NDGITileLayout.h"
#include "RenderCommandFence.h"

class UTexture2D;
class UNDGIModelAsset;
struct FStreamableHandle;

namespace NDGI
{
	namespace Runtime
	{
		/**
		 * Runtime-only packed layout. The large F_uv/F_uvt feature maps live in
		 * BC7 texture arrays; the structured buffer only carries the MLP and the
		 * small F_ut/F_vt planes.
		 */
		struct FCompactTileLayout
		{
			int32 MLPOffset = 0;
			int32 UTOffset = 0;
			int32 VTOffset = 0;
			int32 TotalParams = 0;
		};

		NDGIRUNTIME_API FCompactTileLayout ComputeCompactTileLayout(const Train::FTileTrainConfig& Config);

		/** One model's BC7 feature layers. */
		struct FModelBC7Features
		{
			TArray<uint8> UVBC7;
			TArray<uint8> UVTBC7;
		};

		/** One atlas ready for GPU decoding. */
		struct FAtlasDecodeBinding
		{
			UTexture2D* TargetTexture = nullptr;
			int32 AtlasWidth = 0;
			/** Full texture height, including both coefficient halves. */
			int32 AtlasHeight = 0;
			/** Core tile size in texels (grid spacing). */
			int32 TileSize = 128;
			/** Per-side model halo; the model extent is TileSize + 2*OverlapTexels. */
			int32 OverlapTexels = 0;
			/** Feature resolutions; 0 = derive from the model extent (legacy behavior). */
			int32 FeatureUVSize = 0;
			int32 FeatureUVTSize = 0;
			int32 FeatureUTSize = 0;
			int32 FeatureVTSize = 0;
			int32 TimeSlices = 8;
			bool bHighQuality = true;

			FVector4f Scale0 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
			FVector4f Add0 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			FVector4f Scale1 = FVector4f(1.0f, 1.0f, 1.0f, 1.0f);
			FVector4f Add1 = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);

			/** Per-texel encoding ranges; empty means use the fields above for every texel. */
			TArray<FNDGIEncodingRange> EncodingRanges;
			TArray<uint8> EncodingRangeIndices; // Width * (AtlasHeight/2)

			FVector4f MeanRGB = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			/** Per-component SH mean for the selected time step (signed). */
			FVector4f MeanSH = FVector4f(0.0f, 0.0f, 0.0f, 0.0f);
			/** True when the model was trained with directional SH (Lightmap1.xyz). */
			bool bDirectionalSH = false;
			float Gamma = 2.2f;

			/** GridX * GridY entries, -1 = no model (empty tile). */
			TArray<int32> TileModelIndices;
			/** Concatenated compact parameters for every model in TileModelIndices. */
			TArray<float> PackedParameters;
			/** BC7 feature layers, parallel to the packed model slots. */
			TArray<FModelBC7Features> ModelFeatures;
			/** BC7 layer sizes (one F_uv layer, one F_uvt time slice). */
			int32 UVBC7BytesPerLayer = 0;
			int32 UVTBC7BytesPerLayer = 0;
		};

		/** Builds the runtime compact (MLP + UT/VT) parameter stream for one model. */
		NDGIRUNTIME_API bool BuildCompactModelParameters(
			const UNDGIModelAsset& Model,
			const Train::FTileTrainConfig& Config,
			TArray<float>& OutParameters);

#if WITH_EDITORONLY_DATA
		/** Editor/debug helper: re-encodes raw 8-bit feature texels into the legacy flat layout. */
		NDGIRUNTIME_API bool BuildModelParameters(
			const UNDGIModelAsset& Model,
			const Train::FTileTrainConfig& Config,
			TArray<float>& OutParameters);
#endif

		struct FNDGIAtlasGPUResources;

		enum class EModelPageState : uint8
		{
			Unloaded,
			Loading,
			Ready,
			Uploaded,
			Failed
		};

		/** One disk-streamed model page and its temporary CPU upload payload. */
		struct FModelPage
		{
			FSoftObjectPath Path;
			TSharedPtr<FStreamableHandle> LoadHandle;
			TArray<uint8> PendingUVBC7;
			TArray<uint8> PendingUVTBC7;
			TArray<float> PendingParameters;
			int32 Slot = -1;
			EModelPageState State = EModelPageState::Unloaded;
		};

		/** One cached atlas: packed CPU parameters, persistent GPU buffers, and per-step means. */
		struct FAtlasDecodeCacheEntry
		{
			FAtlasDecodeBinding Binding;
			TArray<FVector4f> StepMeansRGB;
			TArray<FVector4f> StepMeansSH;
			bool bDirectionalSH = false;
			int32 ModelCount = 0;
			/**
			 * Streaming-level packages that reference this atlas. Empty = legacy /
			 * non-partitioned atlas that is always active.
			 */
			TArray<FString> SourceCells;
			FSoftObjectPath TargetTexturePath;
			/** False when the owning cells are unloaded; GPU buffers and the target texture are released. */
			bool bActive = true;
			TSharedPtr<FNDGIAtlasGPUResources> GPU;

			/** Disk-streamed model pages, parallel to the logical model slots. */
			TArray<FModelPage> Pages;

			/** World bounds parallel to logical model slots; may be empty for legacy assets. */
			TArray<FBox> ModelBounds;
			/** Logical model slots currently uploaded into the physical page pool. */
			TArray<int32> ResidentModels;
			/** Logical model slot -> physical slot, or -1 when evicted. */
			TArray<int32> ModelToResidentSlot;
			/** Physical slot -> logical model, or -1 when free. */
			TArray<int32> ResidentSlotModels;
			/** Physical slot that contains uploaded page data. */
			TArray<uint8> ResidentSlotUploaded;
			/** Compact parameters for every physical slot (capacity * TotalParams). */
			TArray<float> ResidentParameters;
			/** Grid cells -> physical slot, or -1. This is what the decode shader reads. */
			TArray<int32> GPUTileModelIndices;
			/** Number of physical slots allocated for this atlas (0 = all models). */
			int32 ResidentCapacity = 0;
		};

		/** A full level's decode data, built once and reused for every time change. */
		struct FLevelDecodeCache : public TSharedFromThis<FLevelDecodeCache>
		{
			TArray<FAtlasDecodeCacheEntry> Entries;
			int32 Tiles = 0;
			int64 ParameterBytes = 0;
			/** Packed parameter + index bytes held on the game thread. */
			int64 CPUBytes = 0;
			/** Persistent GPU buffer bytes (parameters, model indices, ranges, packed indices). */
			int64 GPUBytes = 0;
			/** GPU bytes currently resident for active atlases. */
			int64 ActiveGPUBytes = 0;
			/** GPU bytes used by uploaded pages only (approximate). */
			int64 ResidentPageBytes = 0;
			int32 ActiveAtlases = 0;
			/** Streaming-level packages currently loaded. */
			TSet<FString> ActiveCells;
			/** Virtual-texture page cache statistics. */
			int32 ResidentModels = 0;
			int32 StreamingUploads = 0;
			int32 StreamingPageUploads = 0;
			int64 StreamingPageUploadBytes = 0;
			int32 StreamingEvictions = 0;
			int32 PageLoadRequests = 0;
			int32 PageLoadCompletions = 0;
			int32 PageLoadFailures = 0;
			int32 InFlightPageLoads = 0;
			int32 LoadedPageHandles = 0;
			int64 StagingBytes = 0;
			FVector LastStreamingCamera = FVector(MAX_flt, MAX_flt, MAX_flt);
			double LastStreamingUpdateSeconds = -MAX_dbl;

			bool IsValid() const { return Entries.Num() > 0; }

			/**
			 * Activates atlases referenced by loaded cells and releases the rest.
			 * Returns true when any atlas changed state.
			 */
			bool UpdateActiveCells(const TSet<FString>& LoadedCells);

			/**
			 * Re-selects the model pages that fit the physical page pool for each
			 * active atlas, nearest to CameraLocation first, and incrementally
			 * uploads only the pages that became resident. Returns true if any
			 * GPU-visible page mapping changed.
			 */
			bool UpdateStreaming(const FVector& CameraLocation);

			/**
			 * Loads and uploads every page synchronously. Used by the blocking
			 * cache-build path (capacity 0) and by tests/validation commands.
			 * Returns false if any page failed to load.
			 */
			bool LoadAllPagesBlocking(int32* OutFailedPages = nullptr);

			/** Releases every page handle and staged payload; optionally resets GPU state. */
			void ReleaseAllPages();

			/** Called by the streamable manager when one page finishes loading. */
			void OnPageLoadComplete(int32 EntryIndex, int32 PageIndex);

			/** Test-only resource identity; 0 when no feature texture exists. */
			uint64 GetFeatureTextureResourceId(int32 EntryIndex) const;
		};

		/** Incremental decode-cache build: pack a few atlases per tick, then upload once. */
		struct FDecodeCacheBuildState
		{
			TSharedPtr<FLevelDecodeCache> Cache;
			TWeakObjectPtr<UNDGILevelAsset> LevelAsset;
			/** Loaded streaming-level packages used to gate atlases during the build. */
			TSet<FString> LoadedCells;
			int32 NextAtlas = 0;
			int32 SkippedModels = 0;
			bool bUploadQueued = false;
			bool bDone = false;
			bool bFailed = false;
			FString Error;
			FRenderCommandFence UploadFence;
			double StartSeconds = 0.0;
		};

		/** CPU-side cost of one DecodeLevelAsset call (GPU work completes later on the render thread). */
		struct FDecodeTimings
		{
			double CpuMs = 0.0;
			int32 Atlases = 0;
			int32 Tiles = 0;
		};

		/**
		 * Packs every model and creates persistent GPU parameter/indices buffers.
		 * Runs on the game thread and blocks until the buffers are ready.
		 */
		NDGIRUNTIME_API bool BuildLevelDecodeCache(
			UNDGILevelAsset* LevelAsset,
			TSharedPtr<FLevelDecodeCache>& OutCache,
			double* OutBuildMs = nullptr,
			int32* OutSkippedModels = nullptr);

		/**
		 * Starts a chunked cache build. Call TickBuildLevelDecodeCacheAsync once per
		 * frame; it packs one atlas per call and uploads the GPU buffers at the end.
		 * Avoids blocking the game thread for the full cache-build time.
		 */
		NDGIRUNTIME_API TSharedPtr<FDecodeCacheBuildState> BeginBuildLevelDecodeCacheAsync(
			UNDGILevelAsset* LevelAsset,
			const TSet<FString>& LoadedCells = TSet<FString>());

		/** Returns true when the build finished; OutCache is only set on success. */
		NDGIRUNTIME_API bool TickBuildLevelDecodeCacheAsync(
			const TSharedPtr<FDecodeCacheBuildState>& Build,
			TSharedPtr<FLevelDecodeCache>& OutCache,
			double* OutBuildMs = nullptr,
			int32* OutSkippedModels = nullptr);

		/**
		 * Decodes every cached atlas in a single render graph. TargetTextures may
		 * override the engine lightmap textures (benchmark/write-path testing); the
		 * array must be parallel to Cache.Entries. Timing covers CPU enqueue only;
		 * the caller can FlushRenderingCommands to include GPU completion.
		 */
		NDGIRUNTIME_API bool DecodeCachedLevel(
			const TSharedPtr<FLevelDecodeCache>& Cache,
			float Time,
			const TArray<UTexture2D*>& TargetTextures,
			FDecodeTimings* OutTimings = nullptr,
			TFunction<void()> OnComplete = nullptr);

		NDGIRUNTIME_API bool DecodeCachedLevel(
			const TSharedPtr<FLevelDecodeCache>& Cache,
			float Time,
			FDecodeTimings* OutTimings = nullptr,
			TFunction<void()> OnComplete = nullptr);

		/**
		 * Decodes one atlas for the given normalized time and writes the log-encoded
		 * RGB into coefficient 0 plus directional SH into coefficient 1 of the
		 * uncompressed HQ lightmap texture. Fire-and-forget; executes on the render thread.
		 */
		NDGIRUNTIME_API bool DecodeAtlas(const FAtlasDecodeBinding& Binding, float Time);

		/** Minimum interval between virtual-texture page-cache updates (ndgi.StreamingUpdateSeconds). */
		NDGIRUNTIME_API float GetStreamingUpdateIntervalSeconds();

		/** Decodes every atlas in a level asset (persistent level only in v1). */
		NDGIRUNTIME_API bool DecodeLevelAsset(UNDGILevelAsset* LevelAsset, float Time, FDecodeTimings* OutTimings = nullptr);
	}
}

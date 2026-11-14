// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGILevelAsset.h"

#if WITH_EDITORONLY_DATA
void UNDGILevelAsset::PostLoad()
{
	Super::PostLoad();

	// Migrate pre-streaming level assets. The old hard references forced the
	// editor to load every model package with the level asset. Move them to
	// soft paths and clear the legacy array so PIE uses the page cache too.
	bool bMigrated = false;
	for (FNDGIAtlasEntry& Entry : Atlases)
	{
		const int32 Count = FMath::Max(Entry.TileModels.Num(), Entry.TileModelPaths.Num());
		if (Entry.TileModelPaths.Num() < Count)
		{
			Entry.TileModelPaths.SetNum(Count);
		}
		for (int32 Cell = 0; Cell < Entry.TileModels.Num(); ++Cell)
		{
			if (!Entry.TileModelPaths.IsValidIndex(Cell) || !Entry.TileModelPaths[Cell].IsNull())
			{
				continue;
			}
			if (Entry.TileModels[Cell])
			{
				Entry.TileModelPaths[Cell] = Entry.TileModels[Cell].Get();
				bMigrated = true;
			}
		}
		if (Entry.TileModels.Num() > 0)
		{
			Entry.TileModels.Reset();
			bMigrated = true;
		}
	}

	if (bMigrated)
	{
		MarkPackageDirty();
	}
}
#endif

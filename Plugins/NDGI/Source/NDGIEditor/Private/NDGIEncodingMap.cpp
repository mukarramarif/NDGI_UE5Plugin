// Copyright Epic Games, Inc. All Rights Reserved.

#include "NDGIEncodingMap.h"

#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/MapBuildDataRegistry.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "LightMap.h"
#include "NDGI.h"
#include "StaticMeshComponentLODInfo.h"
#include "StaticMeshResources.h"
#include "UObject/UObjectIterator.h"

namespace NDGI
{
	namespace Editor
	{
		bool BuildEncodingRangeMap(
			UWorld* World,
			UTexture2D* AtlasTexture,
			int32 Width,
			int32 Height,
			FEncodingRangeMap& OutMap,
			FString* OutError)
		{
			OutMap = FEncodingRangeMap();
			if (!World || !AtlasTexture || Width <= 0 || Height <= 0)
			{
				if (OutError)
				{
					*OutError = TEXT("invalid world, atlas, or atlas size");
				}
				return false;
			}

			OutMap.Indices.Init(255, Width * Height); // 255 = not assigned by any component triangle.
			OutMap.Coverage.Init(0, Width * Height);

			for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
			{
				UStaticMeshComponent* Component = *It;
				if (!Component || Component->GetWorld() != World || Component->Mobility != EComponentMobility::Static || Component->LODData.Num() == 0)
				{
					continue;
				}
				const FMeshMapBuildData* BuildData = Component->GetMeshMapBuildData(Component->LODData[0]);
				if (!BuildData || !BuildData->LightMap)
				{
					continue;
				}
				const FLightMap2D* LightMap2D = BuildData->LightMap->GetLightMap2D();
				if (!LightMap2D || !LightMap2D->IsValid(0) || LightMap2D->GetTexture(0) != AtlasTexture)
				{
					continue;
				}

				if (AActor* Owner = Component->GetOwner())
				{
					if (ULevel* Level = Owner->GetLevel())
					{
						OutMap.SourceCells.AddUnique(UWorld::RemovePIEPrefix(Level->GetOutermost()->GetName()));
					}
				}

				FNDGIEncodingRange Range;
				Range.Scale0 = LightMap2D->ScaleVectors[0];
				Range.Add0 = LightMap2D->AddVectors[0];
				Range.Scale1 = LightMap2D->ScaleVectors[1];
				Range.Add1 = LightMap2D->AddVectors[1];

				int32 RangeIndex = INDEX_NONE;
				for (int32 Existing = 0; Existing < OutMap.Ranges.Num(); ++Existing)
				{
					const FNDGIEncodingRange& Other = OutMap.Ranges[Existing];
					auto Same = [](const FVector4f& A, const FVector4f& B)
					{
						return A.Equals(B, 1e-6f);
					};
					if (Same(Other.Scale0, Range.Scale0) && Same(Other.Add0, Range.Add0)
						&& Same(Other.Scale1, Range.Scale1) && Same(Other.Add1, Range.Add1))
					{
						RangeIndex = Existing;
						break;
					}
				}
				if (RangeIndex == INDEX_NONE)
				{
					RangeIndex = OutMap.Ranges.Add(Range);
				}

				UStaticMesh* Mesh = Component->GetStaticMesh();
				const FStaticMeshRenderData* RenderData = Mesh ? Mesh->GetRenderData() : nullptr;
				if (!RenderData || !RenderData->LODResources.IsValidIndex(0))
				{
					continue;
				}
				const FStaticMeshLODResources& LOD = RenderData->LODResources[0];
				const FStaticMeshVertexBuffer& VertexBuffer = LOD.VertexBuffers.StaticMeshVertexBuffer;
				if (VertexBuffer.GetNumTexCoords() < 2 || VertexBuffer.GetNumVertices() == 0)
				{
					continue;
				}

				const FVector2D CoordScale = LightMap2D->GetCoordinateScale();
				const FVector2D CoordBias = LightMap2D->GetCoordinateBias();
				const FIndexArrayView Indices = LOD.IndexBuffer.GetArrayView();
				const int32 NumIndices = Indices.Num();
				const uint32 NumVertices = VertexBuffer.GetNumVertices();
				if (NumIndices < 3)
				{
					continue;
				}

				const FVector2D Size(static_cast<double>(Width), static_cast<double>(Height));
				auto ToAtlasTexel = [&VertexBuffer, &CoordScale, &CoordBias, &Size](uint32 VertexIndex)
				{
					const FVector2f UV = VertexBuffer.GetVertexUV(VertexIndex, 1);
					return FVector2D(
						static_cast<double>(UV.X) * CoordScale.X + CoordBias.X,
						static_cast<double>(UV.Y) * CoordScale.Y + CoordBias.Y) * Size;
				};
				auto Edge = [](const FVector2D& A, const FVector2D& B, const FVector2D& P)
				{
					return (B.X - A.X) * (P.Y - A.Y) - (B.Y - A.Y) * (P.X - A.X);
				};
				auto AssignTexel = [&OutMap, &RangeIndex, Width](int32 X, int32 Y)
				{
					uint8& Index = OutMap.Indices[Y * Width + X];
					if (Index == 255)
					{
						Index = static_cast<uint8>(RangeIndex);
					}
					else if (Index != RangeIndex)
					{
						++OutMap.Conflicts;
					}
				};

				// Rasterize the lightmap UV triangles instead of using a bounding
				// rect: a rect would also claim padding texels between islands and
				// give them a different component's encoding range.
				for (int32 Triangle = 0; Triangle + 2 < NumIndices; Triangle += 3)
				{
					const uint32 I0 = Indices[Triangle + 0];
					const uint32 I1 = Indices[Triangle + 1];
					const uint32 I2 = Indices[Triangle + 2];
					if (I0 >= NumVertices || I1 >= NumVertices || I2 >= NumVertices)
					{
						continue;
					}

					const FVector2D P0 = ToAtlasTexel(I0);
					const FVector2D P1 = ToAtlasTexel(I1);
					const FVector2D P2 = ToAtlasTexel(I2);
					const double Area2 = Edge(P0, P1, P2);
					if (FMath::Abs(Area2) < 1e-8)
					{
						continue;
					}

					const double Sign = Area2 < 0.0 ? -1.0 : 1.0;
					const double Len0 = FVector2D::Distance(P1, P2);
					const double Len1 = FVector2D::Distance(P2, P0);
					const double Len2 = FVector2D::Distance(P0, P1);
					if (Len0 < 1e-8 || Len1 < 1e-8 || Len2 < 1e-8)
					{
						continue;
					}

					// Cover texels whose centers fall inside the triangle, plus a
					// half-texel gutter. Lightmass dilates islands into that gutter
					// and assigns them the owning component's range.
					const int32 X0 = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.X, P1.X, P2.X)) - 1, 0, Width - 1);
					const int32 X1 = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.X, P1.X, P2.X)) + 1, 0, Width - 1);
					const int32 Y0 = FMath::Clamp(FMath::FloorToInt(FMath::Min3(P0.Y, P1.Y, P2.Y)) - 1, 0, Height - 1);
					const int32 Y1 = FMath::Clamp(FMath::CeilToInt(FMath::Max3(P0.Y, P1.Y, P2.Y)) + 1, 0, Height - 1);
					for (int32 Y = Y0; Y <= Y1; ++Y)
					{
						for (int32 X = X0; X <= X1; ++X)
						{
							const FVector2D P(X + 0.5, Y + 0.5);
							const double D0 = Sign * Edge(P1, P2, P) / Len0;
							const double D1 = Sign * Edge(P2, P0, P) / Len1;
							const double D2 = Sign * Edge(P0, P1, P) / Len2;
							if (D0 >= 0.0 && D1 >= 0.0 && D2 >= 0.0)
							{
								OutMap.Coverage[Y * Width + X] = 1;
							}
							if (D0 >= -0.5 && D1 >= -0.5 && D2 >= -0.5)
							{
								AssignTexel(X, Y);
							}
						}
					}
				}
			}

			// Keep 255 for texels no component triangle owns. Bake/export code falls
			// back to range 0 when decoding targets; the runtime and validation
			// preserve those texels instead of writing decoded padding.
			for (int32 Texel = 0; Texel < OutMap.Indices.Num(); ++Texel)
			{
				if (OutMap.Coverage.IsValidIndex(Texel) && OutMap.Coverage[Texel] != 0)
				{
					++OutMap.Covered;
				}
				if (OutMap.Indices[Texel] != 255)
				{
					++OutMap.RangeCovered;
				}
			}
			OutMap.SourceCells.Sort();

			if (OutMap.Ranges.Num() == 0)
			{
				if (OutError)
				{
					*OutError = TEXT("no components reference this atlas");
				}
				return false;
			}
			return true;
		}
	}
}

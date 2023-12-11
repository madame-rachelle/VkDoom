
#include "templates.h"
#include "doom_levelsubmesh.h"
#include "g_levellocals.h"
#include "texturemanager.h"
#include "playsim/p_lnspec.h"
#include "c_dispatch.h"
#include "g_levellocals.h"
#include "a_dynlight.h"
#include "halffloat.h"
#include "hw_renderstate.h"
#include "hw_vertexbuilder.h"
#include "hwrenderer/scene/hw_drawstructs.h"
#include "hwrenderer/scene/hw_drawinfo.h"
#include "hwrenderer/scene/hw_walldispatcher.h"
#include "hwrenderer/scene/hw_flatdispatcher.h"
#include "common/rendering/hwrenderer/data/hw_meshbuilder.h"

VSMatrix GetPlaneTextureRotationMatrix(FGameTexture* gltexture, const sector_t* sector, int plane);
void GetTexCoordInfo(FGameTexture* tex, FTexCoordInfo* tci, side_t* side, int texpos);

EXTERN_CVAR(Bool, gl_texture)
EXTERN_CVAR(Float, lm_scale);

DoomLevelSubmesh::DoomLevelSubmesh(DoomLevelMesh* mesh, FLevelLocals& doomMap, bool staticMesh) : LevelMesh(mesh), StaticMesh(staticMesh)
{
	LightmapSampleDistance = doomMap.LightmapSampleDistance;
	Reset();

	if (StaticMesh)
	{
		CreateStaticSurfaces(doomMap);
		LinkSurfaces(doomMap);

		CreateIndexes();
		SetupLightmapUvs(doomMap);
		BuildTileSurfaceLists();
		UpdateCollision();
	}
}

void DoomLevelSubmesh::Update(FLevelLocals& doomMap, int lightmapStartIndex)
{
	if (!StaticMesh)
	{
		Reset();

		CreateDynamicSurfaces(doomMap);
		LinkSurfaces(doomMap);

		CreateIndexes();
		SetupLightmapUvs(doomMap);
		BuildTileSurfaceLists();
		UpdateCollision();

		if (doomMap.lightmaps)
			PackLightmapAtlas(lightmapStartIndex);
	}
}

void DoomLevelSubmesh::Reset()
{
	Surfaces.Clear();
	WallPortals.Clear();
	Mesh.Vertices.Clear();
	Mesh.Indexes.Clear();
	Mesh.SurfaceIndexes.Clear();
	Mesh.UniformIndexes.Clear();
	Mesh.Uniforms.Clear();
	Mesh.Materials.Clear();
}

void DoomLevelSubmesh::CreateStaticSurfaces(FLevelLocals& doomMap)
{
	// We can't use side->segs since it is null.
	TArray<std::pair<subsector_t*, seg_t*>> sideSegs(doomMap.sides.Size(), true);
	for (unsigned int i = 0; i < doomMap.subsectors.Size(); i++)
	{
		subsector_t* sub = &doomMap.subsectors[i];
		sector_t* sector = sub->sector;
		for (int i = 0, count = sub->numlines; i < count; i++)
		{
			seg_t* seg = sub->firstline + i;
			if (seg->sidedef)
				sideSegs[seg->sidedef->Index()] = { sub, seg };
		}
	}

	MeshBuilder state;

	// Create surface objects for all visible side parts
	for (unsigned int i = 0; i < doomMap.sides.Size(); i++)
	{
		side_t* side = &doomMap.sides[i];
		bool isPolyLine = !!(side->Flags & WALLF_POLYOBJ);
		if (isPolyLine)
			continue;

		subsector_t* sub = sideSegs[i].first;
		seg_t* seg = sideSegs[i].second;
		if (!seg)
			continue;

		sector_t* front = side->sector;
		sector_t* back = (side->linedef->frontsector == front) ? side->linedef->backsector : side->linedef->frontsector;

		HWMeshHelper result;
		HWWallDispatcher disp(&doomMap, &result, ELightMode::ZDoomSoftware);
		HWWall wall;
		wall.sub = sub;
		wall.Process(&disp, state, seg, front, back);

		// Part 1: solid geometry. This is set up so that there are no transparent parts
		state.SetDepthFunc(DF_LEqual);
		state.ClearDepthBias();
		state.EnableTexture(gl_texture);
		state.EnableBrightmap(true);

		for (HWWall& wallpart : result.list)
		{
			if (wallpart.texture && wallpart.texture->isMasked())
			{
				state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
			}
			else
			{
				state.AlphaFunc(Alpha_GEqual, 0.f);
			}

			wallpart.DrawWall(&disp, state, false);

			int pipelineID = 0;
			int startVertIndex = Mesh.Vertices.Size();
			for (auto& it : state.mSortedLists)
			{
				const MeshApplyState& applyState = it.first;

				pipelineID = screen->GetLevelMeshPipelineID(applyState.applyData, applyState.surfaceUniforms, applyState.material);

				int uniformsIndex = Mesh.Uniforms.Size();
				Mesh.Uniforms.Push(applyState.surfaceUniforms);
				Mesh.Materials.Push(applyState.material);

				for (MeshDrawCommand& command : it.second.mDraws)
				{
					for (int i = command.Start, end = command.Start + command.Count; i < end; i++)
					{
						Mesh.Vertices.Push(state.mVertices[i]);
						Mesh.UniformIndexes.Push(uniformsIndex);
					}
				}
				for (MeshDrawCommand& command : it.second.mIndexedDraws)
				{
					for (int i = command.Start, end = command.Start + command.Count; i < end; i++)
					{
						Mesh.Vertices.Push(state.mVertices[state.mIndexes[i]]);
						Mesh.UniformIndexes.Push(uniformsIndex);
					}
				}
			}
			state.mSortedLists.clear();
			state.mVertices.Clear();
			state.mIndexes.Clear();

			DoomLevelMeshSurface surf;
			surf.Submesh = this;
			surf.Type = wallpart.LevelMeshInfo.Type;
			surf.ControlSector = wallpart.LevelMeshInfo.ControlSector;
			surf.TypeIndex = side->Index();
			surf.Side = side;
			surf.AlwaysUpdate = !!(front->Flags & SECF_LM_DYNAMIC);
			surf.SectorGroup = LevelMesh->sectorGroup[front->Index()];
			surf.Alpha = float(side->linedef->alpha);
			surf.MeshLocation.StartVertIndex = startVertIndex;
			surf.MeshLocation.NumVerts = Mesh.Vertices.Size() - startVertIndex;
			surf.Plane = ToPlane(Mesh.Vertices[startVertIndex + 3].fPos(), Mesh.Vertices[startVertIndex + 2].fPos(), Mesh.Vertices[startVertIndex + 1].fPos(), Mesh.Vertices[startVertIndex].fPos());
			surf.Texture = wallpart.texture;
			surf.PipelineID = pipelineID;
			surf.PortalIndex = (surf.Type == ST_MIDDLESIDE) ? LevelMesh->linePortals[side->linedef->Index()] : 0;
			Surfaces.Push(surf);
		}

		for (const HWWall& portal : result.portals)
		{
			WallPortals.Push(portal);
		}
	}

	// Create surfaces for all flats
	for (unsigned int i = 0; i < doomMap.sectors.Size(); i++)
	{
		sector_t* sector = &doomMap.sectors[i];
		for (FSection& section : doomMap.sections.SectionsForSector(i))
		{
			int sectionIndex = doomMap.sections.SectionIndex(&section);

			HWFlatMeshHelper result;
			HWFlatDispatcher disp(&doomMap, &result, ELightMode::ZDoomSoftware);

			HWFlat flat;
			flat.section = &section;
			flat.ProcessSector(&disp, state, sector);

			// Part 1: solid geometry. This is set up so that there are no transparent parts
			state.SetDepthFunc(DF_LEqual);
			state.ClearDepthBias();
			state.EnableTexture(gl_texture);
			state.EnableBrightmap(true);

			for (HWFlat& flatpart : result.list)
			{
				if (flatpart.texture && flatpart.texture->isMasked())
				{
					state.AlphaFunc(Alpha_GEqual, gl_mask_threshold);
				}
				else
				{
					state.AlphaFunc(Alpha_GEqual, 0.f);
				}

				flatpart.DrawFlat(&disp, state, false);

				int pipelineID = 0;
				int uniformsIndex = 0;
				bool foundDraw = false;
				for (auto& it : state.mSortedLists)
				{
					const MeshApplyState& applyState = it.first;

					pipelineID = screen->GetLevelMeshPipelineID(applyState.applyData, applyState.surfaceUniforms, applyState.material);
					uniformsIndex = Mesh.Uniforms.Size();
					Mesh.Uniforms.Push(applyState.surfaceUniforms);
					Mesh.Materials.Push(applyState.material);

					foundDraw = true;
					break;
				}
				state.mSortedLists.clear();
				state.mVertices.Clear();
				state.mIndexes.Clear();

				if (!foundDraw)
					continue;

				DoomLevelMeshSurface surf;
				surf.Submesh = this;
				surf.Type = flatpart.ceiling ? ST_CEILING : ST_FLOOR;
				surf.ControlSector = flatpart.controlsector ? flatpart.controlsector->model : nullptr;
				surf.AlwaysUpdate = !!(sector->Flags & SECF_LM_DYNAMIC);
				surf.SectorGroup = LevelMesh->sectorGroup[sector->Index()];
				surf.Alpha = flatpart.alpha;
				surf.Texture = flatpart.texture;
				surf.PipelineID = pipelineID;
				surf.PortalIndex = LevelMesh->sectorPortals[flatpart.ceiling][i];

				auto plane = surf.ControlSector ? surf.ControlSector->GetSecPlane(!flatpart.ceiling) : sector->GetSecPlane(flatpart.ceiling);
				surf.Plane = FVector4((float)plane.Normal().X, (float)plane.Normal().Y, (float)plane.Normal().Z, -(float)plane.D);

				if (surf.ControlSector)
					surf.Plane = -surf.Plane;

				for (subsector_t* sub : section.subsectors)
				{
					int startVertIndex = Mesh.Vertices.Size();

					for (int i = 0, end = sub->numlines; i < end; i++)
					{
						auto& vt = sub->firstline[end - 1 - i].v1;

						FFlatVertex ffv;
						ffv.x = (float)vt->fX();
						ffv.y = (float)vt->fY();
						ffv.z = (float)plane.ZatPoint(vt);
						ffv.u = (float)vt->fX() / 64.f;
						ffv.v = -(float)vt->fY() / 64.f;
						ffv.lu = 0.0f;
						ffv.lv = 0.0f;
						ffv.lindex = -1.0f;

						Mesh.Vertices.Push(ffv);
						Mesh.UniformIndexes.Push(uniformsIndex);
					}

					surf.TypeIndex = sub->Index();
					surf.Subsector = sub;
					surf.MeshLocation.StartVertIndex = startVertIndex;
					surf.MeshLocation.NumVerts = sub->numlines;
					Surfaces.Push(surf);
				}
			}
		}
	}
}

void DoomLevelSubmesh::CreateDynamicSurfaces(FLevelLocals& doomMap)
{
#if 0
	// Look for polyobjects
	for (unsigned int i = 0; i < doomMap.lines.Size(); i++)
	{
		side_t* side = doomMap.lines[i].sidedef[0];
		bool isPolyLine = !!(side->Flags & WALLF_POLYOBJ);
		if (!isPolyLine)
			continue;

		// Make sure we have a surface array on the polyobj sidedef
		if (!side->surface)
		{
			auto array = std::make_unique<DoomLevelMeshSurface * []>(4);
			memset(array.get(), 0, sizeof(DoomLevelMeshSurface*));
			side->surface = array.get();
			PolyLMSurfaces.Push(std::move(array));
		}

		CreateSideSurfaces(doomMap, side);
	}
#endif
}

void DoomLevelSubmesh::CreateIndexes()
{
	// Order indexes by pipeline
	std::unordered_map<int64_t, TArray<int>> pipelineSurfaces;
	for (size_t i = 0; i < Surfaces.Size(); i++)
	{
		DoomLevelMeshSurface* s = &Surfaces[i];
		pipelineSurfaces[(int64_t(s->PipelineID) << 32) | int64_t(s->IsSky)].Push(i);
	}

	for (const auto& it : pipelineSurfaces)
	{
		LevelSubmeshDrawRange range;
		range.PipelineID = it.first >> 32;
		range.Start = Mesh.Indexes.Size();
		for (unsigned int i : it.second)
		{
			DoomLevelMeshSurface& s = Surfaces[i];
			int numVerts = s.MeshLocation.NumVerts;
			unsigned int pos = s.MeshLocation.StartVertIndex;
			FFlatVertex* verts = &Mesh.Vertices[pos];

			s.MeshLocation.StartElementIndex = Mesh.Indexes.Size();
			s.MeshLocation.NumElements = 0;

			if (s.Type == ST_CEILING)
			{
				for (int j = 2; j < numVerts; j++)
				{
					if (!IsDegenerate(verts[0].fPos(), verts[j - 1].fPos(), verts[j].fPos()))
					{
						Mesh.Indexes.Push(pos);
						Mesh.Indexes.Push(pos + j - 1);
						Mesh.Indexes.Push(pos + j);
						Mesh.SurfaceIndexes.Push((int)i);
						s.MeshLocation.NumElements += 3;
					}
				}
			}
			else if (s.Type == ST_FLOOR)
			{
				for (int j = 2; j < numVerts; j++)
				{
					if (!IsDegenerate(verts[0].fPos(), verts[j - 1].fPos(), verts[j].fPos()))
					{
						Mesh.Indexes.Push(pos + j);
						Mesh.Indexes.Push(pos + j - 1);
						Mesh.Indexes.Push(pos);
						Mesh.SurfaceIndexes.Push((int)i);
						s.MeshLocation.NumElements += 3;
					}
				}
			}
			else if (s.Type == ST_MIDDLESIDE || s.Type == ST_UPPERSIDE || s.Type == ST_LOWERSIDE)
			{
				if (!IsDegenerate(verts[0].fPos(), verts[2].fPos(), verts[1].fPos()))
				{
					Mesh.Indexes.Push(pos + 0);
					Mesh.Indexes.Push(pos + 1);
					Mesh.Indexes.Push(pos + 2);
					Mesh.SurfaceIndexes.Push((int)i);
					s.MeshLocation.NumElements += 3;
				}
				if (!IsDegenerate(verts[0].fPos(), verts[2].fPos(), verts[3].fPos()))
				{
					Mesh.Indexes.Push(pos + 0);
					Mesh.Indexes.Push(pos + 2);
					Mesh.Indexes.Push(pos + 3);
					Mesh.SurfaceIndexes.Push((int)i);
					s.MeshLocation.NumElements += 3;
				}
			}
		}
		range.Count = Mesh.Indexes.Size() - range.Start;

		if ((it.first & 1) == 0)
			DrawList.Push(range);
		else
			PortalList.Push(range);
	}
}

void DoomLevelSubmesh::LinkSurfaces(FLevelLocals& doomMap)
{
	for (auto& surface : Surfaces)
	{
		if (surface.Type == ST_FLOOR || surface.Type == ST_CEILING)
		{
			SetSubsectorLightmap(&surface);
		}
		else
		{
			SetSideLightmap(&surface);
		}
	}
}

void DoomLevelSubmesh::SetSubsectorLightmap(DoomLevelMeshSurface* surface)
{
	if (surface->Subsector->firstline && surface->Subsector->firstline->sidedef)
		surface->Subsector->firstline->sidedef->sector->HasLightmaps = true;

	if (!surface->ControlSector)
	{
		int index = surface->Type == ST_CEILING ? 1 : 0;
		surface->Subsector->surface[index][0] = surface;
	}
	else
	{
		int index = surface->Type == ST_CEILING ? 0 : 1;
		const auto& ffloors = surface->Subsector->sector->e->XFloor.ffloors;
		for (unsigned int i = 0; i < ffloors.Size(); i++)
		{
			if (ffloors[i]->model == surface->ControlSector)
			{
				surface->Subsector->surface[index][i + 1] = surface;
			}
		}
	}
}

void DoomLevelSubmesh::SetSideLightmap(DoomLevelMeshSurface* surface)
{
	if (!surface->ControlSector)
	{
		if (surface->Type == ST_UPPERSIDE)
		{
			surface->Side->surface[0] = surface;
		}
		else if (surface->Type == ST_MIDDLESIDE)
		{
			surface->Side->surface[1] = surface;
			surface->Side->surface[2] = surface;
		}
		else if (surface->Type == ST_LOWERSIDE)
		{
			surface->Side->surface[3] = surface;
		}
	}
	else
	{
		const auto& ffloors = surface->Side->sector->e->XFloor.ffloors;
		for (unsigned int i = 0; i < ffloors.Size(); i++)
		{
			if (ffloors[i]->model == surface->ControlSector)
			{
				surface->Side->surface[4 + i] = surface;
			}
		}
	}
}

bool DoomLevelSubmesh::IsDegenerate(const FVector3 &v0, const FVector3 &v1, const FVector3 &v2)
{
	// A degenerate triangle has a zero cross product for two of its sides.
	float ax = v1.X - v0.X;
	float ay = v1.Y - v0.Y;
	float az = v1.Z - v0.Z;
	float bx = v2.X - v0.X;
	float by = v2.Y - v0.Y;
	float bz = v2.Z - v0.Z;
	float crossx = ay * bz - az * by;
	float crossy = az * bx - ax * bz;
	float crossz = ax * by - ay * bx;
	float crosslengthsqr = crossx * crossx + crossy * crossy + crossz * crossz;
	return crosslengthsqr <= 1.e-6f;
}

void DoomLevelSubmesh::SetupLightmapUvs(FLevelLocals& doomMap)
{
	LMTextureSize = 1024;

	for (auto& surface : Surfaces)
	{
		SetupTileTransform(LMTextureSize, LMTextureSize, surface);
	}
}

void DoomLevelSubmesh::PackLightmapAtlas(int lightmapStartIndex)
{
	std::vector<LevelMeshSurface*> sortedSurfaces;
	sortedSurfaces.reserve(Surfaces.Size());

	for (auto& surface : Surfaces)
	{
		sortedSurfaces.push_back(&surface);
	}

	std::sort(sortedSurfaces.begin(), sortedSurfaces.end(), [](LevelMeshSurface* a, LevelMeshSurface* b) { return a->AtlasTile.Height != b->AtlasTile.Height ? a->AtlasTile.Height > b->AtlasTile.Height : a->AtlasTile.Width > b->AtlasTile.Width; });

	RectPacker packer(LMTextureSize, LMTextureSize, RectPacker::Spacing(0));

	for (LevelMeshSurface* surf : sortedSurfaces)
	{
		int sampleWidth = surf->AtlasTile.Width;
		int sampleHeight = surf->AtlasTile.Height;

		auto result = packer.insert(sampleWidth, sampleHeight);
		int x = result.pos.x, y = result.pos.y;

		surf->AtlasTile.X = x;
		surf->AtlasTile.Y = y;
		surf->AtlasTile.ArrayIndex = lightmapStartIndex + (int)result.pageIndex;

		// calculate final texture coordinates
		for (int i = 0; i < (int)surf->MeshLocation.NumVerts; i++)
		{
			auto& vertex = Mesh.Vertices[surf->MeshLocation.StartVertIndex + i];
			vertex.lu = (vertex.lu + x) / (float)LMTextureSize;
			vertex.lv = (vertex.lv + y) / (float)LMTextureSize;
			vertex.lindex = (float)surf->AtlasTile.ArrayIndex;
		}
	}

	LMTextureCount = (int)packer.getNumPages();

#if 0 // Debug atlas tile locations:
	uint16_t colors[30] =
	{
		floatToHalf(1.0f), floatToHalf(0.0f), floatToHalf(0.0f),
		floatToHalf(0.0f), floatToHalf(1.0f), floatToHalf(0.0f),
		floatToHalf(1.0f), floatToHalf(1.0f), floatToHalf(0.0f),
		floatToHalf(0.0f), floatToHalf(1.0f), floatToHalf(1.0f),
		floatToHalf(1.0f), floatToHalf(0.0f), floatToHalf(1.0f),
		floatToHalf(0.5f), floatToHalf(0.0f), floatToHalf(0.0f),
		floatToHalf(0.0f), floatToHalf(0.5f), floatToHalf(0.0f),
		floatToHalf(0.5f), floatToHalf(0.5f), floatToHalf(0.0f),
		floatToHalf(0.0f), floatToHalf(0.5f), floatToHalf(0.5f),
		floatToHalf(0.5f), floatToHalf(0.0f), floatToHalf(0.5f)
	};
	LMTextureData.Resize(LMTextureSize * LMTextureSize * LMTextureCount * 3);
	uint16_t* pixels = LMTextureData.Data();
	for (DoomLevelMeshSurface& surf : Surfaces)
	{
		surf.AlwaysUpdate = false;
		surf.NeedsUpdate = false;

		int index = surf.Side ? surf.Side->Index() : (surf.Subsector && surf.Subsector->sector ? surf.Subsector->sector->Index() : 0);
		uint16_t* color = colors + (index % 10) * 3;

		int x = surf.AtlasTile.X;
		int y = surf.AtlasTile.Y;
		int w = surf.AtlasTile.Width;
		int h = surf.AtlasTile.Height;
		for (int yy = y; yy < y + h; yy++)
		{
			uint16_t* line = pixels + surf.AtlasTile.ArrayIndex * LMTextureSize * LMTextureSize + yy * LMTextureSize * 3;
			for (int xx = x; xx < x + w; xx++)
			{
				line[xx * 3] = color[0];
				line[xx * 3 + 1] = color[1];
				line[xx * 3 + 2] = color[2];
			}
		}
	}
#endif
}

BBox DoomLevelSubmesh::GetBoundsFromSurface(const LevelMeshSurface& surface) const
{
	constexpr float M_INFINITY = 1e30f; // TODO cleanup

	FVector3 low(M_INFINITY, M_INFINITY, M_INFINITY);
	FVector3 hi(-M_INFINITY, -M_INFINITY, -M_INFINITY);

	for (int i = int(surface.MeshLocation.StartVertIndex); i < int(surface.MeshLocation.StartVertIndex) + surface.MeshLocation.NumVerts; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			if (Mesh.Vertices[i].fPos()[j] < low[j])
			{
				low[j] = Mesh.Vertices[i].fPos()[j];
			}
			if (Mesh.Vertices[i].fPos()[j] > hi[j])
			{
				hi[j] = Mesh.Vertices[i].fPos()[j];
			}
		}
	}

	BBox bounds;
	bounds.Clear();
	bounds.min = low;
	bounds.max = hi;
	return bounds;
}

DoomLevelSubmesh::PlaneAxis DoomLevelSubmesh::BestAxis(const FVector4& p)
{
	float na = fabs(float(p.X));
	float nb = fabs(float(p.Y));
	float nc = fabs(float(p.Z));

	// figure out what axis the plane lies on
	if (na >= nb && na >= nc)
	{
		return AXIS_YZ;
	}
	else if (nb >= na && nb >= nc)
	{
		return AXIS_XZ;
	}

	return AXIS_XY;
}

void DoomLevelSubmesh::SetupTileTransform(int lightMapTextureWidth, int lightMapTextureHeight, LevelMeshSurface& surface)
{
	BBox bounds = GetBoundsFromSurface(surface);
	surface.Bounds = bounds;

	if (surface.SampleDimension <= 0)
	{
		surface.SampleDimension = LightmapSampleDistance;
	}

	surface.SampleDimension = uint16_t(max(int(roundf(float(surface.SampleDimension) / max(1.0f / 4, float(lm_scale)))), 1));

	{
		// Round to nearest power of two
		uint32_t n = uint16_t(surface.SampleDimension);
		n |= n >> 1;
		n |= n >> 2;
		n |= n >> 4;
		n |= n >> 8;
		n = (n + 1) >> 1;
		surface.SampleDimension = uint16_t(n) ? uint16_t(n) : uint16_t(0xFFFF);
	}

	// round off dimensions
	FVector3 roundedSize;
	for (int i = 0; i < 3; i++)
	{
		bounds.min[i] = surface.SampleDimension * (floor(bounds.min[i] / surface.SampleDimension) - 1);
		bounds.max[i] = surface.SampleDimension * (ceil(bounds.max[i] / surface.SampleDimension) + 1);
		roundedSize[i] = (bounds.max[i] - bounds.min[i]) / surface.SampleDimension;
	}

	FVector3 tCoords[2] = { FVector3(0.0f, 0.0f, 0.0f), FVector3(0.0f, 0.0f, 0.0f) };

	PlaneAxis axis = BestAxis(surface.Plane);

	int width;
	int height;
	switch (axis)
	{
	default:
	case AXIS_YZ:
		width = (int)roundedSize.Y;
		height = (int)roundedSize.Z;
		tCoords[0].Y = 1.0f / surface.SampleDimension;
		tCoords[1].Z = 1.0f / surface.SampleDimension;
		break;

	case AXIS_XZ:
		width = (int)roundedSize.X;
		height = (int)roundedSize.Z;
		tCoords[0].X = 1.0f / surface.SampleDimension;
		tCoords[1].Z = 1.0f / surface.SampleDimension;
		break;

	case AXIS_XY:
		width = (int)roundedSize.X;
		height = (int)roundedSize.Y;
		tCoords[0].X = 1.0f / surface.SampleDimension;
		tCoords[1].Y = 1.0f / surface.SampleDimension;
		break;
	}

	// clamp width
	if (width > lightMapTextureWidth - 2)
	{
		tCoords[0] *= ((float)(lightMapTextureWidth - 2) / (float)width);
		width = (lightMapTextureWidth - 2);
	}

	// clamp height
	if (height > lightMapTextureHeight - 2)
	{
		tCoords[1] *= ((float)(lightMapTextureHeight - 2) / (float)height);
		height = (lightMapTextureHeight - 2);
	}

	surface.TileTransform.TranslateWorldToLocal = bounds.min;
	surface.TileTransform.ProjLocalToU = tCoords[0];
	surface.TileTransform.ProjLocalToV = tCoords[1];

	for (int i = 0; i < surface.MeshLocation.NumVerts; i++)
	{
		FVector3 tDelta = Mesh.Vertices[surface.MeshLocation.StartVertIndex + i].fPos() - surface.TileTransform.TranslateWorldToLocal;

		Mesh.Vertices[surface.MeshLocation.StartVertIndex + i].lu = (tDelta | surface.TileTransform.ProjLocalToU);
		Mesh.Vertices[surface.MeshLocation.StartVertIndex + i].lv = (tDelta | surface.TileTransform.ProjLocalToV);
	}

#if 0
	// project tCoords so they lie on the plane
	const FVector4& plane = surface.plane;
	float d = ((bounds.min | FVector3(plane.X, plane.Y, plane.Z)) - plane.W) / plane[axis]; //d = (plane->PointToDist(bounds.min)) / plane->Normal()[axis];
	for (int i = 0; i < 2; i++)
	{
		tCoords[i].MakeUnit();
		d = (tCoords[i] | FVector3(plane.X, plane.Y, plane.Z)) / plane[axis]; //d = dot(tCoords[i], plane->Normal()) / plane->Normal()[axis];
		tCoords[i][axis] -= d;
	}
#endif

	surface.AtlasTile.Width = width;
	surface.AtlasTile.Height = height;
}

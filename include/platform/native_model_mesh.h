#ifndef NATIVE_MODEL_MESH_H
#define NATIVE_MODEL_MESH_H

#include <macros.h>

// Read-only accessor surface over platform/native_model_mesh.c's per-ModelHeader
// topology scan (rung 0, docs/HANDOFF_s30_pica_native.md). Declarations are
// always visible; every accessor is a zero-returning stub when
// CTR_NATIVE_MODEL_RENDERER is off, so callers don't need their own #ifdef --
// same contract as native_track_mesh.h.
//
// WHAT THIS IS FOR
//
// The direction is to stop converting instanced models (karts, drivers,
// crates, weapons) into PS1 packets and hand the PICA real geometry instead.
// Before any of that is built, this answers what the model population actually
// weighs: how many triangles each LOD rung of each model costs, and which
// headers are static versus animated -- a split that is NOT readable from the
// source, only from level data.
//
// The scan is topology only. A model's command list is frame-invariant: the
// same strip walk, the same vertex order, the same texture per primitive on
// every frame of every animation. Only the vertex POSITIONS change per frame,
// and those are delta-decoded serially at draw time. So the shape of the mesh
// is a load-time property and this is the load-time half of it.

struct ModelHeader;

enum NativeModelScanFlags
{
	// Classification. STATIC means RenderBucket_GetFrame's `mh->ptrAnimations
	// == 0` branch (game/RenderBucket/RenderBucket_QueueExecute.c:1947) -- one
	// fixed frame at ptrFrameData, no animation selection, no delta stream.
	NATIVE_MODEL_SCAN_STATIC = 1 << 0,
	NATIVE_MODEL_SCAN_ANIMATED = 1 << 1,

	// The command list did not reach its 0xffffffff terminator inside the walk
	// cap. Treat this header's counts as a floor, not a measurement.
	NATIVE_MODEL_SCAN_WALK_CAPPED = 1 << 2,

	// No command list at all -- nothing to draw, by either path.
	NATIVE_MODEL_SCAN_NO_COMMANDS = 1 << 3,

	// ptrAnimations and ptrFrameData disagree about which kind of model this
	// is (both null, or both set). Retail's own reader trusts ptrAnimations
	// first, so the scan does too and flags the disagreement rather than
	// guessing -- if this is ever non-zero the static/animated split reported
	// off it is not trustworthy.
	NATIVE_MODEL_SCAN_FRAME_MISMATCH = 1 << 4,

	// More distinct (tpage, clut) pairs than the per-header table holds, so
	// texPages is a floor. texLayouts cannot overflow -- the index is nine
	// bits and is tracked in a full 512-bit set.
	NATIVE_MODEL_SCAN_TEX_OVERFLOW = 1 << 5,

	// The header has primitives with texture indices but no texture table to
	// resolve them through, so texPages could not be computed.
	NATIVE_MODEL_SCAN_NO_TEXTABLE = 1 << 6
};

struct NativeModelHeaderScan
{
	const struct ModelHeader *mh;

	// Vertices the delta/plain stream is walked for, i.e. commands that do NOT
	// carry the 0x04000000 cached-vertex bit. This is exactly the number of
	// ptrDeltaArray entries the header consumes, so it is also the size of the
	// model-space position buffer a PICA-native draw would need per instance.
	u32 verts;

	// Triangles the strip walk can emit. A TOPOLOGY MAXIMUM, not a per-frame
	// count: the retail walk additionally drops primitives that fail
	// projection (backfacing, off-screen, degenerate) or texture lookup, and
	// those rejections are camera-dependent, so they cannot be counted here.
	u32 tris;

	// Strip restarts (command bit 0x80000000). Each costs two commands before
	// its first triangle, so tris + 2*strips ~= drawing commands.
	u32 strips;

	// Commands with a zero high half-word: colour-table updates that emit no
	// geometry (RenderBucket_ApplyColorOnlyCommand).
	u32 colorOnly;

	// Distinct non-zero texture-table indices referenced by emitted triangles.
	//
	// This is NOT a material count and NOT a draw-call floor, which is what it
	// was first taken for. A struct TextureLayout carries its own four UV
	// corners alongside tpage and clut (namespace_Decal.h:120), and model
	// primitives take all three UVs from the layout entry rather than from
	// per-vertex attributes -- so the index identifies a FACE's UV record. A
	// nine-instance frame measured 579 of them; they are closer to faces than
	// to textures.
	u16 texLayouts;

	// Distinct (tpage, clut) pairs behind those entries. THIS is the draw-call
	// floor: it is the same batching key the track atlas already packs by, and
	// the number that says how many state changes one instance of this header
	// would really cost.
	u16 texPages;

	u16 flags; // enum NativeModelScanFlags
};

// Drops every cached scan. Called when the level changes -- ModelHeader
// pointers are into level/library allocations and do not survive a load.
void NativeModelMesh_Reset(void);

// Cached topology scan for one ModelHeader, scanning on first request.
// NULL if the gate is off, if mh is null, or if the cache is full (which is
// reported by NativeModelMesh_Overflowed rather than silently).
const struct NativeModelHeaderScan *NativeModelMesh_Scan(const struct ModelHeader *mh);

// Headers scanned since the last Reset, and 1 if any scan was refused for lack
// of table space (so a report can say "these numbers are partial" out loud).
unsigned NativeModelMesh_ScannedCount(void);
int NativeModelMesh_Overflowed(void);

#endif // NATIVE_MODEL_MESH_H

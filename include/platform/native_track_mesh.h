#ifndef NATIVE_TRACK_MESH_H
#define NATIVE_TRACK_MESH_H

#include <macros.h>

// Read-only accessor surface over platform/native_track_mesh.c's resident
// track mesh (docs/NATIVE_TRACK_RENDERER_DESIGN.md 4.1/4.2/12b). Declarations
// are always visible; every accessor is a no-op/zero-returning stub reader
// when CTR_NATIVE_TRACK_RENDERER is off (the .c file's gate simply never
// populates s_trackMesh), so callers don't need their own #ifdef.
//
// Deliberately narrow: this exposes what a draw side (rung 3a's frame
// recorder, later the 3DS upload/draw path) needs to READ, not the mesh
// builder's internal build-time state (weld table, atlas packer, texture
// intern table). NativeTrackVert itself is exposed -- rung 3a's 3DS upload
// re-packs it into a citro3d attribute layout and needs the real field
// layout to do that, so a void* here would just move the same knowledge into
// an untyped cast at every call site instead of removing it.

// 16 bytes -- see platform/native_track_mesh.c's struct comment for what each
// field means and where it comes from (LevVertex / TextureLayout / QuadBlock
// draw-order bytes). Positions are model-space, straight from the game's own
// vertex pool.
struct NativeTrackVert
{
	s16 x, y, z;
	s8 drawOrder;
	u8 faceFlags; /* bit0 doubleSided; bits 1-3 faceMode/rotation scratch */
	u8 r, g, b, a;
	u8 u, v;
	u16 texRef;
};

CTR_STATIC_ASSERT(sizeof(struct NativeTrackVert) == 16);

// 1 once a level's resident mesh has been built successfully (the same latch
// NativeTrackMesh_Frame uses internally); 0 before the first successful build
// of a level, or if the gate is compiled out.
int NativeTrackMesh_IsBuilt(void);

// 1 if the CURRENTLY latched mesh was built for this exact mesh_info pointer.
// The frame recorder's own bail-out check (a level transition mid-frame must
// never draw the previous level's resident mesh against the new level's
// visibility) -- narrower than IsBuilt, which only says "something is built".
int NativeTrackMesh_IsBuiltFor(const struct mesh_info *mesh);

// Number of resident per-leaf index ranges in the latched mesh (0 if
// unbuilt).
unsigned NativeTrackMesh_LeafCount(void);

// Leaf i's BSP index (the same index space as mesh->bspRoot / visMem1->
// bspList[0], i.e. a visibility-lookup key) plus its index-buffer range.
// Returns 0 and leaves the outputs untouched when i is out of range or
// nothing is built; 1 on success. Callers walk i = 0..LeafCount()-1, they
// never need to search -- see NativeTrackMesh_LeafIndexForBsp for the
// reverse direction.
int NativeTrackMesh_LeafAt(unsigned i, unsigned *outBspIndex, unsigned *outFirstIndex, unsigned *outIndexCount);

// Leaf i's STATIC BSP bounding box (model-space corners, copied from
// mesh->bspRoot[bspIndex].box at BUILD time -- see native_track_mesh.c's
// struct NativeTrackLeafRange comment). Exists so the rung 3a step 2 draw
// side (platform/native_track_frame.c) can compute a leaf's view-z range for
// the depth compose WITHOUT dereferencing a raw mesh_info/BSP pointer on the
// worker thread -- this flat copy is validated/latched by the exact same
// build-or-nothing contract every other accessor here already relies on.
// Returns 0 and leaves the outputs untouched when i is out of range or
// nothing is built; 1 on success.
int NativeTrackMesh_LeafBox(unsigned i, s16 outMin[3], s16 outMax[3]);

// Leaf i's signed per-face drawOrder range (min/max over every face the
// build-time emit loop actually wrote into this leaf -- native_track_mesh.c's
// struct NativeTrackLeafRange comment), copied at BUILD time exactly like
// LeafBox above. Review fix (FIX 1a): the draw side's per-leaf OT-bucket
// depth range used to come from the leaf's bounding box ALONE, which does
// not account for the per-face signed drawOrder byte each vertex's own
// otIndex adds in the shader (ctr_track.v.pica) -- a face whose drawOrder
// pushed it outside the box-derived range would extrapolate the shader's
// lerp past a valid ordinal and get clipped away by the GPU (geometry
// deletion, not mis-layering). Widening the CPU-side range by the leaf's own
// [drawOrderMin, drawOrderMax] before it reaches
// NativeTrackDraw_ComputeLeafDepthRange (platform/native_track_frame.c)
// closes that: every vertex the leaf can ever emit now falls inside the
// range its draw uniform describes. Returns 0 and leaves the outputs
// untouched when i is out of range or nothing is built; 1 on success.
int NativeTrackMesh_LeafDrawOrderRange(unsigned i, s8 *outMin, s8 *outMax);

// Reverse lookup: BSP index -> leaf-range index (into LeafAt), or -1 if that
// BSP index has no resident range (excluded leaf -- water/animated-vertex,
// design 4.4 -- or simply not a leaf). Built once per mesh build, alongside
// the mesh itself, specifically so the per-frame visibility walk (rung 3a's
// frame recorder) is O(visible leaves) rather than O(leaves x ranges): a
// frame's visible-leaf list comes from the game's own render-list traversal
// as BSP indices, and this is how those get turned into leaf-range indices
// without a linear search per frame.
int NativeTrackMesh_LeafIndexForBsp(unsigned bspIndex);

// Raw vertex/index arrays backing every leaf range above, and their counts.
// NULL/0 when nothing is built. u16 indices -- see the .c file's build-time
// guard against exceeding that format's range.
const struct NativeTrackVert *NativeTrackMesh_Verts(void);
unsigned NativeTrackMesh_VertCount(void);
const u16 *NativeTrackMesh_Indices(void);
unsigned NativeTrackMesh_IndexCount(void);

#endif

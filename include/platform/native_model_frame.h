#ifndef NATIVE_MODEL_FRAME_H
#define NATIVE_MODEL_FRAME_H

#include <macros.h>

// Per-frame instance sampler for the s30 model-renderer campaign
// (platform/native_model_frame.c). Declarations always visible; both entry
// points are no-ops when CTR_NATIVE_MODEL_RENDERER is off, so the two call
// sites in game/ stay single unconditional calls inside one gate rather than
// spreading platform #ifdefs through the render path.
//
// The load-time census (platform/native_model_census.c) says what the model
// POPULATION weighs. This says what a FRAME actually draws from it, which is
// the only number the ~9ms instance cost can be checked against. They disagree
// on purpose: most of a level's models are never on screen at once.

struct Instance;
struct ModelHeader;

// Called once per instance that RenderBucket_QueueDraw has committed to
// drawing -- i.e. at the point it returns success, after LOD selection and
// after every early-out. Sampling earlier would count instances the frame
// never draws and overstate the cost, which is the specific way this project
// has produced confident wrong numbers before.
void NativeModelFrame_Sample(const struct Instance *inst, const struct ModelHeader *mh, int lodIndex);

// Called once per frame after all instances are queued. Folds the frame into
// the window and prints [MODELFRAME] when the window closes.
void NativeModelFrame_EndFrame(void);

#endif // NATIVE_MODEL_FRAME_H

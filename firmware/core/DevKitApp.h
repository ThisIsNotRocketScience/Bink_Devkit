#pragma once
// Entry points of the DevKit synth. See DevKitHal.h for what a target has to provide.
#include <stddef.h>
#include <stdint.h>

#define KNOB_NUM 8

// The UI core: runs the whole application, never returns.
int DevKit_Run();
// Back to power-on state, for a target that restarts DevKit_Run without a real reset.
void DevKit_Reset();
// One pass of the audio core's loop, between audio blocks.
void DevKit_WorkerStep();
// One audio block: num_frames stereo frames as I2S words (R0, L0, R1, L1, ...).
void process_audio(const uint32_t *input, uint32_t *output, size_t num_frames);
// The parameter DATA is on, and any parameter's value (0..1) and name.
int DevKit_ActiveParam();
bool DevKit_PopupShown();
float DevKit_ParamValue(int param);
const char *DevKit_ParamLabel(int param);
// Called by the target when hal_render_request() reaches the other core.
void DevKit_RenderOffloaded();

/**
 * @file rkrigmetrics.h
 * @brief The numeric rig for the upscaler and anti-aliasing work (doc/vulkan/PLAN-fsr-specialised.md stage 0)
 */

#pragma once

#include "stdtypes.h"

class LLRenderTarget;

// RK_VK_RIG=auto: the whole rig in one run. The scene and the camera must be still (T-pose, mouselook, the mouse left alone). A frame counts only after the camera and the picture sizes have been unchanged for WARM (300)
// frames: a camera that moves (also on exit) or a render scale that changes drops the burst in progress and starts the wait again. When the render scale is 100 percent the rig builds the reference (the upscaler's input
// averaged over 64 jittered frames, written to RK_VK_RIG_REF, default D:/claude/rig/ref.f32); at every scale it measures the upscaler's final picture in bursts of FRAMES (120) consecutive frames, GAP (600) frames apart, and
// logs ONE summary line per burst (tag RigMetrics): the scale and sizes, the frame to frame change of the luminance (flicker: mean, 99th percentile, the share of pixels that change by more than 4/255, on edges and on flat
// parts), the sharpness (mean gradient) and, against the reference of the same run, the error (rmse, psnr, sharpness ratio). The render scale can be stepped from outside with dlssq=N in D:/claude/rt-debug.txt.
// RK_VK_RIG=auto with RK_VK_RIG_ANIM=1 (instead of the still and sway cases): the own avatar's running animation with its clock made a function of the frame count. Needs rt-debug.txt riganim=1 to take the clock over
// (start the animation first: the window is the next second of it), the render scale 1.0 first (the references), RK_VK_RIG_ANIM_FPS (150: the animation time of one frame) and RK_VK_RIG_ANIM_FRAMES (150: the window).
// Lines "animation reference / still / play": references from 64 jittered frames of each of 4 frozen poses, the still error of each pose at each scale, and the error at the frames where the clock, running forward
// and back over the window, passes the poses: the EXCESS is the cost of the avatar's own motion; at 100 percent a repeat check says whether the clock really repeats the pose.
// RK_VK_RIG=ref: only builds the reference. RK_VK_RIG=1: measures against a reference file. Reads the pictures back (the frames of a burst are slow): measuring only, not for fps.
void rk_rig_metrics(LLRenderTarget& out, LLRenderTarget& in, U32 frame);

// RK_VK_RIG=auto with RK_VK_RIG_SWAY=0.5,2 (peak speeds in output pixels a frame at the rest pose; RK_VK_RIG_SWAY_PERIOD frames, default 240): after each still burst the camera yaws around its rest pose, settles for two periods and
// is measured for four. At every crossing of the rest pose the true picture is the reference, so the error there (and the loss of sharpness) against the still numbers of the same scale is the cost of the motion: ghosting and blur,
// logged as "sway burst" lines with the EXCESS. Called once a frame before the camera's matrices are made (LLPipeline::updateJitter); a camera the viewer sets again every frame is read as the base pose, one it does not is restored first.
void rk_rig_camera_sway(U32 frame);

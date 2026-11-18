/**
 * @file rkreadback.h
 * @brief A read of a rectangle of the current render target that does not make the caller wait (doc/vulkan/PLAN-hitchless-loading.md).
 *
 * gRender.readPixels waits for the render worker to replay everything queued before it, then for the GPU copy: tens of milliseconds of the main thread in a login (the avatar bake's
 * morph masks). rk_read_pixels_async queues the same read behind the work already recorded (the target holds exactly what that work drew) and returns a ticket at once; the worker
 * fills the ticket's data and sets mDone. The caller looks at mDone in a later frame (never waits) and keeps the ticket alive as long as it wants (the worker holds a reference too).
 */

#ifndef RK_RKREADBACK_H
#define RK_RKREADBACK_H

#include "stdtypes.h"
#include "rkrenderformat.h"

#include <atomic>
#include <memory>
#include <vector>

struct RKReadbackTicket
{
    std::atomic<int> mDone{ 0 };        // 1 when mData holds the pixels (a failed read leaves zeros)
    std::vector<U8> mData;              // rows packed with the 4 byte alignment of a GL read, as many bytes as the caller asked for
};

// reads a rectangle of the colour attachment of the render target that is current now; `bytes` is the size of the ticket's data (rows * stride)
std::shared_ptr<RKReadbackTicket> rk_read_pixels_async(S32 x, S32 y, S32 width, S32 height, PixelFormat format, ComponentType type, size_t bytes);

#endif // RK_RKREADBACK_H

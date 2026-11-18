/**
 * @file rkvktexanim.h
 * @brief <FS> Texture animation evaluated by the vertex shader (PLAN-aurora-ideas.md 1.1).
 *
 * A face with a texture animation used to get a new texture matrix from the main thread every time its frame changed (LLVOVolume::animateTextures). With FSTexAnimOnGPU the
 * face's matrix slot (the 16 floats the draw already carries, wherever the draw goes: the alpha runs, the geometry packets, the scene table's moving slots) holds the animation's
 * parameters instead, marked by a 0 in float 15 (a real texture matrix has a 1 there), and the pull vertex shaders turn it into the matrix against the frame clock (slot 0 of the
 * texture matrix table, which no draw reads as a matrix, carries the clock). The CPU writes the parameters when they change, not every frame.
 *
 * Parameter layout, as float indices of the 16 (GLSL m[i / 4][i % 4]):
 *    0 packed bits: mode | sizeX << 8 | sizeY << 16     1 rate     2 start     3 length (0: sizeX * sizeY frames)
 *    4 origin high  5 origin low   (the animation's timer origin in seconds: the clock minus this, times the rate, is the frame counter; a smooth mode's accumulated time is folded in)
 *    6 texture entry offset s  7 offset t  8 scale s  9 scale t  10 rotation   (the parts the animation does not drive come from the texture entry, as animateTextures does)
 *   11 sign of the sine in the closed form (the convention the matrix products were found to follow)    15 = 0
 * The clock is two floats, the whole seconds and the rest, so that the difference to an origin stays exact however long the viewer has run.
 *
 * evaluate() below is the CPU twin of ll_texanim_core in llshadermgr.cpp (line for line): the compare mode of FSTexAnimOnGPU, the faces that bake their matrix into texture
 * coordinates, and the programs that are not pull programs use it.
 */
#ifndef LL_LLVKTEXANIM_H
#define LL_LLVKTEXANIM_H

#include "linden_common.h"
#include <atomic>
#include <cmath>
#include <cstring>

namespace LLVkTexAnim
{
    constexpr U32 ON = 0x01, LOOP = 0x02, REVERSE = 0x04, PING_PONG = 0x08, SMOOTH = 0x10, ROTATE = 0x20, SCALE = 0x40;
    constexpr U32 TRANSLATE_RESULT = 0x01;      // (LLViewerTextureAnim::TRANSLATE)

    inline bool isParams(const F32* m16) { return m16[15] == 0.f; }

    inline std::atomic<U64>& clockWord()
    {
        static std::atomic<U64> word{ 0 };
        return word;
    }

    // the frame time in seconds (LLFrameTimer::getElapsedSeconds()), once a frame from the main thread
    inline void setClock(F64 seconds)
    {
        const F32 hi = (F32)floor(seconds);
        const F32 lo = (F32)(seconds - (F64)hi);
        U32 a, b;
        memcpy(&a, &hi, 4);
        memcpy(&b, &lo, 4);
        clockWord().store(((U64)b << 32) | a, std::memory_order_relaxed);
    }

    inline void getClock(F32& hi, F32& lo)
    {
        const U64 w = clockWord().load(std::memory_order_relaxed);
        const U32 a = (U32)w, b = (U32)(w >> 32);
        memcpy(&hi, &a, 4);
        memcpy(&lo, &b, 4);
    }

    // slot 0 of a texture matrix table: the identity, with the clock in the two floats the identity leaves 0 (t[0][0].yz in the shaders)
    inline void writeSlot0(F32* slot0)
    {
        memset(slot0, 0, 64);
        slot0[0] = slot0[5] = slot0[10] = slot0[15] = 1.f;
        getClock(slot0[1], slot0[2]);
    }

    // the bits LLViewerTextureAnim::animateTextures returns when the frame changed (TRANSLATE | SCALE, ROTATE or SCALE)
    inline U32 resultBits(U32 mode, U32 size_x, U32 size_y)
    {
        if (mode & ROTATE) return ROTATE;
        if (mode & SCALE) return SCALE;
        return TRANSLATE_RESULT | ((size_x && size_y) ? SCALE : 0u);
    }

    struct Source
    {
        U32 mMode, mSizeX, mSizeY;
        F32 mRate, mStart, mLength;
        F64 mOrigin;
        F32 mOffS, mOffT, mScaleS, mScaleT, mRot;
        F32 mSineSign;
    };

    inline void makeParams(F32* p, const Source& s)
    {
        const U32 packed = (s.mMode & 0xFFu) | ((s.mSizeX & 0xFFu) << 8) | ((s.mSizeY & 0xFFu) << 16);
        memcpy(&p[0], &packed, 4);
        p[1] = s.mRate;
        p[2] = s.mStart;
        p[3] = s.mLength;
        const F32 hi = (F32)floor(s.mOrigin);
        p[4] = hi;
        p[5] = (F32)(s.mOrigin - (F64)hi);
        p[6] = s.mOffS;
        p[7] = s.mOffT;
        p[8] = s.mScaleS;
        p[9] = s.mScaleT;
        p[10] = s.mRot;
        p[11] = s.mSineSign;
        p[12] = p[13] = p[14] = 0.f;
        p[15] = 0.f;
    }

    // the matrix (LLMatrix4 memory: row vectors, mMatrix[row][col] = out[row * 4 + col]) for the parameters at the clock
    inline void evaluate(const F32* p, F32 clock_hi, F32 clock_lo, F32* out)
    {
        U32 packed;
        memcpy(&packed, &p[0], 4);
        const U32 mode = packed & 0xFFu;
        const F32 size_x = (F32)((packed >> 8) & 0xFFu), size_y = (F32)((packed >> 16) & 0xFFu);
        const F32 rate = p[1], start = p[2], length = p[3];
        F32 elapsed = (clock_hi - p[4]) + (clock_lo - p[5]);
        elapsed = elapsed < 0.f ? 0.f : elapsed;       // (the clock can be a frame older than an origin set this frame)

        const F32 num_frames = length != 0.f ? length : fmaxf(1.f, size_x * size_y);
        F32 full_length = num_frames;
        if (mode & PING_PONG)
        {
            if (mode & SMOOTH) full_length = 2.f * num_frames;
            else if (mode & LOOP) full_length = fmaxf(1.f, 2.f * num_frames - 2.f);
            else full_length = fmaxf(1.f, 2.f * num_frames - 1.f);
        }

        F32 fc = elapsed * rate;
        if (mode & LOOP) fc = fmodf(fc, full_length);
        else fc = fminf(full_length - 1.f, fc);

        if (!(mode & SMOOTH))
        {
            fc = floorf(fc + 0.01f);
            fc = fminf(full_length - 1.f, fc);
        }
        if (mode & PING_PONG)
        {
            if (fc >= num_frames)
            {
                fc = (mode & SMOOTH) ? num_frames - (fc - num_frames) : (num_frames - 1.99f) - (fc - num_frames);
            }
        }
        if (mode & REVERSE)
        {
            fc = (mode & SMOOTH) ? num_frames - fc : (num_frames - 0.99f) - fc;
        }
        fc += start;
        if (!(mode & SMOOTH)) fc = floorf(fc + 0.5f);

        F32 off_s = p[6], off_t = p[7], scale_s = p[8], scale_t = p[9], rot = p[10];
        if (mode & ROTATE)
        {
            rot = fc;
        }
        else if (mode & SCALE)
        {
            scale_s = scale_t = fc;
        }
        else if (size_x != 0.f && size_y != 0.f)
        {
            scale_s = 1.f / size_x;
            scale_t = 1.f / size_y;
            const F32 x_frame = fmodf(fc, size_x);
            const F32 y_frame = (F32)(S32)(fc / size_x);
            off_s = (-0.5f + 0.5f * scale_s) + x_frame * scale_s;
            off_t = (0.5f - 0.5f * scale_t) - y_frame * scale_t;
        }
        else
        {
            off_s = (-0.5f + 0.5f * 1.f) + fc;
            off_t = 0.f;
        }

        const F32 c = cosf(rot), s = sinf(rot) * p[11];
        const F32 m00 = c, m01 = -s, m10 = s, m11 = c;
        memset(out, 0, 64);
        out[0] = m00 * scale_s;
        out[1] = m01 * scale_t;
        out[4] = m10 * scale_s;
        out[5] = m11 * scale_t;
        out[10] = 1.f;
        out[12] = -0.5f * (m00 + m10) * scale_s + off_s + 0.5f;
        out[13] = -0.5f * (m01 + m11) * scale_t + off_t + 0.5f;
        out[15] = 1.f;
    }

    inline void evaluateNow(const F32* p, F32* out)
    {
        F32 hi, lo;
        getClock(hi, lo);
        evaluate(p, hi, lo, out);
    }
}

#endif

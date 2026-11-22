/**
 * @file rkrigmetrics.cpp
 * @brief The numeric rig for the upscaler and anti-aliasing work (doc/vulkan/PLAN-fsr-specialised.md stage 0)
 */

#include "llviewerprecompiledheaders.h"

#include "rkrigmetrics.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <vector>

#include <chrono>

#include "llframetimer.h"
#include "llmotioncontroller.h"
#include "llrender.h"
#include "llrendertarget.h"
#include "llviewercamera.h"
#include "llviewerwindow.h"

extern bool gSnapshot;
extern bool gCubeSnapshot;
F32 rk_fsr_sharp_now();         // pipeline.cpp: the strength of FSR's RCAS sharpening asked for in rt-debug.txt (fsrsharp=), 0 = off

namespace
{

struct RigState
{
    bool mInit = false;
    int mMode = 0;                              // 0 off, 1 measure (a reference from the file), 2 build the reference and write it, 3 auto: build the reference in this run when the scale is 100 percent, measure at every scale
    U32 mWarm = 300, mFrames = 120, mGap = 600;
    std::string mRefPath = "D:/claude/rig/ref.f32";

    // the camera and the picture sizes: a frame only counts after both have been unchanged for mWarm frames
    F32 mCamPrev[6] = {};
    bool mCamHave = false;
    U32 mStable = 0;
    S32 mKey[5] = {};                           // the input and output sizes and the RCAS strength (thousandths): a change of any of them restarts the wait
    U32 mKeyFrames = 0;

    // the reference
    bool mFileTried = false;
    bool mRefBuilt = false;
    S32 mRefW = 0, mRefH = 0;
    std::vector<F32> mRef;
    F32 mRefCam[6] = {};
    F64 mRefSharp = 0.0;
    std::vector<F64> mAcc;
    U32 mAccCount = 0;

    // the burst in progress
    bool mBursting = false;
    U32 mGapLeft = 0;
    std::vector<F32> mPrev;                     // the last frame's luminance of the output
    U32 mPrevFrame = ~0u;
    U32 mBurstStart = 0;
    U32 mCount = 0, mRefFrames = 0;
    F64 mFlk = 0, mFlk2 = 0, mP99 = 0, mFrac = 0, mEdge = 0, mFlat = 0, mSharp = 0, mRmse = 0;
    U32 mBursts = 0;
    F64 mStaticRmse = -1.0, mStaticSharp = -1.0;    // the last comparable still burst's numbers at this scale, the base of the sway's excess
};

// the sway: a yaw of the camera around its rest pose, sin(2 pi (frame - start) / period), whose peak speed (at the rest pose) is given in output pixels a frame
struct SwayState
{
    std::vector<F32> mSpeeds;                   // RK_VK_RIG_SWAY, e.g. "0.5,2"
    U32 mPeriod = 240;                          // RK_VK_RIG_SWAY_PERIOD, even
    bool mHaveBase = false;
    LLCoordFrame mBase, mLastSet;               // the camera the viewer set, and the one this left behind
    bool mActive = false;
    U32 mStart = 0;
    F32 mTheta0 = 0.f;
    // the sway phase of the rig: 0 none (still bursts), 1 settling, 2 measuring
    int mPhase = 0;
    U32 mPhaseFrames = 0;
    size_t mSpeedIdx = 0;
    U32 mCrossings = 0;
    F64 mRmse = 0, mRmse2 = 0, mSharp = 0;
    U32 mBurstsDone = 0;
};

// the animation case (RK_VK_RIG_ANIM=1): the own avatar's animation with its clock made a function of the frame count (LLMotionController::sAnimTimeHook), so a pose repeats exactly. The clock is frozen at
// kPoses poses of a window of the running animation (the references are built from 64 jittered frames of each frozen pose, the still error is measured on each), then runs forward over the window and back (a period of
// 2 M frames, no jump) while the output is compared with the same references at the frames where it passes the poses.
constexpr int kPoses = 4;

struct AnimState
{
    bool mEnabled = false;
    F32 mDt = 1.f / 150.f;                      // the animation time one frame advances (RK_VK_RIG_ANIM_FPS)
    U32 mM = 150;                               // the window in frames, one way (RK_VK_RIG_ANIM_FRAMES)
    U32 mPoseX[kPoses] = {};                    // the poses, in frames into the window
    bool mEngaged = false;                      // rt-debug.txt riganim=1: the rig takes the clock over (the animation must be running)
    bool mHaveBase = false;
    F32 mBase = 0.f;
    int mClock = 0;                             // 0 natural, 1 frozen at mFreezeX, 2 playing
    U32 mFreezeX = 0, mPlayStart = 0;
    int mStage = 0;                             // 0 waiting, 1 reference, 2 still, 3 play warm-up, 4 play measuring
    int mPose = 0;
    U32 mStageFrames = 0, mSamples = 0;
    bool mRefBuilt = false;
    bool mTold = false, mToldWait = false;
    std::vector<F32> mRef[kPoses];
    F64 mFloor[kPoses] = {}, mRefSharp[kPoses] = {};
    std::vector<F64> mSum, mSumSq;
    F64 mAccRmse = 0.0, mAccSharp = 0.0;
    F64 mStillRmse[kPoses] = {}, mStillSharp[kPoses] = {};
    U32 mPlayN[2][kPoses] = {};
    F64 mPlayRmse[2][kPoses] = {}, mPlaySharp = 0.0;
    U32 mPlaySharpN = 0;
    F64 mInRmse[kPoses] = {};
    U32 mInN[kPoses] = {};
};

bool anim_time_hook(LLMotionController* mc, F32& t);

RigState sRig;
SwayState sSway;
AnimState sAnim;

void init()
{
    if (sRig.mInit)
    {
        return;
    }
    sRig.mInit = true;
    if (const char* m = getenv("RK_VK_RIG"))
    {
        const std::string s = m;
        sRig.mMode = s == "ref" ? 2 : (s == "auto" ? 3 : (atoi(m) != 0 ? 1 : 0));
    }
    if (const char* v = getenv("RK_VK_RIG_WARM")) sRig.mWarm = (U32)std::max(1, atoi(v));
    if (const char* v = getenv("RK_VK_RIG_FRAMES")) sRig.mFrames = (U32)std::max(3, atoi(v));
    if (const char* v = getenv("RK_VK_RIG_GAP")) sRig.mGap = (U32)std::max(0, atoi(v));
    if (const char* v = getenv("RK_VK_RIG_REF")) sRig.mRefPath = v;
    if (const char* v = getenv("RK_VK_RIG_ANIM"))
    {
        sAnim.mEnabled = atoi(v) != 0;
        if (const char* f = getenv("RK_VK_RIG_ANIM_FPS")) sAnim.mDt = 1.f / (F32)std::max(10, atoi(f));
        if (const char* f = getenv("RK_VK_RIG_ANIM_FRAMES")) sAnim.mM = (U32)std::max(40, atoi(f));
        for (int i = 0; i < kPoses; ++i)
        {
            sAnim.mPoseX[i] = (U32)(2 * i + 1) * sAnim.mM / (U32)(2 * kPoses);
        }
        if (sAnim.mEnabled)
        {
            LLMotionController::sAnimTimeHook = &anim_time_hook;
        }
    }
    if (const char* v = getenv("RK_VK_RIG_SWAY_PERIOD")) sSway.mPeriod = (U32)std::max(8, atoi(v)) & ~1u;
    if (const char* v = getenv("RK_VK_RIG_SWAY"))
    {
        std::stringstream ss(v);
        std::string tok;
        while (std::getline(ss, tok, ','))
        {
            const F32 s = (F32)atof(tok.c_str());
            if (s > 0.f && sSway.mSpeeds.size() < 4)
            {
                sSway.mSpeeds.push_back(s);
            }
        }
    }
}

bool sway_enabled()
{
    return sRig.mMode == 3 && !sSway.mSpeeds.empty();
}

// luminance 0..1 of an RGBA8 picture
void luminance(const std::vector<U8>& rgba, size_t count, std::vector<F32>& out)
{
    out.resize(count);
    for (size_t i = 0; i < count; ++i)
    {
        const U8* p = &rgba[i * 4];
        out[i] = (0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2]) * (1.f / 255.f);
    }
}

F64 sharpness(const std::vector<F32>& y, S32 w, S32 h)
{
    F64 sum = 0.0;
    U64 n = 0;
    for (S32 j = 1; j < h - 1; ++j)
    {
        for (S32 i = 1; i < w - 1; ++i)
        {
            const size_t k = (size_t)j * w + i;
            sum += 0.5 * (fabsf(y[k + 1] - y[k - 1]) + fabsf(y[k + w] - y[k - w]));
            ++n;
        }
    }
    return n ? sum / (F64)n : 0.0;
}

// the camera: with the sway on its base pose (what the viewer set), else what it is
void cam_now(F32* c)
{
    LLVector3 o, a;
    if (sway_enabled() && sSway.mHaveBase)
    {
        o = sSway.mBase.getOrigin();
        a = sSway.mBase.getAtAxis();
    }
    else
    {
        LLViewerCamera* cam = LLViewerCamera::getInstance();
        o = cam->getOrigin();
        a = cam->getAtAxis();
    }
    c[0] = o.mV[0]; c[1] = o.mV[1]; c[2] = o.mV[2];
    c[3] = a.mV[0]; c[4] = a.mV[1]; c[5] = a.mV[2];
}

void cam_diff(const F32* a, const F32* b, F64& dp, F64& da)
{
    dp = da = 0.0;
    for (int k = 0; k < 3; ++k) dp += fabs(a[k] - b[k]);
    for (int k = 3; k < 6; ++k) da += fabs(a[k] - b[k]);
}

bool same_axes(const LLCoordFrame& a, const LLCoordFrame& b)
{
    return (a.getAtAxis() - b.getAtAxis()).magVecSquared() < 1e-12f && (a.getLeftAxis() - b.getLeftAxis()).magVecSquared() < 1e-12f
        && (a.getUpAxis() - b.getUpAxis()).magVecSquared() < 1e-12f && (a.getOrigin() - b.getOrigin()).magVecSquared() < 1e-12f;
}

void load_reference(S32 w, S32 h)
{
    sRig.mFileTried = true;
    std::ifstream in(sRig.mRefPath, std::ios::binary);
    if (!in)
    {
        return;
    }
    char magic[4] = {};
    S32 rw = 0, rh = 0;
    in.read(magic, 4);
    in.read((char*)&rw, 4);
    in.read((char*)&rh, 4);
    in.read((char*)sRig.mRefCam, sizeof(sRig.mRefCam));
    if (!in || memcmp(magic, "RIGR", 4) != 0 || rw != w || rh != h)
    {
        return;
    }
    sRig.mRef.resize((size_t)w * h);
    in.read((char*)sRig.mRef.data(), (std::streamsize)sRig.mRef.size() * 4);
    if (!in)
    {
        sRig.mRef.clear();
        return;
    }
    sRig.mRefW = w;
    sRig.mRefH = h;
    sRig.mRefSharp = sharpness(sRig.mRef, w, h);
    sRig.mRefBuilt = true;
    LL_INFOS("RigMetrics") << "reference " << sRig.mRefPath << " loaded from the file, " << w << "x" << h << ", sharpness " << sRig.mRefSharp << LL_ENDL;
}

void reset_burst()
{
    sRig.mBursting = false;
    sRig.mCount = 0;
    sRig.mRefFrames = 0;
    sRig.mFlk = sRig.mFlk2 = sRig.mP99 = sRig.mFrac = sRig.mEdge = sRig.mFlat = sRig.mSharp = sRig.mRmse = 0.0;
    sRig.mPrevFrame = ~0u;
}

void reset_sway()
{
    sSway.mActive = false;
    sSway.mPhase = 0;
    sSway.mPhaseFrames = 0;
    sSway.mCrossings = 0;
    sSway.mRmse = sSway.mRmse2 = sSway.mSharp = 0.0;
}

bool camera_is_reference(F64& dp, F64& da)
{
    F32 cam[6];
    cam_now(cam);
    cam_diff(cam, sRig.mRefCam, dp, da);
    return dp <= 0.01 && da <= 0.002;
}

void end_burst(const S32* key, U32 frame)
{
    if (sRig.mCount >= 2)
    {
        const F64 n = (F64)sRig.mCount;
        const F64 flk = sRig.mFlk / n, flk_sd = sqrt(std::max(0.0, sRig.mFlk2 / n - flk * flk));
        std::ostringstream o;
        o << std::fixed << std::setprecision(5) << "burst " << ++sRig.mBursts << " at frame " << sRig.mBurstStart << ".." << frame << " (" << sRig.mCount << " frames) scale " << std::setprecision(3) << (F64)key[0] / (F64)key[2]
          << " input " << key[0] << "x" << key[1] << " output " << key[2] << "x" << key[3] << " rcas " << std::setprecision(2) << key[4] / 1000.0 << std::setprecision(5) << ": flicker mean " << flk << " sd " << flk_sd << " p99 " << sRig.mP99 / n << " changed>4/255 " << sRig.mFrac / n
          << " edge " << sRig.mEdge / n << " flat " << sRig.mFlat / n << " | sharpness " << sRig.mSharp / n;
        if (!sRig.mRefBuilt)
        {
            o << " | no reference yet";
        }
        else if (sRig.mRefFrames != sRig.mCount)
        {
            o << " | reference NOT comparable: the output size differs from the reference's " << sRig.mRefW << "x" << sRig.mRefH;
        }
        else
        {
            F64 dp, da;
            if (!camera_is_reference(dp, da))
            {
                o << " | reference NOT comparable: the camera is not the reference's (position " << dp << " m, axis " << da << ")";
            }
            else
            {
                const F64 rmse = sRig.mRmse / n;
                sRig.mStaticRmse = rmse;
                sRig.mStaticSharp = sRig.mSharp / n;
                o << " | vs reference: rmse " << rmse << " psnr " << (rmse > 1e-9 ? -20.0 * log10(rmse) : 99.0) << " sharpness ratio " << (sRig.mRefSharp > 0 ? sRig.mSharp / n / sRig.mRefSharp : 0.0);
            }
        }
        LL_INFOS("RigMetrics") << o.str() << LL_ENDL;
    }
    reset_burst();
    sRig.mGapLeft = sRig.mGap;
}

void finish_reference(S32 w, S32 h)
{
    std::vector<F32> avg(sRig.mAcc.size());
    for (size_t i = 0; i < avg.size(); ++i)
    {
        avg[i] = (F32)(sRig.mAcc[i] / sRig.mAccCount);
    }
    cam_now(sRig.mRefCam);
    sRig.mRef.swap(avg);
    sRig.mRefW = w;
    sRig.mRefH = h;
    sRig.mRefSharp = sharpness(sRig.mRef, w, h);
    sRig.mRefBuilt = true;
    std::ofstream of(sRig.mRefPath, std::ios::binary | std::ios::trunc);
    of.write("RIGR", 4);
    of.write((const char*)&w, 4);
    of.write((const char*)&h, 4);
    of.write((const char*)sRig.mRefCam, sizeof(sRig.mRefCam));
    of.write((const char*)sRig.mRef.data(), (std::streamsize)sRig.mRef.size() * 4);
    of.close();
    LL_INFOS("RigMetrics") << "reference built in this run (" << w << "x" << h << ", " << sRig.mAccCount << " frames, sharpness " << sRig.mRefSharp << ", camera " << sRig.mRefCam[0] << "," << sRig.mRefCam[1] << "," << sRig.mRefCam[2]
                           << ") and written to " << sRig.mRefPath << LL_ENDL;
    sRig.mAcc.clear();
    sRig.mAccCount = 0;
}

// the sway's angle for this frame, from the speed it is asked for (output pixels a frame at the rest pose)
void start_sway(U32 frame, S32 out_h)
{
    const F32 speed = sSway.mSpeeds[sSway.mSpeedIdx];
    const F32 fov = LLViewerCamera::getInstance()->getView();                       // vertical, radians
    const F32 focal_px = 0.5f * (F32)out_h / tanf(0.5f * fov);
    sSway.mTheta0 = speed * (F32)sSway.mPeriod / (2.f * (F32)F_PI * focal_px);
    sSway.mStart = frame + 1;
    sSway.mActive = true;
    sSway.mPhase = 1;
    sSway.mPhaseFrames = 0;
    sSway.mCrossings = 0;
    sSway.mRmse = sSway.mRmse2 = sSway.mSharp = 0.0;
}

// ---- the animation case ----

U32 clock_x(U32 frame)
{
    if (sAnim.mClock == 1)
    {
        return sAnim.mFreezeX;
    }
    const U32 period = 2 * sAnim.mM;
    const U32 ph = (frame - sAnim.mPlayStart) % period;
    return ph < sAnim.mM ? ph : period - ph;
}

bool anim_time_hook(LLMotionController* mc, F32& t)
{
    if (!sAnim.mEnabled || !sAnim.mEngaged || !mc->mIsSelf || sAnim.mClock == 0)
    {
        return false;
    }
    if (!sAnim.mHaveBase)
    {
        sAnim.mBase = mc->getAnimTime();
        sAnim.mHaveBase = true;
        LL_INFOS("RigMetrics") << "animation clock taken over: base time " << sAnim.mBase << " s, a window of " << sAnim.mM << " frames of " << sAnim.mDt * 1000.f << " ms, period " << 2 * sAnim.mM << " frames" << LL_ENDL;
    }
    t = sAnim.mBase + (F32)clock_x(LLFrameTimer::getFrameCount()) * sAnim.mDt;
    return true;
}

void poll_anim_key()
{
    static std::chrono::steady_clock::time_point last;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::milliseconds(500))
    {
        return;
    }
    last = now;
    bool on = false;
    std::ifstream in("D:/claude/rt-debug.txt");
    std::string line;
    while (std::getline(in, line))
    {
        if (line.compare(0, 8, "riganim=") == 0)
        {
            on = atoi(line.c_str() + 8) != 0;
        }
    }
    sAnim.mEngaged = on;
}

void read_lum(LLRenderTarget& t, S32 w, S32 h, std::vector<F32>& y)
{
    std::vector<U8> rgba((size_t)w * h * 4);
    {
        RenderPassScope pass(t);
        gRender.readPixels(0, 0, w, h, PixelFormat::RGBA, ComponentType::U8, rgba.data());
    }
    luminance(rgba, (size_t)w * h, y);
}

F64 rmse_vs(const std::vector<F32>& a, const std::vector<F32>& b)
{
    if (a.size() != b.size() || a.empty())
    {
        return -1.0;
    }
    F64 se = 0.0;
    for (size_t k = 0; k < a.size(); ++k)
    {
        const F64 e = (F64)a[k] - (F64)b[k];
        se += e * e;
    }
    return sqrt(se / (F64)a.size());
}

void anim_reset()
{
    sAnim.mStage = 0;
    sAnim.mClock = 0;
    sAnim.mPose = 0;
    sAnim.mStageFrames = 0;
    sAnim.mSamples = 0;
    sAnim.mSum.clear();
    sAnim.mSumSq.clear();
}

void anim_begin_freeze(int pose)
{
    sAnim.mPose = pose;
    sAnim.mClock = 1;
    sAnim.mFreezeX = sAnim.mPoseX[pose];
    sAnim.mStageFrames = 0;
    sAnim.mSamples = 0;
    sAnim.mAccRmse = sAnim.mAccSharp = 0.0;
    sAnim.mSum.clear();
    sAnim.mSumSq.clear();
}

void anim_step(LLRenderTarget& out, LLRenderTarget& in, U32 frame, S32 w, S32 h, const S32* key)
{
    poll_anim_key();
    if (!sAnim.mEngaged)
    {
        anim_reset();
        return;
    }
    const U32 kWarm = 150, kRefFrames = 64, kStillFrames = 40, kPlayPeriods = 6;
    const U32 period = 2 * sAnim.mM;
    const F64 scale = (F64)key[0] / (F64)key[2];
    const bool native = key[0] == w && key[1] == h;
    std::vector<F32> y;

    if (sAnim.mStage == 0)
    {
        if (!sAnim.mRefBuilt)
        {
            if (!native)
            {
                if (!sAnim.mToldWait)
                {
                    sAnim.mToldWait = true;
                    LL_INFOS("RigMetrics") << "animation case: waiting for the render scale 1.0 to build the references (the input is " << key[0] << "x" << key[1] << ", the output " << w << "x" << h << ")" << LL_ENDL;
                }
                return;
            }
            sAnim.mStage = 1;
            anim_begin_freeze(0);
        }
        else
        {
            sAnim.mStage = 2;
            anim_begin_freeze(0);
        }
        return;
    }

    if (sAnim.mStage == 1)
    {
        // the reference of a pose: 64 jittered frames of the 100 percent input at the frozen clock
        if (!native)
        {
            anim_reset();
            return;
        }
        // (the first reference needs longer: after a render scale change the exposure and the history are still settling, and pose 0's jitter floor was 4 times the others' on 2026-11-22 with 150 frames)
        if (++sAnim.mStageFrames <= (sAnim.mPose == 0 ? 3 * kWarm : kWarm))
        {
            return;
        }
        read_lum(in, w, h, y);
        if (sAnim.mSum.empty())
        {
            sAnim.mSum.assign(y.size(), 0.0);
            sAnim.mSumSq.assign(y.size(), 0.0);
        }
        for (size_t k = 0; k < y.size(); ++k)
        {
            sAnim.mSum[k] += y[k];
            sAnim.mSumSq[k] += (F64)y[k] * y[k];
        }
        if (++sAnim.mSamples >= kRefFrames)
        {
            const int p = sAnim.mPose;
            const F64 n = (F64)sAnim.mSamples;
            std::vector<F32>& ref = sAnim.mRef[p];
            ref.resize(sAnim.mSum.size());
            F64 var_sum = 0.0;
            for (size_t k = 0; k < ref.size(); ++k)
            {
                const F64 mean = sAnim.mSum[k] / n;
                ref[k] = (F32)mean;
                var_sum += std::max(0.0, sAnim.mSumSq[k] / n - mean * mean);
            }
            sAnim.mFloor[p] = sqrt(var_sum / (F64)ref.size());
            sAnim.mRefSharp[p] = sharpness(ref, w, h);
            LL_INFOS("RigMetrics") << "animation reference pose " << p << " (frame " << sAnim.mPoseX[p] << " of the window): " << sAnim.mSamples << " frames averaged, jitter floor rmse " << sAnim.mFloor[p] << " sharpness " << sAnim.mRefSharp[p] << LL_ENDL;
            if (p + 1 < kPoses)
            {
                anim_begin_freeze(p + 1);
            }
            else
            {
                std::ostringstream o;
                o << std::fixed << std::setprecision(5) << "animation references built; rmse between the poses (is anything moving?):";
                for (int a = 0; a < kPoses; ++a)
                {
                    for (int b = a + 1; b < kPoses; ++b)
                    {
                        o << " " << a << "-" << b << " " << rmse_vs(sAnim.mRef[a], sAnim.mRef[b]);
                    }
                }
                LL_INFOS("RigMetrics") << o.str() << LL_ENDL;
                sAnim.mRefBuilt = true;
                sAnim.mStage = 2;
                anim_begin_freeze(0);
            }
        }
        return;
    }

    if (sAnim.mStage == 2)
    {
        // the still error of a pose at this scale
        if (++sAnim.mStageFrames <= kWarm)
        {
            return;
        }
        read_lum(out, w, h, y);
        sAnim.mAccRmse += rmse_vs(y, sAnim.mRef[sAnim.mPose]);
        sAnim.mAccSharp += sharpness(y, w, h);
        if (++sAnim.mSamples >= kStillFrames)
        {
            const int p = sAnim.mPose;
            sAnim.mStillRmse[p] = sAnim.mAccRmse / (F64)sAnim.mSamples;
            sAnim.mStillSharp[p] = sAnim.mAccSharp / (F64)sAnim.mSamples;
            if (p + 1 < kPoses)
            {
                anim_begin_freeze(p + 1);
            }
            else
            {
                std::ostringstream o;
                F64 mr = 0.0, ms = 0.0;
                o << std::fixed << std::setprecision(5) << "animation still, scale " << std::setprecision(3) << scale << " rcas " << std::setprecision(2) << key[4] / 1000.0 << std::setprecision(5) << ": rmse per pose";
                for (int i = 0; i < kPoses; ++i)
                {
                    o << " " << sAnim.mStillRmse[i];
                    mr += sAnim.mStillRmse[i];
                    ms += sAnim.mStillSharp[i];
                }
                o << " | mean rmse " << mr / kPoses << " sharpness " << ms / kPoses;
                LL_INFOS("RigMetrics") << o.str() << LL_ENDL;
                sAnim.mStage = 3;
                sAnim.mClock = 2;
                sAnim.mPlayStart = frame + 1;
                sAnim.mStageFrames = 0;
            }
        }
        return;
    }

    if (sAnim.mStage == 3)
    {
        if (++sAnim.mStageFrames >= 2 * period)
        {
            sAnim.mStage = 4;
            sAnim.mStageFrames = 0;
            memset(sAnim.mPlayN, 0, sizeof(sAnim.mPlayN));
            memset(sAnim.mPlayRmse, 0, sizeof(sAnim.mPlayRmse));
            memset(sAnim.mInN, 0, sizeof(sAnim.mInN));
            memset(sAnim.mInRmse, 0, sizeof(sAnim.mInRmse));
            sAnim.mPlaySharp = 0.0;
            sAnim.mPlaySharpN = 0;
        }
        return;
    }

    // stage 4: playing; at the frames where the clock passes a pose (forward and back) the output is compared with that pose's reference
    ++sAnim.mStageFrames;
    const U32 ph = (frame - sAnim.mPlayStart) % period;
    for (int i = 0; i < kPoses; ++i)
    {
        const int dir = ph == sAnim.mPoseX[i] ? 0 : (ph == period - sAnim.mPoseX[i] ? 1 : -1);
        if (dir < 0)
        {
            continue;
        }
        read_lum(out, w, h, y);
        sAnim.mPlayRmse[dir][i] += rmse_vs(y, sAnim.mRef[i]);
        ++sAnim.mPlayN[dir][i];
        sAnim.mPlaySharp += sharpness(y, w, h);
        ++sAnim.mPlaySharpN;
        if (native)
        {
            // at 100 percent the input at this pose against the frozen reference: the clock repeats the pose if this is near the jitter floor
            std::vector<F32> yi;
            read_lum(in, w, h, yi);
            sAnim.mInRmse[i] += rmse_vs(yi, sAnim.mRef[i]);
            ++sAnim.mInN[i];
        }
        break;
    }
    if (sAnim.mStageFrames >= kPlayPeriods * period)
    {
        std::ostringstream o;
        F64 fwd = 0.0, back = 0.0, mean = 0.0, still = 0.0, still_sharp = 0.0;
        o << std::fixed << std::setprecision(5) << "animation play, scale " << std::setprecision(3) << scale << " rcas " << std::setprecision(2) << key[4] / 1000.0 << std::setprecision(5) << " (" << kPlayPeriods << " periods): rmse forward";
        for (int i = 0; i < kPoses; ++i)
        {
            const F64 f = sAnim.mPlayN[0][i] ? sAnim.mPlayRmse[0][i] / sAnim.mPlayN[0][i] : 0.0;
            fwd += f;
            o << " " << f;
        }
        o << " back";
        for (int i = 0; i < kPoses; ++i)
        {
            const F64 b = sAnim.mPlayN[1][i] ? sAnim.mPlayRmse[1][i] / sAnim.mPlayN[1][i] : 0.0;
            back += b;
            o << " " << b;
            still += sAnim.mStillRmse[i];
            still_sharp += sAnim.mStillSharp[i];
        }
        fwd /= kPoses;
        back /= kPoses;
        mean = 0.5 * (fwd + back);
        still /= kPoses;
        still_sharp /= kPoses;
        const F64 sharp = sAnim.mPlaySharpN ? sAnim.mPlaySharp / (F64)sAnim.mPlaySharpN : 0.0;
        o << " | mean " << mean << " (forward " << fwd << " back " << back << ") sharpness " << sharp << " | still mean " << still << " sharpness " << still_sharp << " | EXCESS from the animation: rmse +" << mean - still
          << std::setprecision(1) << " (" << (still > 0.0 ? 100.0 * (mean / still - 1.0) : 0.0) << " %), sharpness " << (still_sharp > 0.0 ? 100.0 * (sharp / still_sharp - 1.0) : 0.0) << " %" << std::setprecision(5);
        if (native)
        {
            F64 ratio = 0.0;
            int n = 0;
            for (int i = 0; i < kPoses; ++i)
            {
                if (sAnim.mInN[i] && sAnim.mFloor[i] > 0.0)
                {
                    ratio += (sAnim.mInRmse[i] / sAnim.mInN[i]) / sAnim.mFloor[i];
                    ++n;
                }
            }
            o << " | repeat check (input vs frozen reference / jitter floor, 1 = exact): " << std::setprecision(2) << (n ? ratio / n : 0.0);
        }
        LL_INFOS("RigMetrics") << o.str() << LL_ENDL;
        sAnim.mStage = 2;
        anim_begin_freeze(0);
    }
}

}

void rk_rig_camera_sway(U32 frame)
{
    init();
    if (!sway_enabled() || gCubeSnapshot || gSnapshot)
    {
        return;
    }
    LLViewerCamera* cam = LLViewerCamera::getInstance();
    // the camera the viewer left, or the one this left behind last frame (then the viewer did not set it again and the base stands)
    const bool mine = sSway.mHaveBase && same_axes(*cam, sSway.mLastSet);
    if (!mine)
    {
        sSway.mBase = *cam;
        sSway.mHaveBase = true;
    }
    else
    {
        cam->setAxes(sSway.mBase);
    }
    if (sSway.mActive)
    {
        const F32 theta = sSway.mTheta0 * sinf(2.f * (F32)F_PI * (F32)(S32)(frame - sSway.mStart) / (F32)sSway.mPeriod);
        cam->rotate(theta, LLVector3::z_axis);
    }
    sSway.mLastSet = *cam;
}

void rk_rig_metrics(LLRenderTarget& out, LLRenderTarget& in, U32 frame)
{
    init();
    if (sRig.mMode == 0 || gCubeSnapshot || gSnapshot)
    {
        return;
    }
    const S32 w = (S32)out.getWidth(), h = (S32)out.getHeight();
    if (w < 8 || h < 8)
    {
        return;
    }

    // the camera and the picture sizes must have been unchanged for WARM frames (a moving camera, a changed render scale and the history that follows both spoil every number); with the sway the camera is its base pose
    const S32 key[5] = { (S32)in.getWidth(), (S32)in.getHeight(), w, h, (S32)(rk_fsr_sharp_now() * 1000.f + 0.5f) };
    F32 cam[6];
    cam_now(cam);
    F64 dp = 0.0, da = 0.0;
    if (sRig.mCamHave)
    {
        cam_diff(cam, sRig.mCamPrev, dp, da);
    }
    const bool moved = !sRig.mCamHave || dp > 1e-4 || da > 1e-5;
    memcpy(sRig.mCamPrev, cam, sizeof(cam));
    sRig.mCamHave = true;
    sRig.mStable = moved ? 0 : sRig.mStable + 1;
    const bool key_changed = memcmp(key, sRig.mKey, sizeof(key)) != 0;
    memcpy(sRig.mKey, key, sizeof(key));
    sRig.mKeyFrames = key_changed ? 0 : sRig.mKeyFrames + 1;
    if (moved || key_changed)
    {
        if (sAnim.mEnabled)
        {
            anim_reset();
        }
        reset_burst();
        reset_sway();
        sRig.mAcc.clear();
        sRig.mAccCount = 0;
        sRig.mGapLeft = 0;
        sRig.mStaticRmse = sRig.mStaticSharp = -1.0;
    }
    if (sRig.mStable < sRig.mWarm || sRig.mKeyFrames < sRig.mWarm)
    {
        return;
    }

    if (sAnim.mEnabled)
    {
        // the animation case replaces the still and sway cases of this run
        anim_step(out, in, frame, w, h, key);
        return;
    }

    if (sRig.mMode == 1 && !sRig.mFileTried)
    {
        load_reference(w, h);
    }

    // the reference: the upscaler's input at 100 percent scale (the same size as the output) averaged over 64 consecutive jittered frames
    if ((sRig.mMode == 2 || sRig.mMode == 3) && !sRig.mRefBuilt && key[0] == w && key[1] == h)
    {
        std::vector<U8> rgba((size_t)w * h * 4);
        {
            RenderPassScope pass(in);
            gRender.readPixels(0, 0, w, h, PixelFormat::RGBA, ComponentType::U8, rgba.data());
        }
        std::vector<F32> y;
        luminance(rgba, (size_t)w * h, y);
        if (sRig.mAcc.empty())
        {
            sRig.mAcc.assign((size_t)w * h, 0.0);
            sRig.mAccCount = 0;
        }
        for (size_t i = 0; i < y.size(); ++i)
        {
            sRig.mAcc[i] += y[i];
        }
        if (++sRig.mAccCount >= 64)
        {
            finish_reference(w, h);
        }
        return;
    }
    if (sRig.mMode == 2)
    {
        return;         // (the reference run only builds the reference)
    }

    // the sway: settle for two periods, then measure four periods; the pictures that are read are the ones at the rest pose, where the true picture is the reference's
    if (sSway.mPhase != 0)
    {
        ++sSway.mPhaseFrames;
        if (sSway.mPhase == 1)
        {
            if (sSway.mPhaseFrames >= 2 * sSway.mPeriod)
            {
                sSway.mPhase = 2;
                sSway.mPhaseFrames = 0;
            }
            return;
        }
        const U32 half = sSway.mPeriod / 2;
        if (sRig.mRefBuilt && sRig.mRefW == w && sRig.mRefH == h && frame >= sSway.mStart && (frame - sSway.mStart) % half == 0)
        {
            std::vector<U8> rgba((size_t)w * h * 4);
            {
                RenderPassScope pass(out);
                gRender.readPixels(0, 0, w, h, PixelFormat::RGBA, ComponentType::U8, rgba.data());
            }
            std::vector<F32> y;
            luminance(rgba, (size_t)w * h, y);
            F64 se = 0.0;
            for (size_t k = 0; k < y.size(); ++k)
            {
                const F64 e = (F64)y[k] - (F64)sRig.mRef[k];
                se += e * e;
            }
            const F64 rmse = sqrt(se / (F64)y.size());
            sSway.mRmse += rmse;
            sSway.mRmse2 += rmse * rmse;
            sSway.mSharp += sharpness(y, w, h);
            ++sSway.mCrossings;
        }
        if (sSway.mPhaseFrames >= 4 * sSway.mPeriod)
        {
            if (sSway.mCrossings)
            {
                const F64 n = (F64)sSway.mCrossings;
                const F64 rmse = sSway.mRmse / n, sd = sqrt(std::max(0.0, sSway.mRmse2 / n - rmse * rmse)), sharp = sSway.mSharp / n;
                F64 d2, d3;
                std::ostringstream o;
                o << std::fixed << std::setprecision(5) << "sway burst " << ++sSway.mBurstsDone << " at frames " << sSway.mStart << ".. (" << sSway.mCrossings << " crossings of the rest pose) scale " << std::setprecision(3)
                  << (F64)key[0] / (F64)key[2] << " rcas " << std::setprecision(2) << key[4] / 1000.0 << std::setprecision(3) << " peak speed " << sSway.mSpeeds[sSway.mSpeedIdx] << " output px/frame (period " << sSway.mPeriod << " frames, +-" << sSway.mTheta0 * 180.f / (F32)F_PI << " deg)" << std::setprecision(5)
                  << ": rmse vs reference " << rmse << " sd " << sd << " psnr " << (rmse > 1e-9 ? -20.0 * log10(rmse) : 99.0) << " | sharpness " << sharp;
                if (camera_is_reference(d2, d3) && sRig.mStaticRmse >= 0.0)
                {
                    o << " | still at this scale: rmse " << sRig.mStaticRmse << " sharpness " << sRig.mStaticSharp << " | EXCESS from the motion: rmse +" << rmse - sRig.mStaticRmse << " (" << std::setprecision(1) << 100.0 * (rmse / sRig.mStaticRmse - 1.0)
                      << " %), sharpness " << std::setprecision(1) << 100.0 * (sharp / sRig.mStaticSharp - 1.0) << " %";
                }
                else
                {
                    o << " | no comparable still burst at this scale yet, or the camera is not the reference's";
                }
                LL_INFOS("RigMetrics") << o.str() << LL_ENDL;
            }
            // the next speed, or back to the still bursts
            if (sSway.mSpeedIdx + 1 < sSway.mSpeeds.size())
            {
                ++sSway.mSpeedIdx;
                start_sway(frame, h);
            }
            else
            {
                sSway.mSpeedIdx = 0;
                reset_sway();
                sRig.mGapLeft = sRig.mGap;
            }
        }
        return;
    }

    // a burst of consecutive frames
    if (!sRig.mBursting)
    {
        if (sRig.mGapLeft > 0)
        {
            --sRig.mGapLeft;
            return;
        }
        reset_burst();
        sRig.mBursting = true;
        sRig.mBurstStart = frame;
    }
    std::vector<U8> rgba((size_t)w * h * 4);
    {
        RenderPassScope pass(out);
        gRender.readPixels(0, 0, w, h, PixelFormat::RGBA, ComponentType::U8, rgba.data());
    }
    std::vector<F32> y;
    luminance(rgba, (size_t)w * h, y);
    const bool have_prev = sRig.mPrevFrame + 1 == frame && sRig.mPrev.size() == y.size();
    if (have_prev)
    {
        std::vector<U32> hist(512, 0);
        F64 sum = 0.0, edge_sum = 0.0, flat_sum = 0.0;
        U64 n = 0, n_edge = 0, n_flat = 0, n_changed = 0;
        for (S32 j = 1; j < h - 1; ++j)
        {
            for (S32 i = 1; i < w - 1; ++i)
            {
                const size_t k = (size_t)j * w + i;
                const F32 d = fabsf(y[k] - sRig.mPrev[k]);
                const F32 g = fabsf(y[k + 1] - y[k - 1]) + fabsf(y[k + w] - y[k - w]);
                sum += d;
                ++n;
                if (g > 0.1f)
                {
                    edge_sum += d;
                    ++n_edge;
                }
                else
                {
                    flat_sum += d;
                    ++n_flat;
                }
                if (d > 4.f / 255.f)
                {
                    ++n_changed;
                }
                ++hist[std::min<S32>(511, (S32)(d * (512.f / 0.25f)))];
            }
        }
        const U64 target = (U64)(0.99 * (F64)n);
        U64 acc = 0;
        S32 bin = 0;
        for (; bin < 512; ++bin)
        {
            acc += hist[bin];
            if (acc >= target)
            {
                break;
            }
        }
        const F64 flk = n ? sum / (F64)n : 0.0;
        sRig.mFlk += flk;
        sRig.mFlk2 += flk * flk;
        sRig.mP99 += (F64)(bin + 1) * (0.25 / 512.0);
        sRig.mFrac += n ? (F64)n_changed / (F64)n : 0.0;
        sRig.mEdge += n_edge ? edge_sum / (F64)n_edge : 0.0;
        sRig.mFlat += n_flat ? flat_sum / (F64)n_flat : 0.0;
        sRig.mSharp += sharpness(y, w, h);
        if (sRig.mRefBuilt && sRig.mRefW == w && sRig.mRefH == h)
        {
            F64 se = 0.0;
            for (size_t k = 0; k < y.size(); ++k)
            {
                const F64 e = (F64)y[k] - (F64)sRig.mRef[k];
                se += e * e;
            }
            sRig.mRmse += sqrt(se / (F64)y.size());
            ++sRig.mRefFrames;
        }
        ++sRig.mCount;
    }
    sRig.mPrev.swap(y);
    sRig.mPrevFrame = frame;
    if (frame - sRig.mBurstStart + 1 >= sRig.mFrames)
    {
        end_burst(key, frame);
        if (sway_enabled() && sRig.mRefBuilt && sRig.mRefW == w && sRig.mStaticRmse >= 0.0)
        {
            sSway.mSpeedIdx = 0;
            start_sway(frame, h);
        }
    }
}

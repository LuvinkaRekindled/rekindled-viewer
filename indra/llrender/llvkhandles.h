/**
 * @file llvkhandles.h
 * @brief Texture handles: the bookkeeping behind a stable number that rows, records and uniforms carry and a shader turns into a descriptor index
 *
 * doc/vulkan/PLAN-texture-handles.md (H3). A handle is a dense number in a table (0 is the placeholder). Its content is a descriptor index of the table's descriptor array; a descriptor, once
 * written, never changes, so a change of what a handle shows is a NEW descriptor index and an entry in the change log. Every frame in flight has a copy of the map (handle -> descriptor
 * index) that it brings up to date from the log when it begins: a frame sees one consistent mapping from its first draw to its last, whatever the CPU changes while it runs. Descriptor
 * indices and handle numbers that were given up come back after the frames that could still read them have finished.
 *
 * This class makes no Vulkan call and knows nothing of the viewer or of GL: the caller writes the descriptors and the map copies (applyTo gives it the log). It is meant to be reusable.
 *
 * Copyright (c) 2026, Firestorm native Vulkan renderer. Licensed under the same terms as the rest of the viewer (LGPL 2.1).
 */

#ifndef LL_LLVKHANDLES_H
#define LL_LLVKHANDLES_H

#include <cstdint>
#include <deque>
#include <vector>

namespace LLVk
{

class HandleTable
{
public:
    static constexpr uint32_t TABLES = 4;
    static constexpr uint32_t STRIDE = 65536;       // handles per table: the map is indexed (table << 16) | handle
    static constexpr uint32_t FRAMES = 6;           // frame copies of the map (at most)

    // descriptor_caps: descriptor indices per table (index 0 is the placeholder and is never handed out)
    void init(const uint32_t* descriptor_caps, uint32_t frames)
    {
        mFrames = frames < FRAMES ? frames : FRAMES;
        for (uint32_t t = 0; t < TABLES; ++t)
        {
            mDescCap[t] = descriptor_caps[t];
            mNextDesc[t] = 1;
            mNextHandle[t] = 1;
            mDesc[t].assign(1, 0u);
            mLive[t].assign(1, 1);
            mFreeDesc[t].clear();
            mFreeHandle[t].clear();
        }
        for (uint32_t t = 0; t < TABLES; ++t)
        {
            mOwner[t].clear();
            mPendingRetire[t].clear();
        }
        mLog.clear();
        mLogBase = 0;
        for (uint32_t f = 0; f < FRAMES; ++f) mApplied[f] = 0;
        mRetired.clear();
        mReady = true;
    }
    bool ready() const { return mReady; }

    // a new handle (its content is the placeholder until setContent); 0 when the table is full
    uint32_t newHandle(uint32_t table)
    {
        if (!mReady || table >= TABLES) return 0;
        uint32_t h = 0;
        if (!mFreeHandle[table].empty())
        {
            h = mFreeHandle[table].front();
            mFreeHandle[table].pop_front();
        }
        else if (mNextHandle[table] < STRIDE)
        {
            h = mNextHandle[table]++;
            mDesc[table].resize(h + 1, 0u);
            mLive[table].resize(h + 1, 0);
        }
        else
        {
            ++mRefusedHandles;
            return 0;
        }
        mDesc[table][h] = 0;
        mLive[table][h] = 1;
        ++mLiveCount[table];
        return h;
    }

    // the handle shows something else from now on: returns the descriptor index the caller has to write (the new content), 0 when the table has no descriptor free (the handle keeps what it had).
    // The descriptor it had is given back when `frame` has finished: frames up to that one may still read it through their copy of the map.
    uint32_t setContent(uint32_t table, uint32_t handle, uint64_t frame)
    {
        if (!mReady || table >= TABLES || handle == 0 || handle >= mLive[table].size() || !mLive[table][handle]) return 0;
        uint32_t d = 0;
        if (!mFreeDesc[table].empty())
        {
            d = mFreeDesc[table].back();
            mFreeDesc[table].pop_back();
        }
        else if (mNextDesc[table] < mDescCap[table])
        {
            d = mNextDesc[table]++;
        }
        else
        {
            ++mRefusedDescriptors;
            return 0;
        }
        const uint32_t old = mDesc[table][handle];
        mDesc[table][handle] = d;
        touch(table, d);
        mOwner[table][d] = handle;
        mLog.push_back({ (uint16_t)table, (uint16_t)handle, d });
        if (old != 0)
        {
            touch(table, old);
            mOwner[table][old] = 0;
            mPendingRetire[table][old] = 1;
            mRetired.push_back({ frame, table, 0, old });
        }
        ++mSets;
        return d;
    }

    // the handle shows the placeholder again (its image is gone and nothing replaced it yet): the descriptor it had comes back when `frame` has finished
    void clearContent(uint32_t table, uint32_t handle, uint64_t frame)
    {
        if (!mReady || table >= TABLES || handle == 0 || handle >= mLive[table].size() || !mLive[table][handle]) return;
        const uint32_t old = mDesc[table][handle];
        if (old == 0) return;
        mDesc[table][handle] = 0;
        touch(table, old);
        mOwner[table][old] = 0;
        mPendingRetire[table][old] = 1;
        mLog.push_back({ (uint16_t)table, (uint16_t)handle, 0u });
        mRetired.push_back({ frame, table, 0, old });
    }

    // the handle goes: its entry is the placeholder from the next frame on, its descriptor and its number come back when `frame` has finished
    void release(uint32_t table, uint32_t handle, uint64_t frame)
    {
        if (!mReady || table >= TABLES || handle == 0 || handle >= mLive[table].size() || !mLive[table][handle]) return;
        mLive[table][handle] = 0;
        const uint32_t old = mDesc[table][handle];
        mDesc[table][handle] = 0;
        mLog.push_back({ (uint16_t)table, (uint16_t)handle, 0u });
        if (old != 0)
        {
            touch(table, old);
            mOwner[table][old] = 0;
            mPendingRetire[table][old] = 1;
            mRetired.push_back({ frame, table, 0, old });
        }
        mRetired.push_back({ frame, table, 1, handle });
        --mLiveCount[table];
    }

    // a frame's copy of the map catches up with every change made since it last did (map: TABLES * STRIDE entries)
    void applyTo(uint32_t frame_slot, uint32_t* map)
    {
        if (!mReady || frame_slot >= mFrames || !map) return;
        const uint64_t end = mLogBase + mLog.size();
        for (uint64_t i = mApplied[frame_slot]; i < end; ++i)
        {
            const Entry& e = mLog[(size_t)(i - mLogBase)];
            map[(e.mTable << 16) | e.mHandle] = e.mDesc;
        }
        mApplied[frame_slot] = end;
        uint64_t low = end;
        for (uint32_t f = 0; f < mFrames; ++f) low = mApplied[f] < low ? mApplied[f] : low;
        if (low > mLogBase && low - mLogBase >= 4096)
        {
            mLog.erase(mLog.begin(), mLog.begin() + (size_t)(low - mLogBase));
            mLogBase = low;
        }
    }

    // gives back what was retired by frames that have finished; finished(frame) says whether a frame number has
    template <class F> void retire(F finished)
    {
        while (!mRetired.empty() && finished(mRetired.front().mFrame))
        {
            const Retired& r = mRetired.front();
            if (r.mKind == 0)
            {
                mFreeDesc[r.mTable].push_back(r.mIndex);
                if (r.mIndex < mPendingRetire[r.mTable].size()) mPendingRetire[r.mTable][r.mIndex] = 0;
            }
            else mFreeHandle[r.mTable].push_back(r.mIndex);      // (oldest first: a number is not reused soon, rows that still name it see the placeholder meanwhile)
            mRetired.pop_front();
        }
    }

    // what a handle shows now (0 = the placeholder); for reports
    uint32_t descriptorOf(uint32_t table, uint32_t handle) const { return table < TABLES && handle < mDesc[table].size() ? mDesc[table][handle] : 0; }
    bool live(uint32_t table, uint32_t handle) const { return table < TABLES && handle < mLive[table].size() && mLive[table][handle]; }

    // for the view release guard (Renderer::releaseViewHandles): a descriptor that a handle shows now, or that frames in flight may still read through their copy of the map (it waits to be given back)
    bool descriptorReachable(uint32_t table, uint32_t d) const
    {
        return table < TABLES && d < mOwner[table].size() && (mOwner[table][d] != 0 || mPendingRetire[table][d] != 0);
    }

    uint32_t liveHandles(uint32_t table) const { return mLiveCount[table]; }
    uint32_t descriptorsUsed(uint32_t table) const { return mNextDesc[table] - 1 - (uint32_t)mFreeDesc[table].size(); }
    uint64_t sets() const { return mSets; }
    uint64_t refusedHandles() const { return mRefusedHandles; }
    uint64_t refusedDescriptors() const { return mRefusedDescriptors; }
    size_t pendingRetirements() const { return mRetired.size(); }

private:
    void touch(uint32_t table, uint32_t index)
    {
        if (mOwner[table].size() <= index)
        {
            mOwner[table].resize((size_t)index + 1024, 0u);
            mPendingRetire[table].resize((size_t)index + 1024, 0);
        }
    }
    struct Entry { uint16_t mTable, mHandle; uint32_t mDesc; };
    struct Retired { uint64_t mFrame; uint32_t mTable; uint32_t mKind; uint32_t mIndex; };      // kind 0: a descriptor index, 1: a handle number

    bool mReady = false;
    uint32_t mFrames = 0;
    uint32_t mDescCap[TABLES] = {};
    uint32_t mNextDesc[TABLES] = {};
    uint32_t mNextHandle[TABLES] = {};
    uint32_t mLiveCount[TABLES] = {};
    std::vector<uint32_t> mDesc[TABLES];                // by handle: its descriptor index
    std::vector<uint8_t> mLive[TABLES];
    std::vector<uint32_t> mOwner[TABLES];               // by descriptor index: the handle that shows it now (0: none)
    std::vector<uint8_t> mPendingRetire[TABLES];        // by descriptor index: 1 while it waits in mRetired
    std::vector<uint32_t> mFreeDesc[TABLES];            // (the newest first: a descriptor index is written again before anything can read it)
    std::deque<uint32_t> mFreeHandle[TABLES];           // (the oldest first)
    std::vector<Entry> mLog;
    uint64_t mLogBase = 0;                              // sequence number of mLog[0]
    uint64_t mApplied[FRAMES] = {};
    std::deque<Retired> mRetired;                       // in frame order
    uint64_t mSets = 0, mRefusedHandles = 0, mRefusedDescriptors = 0;
};

}

#endif

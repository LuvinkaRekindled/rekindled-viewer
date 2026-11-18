/**
 * @file rkimagebc7.cpp
 * @brief BC7 at decode: see the header.
 */

#include "linden_common.h"
#include "rkimagebc7.h"
#include "llimage.h"
#include "bc7enc/bc7enc.h"
#include "bc7enc/bc7decomp.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <mutex>
#include <unordered_map>
#include "rkdiag.h"        // <FS> LL_DIAG: the statistics lines are off unless LL_VK_DIAG is set

namespace
{
std::atomic<bool> sEnabled { false };
std::atomic<U32> sCompressed { 0 }, sKeptRaw { 0 };
std::atomic<U64> sEncodeUs { 0 };
std::once_flag sInit;
bc7enc_compress_block_params sParams;

void init_once()
{
    std::call_once(sInit, []()
    {
        bc7enc_compress_block_init();
        bc7enc_compress_block_params_init(&sParams);
        bc7enc_compress_block_params_init_linear_weights(&sParams);
        sParams.m_max_partitions = 0;           // mode 6 (mode 5 / 7 for alpha): the fast profile of the offline check
        sParams.m_uber_level = 0;
        sParams.m_try_least_squares = false;
    });
}

bool is_pow2(U32 v) { return v && (v & (v - 1)) == 0; }

// the 4x4 block at (bx, by) of a level as RGBA (alpha 255 for RGB), edge texels repeated for a level narrower than 4
void gather_block(const U8* level, U32 w, U32 h, U32 c, U32 bx, U32 by, color_rgba* out)
{
    for (U32 y = 0; y < 4; ++y)
    {
        for (U32 x = 0; x < 4; ++x)
        {
            const U32 sx = llmin(bx * 4 + x, w - 1), sy = llmin(by * 4 + y, h - 1);
            const U8* s = level + ((U64)sy * w + sx) * c;
            color_rgba& d = out[y * 4 + x];
            d.m_c[0] = s[0]; d.m_c[1] = s[1]; d.m_c[2] = s[2]; d.m_c[3] = c == 4 ? s[3] : 255;
        }
    }
}

// the block judgement of the offline check, one block: original texels against the decoded ones
bool block_harmed(const color_rgba* a, const color_rgba* b, bool has_alpha)
{
    double sum_a[4] = {}, sum_b[4] = {}, sum_a2[4] = {}, sum_b2[4] = {};
    U32 bmax = 0, alpha_end_err = 0;
    bool alpha_binary = has_alpha;
    const int channels = has_alpha ? 4 : 3;
    for (U32 i = 0; i < 16; ++i)
    {
        for (int k = 0; k < channels; ++k)
        {
            const int d = (int)a[i].m_c[k] - (int)b[i].m_c[k];
            bmax = llmax(bmax, (U32)std::abs(d));
            sum_a[k] += a[i].m_c[k]; sum_b[k] += b[i].m_c[k];
            sum_a2[k] += (double)a[i].m_c[k] * a[i].m_c[k]; sum_b2[k] += (double)b[i].m_c[k] * b[i].m_c[k];
        }
        if (has_alpha)
        {
            const U8 aa = a[i].m_c[3];
            if (aa != 0 && aa != 255) alpha_binary = false;
            if ((aa == 0 || aa == 255) && aa != b[i].m_c[3]) alpha_end_err = llmax(alpha_end_err, (U32)std::abs((int)aa - (int)b[i].m_c[3]));
        }
    }
    double mean_shift = 0, dev_max = 0;
    bool grain_changed = false;
    for (int k = 0; k < channels; ++k)
    {
        mean_shift = llmax(mean_shift, std::abs(sum_a[k] - sum_b[k]) / 16.0);
        const double va = llmax(0.0, sum_a2[k] / 16.0 - (sum_a[k] / 16.0) * (sum_a[k] / 16.0)), vb = llmax(0.0, sum_b2[k] / 16.0 - (sum_b[k] / 16.0) * (sum_b[k] / 16.0));
        const double da = sqrt(va), db = sqrt(vb);
        dev_max = llmax(dev_max, da);
        if (da >= 12.0 && (db < 0.6 * da || db > 1.4 * da)) grain_changed = true;
    }
    const bool smooth = dev_max < 12.0;
    return mean_shift > 6.0 || (!smooth && grain_changed) || (smooth && bmax > 12) || (alpha_binary && alpha_end_err > 8);
}
}

namespace RKImageBC7
{

void setEnabled(bool on) { sEnabled.store(on, std::memory_order_relaxed); }
bool enabled() { return sEnabled.load(std::memory_order_relaxed); }

bool eligible(U32 width, U32 height, U32 components)
{
    return width >= MIN_SIZE && height >= MIN_SIZE && (components == 3 || components == 4) && is_pow2(width) && is_pow2(height);
}

U32 levelWidth(U32 width, U32 k) { return llmax(1u, width >> k); }
U32 levelHeight(U32 height, U32 k) { return llmax(1u, height >> k); }

U32 levelCount(U32 width, U32 height)
{
    U32 n = 1;
    while ((width >> (n - 1)) > 1 || (height >> (n - 1)) > 1)
    {
        ++n;
    }
    return n;
}

U64 blockBytes(U32 width, U32 height)
{
    return (U64)((width + 3) / 4) * ((height + 3) / 4) * 16;
}

void buildChain(const U8* src, U32 width, U32 height, U32 components, std::vector<std::vector<U8>>& levels)
{
    const U32 n = levelCount(width, height);
    levels.clear();
    levels.reserve(n);
    levels.emplace_back(src, src + (size_t)width * height * components);
    U32 lw = width, lh = height;
    for (U32 k = 1; k < n; ++k)
    {
        const U32 nw = levelWidth(width, k), nh = levelHeight(height, k);
        const U32 sx = lw > 1 ? 1 : 0, sy = lh > 1 ? 1 : 0;        // a 1-wide source has no second column
        std::vector<U8> next((size_t)nw * nh * components);
        const U8* s = levels.back().data();
        for (U32 y = 0; y < nh; ++y)
        {
            const U8* r0 = s + (U64)(2 * y) * lw * components;
            const U8* r1 = r0 + (U64)sy * lw * components;
            U8* d = next.data() + (U64)y * nw * components;
            for (U32 x = 0; x < nw; ++x)
            {
                const U8* p00 = r0 + (U64)(2 * x) * components;
                const U8* p01 = p00 + (U64)sx * components;
                const U8* p10 = r1 + (U64)(2 * x) * components;
                const U8* p11 = p10 + (U64)sx * components;
                for (U32 i = 0; i < components; ++i)
                {
                    d[(U64)x * components + i] = (U8)(((U32)p00[i] + p01[i] + p10[i] + p11[i] + 2) >> 2);
                }
            }
        }
        levels.push_back(std::move(next));
        lw = nw;
        lh = nh;
    }
}

// RKImageTex::analyzeAlpha, line for line (the histogram of the quantised alpha plus a 2x2 box-sampled one; its three rules)
S8 classifyAlpha(const U8* data, U32 w, U32 h, U32 components)
{
    if (components != 4 && components != 2)
    {
        return -1;
    }
    const U32 stride = components, offset = components - 1;
    U32 length = w * h;
    U32 alphatotal = 0;
    U32 sample[16] = {};
    if (w >= 2 && h >= 2 && (w & 1) == 0 && (h & 1) == 0)       // the 2x2 walk needs even sides: an odd one read a row or a column past the texels
    {
        const U8* rowstart = data + offset;
        for (U32 y = 0; y < h; y += 2)
        {
            const U8* current = rowstart;
            for (U32 x = 0; x < w; x += 2)
            {
                const U32 s1 = current[0];
                alphatotal += s1;
                const U32 s2 = current[w * stride];
                alphatotal += s2;
                current += stride;
                const U32 s3 = current[0];
                alphatotal += s3;
                const U32 s4 = current[w * stride];
                alphatotal += s4;
                current += stride;
                ++sample[s1 / 16];
                ++sample[s2 / 16];
                ++sample[s3 / 16];
                ++sample[s4 / 16];
                const U32 asum = (s1 + s2 + s3 + s4);
                alphatotal += asum;
                sample[asum / (16 * 4)] += 4;
            }
            rowstart += 2 * w * stride;
        }
        length *= 2;
    }
    else
    {
        const U8* current = data + offset;
        for (U32 i = 0; i < length; i++)
        {
            const U32 s1 = *current;
            alphatotal += s1;
            ++sample[s1 / 16];
            current += stride;
        }
    }
    U32 midrangetotal = 0;
    for (U32 i = 2; i < 13; i++) midrangetotal += sample[i];
    U32 lowerhalftotal = 0;
    for (U32 i = 0; i < 8; i++) lowerhalftotal += sample[i];
    U32 upperhalftotal = 0;
    for (U32 i = 8; i < 16; i++) upperhalftotal += sample[i];
    if (midrangetotal > length / 48 || (lowerhalftotal == length && alphatotal != 0) || (upperhalftotal == length && alphatotal != 255 * length))
    {
        return 0;
    }
    return 1;
}

LLPointer<LLImageRaw> assemble(const std::vector<std::vector<U8>>& block_levels, U32 width, U32 height, U32 components, S8 alpha_is_mask)
{
    if (block_levels.empty())
    {
        return nullptr;
    }
    U64 total = 0;
    for (const auto& l : block_levels) total += l.size();
    if (total == 0 || total > 0x7fffffff)
    {
        return nullptr;
    }
    U8* data = (U8*)ll_aligned_malloc_16((size_t)total);
    if (!data)
    {
        return nullptr;
    }
    // the smallest level first, the finest last (what RKImageTex::setImage(data_hasmips) reads: it walks down from the finest by subtracting each smaller level's size)
    U8* p = data;
    for (size_t k = block_levels.size(); k-- > 0;)
    {
        memcpy(p, block_levels[k].data(), block_levels[k].size());
        p += block_levels[k].size();
    }
    LLPointer<LLImageRaw> raw = new LLImageRaw();
    raw->setCompressedChain(data, (S32)total, (U16)width, (U16)height, (S8)components, FORMAT_BC7, (U8)block_levels.size(), alpha_is_mask);
    return raw;
}

bool splitLevels(const LLImageRaw* compressed, std::vector<std::vector<U8>>& out)
{
    out.clear();
    if (!compressed || !compressed->isCompressed() || !compressed->getData())
    {
        return false;
    }
    const U32 w = compressed->getWidth(), h = compressed->getHeight(), n = compressed->getLevelCount();
    U64 total = 0;
    for (U32 k = 0; k < n; ++k) total += blockBytes(levelWidth(w, k), levelHeight(h, k));
    if (total != (U64)compressed->getDataSize())
    {
        return false;
    }
    out.resize(n);
    const U8* p = compressed->getData();
    for (U32 k = n; k-- > 0;)
    {
        const U64 bytes = blockBytes(levelWidth(w, k), levelHeight(h, k));
        out[k].assign(p, p + bytes);
        p += bytes;
    }
    return true;
}

LLPointer<LLImageRaw> compress(const std::vector<std::vector<U8>>& levels, U32 width, U32 height, U32 components, F32 max_harmed_percent, F32& harmed_percent, S8 alpha_is_mask)
{
    harmed_percent = 100.f;
    if (levels.empty() || !eligible(width, height, components))
    {
        return nullptr;
    }
    init_once();
    const auto t0 = std::chrono::steady_clock::now();
    const bool has_alpha = components == 4;
    std::vector<std::vector<U8>> block_levels(levels.size());
    color_rgba pixels[16], decoded[16];
    U32 harmed = 0, blocks_finest = 0;
    // <FS> 2026-11-14 (the user's ears bake, Aux 1: the same bytes of the server gave the wrong picture BC7 encoded at login and the right one uploaded raw after a rebake): how far the finest level's decoded blocks are from
    // its source, counted over every block: the sum and the largest of the absolute differences per channel (0 red .. 3 alpha), and the number of blocks whose alpha moved by more than 64
    U64 err_sum[4] = {};
    U32 err_max[4] = {}, alpha_far_blocks = 0;
    for (size_t k = 0; k < levels.size(); ++k)
    {
        const U32 lw = levelWidth(width, (U32)k), lh = levelHeight(height, (U32)k);
        const U32 bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        std::vector<U8>& blocks = block_levels[k];
        blocks.resize((size_t)bw * bh * 16);
        for (U32 by = 0; by < bh; ++by)
        {
            for (U32 bx = 0; bx < bw; ++bx)
            {
                gather_block(levels[k].data(), lw, lh, components, bx, by, pixels);
                U8* block = blocks.data() + ((size_t)by * bw + bx) * 16;
                bc7enc_compress_block(block, pixels, &sParams);
                if (k == 0)
                {
                    bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(decoded));
                    if (block_harmed(pixels, decoded, has_alpha)) ++harmed;
                    U32 alpha_block_max = 0;
                    for (U32 i = 0; i < 16; ++i)
                    {
                        for (U32 ch = 0; ch < 4; ++ch)
                        {
                            const U32 d = (U32)std::abs((int)pixels[i].m_c[ch] - (int)decoded[i].m_c[ch]);
                            err_sum[ch] += d;
                            err_max[ch] = llmax(err_max[ch], d);
                            if (ch == 3) alpha_block_max = llmax(alpha_block_max, d);
                        }
                    }
                    if (has_alpha && alpha_block_max > 64) ++alpha_far_blocks;
                }
            }
        }
        if (k == 0)
        {
            blocks_finest = bw * bh;
            harmed_percent = 100.f * (F32)harmed / (F32)llmax(1u, blocks_finest);
            {
                // a finest level that came back far from its source (any channel's mean error above 3, or an alpha that moved by more than 64 in any block): logged with the source's hash (the BakeTex log's raw
                // hash of the same texture decoded raw is the same number), so a bad encode of a known texture can be told from a bad upload
                const double texels = 16.0 * (double)llmax(1u, blocks_finest);
                const double mean_r = err_sum[0] / texels, mean_g = err_sum[1] / texels, mean_b = err_sum[2] / texels, mean_a = err_sum[3] / texels;
                if (llmax(llmax(mean_r, mean_g), llmax(mean_b, mean_a)) > 3.0 || alpha_far_blocks > 0)
                {
                    static std::atomic<U32> logged { 0 };
                    if (logged++ < 200)
                    {
                        LL_DIAG("BC7") << "BC7 encode off its source: " << width << "x" << height << " x" << components << ", source hash " << std::hex << fs_sparse_hash(levels[0].data(), levels[0].size()) << std::dec
                                        << ", harmed blocks " << harmed_percent << "%, mean error r " << mean_r << " g " << mean_g << " b " << mean_b << " a " << mean_a << ", largest r " << err_max[0] << " g " << err_max[1]
                                        << " b " << err_max[2] << " a " << err_max[3] << ", blocks with an alpha moved by more than 64: " << alpha_far_blocks << " of " << blocks_finest << LL_ENDL;
                    }
                }
            }
            if (harmed_percent > max_harmed_percent)
            {
                sEncodeUs += (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
                ++sKeptRaw;
                return nullptr;         // the rest of the chain is not encoded: the texture stays raw
            }
        }
    }
    LLPointer<LLImageRaw> result = assemble(block_levels, width, height, components, alpha_is_mask);
    sEncodeUs += (U64)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    if (result.notNull())
    {
        ++sCompressed;
        if (width >= 512)
        {
            // <FS> each encode of 512 or more: the source's hash (the same as the BakeTex raw hash of a raw decode of the same bytes) and the chain's (the BakeTex hash of the BC7 result), 300 lines at most
            static std::atomic<U32> logged { 0 };
            if (logged++ < 300)
            {
                LL_DIAG("BC7") << "BC7 encoded " << width << "x" << height << " x" << components << ": source hash " << std::hex << fs_sparse_hash(levels[0].data(), levels[0].size()) << ", chain hash "
                                << fs_sparse_hash(result->getData(), (size_t)result->getDataSize()) << std::dec << ", harmed blocks " << harmed_percent << "%" << LL_ENDL;
            }
        }
    }
    return result;
}

void levelHashes(const LLImageRaw* compressed, std::vector<U64>& out)
{
    out.clear();
    if (!compressed || !compressed->isCompressed() || !compressed->getData())
    {
        return;
    }
    const U32 w = compressed->getWidth(), h = compressed->getHeight(), n = compressed->getLevelCount();
    U64 total = 0;
    for (U32 k = 0; k < n; ++k) total += blockBytes(levelWidth(w, k), levelHeight(h, k));
    if (total != (U64)compressed->getDataSize())
    {
        return;
    }
    out.resize(n);
    const U8* p = compressed->getData();
    for (U32 k = n; k-- > 0;)       // (the smallest level is first in the buffer)
    {
        const U64 bytes = blockBytes(levelWidth(w, k), levelHeight(h, k));
        out[k] = fs_sparse_hash(p, (size_t)bytes);
        p += bytes;
    }
}

namespace
{
std::mutex sRegistryMutex;
std::unordered_map<U64, std::string> sRegistry;
std::deque<U64> sRegistryOrder;
}

void registerLevels(const std::vector<U64>& hashes, const std::string& id_text, S32 discard, U32 width, U32 height)
{
    std::lock_guard<std::mutex> lock(sRegistryMutex);
    for (size_t k = 0; k < hashes.size(); ++k)
    {
        if (blockBytes(levelWidth(width, (U32)k), levelHeight(height, (U32)k)) < 1024)
        {
            continue;       // (tiny levels are flat colours that many textures share)
        }
        std::string what = id_text + " level " + std::to_string(k) + " (" + std::to_string(levelWidth(width, (U32)k)) + "x" + std::to_string(levelHeight(height, (U32)k)) + ", upload at discard " + std::to_string(discard) + ")";
        auto found = sRegistry.find(hashes[k]);
        if (found == sRegistry.end())
        {
            sRegistryOrder.push_back(hashes[k]);
            sRegistry.emplace(hashes[k], std::move(what));
            if (sRegistryOrder.size() > 30000)
            {
                sRegistry.erase(sRegistryOrder.front());
                sRegistryOrder.pop_front();
            }
        }
        else
        {
            found->second = std::move(what);        // the latest upload that held it
        }
    }
}

std::string lookupLevel(U64 hash)
{
    std::lock_guard<std::mutex> lock(sRegistryMutex);
    auto found = sRegistry.find(hash);
    return found == sRegistry.end() ? std::string() : found->second;
}

Stats takeStats()
{
    Stats s;
    s.mCompressed = sCompressed.exchange(0);
    s.mKeptRaw = sKeptRaw.exchange(0);
    s.mEncodeMicroseconds = sEncodeUs.exchange(0);
    return s;
}

}

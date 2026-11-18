/**
 * @file rkvknative.cpp
 * @brief The native shader path: compile Vulkan GLSL written from scratch, and stand in for the viewer's translated programs at the draw. See rkvknative.h.
 */

#if defined(LL_VULKAN)

#include "linden_common.h"

#include "rkvknative.h"
#include "rkvkrenderer.h"
#include "rkvkcontext.h"

#include <glslang/Public/ShaderLang.h>
#include <regex>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>

#include <chrono>
#include <fstream>
#include <memory>
#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>

#include <windows.h>

#include "lldir.h"
#include "llfile.h"

namespace LLVk
{

namespace
{

const char* const kBlockNames[UC_COUNT] = { "LLLooseUniforms", "LLLooseDraw", "LLLooseMaterial", "LLLooseLight", "LLLooseSkin" };

bool read_file(const std::string& path, std::string& out)
{
    std::ifstream f(path, std::ios::binary);
    if (!f)
    {
        return false;
    }
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

// #include "lib/x.glsl": paths are relative to the native directory; every file read is remembered for hot reload
class NativeIncluder : public glslang::TShader::Includer
{
public:
    NativeIncluder(const std::string& root, std::vector<std::string>* files) : mRoot(root), mFiles(files) {}
    IncludeResult* includeLocal(const char* header, const char* /*includer*/, size_t /*depth*/) override { return load(header); }
    IncludeResult* includeSystem(const char* header, const char* /*includer*/, size_t /*depth*/) override { return load(header); }
    void releaseInclude(IncludeResult* r) override
    {
        if (r)
        {
            delete static_cast<std::string*>(r->userData);
            delete r;
        }
    }

private:
    IncludeResult* load(const char* header)
    {
        const std::string path = mRoot + "/" + header;
        std::string* text = new std::string();
        if (!read_file(path, *text))
        {
            delete text;
            return nullptr;
        }
        if (mFiles)
        {
            mFiles->push_back(path);
        }
        return new IncludeResult(header, text->data(), text->size(), text);
    }
    std::string mRoot;
    std::vector<std::string>* mFiles;
};

// the GLSL name of a reflected uniform from its GL type enum (the public part of glslang's reflection)
std::string glsl_type_name(int gl)
{
    switch (gl)
    {
    case 0x1406: return "float";
    case 0x8B50: return "vec2";
    case 0x8B51: return "vec3";
    case 0x8B52: return "vec4";
    case 0x1404: return "int";
    case 0x8B53: return "ivec2";
    case 0x8B54: return "ivec3";
    case 0x8B55: return "ivec4";
    case 0x1405: return "uint";
    case 0x8DC6: return "uvec2";
    case 0x8DC7: return "uvec3";
    case 0x8DC8: return "uvec4";
    case 0x8B5A: return "mat2";
    case 0x8B5B: return "mat3";
    case 0x8B5C: return "mat4";
    default: return "unknown";
    }
}

// std140 bytes a member occupies (an array counts every element at a 16 byte stride)
U32 std140_bytes(const std::string& type, U32 array_size)
{
    U32 elem = 4;
    if (type == "vec2" || type == "ivec2" || type == "uvec2") elem = 8;
    else if (type == "vec3" || type == "ivec3" || type == "uvec3") elem = 12;
    else if (type == "vec4" || type == "ivec4" || type == "uvec4") elem = 16;
    else if (type == "mat2") elem = 32;
    else if (type == "mat3") elem = 48;
    else if (type == "mat4") elem = 64;
    if (array_size > 0)
    {
        return (elem + 15) / 16 * 16 * array_size;
    }
    return elem;
}

// ---- the compile cache: a native program's SPIR-V and its reflection, kept on disk between runs ------------------------------------------------
// Compiling the 196 programs of a run with glslang took 720 to 750 ms on the render thread with the main thread blocked, at every start and at every shader setting change (AUDIT-shaders-2026-11-18.md
// section 1 items 2 and 3). A program's result depends on its stage files, their includes and the defines; the cache entry records the content hash of every file the build read and is used only when
// every one of them is the same as now. One file a program under <cache>/native_spirv/, written after a successful compile. LL_VK_NATIVE_CACHE=0 turns it off.

constexpr U32 kNativeCacheVersion = 1;
// (the build of this file: a change of the reflection code in it drops every entry)
const char* const kNativeCacheTag = __DATE__ " " __TIME__;

struct FileMemo { U64 mStamp = 0, mSize = 0, mHash = 0; };
std::mutex gFileMemoLock;
std::unordered_map<std::string, FileMemo> gFileMemo;

U64 fnv1a(const void* data, size_t n, U64 h = 1469598103934665603ull)
{
    const U8* p = (const U8*)data;
    for (size_t i = 0; i < n; ++i)
    {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// the content hash of a file, remembered for the time stamp and size it was made at (the includes of the programs are read by hundreds of them)
bool file_content_hash(const std::string& path, U64& hash)
{
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &d))
    {
        return false;
    }
    const U64 stamp = ((U64)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
    const U64 size = ((U64)d.nFileSizeHigh << 32) | d.nFileSizeLow;
    {
        std::lock_guard<std::mutex> lock(gFileMemoLock);
        auto it = gFileMemo.find(path);
        if (it != gFileMemo.end() && it->second.mStamp == stamp && it->second.mSize == size)
        {
            hash = it->second.mHash;
            return true;
        }
    }
    std::string text;
    if (!read_file(path, text))
    {
        return false;
    }
    hash = fnv1a(text.data(), text.size());
    std::lock_guard<std::mutex> lock(gFileMemoLock);
    gFileMemo[path] = { stamp, size, hash };
    return true;
}

struct CacheWriter
{
    std::string mBytes;
    void u32(U32 v) { mBytes.append((const char*)&v, 4); }
    void u64(U64 v) { mBytes.append((const char*)&v, 8); }
    void str(const std::string& s) { u32((U32)s.size()); mBytes.append(s); }
    void words(const std::vector<U32>& w) { u32((U32)w.size()); mBytes.append((const char*)w.data(), w.size() * 4); }
};

struct CacheReader
{
    const char* mP;
    const char* mEnd;
    bool mOk = true;
    CacheReader(const std::string& s) : mP(s.data()), mEnd(s.data() + s.size()) {}
    bool take(void* dst, size_t n)
    {
        if (!mOk || (size_t)(mEnd - mP) < n)
        {
            mOk = false;
            return false;
        }
        memcpy(dst, mP, n);
        mP += n;
        return true;
    }
    U32 u32() { U32 v = 0; take(&v, 4); return v; }
    U64 u64() { U64 v = 0; take(&v, 8); return v; }
    std::string str()
    {
        const U32 n = u32();
        if (!mOk || (size_t)(mEnd - mP) < n)
        {
            mOk = false;
            return std::string();
        }
        std::string s(mP, n);
        mP += n;
        return s;
    }
    std::vector<U32> words()
    {
        const U32 n = u32();
        if (!mOk || (size_t)(mEnd - mP) < (size_t)n * 4)
        {
            mOk = false;
            return {};
        }
        std::vector<U32> w(n);
        memcpy(w.data(), mP, (size_t)n * 4);
        mP += (size_t)n * 4;
        return w;
    }
    std::vector<std::string> strs()
    {
        std::vector<std::string> v;
        const U32 n = u32();
        for (U32 i = 0; i < n && mOk; ++i)
        {
            v.push_back(str());
        }
        return v;
    }
};

bool native_cache_on()
{
    static const bool on = !(getenv("LL_VK_NATIVE_CACHE") && getenv("LL_VK_NATIVE_CACHE")[0] == '0');
    return on;
}

// what the entry of a program is named after: its name, its stage files and the defines it is built with (the files' contents are checked when the entry is read)
std::string native_cache_path(const std::string& name, const std::string stage_files[3], const std::vector<std::pair<std::string, std::string>>& defines, U64& key)
{
    std::string text = name;
    for (int s = 0; s < 3; ++s)
    {
        text += '\n';
        text += stage_files[s];
    }
    for (const auto& d : defines)
    {
        text += '\n';
        text += d.first + "=" + d.second;
    }
    key = fnv1a(text.data(), text.size());
    static const std::string dir = []() {
        const std::string d = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "native_spirv");
        LLFile::mkdir(d);
        return d;
    }();
    char hex[32];
    snprintf(hex, sizeof(hex), "%016llx.bin", (unsigned long long)key);
    return dir + gDirUtilp->getDirDelimiter() + hex;
}

std::atomic<U32> gNativeCacheHits{ 0 }, gNativeCacheMisses{ 0 };

bool native_cache_load(const std::string& path, U64 key, const std::string& root, ProgramInfo& out, std::vector<std::string>* files)
{
    std::string bytes;
    if (!read_file(path, bytes))
    {
        return false;
    }
    CacheReader r(bytes);
    if (r.u32() != 0x534e4c4cu || r.u32() != kNativeCacheVersion || r.str() != kNativeCacheTag || r.u64() != key)
    {
        return false;
    }
    std::vector<std::string> read_files;
    const U32 nfiles = r.u32();
    for (U32 i = 0; i < nfiles && r.mOk; ++i)
    {
        const std::string rel = r.str();
        const U64 want = r.u64();
        U64 have = 0;
        if (!r.mOk || !file_content_hash(root + "/" + rel, have) || have != want)
        {
            return false;
        }
        read_files.push_back(root + "/" + rel);
    }
    ProgramInfo info;
    const U32 nu = r.u32();
    for (U32 i = 0; i < nu && r.mOk; ++i)
    {
        LooseUniform u;
        u.mName = r.str();
        u.mType = r.str();
        u.mArraySize = r.u32();
        u.mOffset = r.u32();
        u.mClass = (U8)r.u32();
        u.mSize = r.u32();
        u.mInit = r.str();
        info.mUniforms.push_back(u);
    }
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        info.mBlockSizes[k] = r.u32();
    }
    const U32 ns = r.u32();
    for (U32 i = 0; i < ns && r.mOk; ++i)
    {
        SamplerInfo s;
        s.mName = r.str();
        s.mType = r.str();
        s.mBinding = r.u32();
        s.mIndexUniform = (S32)r.u32();
        info.mSamplers.push_back(s);
    }
    info.mBindless = r.u32() != 0;
    info.mUsesRt = r.u32() != 0;
    info.mAttributes = r.strs();
    {
        const U32 n = r.u32();
        for (U32 i = 0; i < n && r.mOk; ++i)
        {
            info.mAttributeLocations.push_back(r.u32());
        }
    }
    info.mAttributeTypes = r.strs();
    info.mOutputs = r.strs();
    for (int s = 0; s < 3; ++s)
    {
        info.mSpirv[s] = r.words();
    }
    {
        const U32 n = r.u32();
        for (U32 i = 0; i < n && r.mOk; ++i)
        {
            const U32 id = r.u32();
            const U32 value = r.u32();
            info.mSpec.push_back({ id, value });
        }
    }
    info.mError = r.str();
    if (!r.mOk || r.mP != r.mEnd || (info.mSpirv[0].empty() && info.mSpirv[1].empty()))
    {
        return false;
    }
    out = std::move(info);
    if (files)
    {
        *files = read_files;
    }
    return true;
}

void native_cache_store(const std::string& path, U64 key, const std::string& root, const std::vector<std::string>& read_files, const ProgramInfo& info)
{
    CacheWriter w;
    w.u32(0x534e4c4cu);
    w.u32(kNativeCacheVersion);
    w.str(kNativeCacheTag);
    w.u64(key);
    std::vector<std::pair<std::string, U64>> hashes;
    const std::string prefix = root + "/";
    for (const std::string& f : read_files)
    {
        if (f.compare(0, prefix.size(), prefix) != 0)
        {
            return;         // (a file outside the native directory: not cached)
        }
        bool dup = false;
        for (const auto& h : hashes)
        {
            dup = dup || h.first == f;
        }
        U64 hash = 0;
        if (!dup && !file_content_hash(f, hash))
        {
            return;
        }
        if (!dup)
        {
            hashes.push_back({ f, hash });
        }
    }
    w.u32((U32)hashes.size());
    for (const auto& h : hashes)
    {
        w.str(h.first.substr(prefix.size()));
        w.u64(h.second);
    }
    w.u32((U32)info.mUniforms.size());
    for (const LooseUniform& u : info.mUniforms)
    {
        w.str(u.mName);
        w.str(u.mType);
        w.u32(u.mArraySize);
        w.u32(u.mOffset);
        w.u32(u.mClass);
        w.u32(u.mSize);
        w.str(u.mInit);
    }
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        w.u32(info.mBlockSizes[k]);
    }
    w.u32((U32)info.mSamplers.size());
    for (const SamplerInfo& s : info.mSamplers)
    {
        w.str(s.mName);
        w.str(s.mType);
        w.u32(s.mBinding);
        w.u32((U32)s.mIndexUniform);
    }
    w.u32(info.mBindless ? 1u : 0u);
    w.u32(info.mUsesRt ? 1u : 0u);
    w.u32((U32)info.mAttributes.size());
    for (const std::string& s : info.mAttributes) { w.str(s); }
    w.u32((U32)info.mAttributeLocations.size());
    for (U32 l : info.mAttributeLocations) { w.u32(l); }
    w.u32((U32)info.mAttributeTypes.size());
    for (const std::string& s : info.mAttributeTypes) { w.str(s); }
    w.u32((U32)info.mOutputs.size());
    for (const std::string& s : info.mOutputs) { w.str(s); }
    for (int s = 0; s < 3; ++s)
    {
        w.words(info.mSpirv[s]);
    }
    w.u32((U32)info.mSpec.size());
    for (const auto& sp : info.mSpec)
    {
        w.u32(sp.first);
        w.u32(sp.second);
    }
    w.str(info.mError);
    const std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb"))
    {
        const bool ok = fwrite(w.mBytes.data(), 1, w.mBytes.size(), f) == w.mBytes.size();
        fclose(f);
        if (ok)
        {
            remove(path.c_str());
            rename(tmp.c_str(), path.c_str());
        }
        else
        {
            remove(tmp.c_str());
        }
    }
}

std::string stage_head(int stage, const std::vector<std::pair<std::string, std::string>>& defines)
{
    std::string head = "#version 460\n#extension GL_GOOGLE_cpp_style_line_directive : enable\n#extension GL_GOOGLE_include_directive : require\n";
    head += stage == 0 ? "#define LL_VERTEX_STAGE 1\n" : (stage == 1 ? "#define LL_FRAGMENT_STAGE 1\n" : "#define LL_GEOMETRY_STAGE 1\n");
    for (const auto& d : defines)
    {
        head += "#define " + d.first + " " + d.second + "\n";
        if (d.first == "LL_RT_QUERY")
        {
            // a program that traces rays against the frame's scene (the manifest define LL_RT_QUERY; native/lib/rt_scene.glsl declares set 3): the extension has to come before any code, so the head has it
            head += "#extension GL_EXT_ray_query : require\n";
        }
    }
    head += "#include \"ll_native.glsl\"\n";
    return head;
}

} // namespace

bool compileNativeProgram(const std::string& name, const std::string& root, const std::string& vertex_file, const std::string& fragment_file,
                          const std::vector<std::pair<std::string, std::string>>& defines, ProgramInfo& out, std::vector<std::string>* files, std::string* error, const std::string& geometry_file)
{
    const auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& why)
    {
        out.mError = why;
        if (error)
        {
            *error = why;
        }
        LL_WARNS("VkNative") << "native program '" << name << "': " << why << LL_ENDL;
        return false;
    };
    // the compile cache: an entry whose every file is as it was is the answer (no glslang, no parse)
    const std::string cache_stage_files[3] = { vertex_file, fragment_file, geometry_file };
    std::string cache_path;
    U64 cache_key = 0;
    if (native_cache_on())
    {
        cache_path = native_cache_path(name, cache_stage_files, defines, cache_key);
        if (native_cache_load(cache_path, cache_key, root, out, files))
        {
            const U32 hits = ++gNativeCacheHits;
            if (hits % 100 == 0)
            {
                LL_INFOS("VkNative") << "native compile cache: " << hits << " programs from the cache, " << gNativeCacheMisses.load() << " compiled" << LL_ENDL;
            }
            return true;
        }
        ++gNativeCacheMisses;
    }
    static const bool glslang_ready = glslang::InitializeProcess();
    if (!glslang_ready)
    {
        return fail("glslang did not start");
    }
    out = ProgramInfo();
    std::vector<std::string> read_files;
    NativeIncluder includer(root, &read_files);
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    std::vector<std::string> keep;      // the text glslang points into
    glslang::TProgram program;
    bool have_stage[3] = { false, false, false };
    size_t body_index[3] = { 0, 0, 0 };        // where the stage's source is in 'keep'
    std::vector<std::string> stage_includes[3];     // the files the stage's #include lines read (a vertex input declared in an include is a vertex input of the program)
    const std::string stage_files[3] = { vertex_file, fragment_file, geometry_file };
    const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
    keep.reserve(8);
    for (int s = 0; s < 3; ++s)
    {
        if (stage_files[s].empty())
        {
            continue;
        }
        const std::string path = root + "/" + stage_files[s];
        std::string body;
        if (!read_file(path, body))
        {
            return fail("cannot read " + path);
        }
        read_files.push_back(path);
        const size_t includes_from = read_files.size();
        const EShLanguage lang = s == 0 ? EShLangVertex : (s == 1 ? EShLangFragment : EShLangGeometry);
        keep.push_back(stage_head(s, defines));
        keep.push_back(body);
        body_index[s] = keep.size() - 1;
        const char* strings[2] = { keep[keep.size() - 2].c_str(), keep[keep.size() - 1].c_str() };
        const int lengths[2] = { (int)keep[keep.size() - 2].size(), (int)keep[keep.size() - 1].size() };
        const char* names[2] = { "native-prelude", stage_files[s].c_str() };
        auto sh = std::make_unique<glslang::TShader>(lang);
        sh->setStringsWithLengthsAndNames(strings, lengths, names, 2);
        sh->setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 460);
        sh->setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
        sh->setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
        if (!sh->parse(GetDefaultResources(), 460, ECoreProfile, false, false, msgs, includer))
        {
            return fail("compile failed for " + stage_files[s] + ": " + sh->getInfoLog());
        }
        stage_includes[s].assign(read_files.begin() + includes_from, read_files.end());
        program.addShader(sh.get());
        shaders.push_back(std::move(sh));
        have_stage[s] = true;
    }
    if (!have_stage[0] && !have_stage[1])
    {
        return fail("no stages");
    }
    if (have_stage[2] && !(have_stage[0] && have_stage[1]))
    {
        return fail("a geometry stage needs a vertex and a fragment stage");
    }
    if (!program.link(msgs))
    {
        return fail(std::string("link failed: ") + program.getInfoLog());
    }
    program.buildReflection(EShReflectionAllBlockVariables);

    // the loose blocks and the members in them
    for (int bi = 0; bi < program.getNumUniformBlocks(); ++bi)
    {
        for (U32 k = 0; k < UC_COUNT; ++k)
        {
            if (program.getUniformBlock(bi).name == kBlockNames[k])
            {
                out.mBlockSizes[k] = (U32)program.getUniformBlock(bi).size;
            }
        }
    }
    for (int ui = 0; ui < program.getNumUniformVariables(); ++ui)
    {
        const glslang::TObjectReflection& r = program.getUniform(ui);
        // a block without an instance name reports its members by their own names; the block they belong to says the class
        const int bidx = program.getUniformBlockIndex(ui);
        if (bidx < 0)
        {
            continue;
        }
        U32 k = UC_COUNT;
        for (U32 c = 0; c < UC_COUNT; ++c)
        {
            if (program.getUniformBlock(bidx).name == kBlockNames[c])
            {
                k = c;
            }
        }
        if (k == UC_COUNT)
        {
            continue;
        }
        std::string member = r.name;
        const std::string prefix = std::string(kBlockNames[k]) + ".";
        if (member.compare(0, prefix.size(), prefix) == 0)
        {
            member = member.substr(prefix.size());
        }
        const size_t br = member.find('[');
        if (br != std::string::npos)
        {
            member.resize(br);
        }
        bool seen = false;
        for (const LooseUniform& e : out.mUniforms)
        {
            seen = seen || e.mName == member;
        }
        if (seen)
        {
            continue;
        }
        LooseUniform lu;
        lu.mName = member;
        lu.mType = glsl_type_name(r.glDefineType);
        // glslang names an array member without its [0] here: a stride says it is an array and the size is the number of elements
        lu.mArraySize = (br != std::string::npos || r.arrayStride > 0) ? (U32)std::max(r.size, 1) : 0;
        if (lu.mArraySize > 0)
        {
            LL_INFOS("VkNative") << "native program '" << name << "': array uniform " << member << " " << lu.mType << "[" << lu.mArraySize << "] (reflection size " << r.size << ", stride " << r.arrayStride << ", offset " << r.offset << ")" << LL_ENDL;
        }
        lu.mOffset = (U32)r.offset;
        lu.mClass = (U8)k;
        lu.mSize = std140_bytes(lu.mType, lu.mArraySize);
        out.mUniforms.push_back(lu);
    }
    if (out.mUniforms.empty())
    {
        std::string dump;
        for (int bi = 0; bi < program.getNumUniformBlocks(); ++bi) dump += " block[" + program.getUniformBlock(bi).name + "]";
        for (int ui = 0; ui < program.getNumUniformVariables(); ++ui) dump += " uniform[" + program.getUniform(ui).name + "]";
        LL_INFOS("VkNative") << "native program '" << name << "': no loose uniforms found; the reflection has:" << dump << LL_ENDL;
    }
    // texture slots: `uint ll_ti_<name>` in the draw block (ll_ti_cube_<name>: a cube map)
    for (size_t i = 0; i < out.mUniforms.size(); ++i)
    {
        const LooseUniform& u = out.mUniforms[i];
        if (u.mClass != UC_DRAW || u.mName.compare(0, 6, "ll_ti_") != 0)
        {
            continue;
        }
        SamplerInfo si;
        si.mName = u.mName.substr(6);
        si.mType = "sampler2D";
        if (si.mName.compare(0, 10, "cubearray_") == 0)
        {
            si.mName = si.mName.substr(10);
            si.mType = "samplerCubeArray";      // `ll_ti_cubearray_<name>`: a cube map array (the reflection probes)
        }
        else if (si.mName.compare(0, 5, "cube_") == 0)
        {
            si.mName = si.mName.substr(5);
            si.mType = "samplerCube";
        }
        else if (si.mName.compare(0, 7, "shadow_") == 0)
        {
            si.mName = si.mName.substr(7);
            si.mType = "sampler2DShadow";       // a depth texture read with a comparison
        }
        si.mBinding = (U32)out.mSamplers.size();
        si.mIndexUniform = (S32)i;
        out.mSamplers.push_back(si);
    }
    out.mBindless = !out.mSamplers.empty();
    for (const std::string& text : keep)
    {
        if (text.find("ll_tbl") != std::string::npos)
        {
            out.mBindless = true;       // a program that reads the tables with a slot it is given (a varying) has no ll_ti_ sampler but needs the bindless set
        }
    }
    // vertex inputs and fragment outputs are read from the declarations (explicit locations are the contract)
    {
        static const std::regex in_re(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*in\s+(\w+)\s+(\w+)\s*;)");
        static const std::regex out_re(R"(layout\s*\(\s*location\s*=\s*(\d+)\s*\)\s*out\s+(\w+)\s+(\w+)\s*;)");
        if (have_stage[0])
        {
            // the vertex shader's own text and the files its #include lines read (lib/objectskin.glsl declares the weights): a declaration in an include was never seen here, and the program drew with no weights
            std::string body = keep[body_index[0]];
            for (const std::string& inc : stage_includes[0])
            {
                std::string text;
                if (read_file(inc, text))
                {
                    body += '\n';
                    body += text;
                }
            }
            for (std::sregex_iterator it(body.begin(), body.end(), in_re), end; it != end; ++it)
            {
                out.mAttributeLocations.push_back((U32)atoi((*it)[1].str().c_str()));
                out.mAttributeTypes.push_back((*it)[2].str());
                out.mAttributes.push_back((*it)[3].str());
            }
        }
        if (have_stage[1])
        {
            const std::string& body = keep[body_index[1]];
            for (std::sregex_iterator it(body.begin(), body.end(), out_re), end; it != end; ++it)
            {
                out.mOutputs.push_back((*it)[3].str());
            }
        }
    }

    for (int s = 0; s < 3; ++s)
    {
        if (!have_stage[s])
        {
            continue;
        }
        glslang::TIntermediate* inter = program.getIntermediate(s == 0 ? EShLangVertex : (s == 1 ? EShLangFragment : EShLangGeometry));
        if (!inter)
        {
            return fail("no intermediate for a stage");
        }
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.validate = false;
        glslang::GlslangToSpv(*inter, out.mSpirv[s], &logger, &options);
        if (out.mSpirv[s].empty())
        {
            return fail("SPIR-V generation produced nothing: " + logger.getAllMessages());
        }
    }
    for (const auto& d : defines)
    {
        out.mUsesRt = out.mUsesRt || d.first == "LL_RT_QUERY";       // (the draw binds the ray tracing descriptor set 3: the frame's scene)
    }
    if (files)
    {
        *files = read_files;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const size_t words = out.mSpirv[0].size() + out.mSpirv[1].size() + out.mSpirv[2].size();
    LL_INFOS("VkNative") << "native program '" << name << "': " << out.mSpirv[0].size() << " + " << out.mSpirv[1].size() << (have_stage[2] ? " + " + std::to_string(out.mSpirv[2].size()) : std::string()) << " SPIR-V words, " << out.mUniforms.size() << " uniforms, " << out.mSamplers.size()
        << " texture slots, compiled in " << ms << " ms" << LL_ENDL;
    if (words > 40000 || ms > 150.0)
    {
        LL_WARNS("VkNative") << "native program '" << name << "' is over the size budget (40000 SPIR-V words, 150 ms): split it" << LL_ENDL;
    }
    if (native_cache_on())
    {
        native_cache_store(cache_path, cache_key, root, read_files, out);
    }
    return true;
}

// ---- the manifest: the native program of each line ----------------------------------------------------------------------------
// (Every program is built from its manifest line by LLShaderProgram::link: nativeOnlyLine. There is no stock program behind a native one and no swap at the draw any more.)

namespace
{

// rt-debug.txt keys (the file the other live keys use), read at most twice a second
int debug_key(const std::string& key, int def)
{
    struct Entry { int mValue; std::chrono::steady_clock::time_point mRead; };
    static std::unordered_map<std::string, Entry> cache;
    static std::mutex cache_lock;       // (the main thread asks for keys too)
    std::lock_guard<std::mutex> guard(cache_lock);
    const auto now = std::chrono::steady_clock::now();
    auto it = cache.find(key);
    if (it != cache.end() && now - it->second.mRead < std::chrono::milliseconds(500))
    {
        return it->second.mValue;
    }
    static const std::string path = []() { const char* e = getenv("LL_VK_RT_DEBUG_FILE"); return std::string(e && e[0] ? e : "<work>/rt-debug.txt"); }();
    int value = def;
    std::string text;
    if (read_file(path, text))
    {
        std::istringstream in(text);
        std::string line;
        const std::string prefix = key + "=";
        while (std::getline(in, line))
        {
            if (line.compare(0, prefix.size(), prefix) == 0)
            {
                value = atoi(line.c_str() + prefix.size());
            }
        }
    }
    cache[key] = { value, now };
    return value;
}

struct Def
{
    std::string mTarget, mVertex, mFragment, mFamily, mGeometry;       // mGeometry: the option `geometry <file>` (a third stage, optional)
    std::vector<std::pair<std::string, std::string>> mDefines;
    bool mDefaultOn = false;                                // `default on` in the manifest
    bool mRtQuery = false;                                  // `rtquery` in the manifest: in a session with ray queries the program is built with LL_RT_QUERY (the ray query code of lib/rt.glsl, the extension and the draw's set 3: PLAN-native-only-end-state.md, A3)
    bool mNativeOnly = false;                               // set by load_manifest: a converted family, the only line of its program, default on
};

struct Manager
{
    std::string mRoot;
    std::vector<std::unique_ptr<Def>> mDefs;
    bool mReady = false;
};

std::string trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    size_t b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::atomic<bool> gRtSession(false);        // set by nativeSetRtSession: this session has ray queries

// <FS> shader cleanup (stage 4): the families whose programs are native only (no stock program behind them). One line per step of DESIGN-stock-carrier-removal.md section 4.
bool native_only_family(const std::string& family)
{
    static const char* const kNativeOnlyFamilies[] = { "copy", "blur", "post", "resolve", "alpha", "gbuffer", "shadow", "light", "sun", "water" };
    for (const char* f : kNativeOnlyFamilies)
    {
        if (family == f)
        {
            return true;
        }
    }
    return false;
}

void load_manifest(Manager& m)
{
    m.mReady = true;
    const char* e = getenv("LL_VK_NATIVE_DIR");
    if (e && e[0])
    {
        m.mRoot = e;
    }
    else
    {
        char exe[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exe, MAX_PATH);
        std::string dir = exe;
        dir.resize(dir.find_last_of("\\/"));
        m.mRoot = dir + "/app_settings/shaders/native";
    }
    for (char& c : m.mRoot)
    {
        if (c == '\\') c = '/';
    }
    std::string text;
    if (!read_file(m.mRoot + "/manifest.txt", text))
    {
        LL_INFOS("VkNative") << "no native shaders (" << m.mRoot << "/manifest.txt)" << LL_ENDL;
        return;
    }
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line))
    {
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        std::vector<std::string> f;
        std::istringstream ls(line);
        std::string part;
        while (std::getline(ls, part, '|'))
        {
            f.push_back(trim(part));
        }
        if (f.size() < 5 || f[0] != "override")
        {
            continue;
        }
        auto d = std::make_unique<Def>();
        d->mTarget = f[1];
        d->mVertex = f[2];
        d->mFragment = f[3];
        d->mFamily = f[4];
        for (size_t i = 5; i < f.size(); ++i)
        {
            const size_t sp = f[i].find(' ');
            const std::string kind = f[i].substr(0, sp);
            const std::string rest = sp == std::string::npos ? "" : trim(f[i].substr(sp + 1));
            if (kind == "default" && rest == "on")
            {
                d->mDefaultOn = true;
                continue;
            }
            if (kind == "rtquery")
            {
                d->mRtQuery = true;
                continue;
            }
            if (kind == "geometry")
            {
                d->mGeometry = rest;
                continue;
            }
            const size_t eq = rest.find('=');
            if (kind == "define" && eq != std::string::npos)
            {
                d->mDefines.push_back({ rest.substr(0, eq), rest.substr(eq + 1) });
            }
        }
        m.mDefs.push_back(std::move(d));
    }
    for (size_t i = 0; i < m.mDefs.size(); ++i)
    {
        Def& d = *m.mDefs[i];
        int lines = 0;
        for (const auto& other : m.mDefs)
        {
            if (other->mTarget == d.mTarget)
            {
                ++lines;
            }
        }
        d.mNativeOnly = native_only_family(d.mFamily) && lines == 1 && d.mDefaultOn;
    }
    LL_INFOS("VkNative") << m.mDefs.size() << " native program(s) in " << m.mRoot << "/manifest.txt" << LL_ENDL;
}

// the viewer's global defines (set from the main thread, read by the worker)
std::mutex gGlobalLock;
std::vector<std::pair<std::string, std::string>> gGlobalDefines;

// <FS> shader cleanup (stage 4 step 1): the lookup of createShader (main thread), with its own parse of the manifest and its own lock
std::mutex gLookupLock;
Manager gLookup;

} // namespace

void nativeSetGlobalDefines(const std::vector<std::pair<std::string, std::string>>& defines)
{
    std::lock_guard<std::mutex> guard(gGlobalLock);
    gGlobalDefines = defines;
}

const std::string& nativeRoot()
{
    std::lock_guard<std::mutex> guard(gLookupLock);
    if (!gLookup.mReady)
    {
        load_manifest(gLookup);
    }
    return gLookup.mRoot;
}

bool nativeOnlyLine(const std::string& program, std::string& vertex, std::string& fragment, std::vector<std::pair<std::string, std::string>>& defines, std::string& geometry)
{
    std::lock_guard<std::mutex> guard(gLookupLock);
    if (!gLookup.mReady)
    {
        load_manifest(gLookup);
    }
    const Def* line = nullptr;
    int count = 0;
    for (const auto& d : gLookup.mDefs)
    {
        if (d->mTarget == program)
        {
            ++count;
            if (!line)
            {
                line = d.get();
            }
        }
    }
    if (!line || count != 1 || !line->mNativeOnly)
    {
        return false;
    }
    vertex = line->mVertex;
    fragment = line->mFragment;
    geometry = line->mGeometry;
    defines = line->mDefines;
    if (line->mRtQuery && gRtSession.load())
    {
        defines.push_back({ "LL_RT_QUERY", "1" });      // a session with ray queries (the manifest option rtquery): the program traces its sun shadow (compileNativeProgram adds the extension and sets mUsesRt)
    }
    return true;
}

std::vector<std::pair<std::string, std::string>> nativeGlobalDefinesCopy()
{
    std::lock_guard<std::mutex> guard(gGlobalLock);
    return gGlobalDefines;
}

void nativeSetRtSession(bool on)
{
    gRtSession = on;
}

int nativeKey(const std::string& key, int def)
{
    return debug_key(key, def);
}

} // namespace LLVk

#endif // LL_VULKAN

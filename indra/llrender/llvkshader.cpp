/**
 * @file llvkshader.cpp
 * @brief GL GLSL -> Vulkan SPIR-V program compiler (see llvkshader.h)
 */

#include "linden_common.h"

#include "llvkshader.h"
#include "llvkcontext.h"
#include "lldir.h"
#include "llfile.h"

#if defined(LL_VULKAN)

#include "llvkplatform.h"

#include <algorithm>
#include <regex>
#include <cctype>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <sstream>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/SPIRV/GlslangToSpv.h>
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace LLVk
{

namespace
{

//-----------------------------------------------------------------------------------------------------
// small text helpers
//-----------------------------------------------------------------------------------------------------

bool is_ident_char(char c)
{
    return std::isalnum((unsigned char)c) || c == '_';
}

std::string trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

bool starts_with(const std::string& s, const char* prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

//-----------------------------------------------------------------------------------------------------
// GLSL declaration parsing (only what the viewer's shaders need)
//-----------------------------------------------------------------------------------------------------

struct Declarator
{
    std::string mName;
    std::string mArray;     // "[N]" or empty
    std::string mInit;
};

struct Decl
{
    std::vector<std::string> mQuals;
    std::string mLayout;    // contents of layout( ... )
    std::string mType;
    std::string mTypeArray; // "[N]" when the array was written on the type ("vec4[10] name")
    std::vector<Declarator> mDeclarators;
    bool mFunction = false; // a function prototype, not a variable
    bool mOk = false;

    bool has(const char* q) const { return std::find(mQuals.begin(), mQuals.end(), q) != mQuals.end(); }
};

const char* const QUALIFIERS[] = {
    "uniform", "in", "out", "inout", "const", "flat", "smooth", "noperspective", "centroid", "sample", "highp", "mediump",
    "lowp", "invariant", "precise", "coherent", "volatile", "restrict", "readonly", "writeonly", "buffer", "patch",
    "varying", "attribute", "shared"
};

bool is_qualifier(const std::string& t)
{
    for (const char* q : QUALIFIERS)
    {
        if (t == q) return true;
    }
    return false;
}

bool is_opaque_type(const std::string& t)
{
    return starts_with(t, "sampler") || starts_with(t, "isampler") || starts_with(t, "usampler") || starts_with(t, "image")
        || starts_with(t, "iimage") || starts_with(t, "uimage") || starts_with(t, "texture") || starts_with(t, "atomic_uint");
}

// parses a statement that has no braces (variable declaration or function prototype)
Decl parse_decl(const std::string& text_in)
{
    Decl d;
    std::string text = trim(text_in);
    if (!text.empty() && text.back() == ';')
    {
        text.pop_back();
    }
    size_t i = 0;
    const size_t n = text.size();
    auto skip_ws = [&]() { while (i < n && std::isspace((unsigned char)text[i])) ++i; };
    auto read_ident = [&]() {
        size_t s = i;
        while (i < n && is_ident_char(text[i])) ++i;
        return text.substr(s, i - s);
    };

    for (;;)
    {
        skip_ws();
        const size_t save = i;
        std::string tok = read_ident();
        if (tok.empty())
        {
            return d;
        }
        if (tok == "layout")
        {
            skip_ws();
            if (i >= n || text[i] != '(')
            {
                return d;
            }
            int depth = 0;
            size_t s = i + 1;
            for (; i < n; ++i)
            {
                if (text[i] == '(') ++depth;
                else if (text[i] == ')' && --depth == 0) break;
            }
            if (i >= n)
            {
                return d;
            }
            if (!d.mLayout.empty()) d.mLayout += ", ";
            d.mLayout += trim(text.substr(s, i - s));
            ++i;
            continue;
        }
        if (is_qualifier(tok))
        {
            d.mQuals.push_back(tok);
            continue;
        }
        d.mType = tok;
        (void)save;
        break;
    }

    // array written on the type: "vec4[4] name"
    skip_ws();
    if (i < n && text[i] == '[')
    {
        const size_t close = text.find(']', i);
        if (close == std::string::npos)
        {
            return d;
        }
        d.mTypeArray = text.substr(i, close - i + 1);
        i = close + 1;
    }

    // the name that follows decides between function prototype and variable
    {
        size_t probe = i;
        while (probe < n && std::isspace((unsigned char)text[probe])) ++probe;
        while (probe < n && is_ident_char(text[probe])) ++probe;
        while (probe < n && std::isspace((unsigned char)text[probe])) ++probe;
        if (probe < n && text[probe] == '(')
        {
            d.mFunction = true;
            d.mOk = true;
            return d;
        }
    }

    // declarators separated by commas at depth 0
    std::string rest = text.substr(i);
    std::vector<std::string> parts;
    {
        int depth = 0;
        std::string cur;
        for (char c : rest)
        {
            if (c == '(' || c == '[' || c == '{') ++depth;
            else if (c == ')' || c == ']' || c == '}') --depth;
            if (c == ',' && depth == 0)
            {
                parts.push_back(cur);
                cur.clear();
            }
            else
            {
                cur += c;
            }
        }
        parts.push_back(cur);
    }
    for (const std::string& part_raw : parts)
    {
        std::string part = trim(part_raw);
        if (part.empty())
        {
            return d;
        }
        Declarator dc;
        size_t k = 0;
        while (k < part.size() && is_ident_char(part[k])) ++k;
        dc.mName = part.substr(0, k);
        if (dc.mName.empty())
        {
            return d;
        }
        while (k < part.size() && std::isspace((unsigned char)part[k])) ++k;
        if (k < part.size() && part[k] == '[')
        {
            size_t close = part.find(']', k);
            if (close == std::string::npos)
            {
                return d;
            }
            dc.mArray = part.substr(k, close - k + 1);
            k = close + 1;
            while (k < part.size() && std::isspace((unsigned char)part[k])) ++k;
            if (k < part.size() && part[k] == '[')
            {
                return d;   // multi-dimensional arrays are not supported
            }
        }
        if (k < part.size())
        {
            if (part[k] != '=')
            {
                return d;
            }
            dc.mInit = trim(part.substr(k + 1));
        }
        if (dc.mArray.empty())
        {
            dc.mArray = d.mTypeArray;
        }
        d.mDeclarators.push_back(dc);
    }
    d.mOk = !d.mDeclarators.empty();
    return d;
}

// removes every layout( ... ) group from a declaration head
std::string strip_layouts(const std::string& s)
{
    std::string out;
    size_t i = 0;
    while (i < s.size())
    {
        if (s.compare(i, 6, "layout") == 0 && (i == 0 || !is_ident_char(s[i - 1])))
        {
            size_t j = i + 6;
            while (j < s.size() && std::isspace((unsigned char)s[j])) ++j;
            if (j < s.size() && s[j] == '(')
            {
                int depth = 0;
                for (; j < s.size(); ++j)
                {
                    if (s[j] == '(') ++depth;
                    else if (s[j] == ')' && --depth == 0) { ++j; break; }
                }
                i = j;
                continue;
            }
        }
        out += s[i++];
    }
    return trim(out);
}

// "[N]" -> N (literal only); 0 and ok=false otherwise
U32 array_size(const std::string& arr, bool& ok)
{
    ok = true;
    if (arr.empty())
    {
        return 0;
    }
    std::string inner = trim(arr.substr(1, arr.size() - 2));
    if (inner.empty())
    {
        ok = false;
        return 0;
    }
    for (char c : inner)
    {
        if (!std::isdigit((unsigned char)c))
        {
            ok = false;
            return 0;
        }
    }
    return (U32)std::stoul(inner);
}

//-----------------------------------------------------------------------------------------------------
// type sizes: std140 for the loose uniform block, location counts for varyings
//-----------------------------------------------------------------------------------------------------

struct TypeInfo
{
    U32 mAlign = 0;
    U32 mSize = 0;          // size of one element, not padded to the array stride
    U32 mLocations = 0;
    bool mBool = false;
    bool mOk = false;
};

U32 align_up(U32 v, U32 a)
{
    return (v + a - 1) / a * a;
}

TypeInfo type_info(const std::string& t)
{
    TypeInfo ti;
    auto scalar = [&](bool is_bool) { ti.mAlign = 4; ti.mSize = 4; ti.mLocations = 1; ti.mBool = is_bool; ti.mOk = true; };
    if (t == "float" || t == "int" || t == "uint") { scalar(false); return ti; }
    if (t == "bool") { scalar(true); return ti; }

    // vecN / ivecN / uvecN / bvecN
    for (const char* prefix : { "vec", "ivec", "uvec", "bvec" })
    {
        const size_t pl = strlen(prefix);
        if (t.size() == pl + 1 && t.compare(0, pl, prefix) == 0 && t[pl] >= '2' && t[pl] <= '4')
        {
            const U32 n = (U32)(t[pl] - '0');
            ti.mSize = n * 4;
            ti.mAlign = n == 2 ? 8 : 16;
            ti.mLocations = 1;
            ti.mBool = prefix[0] == 'b';
            ti.mOk = true;
            return ti;
        }
    }

    // matN or matCxR: C columns of R floats; std140 pads every column to 16 bytes
    if (starts_with(t, "mat"))
    {
        U32 cols = 0;
        if (t.size() == 4 && t[3] >= '2' && t[3] <= '4')
        {
            cols = (U32)(t[3] - '0');
        }
        else if (t.size() == 6 && t[4] == 'x' && t[3] >= '2' && t[3] <= '4' && t[5] >= '2' && t[5] <= '4')
        {
            cols = (U32)(t[3] - '0');
        }
        if (cols)
        {
            ti.mAlign = 16;
            ti.mSize = cols * 16;
            ti.mLocations = cols;
            ti.mOk = true;
            return ti;
        }
    }
    return ti;
}

//-----------------------------------------------------------------------------------------------------
// splitting preprocessed GLSL into top-level items
//-----------------------------------------------------------------------------------------------------

struct Item
{
    std::string mText;
    bool mHasBody = false;      // contains braces (function, struct, interface block)
};

struct SplitResult
{
    std::vector<Item> mItems;
    std::vector<std::string> mExtensions;   // "#extension ..." lines
};

SplitResult split_items(const std::string& src)
{
    SplitResult r;
    const size_t n = src.size();
    size_t i = 0;
    int depth = 0;
    bool line_start = true;
    std::string cur;
    bool has_body = false;

    auto flush = [&]() {
        if (!trim(cur).empty())
        {
            r.mItems.push_back({ cur, has_body });
        }
        cur.clear();
        has_body = false;
    };

    while (i < n)
    {
        const char c = src[i];
        if (c == '#' && line_start && depth == 0 && trim(cur).empty())
        {
            size_t e = src.find('\n', i);
            if (e == std::string::npos) e = n;
            std::string line = trim(src.substr(i, e - i));
            if (starts_with(line, "#extension"))
            {
                r.mExtensions.push_back(line);
            }
            i = e;
            continue;
        }
        if (c == '\n')
        {
            line_start = true;
        }
        else if (!std::isspace((unsigned char)c))
        {
            line_start = false;
        }

        cur += c;
        ++i;
        if (c == '{')
        {
            ++depth;
            has_body = true;
        }
        else if (c == '}')
        {
            if (--depth == 0)
            {
                // what follows decides whether the item ends here (function) or continues (block instance, struct ';')
                size_t j = i;
                while (j < n && std::isspace((unsigned char)src[j])) ++j;
                size_t k = j;
                while (k < n && is_ident_char(src[k])) ++k;
                const std::string ident = src.substr(j, k - j);
                size_t m = k;
                while (m < n && std::isspace((unsigned char)src[m])) ++m;
                const char next = m < n ? src[m] : '\0';
                const bool continues = (ident.empty() && next == ';') || (!ident.empty() && (next == ';' || next == '[' || next == ','));
                if (!continues)
                {
                    flush();
                }
            }
        }
        else if (c == ';' && depth == 0)
        {
            flush();
        }
    }
    flush();
    return r;
}

//-----------------------------------------------------------------------------------------------------
// glslang plumbing
//-----------------------------------------------------------------------------------------------------

EShLanguage to_glslang_stage(StageSource::Stage s)
{
    switch (s)
    {
    case StageSource::Vertex: return EShLangVertex;
    case StageSource::Fragment: return EShLangFragment;
    default: return EShLangGeometry;
    }
}

bool init_glslang()
{
    static bool initialised = glslang::InitializeProcess();
    return initialised;
}

void set_env(glslang::TShader& sh, EShLanguage lang)
{
    sh.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
    sh.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_3);
    sh.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_6);
}

// replaces the "#version" line the viewer emits (420 etc.) with 450, the Vulkan GLSL baseline
std::string force_version_450(const std::string& src)
{
    const size_t v = src.find("#version");
    if (v == std::string::npos)
    {
        return "#version 450\n" + src;
    }
    size_t e = src.find('\n', v);
    if (e == std::string::npos) e = src.size();
    return src.substr(0, v) + "#version 450" + src.substr(e);
}

struct VaryingInfo
{
    U32 mLocation = 0;
    U32 mSize = 0;
};

struct UniformEntry
{
    std::string mName;
    std::string mType;
    std::string mArray;
    std::string mInit;
    U32 mArraySize = 0;
    U32 mOffset = 0;
    U32 mSize = 0;
    U8 mClass = UC_PASS;
};

const char* const kClassBlockNames[UC_COUNT] = { "LLLooseUniforms", "LLLooseDraw", "LLLooseMaterial", "LLLooseLight", "LLLooseSkin" };

struct SamplerEntry
{
    std::string mName;
    std::string mType;
    std::string mArray;
    U32 mBinding = 0;
    S32 mIndexUniform = -1;
};

// the table (set 2 binding) a sampler type reads, -1: not supported (the program does not compile)
static int bindless_table(const std::string& type)
{
    if (type == "sampler2D") return 0;
    if (type == "samplerCube") return 1;
    if (type == "samplerCubeArray") return 2;
    if (type == "sampler2DShadow") return 3;
    return -1;
}



struct StageWork
{
    StageSource::Stage mStage;
    std::string mName;
    SplitResult mSplit;
    bool mUsesRt = false;       // the (preprocessed) source mentions ll_rt_tlas: the stage gets the ray query extension and the set 3 declaration
};

} // anonymous namespace

//-----------------------------------------------------------------------------------------------------
// uniform classes
//-----------------------------------------------------------------------------------------------------
// The class of a loose uniform: how often its value changes (doc/vulkan/uniform-classes.tsv, measured on the plaza). Everything not listed changes about once per pass.
UniformClass uniformClassOf(const std::string& name)
{
    static const std::unordered_map<std::string, UniformClass> table = []()
    {
        std::unordered_map<std::string, UniformClass> t;
        for (const char* n : { "modelview_projection_matrix", "modelview_matrix", "texture_matrix0", "normal_matrix", "texture_matrix1" }) t[n] = UC_DRAW;
        for (const char* n : { "specular_color", "minimum_alpha", "env_intensity", "metallicFactor", "roughnessFactor", "texture_base_color_transform", "texture_emissive_transform",
                               "texture_metallic_roughness_transform", "texture_normal_transform", "emissiveColor", "emissive_brightness", "color" }) t[n] = UC_MATERIAL;
        for (const char* n : { "center", "noiseSine", "size", "box_center", "proj_mat", "proj_origin", "proj_p", "falloff", "box_size", "proj_range", "proj_near", "proj_n", "proj_focus",
                               "proj_ambiance", "proj_lod", "proj_ambient_lod", "proj_shadow_idx", "lightnorm", "light", "light_col", "light_count", "proj_shadow_res",
                               "spot_shadow_bias", "spot_shadow_offset" }) t[n] = UC_LIGHT;
        t["matrixPalette"] = UC_SKIN;
        return t;
    }();
    if (name.compare(0, 6, "ll_ti_") == 0)
    {
        return UC_DRAW;         // the texture slots of a bindless program change with every draw
    }
    auto it = table.find(name);
    return it == table.end() ? UC_PASS : it->second;
}


//-----------------------------------------------------------------------------------------------------
// compileProgram
//-----------------------------------------------------------------------------------------------------

namespace
{
std::map<std::string, std::vector<std::string>> gDebugSamplers;
}

const std::vector<std::string>* findDebugSamplers(const std::string& program_name)
{
    auto it = gDebugSamplers.find(program_name);
    return it == gDebugSamplers.end() ? nullptr : &it->second;
}

bool debugShadersEnabled()
{
    static const bool enabled = getenv("LL_VK_DEBUG_SHADERS") != nullptr;
    return enabled;
}

// Renames the fragment shader's main() and appends a new one that can overwrite frag_color: mode 1 solid magenta, 2 a sampler2D
// shown at the pixel's screen position, 3 the original alpha as grey, 4 the fragment depth as grey. Returns the source unchanged
// when the shader does not have the expected shape.
static std::string debug_wrap_fragment(const std::string& src, std::vector<std::string>& sampler_names)
{
    static const std::regex main_re(R"(\bvoid\s+main\s*\(\s*(void)?\s*\))");
    static const std::regex out_re(R"(\bout\s+vec4\s+frag_color\s*;)");
    static const std::regex sampler_re(R"(\buniform\s+sampler2D\s+(\w+)\s*;)");
    std::smatch m;
    if (!std::regex_search(src, m, main_re) || !std::regex_search(src, out_re))
    {
        return src;
    }
    std::string out = src.substr(0, m.position(0)) + "void ll_dbg_original_main()" + src.substr(m.position(0) + m.length(0));
    sampler_names.clear();
    for (auto it = std::sregex_iterator(src.begin(), src.end(), sampler_re); it != std::sregex_iterator(); ++it)
    {
        const std::string name = (*it)[1];
        if (std::find(sampler_names.begin(), sampler_names.end(), name) == sampler_names.end() && sampler_names.size() < 24)
        {
            sampler_names.push_back(name);
        }
    }
    std::ostringstream add;
    add << "\nuniform int ll_dbg_mode;\nuniform int ll_dbg_sampler;\nuniform vec2 ll_dbg_size;\n";
    add << "void main()\n{\n    ll_dbg_original_main();\n";
    add << "    if (ll_dbg_mode == 1) { frag_color = vec4(1.0, 0.0, 1.0, 1.0); }\n";
    add << "    else if (ll_dbg_mode == 3) { frag_color = vec4(vec3(frag_color.a), 1.0); }\n";
    add << "    else if (ll_dbg_mode == 4) { frag_color = vec4(vec3(gl_FragCoord.z), 1.0); }\n";
    add << "    else if (ll_dbg_mode == 2)\n    {\n        vec2 uv = gl_FragCoord.xy / ll_dbg_size;\n";
    for (size_t i = 0; i < sampler_names.size(); ++i)
    {
        add << "        " << (i ? "else " : "") << "if (ll_dbg_sampler == " << i << ") { frag_color = vec4(texture(" << sampler_names[i] << ", uv).rgb, 1.0); }\n";
    }
    add << "    }\n}\n";
    return out + add.str();
}

// H6: the pull shaders measure the need of each texture (llFbNote); LL_VK_TEXFB=0 turns that off in the shaders (the call sites stay, the function is empty)
static bool textureFeedbackShaders()
{
    // off unless LL_VK_TEXFB is set to something but 0: the pull shaders then have exactly the text they had before the measurement (see fbPlainShaders in llshadermgr.cpp)
    static const bool on = []{ const char* e = getenv("LL_VK_TEXFB"); return e && atoi(e) != 0 && !getenv("LL_VK_FB_OFF"); }();
    return on;
}
// LL_VK_TEXFB=2 (a test of the cost, not a mode to use: depth is written for fragments that are discarded afterwards): the programs that measure test depth before the shader runs
static bool textureFeedbackEarlyTests()
{
    static const bool on = []{ const char* e = getenv("LL_VK_TEXFB"); return e && atoi(e) == 2; }();
    return on;
}

static bool compileProgramUncached(const std::string& program_name, const std::vector<StageSource>& stages, const ShaderEnv& env,
                                   ProgramInfo& out, const std::string& dump_dir)
{
    out = ProgramInfo();
    if (!init_glslang())
    {
        out.mError = "glslang failed to initialise";
        return false;
    }
    const TBuiltInResource* resources = GetDefaultResources();

    auto fail = [&](const std::string& msg) {
        out.mError = msg;
        return false;
    };

    // ---- 1. preprocess every stage object and split it into top-level items ---------------------------
    std::vector<StageWork> works;
    for (const StageSource& st : stages)
    {
        StageWork w;
        w.mStage = st.mStage;
        w.mName = st.mName;

        const EShLanguage lang = to_glslang_stage(st.mStage);
        glslang::TShader pre(lang);
        const std::string versioned = force_version_450(st.mSource);
        const char* strings[1] = { versioned.c_str() };
        pre.setStrings(strings, 1);
        set_env(pre, lang);
        glslang::TShader::ForbidIncluder includer;
        std::string preprocessed;
        if (!pre.preprocess(resources, 450, ENoProfile, false, false, EShMsgDefault, &preprocessed, includer))
        {
            return fail("preprocess failed for " + st.mName + ": " + pre.getInfoLog());
        }
        if (st.mStage == StageSource::Fragment && debugShadersEnabled())
        {
            // on the preprocessed text, so declarations in inactive #if branches are not mistaken for real ones
            std::vector<std::string> sampler_names;
            const std::string wrapped = debug_wrap_fragment(preprocessed, sampler_names);
            if (wrapped.size() != preprocessed.size())
            {
                preprocessed = wrapped;
                gDebugSamplers[program_name] = sampler_names;
            }
        }
        w.mUsesRt = preprocessed.find("ll_rt_tlas") != std::string::npos;
        w.mSplit = split_items(preprocessed);
        works.push_back(std::move(w));
    }

    // ---- 2. collect what the program declares ----------------------------------------------------------
    std::vector<UniformEntry> uniforms;                 // in order of first appearance
    std::map<std::string, size_t> uniform_index;
    std::vector<SamplerEntry> samplers;
    std::map<std::string, size_t> sampler_index;
    std::map<std::string, VaryingInfo> varyings;        // vertex out / fragment in, by name
    std::map<std::string, VaryingInfo> frag_outputs;
    U32 next_varying = 0;
    U32 next_frag_output = 0;
    std::set<std::string> attributes;
    std::map<std::string, std::string> attribute_types;
    bool has_own_table = false;                         // the shader declares a set-2 table itself (the pull shaders: the textures of their rows)
    std::string own_table_name, own_table_type;         // ... its name and type: the translated samplers use it as table 0 (no second declaration at binding 0)

    for (const StageWork& w : works)
    {
        for (const Item& item : w.mSplit.mItems)
        {
            if (item.mHasBody)
            {
                continue;
            }
            Decl d = parse_decl(item.mText);
            if (!d.mOk || d.mFunction)
            {
                continue;
            }

            if (d.has("uniform"))
            {
                for (const Declarator& dc : d.mDeclarators)
                {
                    if (is_opaque_type(d.mType))
                    {
                        if (d.mLayout.find("set") != std::string::npos)
                        {
                            has_own_table = true;
                            own_table_name = dc.mName;
                            own_table_type = d.mType;
                            continue;       // <FS> a sampler array with a layout of its own (the bindless table, set 2): not one of the program's samplers
                        }
                        if (!sampler_index.count(dc.mName))
                        {
                            sampler_index[dc.mName] = samplers.size();
                            SamplerEntry s;
                            s.mName = dc.mName;
                            s.mType = d.mType;
                            s.mArray = dc.mArray;
                            s.mBinding = (U32)samplers.size();
                            samplers.push_back(s);
                        }
                        continue;
                    }
                    auto it = uniform_index.find(dc.mName);
                    if (it != uniform_index.end())
                    {
                        if (uniforms[it->second].mType != d.mType || uniforms[it->second].mArray != dc.mArray)
                        {
                            return fail("uniform '" + dc.mName + "' is declared with different types in " + w.mName);
                        }
                        continue;
                    }
                    bool ok = true;
                    UniformEntry u;
                    u.mName = dc.mName;
                    u.mType = d.mType;
                    u.mArray = dc.mArray;
                    u.mInit = dc.mInit;
                    u.mArraySize = array_size(dc.mArray, ok);
                    if (!ok)
                    {
                        return fail("uniform '" + dc.mName + "' has a non-literal array size '" + dc.mArray + "' in " + w.mName);
                    }
                    uniform_index[dc.mName] = uniforms.size();
                    uniforms.push_back(u);
                }
            }
            else if (d.has("in") || d.has("out"))
            {
                const bool is_in = d.has("in");
                for (const Declarator& dc : d.mDeclarators)
                {
                    bool ok = true;
                    U32 count = 1;
                    if (w.mStage == StageSource::Geometry && dc.mArray == "[]")
                    {
                        count = 1;      // per-vertex input arrays: one location per element type
                    }
                    else
                    {
                        count = std::max<U32>(1, array_size(dc.mArray, ok));
                    }
                    const TypeInfo ti = type_info(d.mType);
                    if (!ok || !ti.mOk)
                    {
                        return fail("cannot size varying '" + dc.mName + "' (" + d.mType + dc.mArray + ") in " + w.mName);
                    }
                    const U32 size = ti.mLocations * count;
                    if (w.mStage == StageSource::Vertex)
                    {
                        if (is_in)
                        {
                            attributes.insert(dc.mName);
                            attribute_types[dc.mName] = d.mType;
                        }
                        else if (!varyings.count(dc.mName))
                        {
                            varyings[dc.mName] = { next_varying, size };
                            next_varying += size;
                        }
                    }
                    else if (w.mStage == StageSource::Geometry)
                    {
                        if (!varyings.count(dc.mName))
                        {
                            varyings[dc.mName] = { next_varying, size };
                            next_varying += size;
                        }
                    }
                    else if (w.mStage == StageSource::Fragment)
                    {
                        if (is_in)
                        {
                            if (!varyings.count(dc.mName))
                            {
                                varyings[dc.mName] = { next_varying, size };
                                next_varying += size;
                            }
                        }
                        else if (!frag_outputs.count(dc.mName))
                        {
                            frag_outputs[dc.mName] = { next_frag_output, size };
                            next_frag_output += size;
                        }
                    }
                }
            }
        }
    }

    // <FS> bindless samplers: every sampler of the program is a plain 2D / cube / cube array / 2D shadow sampler (not an array), no geometry stage, no table of its own.
    // Each sampler becomes a table slot read from a loose uint uniform (ll_ti_<name>); the viewer's sampler name is a macro for the table element. A program with a 2D table of its own (the pull shaders:
    // the textures of the rows) takes it as table 0 and has its fixed samplers (the deferred buffers, the shadow maps) translated as well.
    bool bindless = !samplers.empty() && (!has_own_table || own_table_type == "sampler2D");
    for (const SamplerEntry& s : samplers)
    {
        if (!s.mArray.empty() || bindless_table(s.mType) < 0)
        {
            bindless = false;
        }
    }
    for (const StageWork& w : works)
    {
        if (w.mStage == StageSource::Geometry)
        {
            bindless = false;
        }
    }
    if (!bindless && !samplers.empty())
    {
        // every texture is read through the handle map: a program whose samplers cannot be translated cannot draw
        std::string why = has_own_table ? "declares a table of its own that is not a plain 2D sampler array" : "";
        for (const SamplerEntry& s : samplers)
        {
            if (!s.mArray.empty()) why = "sampler array " + s.mName + s.mArray;
            else if (bindless_table(s.mType) < 0) why = "sampler type '" + s.mType + "' (" + s.mName + ")";
        }
        for (const StageWork& w : works)
        {
            if (w.mStage == StageSource::Geometry) why = "geometry stage";
        }
        return fail("its samplers cannot be read through the texture handles: " + why);
    }
    std::ostringstream bindless_decl;
    if (bindless)
    {
        static const char* const table_types[4] = { "sampler2D", "samplerCube", "samplerCubeArray", "sampler2DShadow" };
        bool used[4] = {};
        for (SamplerEntry& s : samplers)
        {
            used[bindless_table(s.mType)] = true;
            UniformEntry u;
            u.mName = "ll_ti_" + s.mName;
            u.mType = "uint";
            s.mIndexUniform = (S32)uniforms.size();
            uniform_index[u.mName] = uniforms.size();
            uniforms.push_back(u);
        }
        for (const StageWork& w : works)
        {
            if (w.mUsesRt)
            {
                used[0] = true;         // the ray query alpha test reads the 2D table (ll_rt_tex)
            }
        }
        for (int t = 0; t < 4; ++t)
        {
            if (used[t])
            {
                if (t == 0 && has_own_table)
                {
                    // the program has a table of its own (declared in the source of one of its stages): it is table 0, declared here for every stage (the source's own declaration is left out below)
                    bindless_decl << "layout(set = 2, binding = 0) uniform sampler2D " << own_table_name << "[];\n#define ll_vk_tbl0 " << own_table_name << "\n";
                }
                else
                {
                    bindless_decl << "layout(set = 2, binding = " << t << ") uniform " << table_types[t] << " ll_vk_tbl" << t << "[];\n";
                }
            }
        }
        if (has_own_table && !used[0])
        {
            bindless_decl << "layout(set = 2, binding = 0) uniform sampler2D " << own_table_name << "[];\n#define ll_vk_tbl0 " << own_table_name << "\n";
        }
        for (const SamplerEntry& s : samplers)
        {
            bindless_decl << "#define " << s.mName << " ll_vk_tbl" << bindless_table(s.mType) << "[LL_HM(ll_ti_" << s.mName << ")]\n";
        }
    }

    // <FS> texture handles: LL_HM(id) is the descriptor index of the texture id (the frame's map, set 2 binding 4). Defined for every program (the pull
    // shaders declare the table themselves and use it in code the shader manager injects); the map is declared where a program has a table of its own, translated samplers or ray queries.
    std::ostringstream handle_decl;
    {
        bool uses_rt = false;
        for (const StageWork& w : works)
        {
            uses_rt = uses_rt || w.mUsesRt;
        }
        if (bindless || has_own_table || uses_rt)
        {
            handle_decl << "layout(std430, set = 2, binding = 4) readonly buffer LlHandleMap { uint ll_hm[]; };\n#define LL_HM(id) ll_hm[(id)]\n";
        }
        else
        {
            handle_decl << "#define LL_HM(id) (id)\n";
        }
    }

    // std140 layout of the loose uniform blocks: one block per class, each with its own offsets
    U32 class_offset[UC_COUNT] = {};
    bool class_used[UC_COUNT] = {};
    for (UniformEntry& u : uniforms)
    {
        const TypeInfo ti = type_info(u.mType);
        if (!ti.mOk)
        {
            return fail("uniform '" + u.mName + "' has unsupported type '" + u.mType + "'");
        }
        u.mClass = uniformClassOf(u.mName);
        class_used[u.mClass] = true;
        U32& offset = class_offset[u.mClass];
        if (u.mArraySize > 0)
        {
            const U32 stride = align_up(ti.mSize, 16);
            offset = align_up(offset, 16);
            u.mOffset = offset;
            u.mSize = stride * u.mArraySize;
        }
        else
        {
            offset = align_up(offset, ti.mAlign);
            u.mOffset = offset;
            u.mSize = ti.mSize;
        }
        offset += u.mSize;
    }
    U32 block_sizes[UC_COUNT] = {};
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        block_sizes[k] = class_used[k] ? align_up(std::max<U32>(class_offset[k], 16), 16) : 0;
    }

    std::ostringstream block;
    std::ostringstream bool_defines;
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        if (!class_used[k])
        {
            continue;
        }
        block << "layout(std140, set = 0, binding = " << UC_BINDING[k] << ") uniform " << kClassBlockNames[k] << "\n{\n";
        for (const UniformEntry& u : uniforms)
        {
            if (u.mClass != k)
            {
                continue;
            }
            const TypeInfo ti = type_info(u.mType);
            if (ti.mBool)
            {
                // booleans are not allowed in uniform blocks: store as uint and expose a bool-valued macro
                const std::string utype = u.mType == "bool" ? "uint" : std::string("uvec") + u.mType.back();
                block << "    " << utype << " " << u.mName << "_ll_b" << u.mArray << ";\n";
                if (u.mArraySize != 0)
                {
                    return fail("boolean array uniform '" + u.mName + "' is not supported");
                }
                if (u.mType == "bool")
                {
                    bool_defines << "#define " << u.mName << " (" << u.mName << "_ll_b != 0u)\n";
                }
                else
                {
                    bool_defines << "#define " << u.mName << " notEqual(" << u.mName << "_ll_b, " << utype << "(0u))\n";
                }
            }
            else
            {
                block << "    " << u.mType << " " << u.mName << u.mArray << ";\n";
            }
        }
        block << "};\n";
    }

    // ---- 3. rewrite each stage object, then compile and link -------------------------------------------
    std::vector<std::unique_ptr<glslang::TShader>> shaders;
    std::vector<std::string> kept_sources;      // glslang keeps pointers into these
    kept_sources.reserve(works.size());
    glslang::TProgram program;
    std::set<StageSource::Stage> present_stages;
    bool fb_defined = false;                    // H6: the body of llFbNote goes into one fragment object of the program

    for (const StageWork& w : works)
    {
        std::ostringstream text;
        text << (w.mUsesRt ? "#version 460\n" : "#version 450\n");      // <FS> glslang has the ray query types from 460 on
        if (bindless)
        {
            text << "#extension GL_EXT_nonuniform_qualifier : enable\n";
        }
        for (const std::string& ext : w.mSplit.mExtensions)
        {
            text << ext << "\n";
        }
        if (w.mUsesRt)
        {
            // <FS> ray queries against the frame's scene (llvkrt.cpp)
            text << "#extension GL_EXT_ray_query : require\n#extension GL_EXT_buffer_reference : require\n#extension GL_EXT_buffer_reference_uvec2 : require\n"
                    "#extension GL_EXT_nonuniform_qualifier : enable\n";
        }
        text << bool_defines.str() << block.str() << bindless_decl.str() << handle_decl.str();
        if (w.mStage == StageSource::Fragment && has_own_table && textureFeedbackShaders())
        {
            // <FS> H6: what this draw needs of a texture (doc/vulkan/PLAN-texture-handles.md): llFbNote(id, uv) records, per texture id of the 2D table, the smallest area of texture coordinates one pixel
            // covered (float bits, atomicMin; 0xFFFFFFFF = not sampled). One invocation of each quad writes (the derivatives are the quad's), and only when it is finer than what is there.
            text << "layout(std430, set = 2, binding = 5) buffer LlFeedback { uint ll_fb[]; };\n";
            if (textureFeedbackEarlyTests())
            {
                text << "layout(early_fragment_tests) in;\n";
            }
            if (fb_defined)
            {
                text << "void llFbNote(uint id, vec2 uv);\n";        // (the body is in another fragment object of the program: the linker refuses two)
            }
            else
            {
                fb_defined = true;
                text << "void llFbNote(uint id, vec2 uv)\n{\n"
                        "    vec2 dx = dFdx(uv);\n    vec2 dy = dFdy(uv);\n"
                        "    if (id == 0u || id >= 65536u || ((int(gl_FragCoord.x) | int(gl_FragCoord.y)) & 1) != 0)\n    {\n        return;\n    }\n"
                        "    float area = abs(dx.x * dy.y - dx.y * dy.x);\n"
                        "    if (!(area > 1e-12))\n    {\n        return;\n    }\n"
                        "    uint bits = floatBitsToUint(area);\n"
                        "    if (bits < ll_fb[id])\n    {\n        atomicMin(ll_fb[id], bits);\n    }\n}\n";
            }
        }
        if (w.mUsesRt)
        {
            // set 3: the scene (binding 0) and the geometry records of its alpha masked structures (binding 1, GpuGeom in llvkrt.cpp); the frame's 2D table as ll_rt_tex
            text << "layout(set = 3, binding = 0) uniform accelerationStructureEXT ll_rt_tlas;\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LLRtPositions { vec4 v[]; };\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer LLRtTexcoords { vec2 t[]; };\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer LLRtColors { uint c[]; };\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LLRtNormals { vec4 n[]; };\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer LLRtIndices { uint w[]; };\n"
                    "struct LLRtGeom { uvec2 positions; uvec2 indices; uvec2 texcoords; uvec2 colors; uvec2 slots; uint flags; float cutoff; uint first; uint material; vec2 uv0; vec2 uv1; vec2 uv2; uvec2 normals; };\n"
                    "struct LLRtMaterial { uvec2 slots; uvec2 slots2; vec4 color; vec4 emissive; vec4 params; vec2 uv0; vec2 uv1; vec2 uv2; vec2 pad; };\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 16) readonly buffer LLRtMaterials { LLRtMaterial m[]; };\n"
                    "layout(std430, set = 3, binding = 1) readonly buffer LLRtGeoms { LLRtGeom g[]; } ll_rt_geoms;\n"
                    "layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer LLRtPins { uint s[]; };\n"
                    "uint ll_rt_slot(uint pin) { return pin < 32768u ? LLRtPins(uvec2(floatBitsToUint(ll_rt_geoms.g[0].uv0.x), floatBitsToUint(ll_rt_geoms.g[0].uv0.y))).s[pin] : 0u; }\n";
            if (bindless)
            {
                text << "#define ll_rt_tex ll_vk_tbl0\n";
            }
            else
            {
                text << "layout(set = 2, binding = 0) uniform sampler2D ll_rt_tex[];\n";
            }
        }

        // a sampler of the program is a macro (the table element): a function parameter declared with the same name (SMAA: `sampler2D blendTex` in the prototypes) would expand to an array declaration.
        // The parameter and its uses in that function get another name; they mean the parameter there anyway.
        auto rename_sampler_params = [&](const std::string& in) -> std::string
        {
            if (!bindless)
            {
                return in;
            }
            std::string out = in;
            for (const SamplerEntry& smp : samplers)
            {
                if (out.find(smp.mName) == std::string::npos)
                {
                    continue;
                }
                const std::regex param("\\bsampler\\w*\\s+" + smp.mName + "\\b");
                if (std::regex_search(out, param))
                {
                    out = std::regex_replace(out, std::regex("\\b" + smp.mName + "\\b"), smp.mName + "_llp");
                }
            }
            return out;
        };

        bool have_main = false;
        for (const Item& item : w.mSplit.mItems)
        {
            if (item.mHasBody)
            {
                const size_t brace = item.mText.find('{');
                const std::string head = trim(item.mText.substr(0, brace));
                const std::string head_no_layout = strip_layouts(head);

                if (starts_with(head_no_layout, "struct") || head_no_layout.find('(') != std::string::npos)
                {
                    // struct definition or function with body
                    if (w.mStage == StageSource::Vertex && head_no_layout.find('(') != std::string::npos)
                    {
                        // void main(...) -> wrapped later
                        size_t p = head.find("main");
                        const bool is_main = starts_with(head, "void") && p != std::string::npos
                            && (p == 0 || !is_ident_char(head[p - 1])) && !is_ident_char(head[p + 4]);
                        if (is_main)
                        {
                            std::string renamed = item.mText;
                            const size_t mp = renamed.find("main");
                            renamed.replace(mp, 4, "ll_vk_original_main");
                            text << renamed << "\n";
                            have_main = true;
                            continue;
                        }
                    }
                    text << rename_sampler_params(item.mText) << "\n";
                    continue;
                }

                // interface block: add the layout the viewer's GL code would have set with glUniformBlockBinding
                Decl d;
                {
                    // parse only the head: qualifiers, then the block name
                    d = parse_decl(head + " ll_block_placeholder");
                }
                if (d.mOk && d.has("uniform"))
                {
                    auto it = std::find(env.mUniformBlockNames.begin(), env.mUniformBlockNames.end(), d.mType);
                    if (it == env.mUniformBlockNames.end())
                    {
                        return fail("uniform block '" + d.mType + "' in " + w.mName + " has no binding");
                    }
                    const U32 binding = 1 + (U32)(it - env.mUniformBlockNames.begin());
                    text << "layout(std140, set = 0, binding = " << binding << ") uniform " << d.mType
                         << item.mText.substr(brace) << "\n";
                    continue;
                }
                text << item.mText << "\n";
                continue;
            }

            Decl d = parse_decl(item.mText);
            if (!d.mOk || d.mFunction)
            {
                text << rename_sampler_params(item.mText) << "\n";
                continue;
            }

            auto emit_with_layout = [&](const Declarator& dc, const std::string& layout, bool is_uniform_opaque) {
                std::ostringstream q;
                q << "layout(" << layout << ") ";
                for (const std::string& qual : d.mQuals)
                {
                    q << qual << " ";
                }
                q << d.mType << " " << dc.mName << dc.mArray << ";\n";
                (void)is_uniform_opaque;
                text << q.str();
            };

            if (d.has("uniform"))
            {
                if (is_opaque_type(d.mType))
                {
                    if (d.mLayout.find("set") != std::string::npos)
                    {
                        if (bindless && has_own_table)
                        {
                            continue;       // (declared above, for every stage of the program)
                        }
                        text << item.mText << "\n";        // <FS> the bindless table keeps the layout the shader gave it
                        continue;
                    }
                    continue;       // (a program with samplers is translated: declared through the tables and the defines above)
                }
                // loose non-opaque uniforms were moved into the block
                continue;
            }

            if (d.has("in") || d.has("out"))
            {
                const bool is_in = d.has("in");
                for (const Declarator& dc : d.mDeclarators)
                {
                    U32 location = 0;
                    if (d.mLayout.find("location") != std::string::npos)
                    {
                        text << item.mText << "\n";
                        goto next_item;
                    }
                    if (w.mStage == StageSource::Vertex && is_in)
                    {
                        auto it = std::find(env.mAttributeNames.begin(), env.mAttributeNames.end(), dc.mName);
                        if (it == env.mAttributeNames.end())
                        {
                            return fail("vertex attribute '" + dc.mName + "' in " + w.mName + " is not a reserved attribute");
                        }
                        location = (U32)(it - env.mAttributeNames.begin());
                    }
                    else if (w.mStage == StageSource::Fragment && !is_in)
                    {
                        location = frag_outputs[dc.mName].mLocation;
                    }
                    else
                    {
                        location = varyings[dc.mName].mLocation;
                    }
                    emit_with_layout(dc, "location = " + std::to_string(location), false);
                }
                continue;
            }

            text << item.mText << "\n";
        next_item:;
        }

        if (w.mStage == StageSource::Vertex && have_main)
        {
            // GL clip space z is [-w, w]; Vulkan's is [0, w]. This maps one onto the other without changing the
            // depth values the viewer's shaders reconstruct from the depth buffer.
            text << "void main()\n{\n    ll_vk_original_main();\n    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";
        }

        const EShLanguage lang = to_glslang_stage(w.mStage);
        kept_sources.push_back(text.str());
        auto sh = std::make_unique<glslang::TShader>(lang);
        const char* strings[1] = { kept_sources.back().c_str() };
        sh->setStrings(strings, 1);
        set_env(*sh, lang);
        const EShMessages msgs = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);
        if (!sh->parse(resources, 450, ECoreProfile, false, false, msgs))
        {
            if (!dump_dir.empty())
            {
                std::string file = program_name + "__" + w.mName;
                for (char& ch : file)
                {
                    if (!std::isalnum((unsigned char)ch) && ch != '_' && ch != '-' && ch != '.') ch = '_';
                }
                CreateDirectoryA(dump_dir.c_str(), nullptr);
                const std::string path = dump_dir + "\\" + file + ".glsl";
                if (FILE* f = fopen(path.c_str(), "wb"))
                {
                    fwrite(kept_sources.back().data(), 1, kept_sources.back().size(), f);
                    fclose(f);
                }
            }
            return fail("compile failed for " + w.mName + ": " + sh->getInfoLog());
        }
        program.addShader(sh.get());
        present_stages.insert(w.mStage);
        shaders.push_back(std::move(sh));
    }

    if (!program.link((EShMessages)(EShMsgSpvRules | EShMsgVulkanRules)))
    {
        return fail(std::string("link failed: ") + program.getInfoLog());
    }

    // cross-check the std140 offsets computed above against glslang's own reflection of the compiled program
    if (!uniforms.empty())
    {
        program.buildReflection(EShReflectionAllBlockVariables);
        std::map<std::string, int> reflected;   // member name -> byte offset
        int reflected_block_size[UC_COUNT];
        for (U32 k = 0; k < UC_COUNT; ++k)
        {
            reflected_block_size[k] = -1;
        }
        for (int bi = 0; bi < program.getNumUniformBlocks(); ++bi)
        {
            for (U32 k = 0; k < UC_COUNT; ++k)
            {
                if (program.getUniformBlock(bi).name == kClassBlockNames[k])
                {
                    reflected_block_size[k] = program.getUniformBlock(bi).size;
                }
            }
        }
        for (int ui = 0; ui < program.getNumUniformVariables(); ++ui)
        {
            const glslang::TObjectReflection& r = program.getUniform(ui);
            for (U32 k = 0; k < UC_COUNT; ++k)
            {
                const std::string prefix = std::string(kClassBlockNames[k]) + ".";
                if (r.name.compare(0, prefix.size(), prefix) == 0)
                {
                    std::string member = r.name.substr(prefix.size());
                    const size_t br = member.find('[');
                    if (br != std::string::npos)
                    {
                        member.resize(br);
                    }
                    if (!reflected.count(member))
                    {
                        reflected[member] = r.offset;
                    }
                }
            }
        }
        for (const UniformEntry& u : uniforms)
        {
            const std::string key = type_info(u.mType).mBool ? u.mName + "_ll_b" : u.mName;
            auto it = reflected.find(key);
            if (it != reflected.end() && (U32)it->second != u.mOffset)
            {
                return fail("std140 offset mismatch for '" + u.mName + "': computed " + std::to_string(u.mOffset)
                            + ", glslang " + std::to_string(it->second));
            }
        }
        for (U32 k = 0; k < UC_COUNT; ++k)
        {
            if (reflected_block_size[k] >= 0 && (U32)reflected_block_size[k] > block_sizes[k])
            {
                return fail(std::string("loose uniform block size mismatch (") + kClassBlockNames[k] + "): computed " + std::to_string(block_sizes[k]) + ", glslang " + std::to_string(reflected_block_size[k]));
            }
        }
    }

    for (StageSource::Stage st : present_stages)
    {
        const EShLanguage lang = to_glslang_stage(st);
        glslang::TIntermediate* inter = program.getIntermediate(lang);
        if (!inter)
        {
            return fail("no intermediate for stage");
        }
        spv::SpvBuildLogger logger;
        glslang::SpvOptions options;
        options.validate = false;
        glslang::GlslangToSpv(*inter, out.mSpirv[st], &logger, &options);
        if (out.mSpirv[st].empty())
        {
            return fail("SPIR-V generation produced nothing: " + logger.getAllMessages());
        }
    }

    // ---- 4. reflection for the caller ---------------------------------------------------------------------
    for (const UniformEntry& u : uniforms)
    {
        LooseUniform lu;
        lu.mName = u.mName;
        lu.mType = u.mType;
        lu.mArraySize = u.mArraySize;
        lu.mOffset = u.mOffset;
        lu.mClass = u.mClass;
        lu.mSize = u.mSize;
        lu.mInit = u.mInit;
        out.mUniforms.push_back(lu);
    }
    for (U32 k = 0; k < UC_COUNT; ++k)
    {
        out.mBlockSizes[k] = block_sizes[k];
    }
    for (const SamplerEntry& s : samplers)
    {
        out.mSamplers.push_back({ s.mName, s.mType, s.mBinding, s.mIndexUniform });
    }
    out.mBindless = bindless;
    for (const StageWork& w : works)
    {
        out.mUsesRt = out.mUsesRt || w.mUsesRt;
    }
    out.mAttributes.assign(attributes.begin(), attributes.end());
    for (const std::string& a : out.mAttributes)
    {
        auto it = std::find(env.mAttributeNames.begin(), env.mAttributeNames.end(), a);
        out.mAttributeLocations.push_back(it == env.mAttributeNames.end() ? 0 : (U32)(it - env.mAttributeNames.begin()));
        out.mAttributeTypes.push_back(attribute_types[a]);
    }
    for (const auto& kv : frag_outputs)
    {
        out.mOutputs.push_back(kv.first);
    }
    return true;
}

// ---- disk cache of compiled programs --------------------------------------------------------------------------
// Compiling the viewer's ~200 programs with glslang costs many seconds at every start. The result (SPIR-V plus the reflection data)
// is stored per program under <cache>/vk_spirv_cache, keyed by a hash of everything that goes into the compile: the stage sources,
// the environment lists, and this file's build time stamp (the rewriting rules live in this file, so a rebuild drops stale entries).
// LL_VK_SPIRV_CACHE=0 turns it off.
namespace
{
constexpr U32 kCacheMagic = 0x43535056;     // "VPSC"
constexpr U32 kCacheVersion = 4;

struct Hasher
{
    U64 a = 0xcbf29ce484222325ull, b = 0x84222325cbf29ce4ull;
    void bytes(const void* d, size_t n)
    {
        const U8* p = (const U8*)d;
        for (size_t i = 0; i < n; ++i)
        {
            a = (a ^ p[i]) * 0x100000001b3ull;
            b = (b ^ (U8)(p[i] + 0x5b)) * 0x9E3779B97F4A7C15ull;
            b ^= b >> 29;
        }
    }
    void str(const std::string& v) { U64 n = v.size(); bytes(&n, sizeof(n)); bytes(v.data(), v.size()); }
    std::string hex() const
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)a, (unsigned long long)b);
        return buf;
    }
};

struct CacheWriter
{
    std::string mData;
    void u32(U32 v) { mData.append((const char*)&v, 4); }
    void str(const std::string& v) { u32((U32)v.size()); mData.append(v); }
    void strs(const std::vector<std::string>& v) { u32((U32)v.size()); for (const std::string& e : v) str(e); }
    void u32s(const std::vector<U32>& v) { u32((U32)v.size()); mData.append((const char*)v.data(), v.size() * 4); }
};

struct CacheReader
{
    const std::string& mData;
    size_t mPos = 0;
    bool mOk = true;
    explicit CacheReader(const std::string& d) : mData(d) {}
    U32 u32()
    {
        U32 v = 0;
        if (mPos + 4 > mData.size()) { mOk = false; return 0; }
        memcpy(&v, mData.data() + mPos, 4);
        mPos += 4;
        return v;
    }
    std::string str()
    {
        const U32 n = u32();
        if (!mOk || mPos + n > mData.size()) { mOk = false; return std::string(); }
        std::string v = mData.substr(mPos, n);
        mPos += n;
        return v;
    }
    std::vector<std::string> strs()
    {
        std::vector<std::string> v;
        const U32 n = u32();
        for (U32 i = 0; mOk && i < n; ++i) v.push_back(str());
        return v;
    }
    std::vector<U32> u32s()
    {
        std::vector<U32> v;
        const U32 n = u32();
        if (!mOk || (size_t)n * 4 > mData.size() - mPos) { mOk = false; return v; }
        v.resize(n);
        memcpy(v.data(), mData.data() + mPos, (size_t)n * 4);
        mPos += (size_t)n * 4;
        return v;
    }
};

std::string spirv_cache_dir()
{
    static const std::string dir = []() {
        const char* off = getenv("LL_VK_SPIRV_CACHE");
        if (off && off[0] == '0') return std::string();
        std::string d = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "vk_spirv_cache");
        LLFile::mkdir(d);
        return d;
    }();
    return dir;
}

std::string spirv_cache_key(const std::vector<StageSource>& stages, const ShaderEnv& env)
{
    Hasher h;
    h.str(std::string(__DATE__ " " __TIME__));
    for (const StageSource& st : stages)
    {
        U32 stage = (U32)st.mStage;
        h.bytes(&stage, sizeof(stage));
        h.str(st.mSource);
    }
    for (const std::string& n : env.mAttributeNames) h.str(n);
    h.str(textureFeedbackShaders() ? (textureFeedbackEarlyTests() ? "|handles|fb|early" : "|handles|fb") : "|handles");       // (the translation: the samplers are ids read through the handle map, and the pull shaders measure the need)

    h.str("|blocks");
    for (const std::string& n : env.mUniformBlockNames) h.str(n);
    return h.hex();
}

bool spirv_cache_load(const std::string& path, ProgramInfo& out)
{
    llifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    CacheReader r(data);
    if (r.u32() != kCacheMagic || r.u32() != kCacheVersion) return false;
    ProgramInfo info;
    const U32 nu = r.u32();
    for (U32 i = 0; r.mOk && i < nu; ++i)
    {
        LooseUniform u;
        u.mName = r.str(); u.mType = r.str(); u.mArraySize = r.u32(); u.mOffset = r.u32(); u.mSize = r.u32(); u.mInit = r.str(); u.mClass = (U8)r.u32();
        info.mUniforms.push_back(std::move(u));
    }
    for (U32 k = 0; k < UC_COUNT; ++k) info.mBlockSizes[k] = r.u32();
    const U32 ns = r.u32();
    for (U32 i = 0; r.mOk && i < ns; ++i)
    {
        SamplerInfo sm;
        sm.mName = r.str(); sm.mType = r.str(); sm.mBinding = r.u32(); sm.mIndexUniform = (S32)r.u32();
        info.mSamplers.push_back(std::move(sm));
    }
    info.mBindless = r.u32() != 0;
    info.mUsesRt = r.u32() != 0;
    info.mAttributes = r.strs();
    info.mAttributeLocations = r.u32s();
    info.mAttributeTypes = r.strs();
    info.mOutputs = r.strs();
    for (int st = 0; st < 3; ++st) info.mSpirv[st] = r.u32s();
    if (r.u32() != kCacheMagic || !r.mOk || r.mPos != data.size()) return false;
    out = std::move(info);
    return true;
}

void spirv_cache_store(const std::string& path, const ProgramInfo& info)
{
    CacheWriter w;
    w.u32(kCacheMagic); w.u32(kCacheVersion);
    w.u32((U32)info.mUniforms.size());
    for (const LooseUniform& u : info.mUniforms)
    {
        w.str(u.mName); w.str(u.mType); w.u32(u.mArraySize); w.u32(u.mOffset); w.u32(u.mSize); w.str(u.mInit); w.u32(u.mClass);
    }
    for (U32 k = 0; k < UC_COUNT; ++k) w.u32(info.mBlockSizes[k]);
    w.u32((U32)info.mSamplers.size());
    for (const SamplerInfo& sm : info.mSamplers) { w.str(sm.mName); w.str(sm.mType); w.u32(sm.mBinding); w.u32((U32)sm.mIndexUniform); }
    w.u32(info.mBindless ? 1u : 0u);
    w.u32(info.mUsesRt ? 1u : 0u);
    w.strs(info.mAttributes);
    w.u32s(info.mAttributeLocations);
    w.strs(info.mAttributeTypes);
    w.strs(info.mOutputs);
    for (int st = 0; st < 3; ++st) w.u32s(info.mSpirv[st]);
    w.u32(kCacheMagic);
    const std::string tmp = path + ".tmp";
    {
        llofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return;
        f.write(w.mData.data(), (std::streamsize)w.mData.size());
        if (!f.good()) { f.close(); LLFile::remove(tmp, ENOENT); return; }
    }
    if (LLFile::rename(tmp, path, ENOENT) != 0) LLFile::remove(tmp, ENOENT);
}

U32 sCacheHits = 0, sCacheMisses = 0;
}

bool compileProgramImpl(const std::string& program_name, const std::vector<StageSource>& stages, const ShaderEnv& env,
                    ProgramInfo& out, const std::string& dump_dir)
{
    // the debug wrapper records per-program sampler names while compiling, and a dump request needs the compile to run
    const std::string dir = (debugShadersEnabled() || !dump_dir.empty()) ? std::string() : spirv_cache_dir();
    std::string path;
    if (!dir.empty())
    {
        path = gDirUtilp->add(dir, spirv_cache_key(stages, env) + ".bin");
        if (spirv_cache_load(path, out))
        {
            if (++sCacheHits % 50 == 0)
            {
                LL_INFOS("VkShader") << "SPIR-V disk cache: " << sCacheHits << " programs loaded, " << sCacheMisses << " compiled" << LL_ENDL;
            }
            return true;
        }
    }
    const bool ok = compileProgramUncached(program_name, stages, env, out, dump_dir);
    if (ok && !path.empty())
    {
        ++sCacheMisses;
        spirv_cache_store(path, out);
    }
    return ok;
}

bool compileProgram(const std::string& program_name, const std::vector<StageSource>& stages, const ShaderEnv& env,
                    ProgramInfo& out, const std::string& dump_dir)
{
    return compileProgramImpl(program_name, stages, env, out, dump_dir);
}

namespace
{
std::map<std::string, ProgramInfo>& registry()
{
    static std::map<std::string, ProgramInfo> sRegistry;
    return sRegistry;
}
}

void registerCompiledProgram(const std::string& name, const ProgramInfo& info)
{
    registry()[name] = info;
}

const ProgramInfo* findCompiledProgram(const std::string& name)
{
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : &it->second;
}

} // namespace LLVk

#endif // LL_VULKAN

/**
 * @file llrender.h
 * @brief LLRender definition
 *
 *  This class acts as a wrapper for OpenGL calls.
 *  The goal of this class is to minimize the number of api calls due to legacy rendering
 *  code, to define an interface for a multiple rendering API abstraction of the UI
 *  rendering, and to abstract out direct rendering calls in a way that is cleaner and easier to maintain.
 *
 * $LicenseInfo:firstyear=2001&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_LLGLRENDER_H
#define LL_LLGLRENDER_H

//#include "linden_common.h"

#include "v2math.h"
#include "v3math.h"
#include "v4coloru.h"
#include "v4math.h"
#include "llstrider.h"
#include "llpointer.h"
#include "llglheaders.h"
#include "llrenderstate.h"
#include "llrenderformat.h"
#include "llrendermatrix.h"
#include "llmatrix4a.h"
#include "glm/mat4x4.hpp"
#include <boost/align/aligned_allocator.hpp>

#include <array>
#include <list>

class LLVertexBuffer;
class LLCubeMap;
class LLImageGL;
class LLRenderTarget;
class LLTexture;
class LLVertexBufferData;

#define LL_MATRIX_STACK_DEPTH 32

constexpr U32 LL_NUM_TEXTURE_LAYERS = 32;
constexpr U32 LL_NUM_LIGHT_UNITS = 8;

class LLTexUnit
{
    friend class LLRender;
public:
    static U32 sWhiteTexture;

    typedef enum
    {
        TT_TEXTURE = 0,         // Standard 2D Texture
        TT_RECT_TEXTURE,        // Non power of 2 texture
        TT_CUBE_MAP,            // 6-sided cube map texture
        TT_CUBE_MAP_ARRAY,      // Array of cube maps
        TT_MULTISAMPLE_TEXTURE, // see GL_ARB_texture_multisample
        TT_TEXTURE_3D,          // standard 3D Texture
        TT_NONE,                // No texture type is currently enabled
    } eTextureType;

    typedef enum
    {
        TAM_WRAP = 0,           // Standard 2D Texture
        TAM_MIRROR,             // Non power of 2 texture
        TAM_CLAMP               // No texture type is currently enabled
    } eTextureAddressMode;

    typedef enum
    {   // Note: If mipmapping or anisotropic are not enabled or supported it should fall back gracefully
        TFO_POINT = 0,          // Equal to: min=point, mag=point, mip=none.
        TFO_BILINEAR,           // Equal to: min=linear, mag=linear, mip=point.
        TFO_TRILINEAR,          // Equal to: min=linear, mag=linear, mip=linear.
        TFO_ANISOTROPIC         // Equal to: min=anisotropic, max=anisotropic, mip=linear.
    } eTextureFilterOptions;

    typedef enum
    {
        TMG_NONE = 0,           // Mipmaps are not automatically generated for this texture.
        TMG_AUTO,               // Mipmaps are automatically generated for this texture.
        TMG_MANUAL              // Mipmaps are manually generated for this texture.
    } eTextureMipGeneration;

    typedef enum
    {
        TB_REPLACE = 0,
        TB_ADD,
        TB_MULT,
        TB_MULT_X2,
        TB_ALPHA_BLEND,
        TB_COMBINE          // Doesn't need to be set directly, setTexture___Blend() set TB_COMBINE automatically
    } eTextureBlendType;

    typedef enum
    {
        TBO_REPLACE = 0,            // Use Source 1
        TBO_MULT,                   // Multiply: ( Source1 * Source2 )
        TBO_MULT_X2,                // Multiply then scale by 2:  ( 2.0 * ( Source1 * Source2 ) )
        TBO_MULT_X4,                // Multiply then scale by 4:  ( 4.0 * ( Source1 * Source2 ) )
        TBO_ADD,                    // Add: ( Source1 + Source2 )
        TBO_ADD_SIGNED,             // Add then subtract 0.5: ( ( Source1 + Source2 ) - 0.5 )
        TBO_SUBTRACT,               // Subtract Source2 from Source1: ( Source1 - Source2 )
        TBO_LERP_VERT_ALPHA,        // Interpolate based on Vertex Alpha (VA): ( Source1 * VA + Source2 * (1-VA) )
        TBO_LERP_TEX_ALPHA,         // Interpolate based on Texture Alpha (TA): ( Source1 * TA + Source2 * (1-TA) )
        TBO_LERP_PREV_ALPHA,        // Interpolate based on Previous Alpha (PA): ( Source1 * PA + Source2 * (1-PA) )
        TBO_LERP_CONST_ALPHA        // Interpolate based on Const Alpha (CA): ( Source1 * CA + Source2 * (1-CA) )
    } eTextureBlendOp;

    typedef enum
    {
        TBS_PREV_COLOR = 0,         // Color from the previous texture stage
        TBS_PREV_ALPHA,
        TBS_ONE_MINUS_PREV_COLOR,
        TBS_ONE_MINUS_PREV_ALPHA,
        TBS_TEX_COLOR,              // Color from the texture bound to this stage
        TBS_TEX_ALPHA,
        TBS_ONE_MINUS_TEX_COLOR,
        TBS_ONE_MINUS_TEX_ALPHA,
        TBS_VERT_COLOR,             // The vertex color currently set
        TBS_VERT_ALPHA,
        TBS_ONE_MINUS_VERT_COLOR,
        TBS_ONE_MINUS_VERT_ALPHA,
        TBS_CONST_COLOR,            // The constant color value currently set
        TBS_CONST_ALPHA,
        TBS_ONE_MINUS_CONST_COLOR,
        TBS_ONE_MINUS_CONST_ALPHA
    } eTextureBlendSrc;

    typedef enum
    {
        TCS_LINEAR = 0,
        TCS_SRGB
    } eTextureColorSpace;

    LLTexUnit(S32 index = -1);

    // Refreshes renderer state of the texture unit to the cached values
    // Needed when the render context has changed and invalidated the current state
    void refreshState(void);

    // returns the index of this texture unit
    S32 getIndex(void) const { return mIndex; }

    // The unit holds nothing: white when it held a 2D texture, no texture (slot 0) for the other types (what disable() was)
    void clear();

    // Binds the LLImageGL to this texture unit
    // (automatically enables the unit for the LLImageGL's texture type)
    bool bind(LLImageGL* texture, bool for_rendering = false, bool forceBind = false, S32 usename = 0);
    bool bind(LLTexture* texture, bool for_rendering = false, bool forceBind = false);

    // bind implementation for inner loops
    // makes the following assumptions:
    //  - No need for gGL.flush()
    //  - texture is not null
    //  - gl_tex->getTexName() is not zero
    //  - This texture is not being bound redundantly
    //  - USE_SRGB_DECODE is disabled
    //  - mTexOptionsDirty is false
    //  -
    void bindFast(LLTexture* texture);
    // <FS> render packets: the effects of bindFast() on the texture (marks it in use, regenerates a deleted one, applies changed options) without binding it; returns its GL name
    U32 prepareFast(LLTexture* texture);
    // <FS> binds the image on this unit and sends its address mode and filtering options (what bindFast does when they are dirty)
    void applyTexOptions(LLImageGL* gl_tex);

    // Binds a cubemap to this texture unit
    // (automatically enables the texture unit for cubemaps)
    bool bind(LLCubeMap* cubeMap);

    // Binds a render target to this texture unit
    // (automatically enables the texture unit for the RT's texture type)
    bool bind(LLRenderTarget * renderTarget, bool bindDepth = false);

    // Manually binds a texture to the texture unit
    // (automatically enables the tex unit for the given texture type)
    bool bindManual(eTextureType type, U32 texture, bool hasMips = false, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    // F3.4: the intent of unbind(TT_TEXTURE) before an untextured draw: the unit holds the white texture
    void bindWhite(const char* file = __builtin_FILE(), int line = __builtin_LINE());
    // the same without the flush (inner loops: the batch is known to be empty)
    void bindWhiteFast(const char* file = __builtin_FILE(), int line = __builtin_LINE());
    // F3.4: the unit holds no texture of that type (slot 0)
    void bindNone(eTextureType type);

    // Unbinds the currently bound texture of the given type
    // (only if there's a texture of the given type currently bound)
    void unbind(eTextureType type, const char* file = __builtin_FILE(), int line = __builtin_LINE());      // file and line: LL_VK_TEXUNIT_VERIFY names the site that last wrote unit 0

    // Fast but unsafe version of unbind
    void unbindFast(eTextureType type, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    // Sets the addressing mode used to sample the texture
    // Warning: this stays set for the bound texture forever,
    // make sure you want to permanently change the address mode  for the bound texture.
    void setTextureAddressMode(eTextureAddressMode mode);
    // F3.4: the sampler parameters of a texture by name (no unit involved); flush first like the unit forms
    static void applyAddressMode(U32 tex_name, eTextureType tex_type, eTextureAddressMode mode);
    static void applyFilteringOption(U32 tex_name, eTextureType tex_type, eTextureFilterOptions option, bool has_mips);
    // MUST already be active and bound
    void setTextureAddressModeFast(eTextureAddressMode mode, eTextureType tex_type);

    // Sets the filtering options used to sample the texture
    // Warning: this stays set for the bound texture forever,
    // make sure you want to permanently change the filtering for the bound texture.
    void setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option);
    // MUST already be active and bound
    void setTextureFilteringOptionFast(LLTexUnit::eTextureFilterOptions option, eTextureType tex_type);

    static TextureTarget getInternalType(eTextureType type);

    U32 getCurrTexture(void) { return mCurrTexture; }

    eTextureType getCurrType(void) { return mCurrTexType; }

    void setHasMipMaps(bool hasMips) { mHasMipMaps = hasMips; }

protected:
    friend class LLRender;

    S32                 mIndex;
    U32                 mCurrTexture;
    eTextureType        mCurrTexType;
    bool                mHasMipMaps;

};

class LLLightState
{
public:
    LLLightState(S32 index = -1);

    void enable();
    void disable();
    void setDiffuse(const LLColor4& diffuse);
    void setDiffuseB(const LLColor4& diffuse);
    void setAmbient(const LLColor4& ambient);
    void setSpecular(const LLColor4& specular);
    void setPosition(const LLVector4& position);
    void setConstantAttenuation(const F32& atten);
    void setLinearAttenuation(const F32& atten);
    void setQuadraticAttenuation(const F32& atten);
    void setSpotExponent(const F32& exponent);
    void setSpotCutoff(const F32& cutoff);
    void setSpotDirection(const LLVector3& direction);
    void setSunPrimary(bool v);
    void setSize(F32 size);
    void setFalloff(F32 falloff);

protected:
    friend class LLRender;

    S32 mIndex;
    bool mEnabled;
    LLColor4 mDiffuse;
    LLColor4 mDiffuseB;
    bool     mSunIsPrimary;
    LLColor4 mAmbient;
    LLColor4 mSpecular;
    LLVector4 mPosition;
    LLVector3 mSpotDirection;

    F32 mConstantAtten;
    F32 mLinearAtten;
    F32 mQuadraticAtten;

    F32 mSpotExponent;
    F32 mSpotCutoff;
    F32 mSize = 0.f;
    F32 mFalloff = 0.f;
};

class LLRender
{
    friend class LLTexUnit;
public:

    enum eTexIndex : U8
    {
        // Channels for material textures
        DIFFUSE_MAP            = 0,
        ALTERNATE_DIFFUSE_MAP  = 1,
        NORMAL_MAP             = 1,
        SPECULAR_MAP           = 2,
        // Channels for PBR textures
        BASECOLOR_MAP          = 3,
        METALLIC_ROUGHNESS_MAP = 4,
        GLTF_NORMAL_MAP        = 5,
        EMISSIVE_MAP           = 6,
        // Total number of channels
        NUM_TEXTURE_CHANNELS   = 7,
    };

    enum eVolumeTexIndex : U8
    {
        LIGHT_TEX = 0,
        SCULPT_TEX,
        NUM_VOLUME_TEXTURE_CHANNELS,
    };

    enum eGeomModes : U8
    {
        TRIANGLES = 0,
        TRIANGLE_STRIP,
        TRIANGLE_FAN,
        POINTS,
        LINES,
        LINE_STRIP,
        LINE_LOOP,
        NUM_MODES
    };

    enum eCompareFunc : U8
    {
        CF_NEVER = 0,
        CF_ALWAYS,
        CF_LESS,
        CF_LESS_EQUAL,
        CF_EQUAL,
        CF_NOT_EQUAL,
        CF_GREATER_EQUAL,
        CF_GREATER,
        CF_DEFAULT
    };

    enum eBlendType : U8
    {
        BT_ALPHA = 0,
        BT_ADD,
        BT_ADD_WITH_ALPHA,  // Additive blend modulated by the fragment's alpha.
        BT_MULT,
        BT_MULT_ALPHA,
        BT_MULT_X2,
        BT_REPLACE
    };

    // WARNING:  this MUST match the LL_PART_BF enum in LLPartData, so set values explicitly in case someone
    // decides to add more or reorder them
    enum eBlendFactor : U8
    {
        BF_ONE = 0,
        BF_ZERO = 1,
        BF_DEST_COLOR = 2,
        BF_SOURCE_COLOR = 3,
        BF_ONE_MINUS_DEST_COLOR = 4,
        BF_ONE_MINUS_SOURCE_COLOR = 5,
        BF_DEST_ALPHA = 6,
        BF_SOURCE_ALPHA = 7,
        BF_ONE_MINUS_DEST_ALPHA = 8,
        BF_ONE_MINUS_SOURCE_ALPHA = 9,
        BF_UNDEF
    };

    enum eMatrixMode : U8
    {
        MM_MODELVIEW = 0,
        MM_PROJECTION,
        MM_TEXTURE0,
        MM_TEXTURE1,
        MM_TEXTURE2,
        MM_TEXTURE3,
        NUM_MATRIX_MODES
    };

    LLRender();
    ~LLRender();
    bool init(bool needs_vertex_buffer);
    void initVertexBuffer();
    void resetVertexBuffer();
    void shutdown();

    // Refreshes renderer state to the cached values
    // Needed when the render context has changed and invalidated the current state
    void refreshState(void);


    // F3.3 (llrendermatrix.h): the matrix operations name their matrix; there is no "current matrix mode" any more
    void pushMatrixOf(eMatrixMode mode);
    void popMatrixOf(eMatrixMode mode);
    void setMatrixOf(eMatrixMode mode, const glm::mat4& m);
    void multMatrixOf(eMatrixMode mode, const glm::mat4& m);
    void loadIdentityOf(eMatrixMode mode);
    void translateOf(eMatrixMode mode, const glm::vec3& v);
    void rotateOf(eMatrixMode mode, F32 degrees, const glm::vec3& axis);
    void scaleOf(eMatrixMode mode, const glm::vec3& v);
    void orthoOf(eMatrixMode mode, F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar);
    const glm::mat4& matrixOf(eMatrixMode mode) const { return mMatrix[mode][mMatIdx[mode]]; }
    U32 matrixDepth(eMatrixMode mode) const { return mMatIdx[mode]; }

    const glm::mat4& getModelviewMatrix();
    const glm::mat4& getProjectionMatrix();

    void syncMatrices();
    void syncLightState();

    void translateUI(F32 x, F32 y, F32 z);
    void scaleUI(F32 x, F32 y, F32 z);
    void pushUIMatrix();
    void popUIMatrix();
    void loadUIIdentity();
    LLVector3 getUITranslation();
    LLVector3 getUIScale();

    void flush();

    // if list is set, will store buffers in list for later use, if list isn't set, will use cache
    void beginList(std::list<LLVertexBufferData> *list);
    void endList();

    void begin(const U32& mode);
    void end();

    U8 getMode() const { return mMode; }

    void vertex2i(const S32& x, const S32& y);
    void vertex2f(const F32& x, const F32& y);
    void vertex3f(const F32& x, const F32& y, const F32& z);
    void vertex2fv(const F32* v);
    void vertex3fv(const F32* v);

    void texCoord2i(const S32& x, const S32& y, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    void texCoord2f(const F32& x, const F32& y, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    void texCoord2fv(const F32* tc, const char* file = __builtin_FILE(), int line = __builtin_LINE());

    void color4ub(const U8& r, const U8& g, const U8& b, const U8& a);
    void color4f(const F32& r, const F32& g, const F32& b, const F32& a);
    void color4fv(const F32* c);
    void color3f(const F32& r, const F32& g, const F32& b);
    void color3fv(const F32* c);
    void color4ubv(const U8* c);

    void diffuseColor3f(F32 r, F32 g, F32 b);
    void diffuseColor3fv(const F32* c);
    void diffuseColor4f(F32 r, F32 g, F32 b, F32 a);
    void diffuseColor4fv(const F32* c);
    void diffuseColor4ubv(const U8* c);
    void diffuseColor4ub(U8 r, U8 g, U8 b, U8 a);

    void vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U*, S32 vert_count);

    void setColorMask(bool writeColor, bool writeAlpha);
    void setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha);
    // <FS> Phase 2a: GL state/clear calls that used to be made directly by the viewer. They are thin
    // pass-throughs today (no flush, so behaviour is unchanged); the Vulkan backend will implement them.
    // Mask/face arguments are the GL enum values (ClearBits::Color, GL_FRONT, ...).
    void clearBuffers(ClearBits mask);
    void setClearColor(F32 r, F32 g, F32 b, F32 a);
    const F32* getClearColor() const { return mClearColor; }       // F3.5d: the pass scope clears to this unless it is given a colour
    void setViewport(S32 x, S32 y, S32 width, S32 height);
    void setCullFace(LLRenderState::CullFace face);       // F3.2: typed (llrenderstate.h)
    void setPolygonOffset(F32 factor, F32 units);
    void setPolygonMode(LLRenderState::PolygonMode mode);
    void setPointSize(F32 size);
    void setStencilFunc(LLRenderState::CompareOp op, S32 ref, U32 mask);
    void setStencilOp(LLRenderState::StencilOp sfail, LLRenderState::StencilOp dpfail, LLRenderState::StencilOp dppass);
    void setStencilMask(U32 mask);
    // reads the colour attachment of the target of the innermost open pass (the back buffer when none is bound)
    void readPixels(S32 x, S32 y, S32 width, S32 height, PixelFormat format, ComponentType type, void* data, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    // queries (llvkquery.h): occlusion counts and GPU timers around draws; the kinds are the ones LLVkQuery::Kind names
    enum QueryKind { QUERY_ANY_SAMPLES = 0, QUERY_SAMPLES_PASSED = 1, QUERY_TIME_ELAPSED = 2, QUERY_PRIMITIVES_GENERATED = 3 };
    void genQueries(S32 count, U32* ids);
    void deleteQueries(S32 count, const U32* ids);
    void beginQuery(QueryKind kind, U32 id);
    void endQuery(QueryKind kind);
    bool queryReady(U32 id);
    U64 queryResult(U32 id);
    void genBuffers(S32 count, U32* ids);
    void deleteBuffers(S32 count, const U32* ids);
    void bufferData(U32 buffer, BufferKind kind, S64 size, const void* data);     // names the buffer: nothing is bound
    void bindUniformBlock(U32 binding, U32 id);
    void copyTexSubImage2D(U32 texture, TextureTarget target, S32 level, S32 xoffset, S32 yoffset, S32 x, S32 y, S32 width, S32 height, const LLRenderTarget* source);     // source_fbo: the render target read from (0: the back buffer)
    void copyTexSubImage3D(U32 texture, TextureTarget target, S32 level, S32 xoffset, S32 yoffset, S32 zoffset, S32 x, S32 y, S32 width, S32 height, const LLRenderTarget* source);
    void generateMipmap(U32 texture);
    // F3.6f: sampler settings of one texture, by name (a setting acts on the texture whatever its dimension): typed, no GL parameter numbers
    void setTexFilter(U32 texture, bool mag, TextureFilter filter);
    void setTexWrap(U32 texture, TextureWrap s, TextureWrap t);
    void setTexCompare(U32 texture, bool on, LLRenderState::CompareOp func = LLRenderState::CompareOp::LessEqual);        // on: compare the reference to the texture with func (depth textures); off: plain sampling
    void texImage2D(U32 texture, TextureTarget target, S32 level, TexelFormat internal_format, S32 width, S32 height, PixelFormat format, ComponentType type, const void* data, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    void bindTextureSlot(S32 unit, U32 id);     // raw slot write, bypasses the LLTexUnit cache (as the old direct call did)
    void deleteTextures(S32 count, const U32* ids);
    F32 getLineWidth() const { return mLineWidth; }
    void setScissor(S32 x, S32 y, S32 width, S32 height);
    void getTexLevelParameteriv(U32 texture, TextureTarget target, S32 level, LevelQuery query, S32* result);
    void getTexImage(U32 texture, TextureTarget target, S32 level, PixelFormat format, ComponentType type, void* pixels, const char* file = __builtin_FILE(), int line = __builtin_LINE());
    void setSceneBlendType(eBlendType type);

    // applies blend func to both color and alpha
    void blendFunc(eBlendFactor sfactor, eBlendFactor dfactor);
    // applies separate blend functions to color and alpha
    void blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
               eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor);

    LLLightState* getLight(U32 index);
    void setAmbientLightColor(const LLColor4& color);

    void setLineWidth(F32 line_width); // <FS> Line width OGL core profile fix by Rye Mutt

    LLTexUnit* getTexUnit(U32 index);



    void debugTexUnits(void);

    void clearErrors();

    struct Vertex
    {
        F32 v[3];
        U8 c[4];
        F32 uv[2];
    };

public:
    static U32 sUICalls;
    static U32 sUIVerts;
    static bool sGLCoreProfile;
    static bool sNsightDebugSupport;
    static LLVector2 sUIGLScaleFactor;
    static bool sClassicMode; // classic sky mode active

private:
    friend class LLLightState;

    LLVertexBuffer* genBuffer(U32 attribute_mask, S32 count);
    void drawBuffer(LLVertexBuffer* vb, U32 mode, S32 count);
    void resetStriders(S32 count);

    U32 mMatIdx[NUM_MATRIX_MODES];
    U32 mMatHash[NUM_MATRIX_MODES];
    glm::mat4 mMatrix[NUM_MATRIX_MODES][LL_MATRIX_STACK_DEPTH];
    U32 mCurMatHash[NUM_MATRIX_MODES];
    U32 mLightHash;
    LLColor4 mAmbientLightColor;

    bool            mDirty;
    U32             mCount;
    const char*     mBatchTexFile = nullptr;        // F3.4: where the first texCoord call of the batch came from (LL_VK_TEXUNIT_VERIFY)
    int             mBatchTexLine = 0;
    bool            mBatchHasTexCoord = false;     // F3.4: a texCoord call since the last flush (an untextured batch gets white from its draw record)
    U32             mMode;
    F32             mClearColor[4] = { 0.f, 0.f, 0.f, 0.f };     // the colour clearBuffers() clears to
    bool                mCurrColorMask[4];
    F32             mLineWidth; // <FS> Line width OGL core profile fix by Rye Mutt
    // <FS:Ansariel> Don't ignore OpenGL max line width
    F32             mMaxLineWidthSmooth;
    F32             mMaxLineWidthAliased;
    // </FS:Ansariel>

    LLPointer<LLVertexBuffer>   mBuffer;
    LLStrider<LLVector4a>       mVerticesp;
    LLStrider<LLVector2>        mTexcoordsp;
    LLStrider<LLColor4U>        mColorsp;
    std::array<LLTexUnit, LL_NUM_TEXTURE_LAYERS> mTexUnits;
    LLTexUnit           mDummyTexUnit;
    std::array<LLLightState, LL_NUM_LIGHT_UNITS> mLightState;

    eBlendFactor mCurrBlendColorSFactor;
    eBlendFactor mCurrBlendColorDFactor;
    eBlendFactor mCurrBlendAlphaSFactor;
    eBlendFactor mCurrBlendAlphaDFactor;

    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIOffset;
    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIScale;
};

extern F32 gGLModelView[16];
extern F32 gGLLastModelView[16];
extern F32 gGLLastProjection[16];
extern F32 gGLProjection[16];
extern S32 gGLViewport[4];
extern glm::mat4 gGLDeltaModelView;
extern glm::mat4 gGLInverseDeltaModelView;

extern thread_local LLRender gGL;

// This rotation matrix moves the default OpenGL reference frame
// (-Z at, Y up) to Cory's favorite reference frame (X at, Z up)
const F32 OGL_TO_CFR_ROTATION[16] = {  0.f,  0.f, -1.f,  0.f,   // -Z becomes X
                                      -1.f,  0.f,  0.f,  0.f,   // -X becomes Y
                                       0.f,  1.f,  0.f,  0.f,   //  Y becomes Z
                                       0.f,  0.f,  0.f,  1.f };

glm::mat4 copy_matrix(F32* src);
glm::mat4 get_current_modelview();
glm::mat4 get_current_projection();
glm::mat4 get_last_modelview();
glm::mat4 get_last_projection();

void copy_matrix(const glm::mat4& src, F32* dst);
void set_current_modelview(const glm::mat4& mat);
void set_current_projection(const glm::mat4& mat);
void set_last_modelview(const glm::mat4& mat);
void set_last_projection(const glm::mat4& mat);

// glh compat
glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec);

#define LL_SHADER_LOADING_WARNS(...) LL_WARNS()

#endif

/**
 * @file llrendermatrix.h
 * @brief F3.3: the matrix vocabulary of the pass code: operations that name the matrix they change, and scoped guards (no GL, no Vulkan in this header).
 *
 * The matrices themselves are still LLRender's stacks (llrender.h). Every operation here names its matrix: it never reads or writes the "current matrix mode" that the old
 * calls (matrixMode, pushMatrix, translatef ...) depend on. pushMatrix / popMatrix flush the immediate-mode batch (pending vertices belong to the old matrix); so do these,
 * through the same LLRender functions.
 *
 *   LLRenderMatrix::push(Kind::Modelview);                 was: gGL.matrixMode(LLRender::MM_MODELVIEW); gGL.pushMatrix();   (the matrixMode call stays until the mode is gone)
 *   LLRenderMatrix::translate(Kind::Modelview, x, y, z);   was: gGL.translatef(x, y, z);
 *   LLRenderMatrix::load(Kind::Projection, ptr);           was: gGL.loadMatrix(ptr);
 *   ModelviewScope m;  m.translate(x, y, z);               push when made, pop when destroyed
 *
 * LL_VK_MATRIX_GUARD_CHECK=1: (1) a migrated call (the ones that take the caller's file and line) warns when the hidden current matrix mode is not the matrix it names, which
 * is where the migration read the mode wrong; (2) when a scope is destroyed the stack of its matrix must be as deep as the scope left it; (3) the stacks that are not empty at
 * the end of a frame are reported once per new depth.
 */

#ifndef LL_LLRENDERMATRIX_H
#define LL_LLRENDERMATRIX_H

#include "stdtypes.h"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"

namespace LLRenderMatrix
{

enum class Kind : U8 { Modelview, Projection, Texture0, Texture1, Texture2, Texture3, CurrentTexture };      // the order of LLRender::eMatrixMode; CurrentTexture: the texture matrix of the texture unit that is active when the call is made (what matrixMode(MM_TEXTURE) selects)

extern bool sGuardCheck;                                                                       // LL_VK_MATRIX_GUARD_CHECK=1

// ---- the operations (llrender.cpp), each on the named matrix; file and line are for the mode check (nullptr: no check, what the scopes pass) ----
#define LL_MATRIX_SITE const char* file = __builtin_FILE(), int line = __builtin_LINE()
void push(Kind kind, LL_MATRIX_SITE);
void pop(Kind kind, LL_MATRIX_SITE);
void set(Kind kind, const glm::mat4& m, LL_MATRIX_SITE);
void load(Kind kind, const F32* m, LL_MATRIX_SITE);                                           // 16 floats, column major (glLoadMatrixf)
void multiply(Kind kind, const glm::mat4& m, LL_MATRIX_SITE);
void multiply(Kind kind, const F32* m, LL_MATRIX_SITE);
void identity(Kind kind, LL_MATRIX_SITE);
void translate(Kind kind, const glm::vec3& v, LL_MATRIX_SITE);
void translate(Kind kind, F32 x, F32 y, F32 z, LL_MATRIX_SITE);
void rotate(Kind kind, F32 degrees, const glm::vec3& axis, LL_MATRIX_SITE);
void rotate(Kind kind, F32 degrees, F32 x, F32 y, F32 z, LL_MATRIX_SITE);
void scale(Kind kind, const glm::vec3& v, LL_MATRIX_SITE);
void scale(Kind kind, F32 x, F32 y, F32 z, LL_MATRIX_SITE);
void ortho(Kind kind, F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar, LL_MATRIX_SITE);
#undef LL_MATRIX_SITE
const glm::mat4& get(Kind kind);
U32 depth(Kind kind);                                                                          // the stack index (0: nothing pushed)
void warnUnbalanced(Kind kind, U32 expected_depth, U32 found_depth, const char* file, int line);
void checkFrameEnd();                                                                          // before the present: reports stacks that are not empty (check mode)

// ---- the guard ----
template<Kind K>
class Scope
{
public:
    explicit Scope(const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : mDepth(0), mLine(line), mFile(file)
    {
        push(K, nullptr, 0);
        mDepth = depth(K);
    }
    // a scope that sets the matrix at once
    explicit Scope(const glm::mat4& m, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : Scope(file, line)
    {
        set(K, m, nullptr, 0);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope()
    {
        if (sGuardCheck && depth(K) != mDepth)
        {
            warnUnbalanced(K, mDepth, depth(K), mFile, mLine);
        }
        pop(K, nullptr, 0);
    }

    void set(const glm::mat4& m) { LLRenderMatrix::set(K, m, nullptr, 0); }
    void multiply(const glm::mat4& m) { LLRenderMatrix::multiply(K, m, nullptr, 0); }
    void identity() { LLRenderMatrix::identity(K, nullptr, 0); }
    void translate(F32 x, F32 y, F32 z) { LLRenderMatrix::translate(K, x, y, z, nullptr, 0); }
    void translate(const glm::vec3& v) { LLRenderMatrix::translate(K, v, nullptr, 0); }
    void rotate(F32 degrees, const glm::vec3& axis) { LLRenderMatrix::rotate(K, degrees, axis, nullptr, 0); }
    void rotate(F32 degrees, F32 x, F32 y, F32 z) { LLRenderMatrix::rotate(K, degrees, x, y, z, nullptr, 0); }
    void scale(F32 x, F32 y, F32 z) { LLRenderMatrix::scale(K, x, y, z, nullptr, 0); }
    void scale(const glm::vec3& v) { LLRenderMatrix::scale(K, v, nullptr, 0); }
    void ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar) { LLRenderMatrix::ortho(K, left, right, bottom, top, zNear, zFar, nullptr, 0); }
    const glm::mat4& get() const { return LLRenderMatrix::get(K); }

private:
    U32 mDepth;
    int mLine;
    const char* mFile;
};

using ModelviewScope = Scope<Kind::Modelview>;
using ProjectionScope = Scope<Kind::Projection>;
using Texture0Scope = Scope<Kind::Texture0>;

}

#endif // LL_LLRENDERMATRIX_H

/**
 * @file llrendermatrix.h
 * @brief F3.3: the matrix vocabulary of the pass code: operations that name the matrix they change, and scoped guards (no GL, no Vulkan in this header).
 *
 * The matrices themselves are still LLRender's stacks (llrender.h). Every operation here names its matrix: it never reads or writes the "current matrix mode" that the old
 * calls (matrixMode, pushMatrix, translatef ...) depend on. pushMatrix / popMatrix flush the immediate-mode batch (pending vertices belong to the old matrix); so do these,
 * through the same LLRender functions.
 *
 *   LLRenderMatrix::push(Kind::Modelview);                 was: gRender.matrixMode(LLRender::MM_MODELVIEW); gRender.pushMatrix();
 *   LLRenderMatrix::translate(Kind::Modelview, x, y, z);   was: gRender.translatef(x, y, z);
 *   LLRenderMatrix::load(Kind::Projection, ptr);           was: gRender.loadMatrix(ptr);
 *   ModelviewScope m;  m.translate(x, y, z);               push when made, pop when destroyed
 *
 * LL_VK_MATRIX_GUARD_CHECK=1: (1) when a scope is destroyed the stack of its matrix must be as deep as the scope left it (an unbalanced push or pop inside the scope shows up as a
 * warning with the file and line of the constructor), (2) the stacks that are not empty at the end of a frame are reported once per new depth.
 */

#ifndef LL_LLRENDERMATRIX_H
#define LL_LLRENDERMATRIX_H

#include "stdtypes.h"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"

namespace LLRenderMatrix
{

enum class Kind : U8 { Modelview, Projection, Texture0, Texture1, Texture2, Texture3 };      // the order of LLRender::eMatrixMode

inline Kind textureKind(S32 unit) { return (Kind)((U8)Kind::Texture0 + (unit < 0 ? 0 : (unit > 3 ? 3 : unit))); }     // the texture matrix of a texture unit (the shaders use units 0-3)

extern bool sGuardCheck;                                                                       // LL_VK_MATRIX_GUARD_CHECK=1

// ---- the operations (llrender.cpp), each on the named matrix ----
void push(Kind kind);
void pop(Kind kind);
void set(Kind kind, const glm::mat4& m);
void load(Kind kind, const F32* m);                                           // 16 floats, column major (glLoadMatrixf)
void multiply(Kind kind, const glm::mat4& m);
void multiply(Kind kind, const F32* m);
void identity(Kind kind);
void translate(Kind kind, const glm::vec3& v);
void translate(Kind kind, F32 x, F32 y, F32 z);
void rotate(Kind kind, F32 degrees, const glm::vec3& axis);
void rotate(Kind kind, F32 degrees, F32 x, F32 y, F32 z);
void scale(Kind kind, const glm::vec3& v);
void scale(Kind kind, F32 x, F32 y, F32 z);
void ortho(Kind kind, F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar);
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
        push(K);
        mDepth = depth(K);
    }
    // a scope that sets the matrix at once
    explicit Scope(const glm::mat4& m, const char* file = __builtin_FILE(), int line = __builtin_LINE())
        : Scope(file, line)
    {
        set(K, m);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    ~Scope()
    {
        if (sGuardCheck && depth(K) != mDepth)
        {
            warnUnbalanced(K, mDepth, depth(K), mFile, mLine);
        }
        pop(K);
    }

    void set(const glm::mat4& m) { LLRenderMatrix::set(K, m); }
    void multiply(const glm::mat4& m) { LLRenderMatrix::multiply(K, m); }
    void identity() { LLRenderMatrix::identity(K); }
    void translate(F32 x, F32 y, F32 z) { LLRenderMatrix::translate(K, x, y, z); }
    void translate(const glm::vec3& v) { LLRenderMatrix::translate(K, v); }
    void rotate(F32 degrees, const glm::vec3& axis) { LLRenderMatrix::rotate(K, degrees, axis); }
    void rotate(F32 degrees, F32 x, F32 y, F32 z) { LLRenderMatrix::rotate(K, degrees, x, y, z); }
    void scale(F32 x, F32 y, F32 z) { LLRenderMatrix::scale(K, x, y, z); }
    void scale(const glm::vec3& v) { LLRenderMatrix::scale(K, v); }
    void ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar) { LLRenderMatrix::ortho(K, left, right, bottom, top, zNear, zFar); }
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

/**
 * @file llrendermatrix.h
 * @brief F3.3: the matrix vocabulary of the pass code: scoped guards that name the matrix they change (no GL, no Vulkan in this header).
 *
 * The matrices themselves are still LLRender's stacks (llrender.h). A scope pushes the named matrix when it is made (this is pushMatrix with the matrix mode folded in:
 * it never reads or writes the "current matrix mode") and pops it when it goes out of scope. Every call that changes a matrix names it. pushMatrix / popMatrix flush the
 * immediate-mode batch (pending vertices belong to the old matrix); the scopes do the same, through the same LLRender functions.
 *
 *   ModelviewScope m;                       was: gGL.matrixMode(LLRender::MM_MODELVIEW); gGL.pushMatrix(); ... gGL.popMatrix();
 *   m.translate(x, y, z); m.rotate(a, axis) was: gGL.translatef(x, y, z); gGL.rotatef(a, ax, ay, az);
 *   m.set(mat);                             was: gGL.loadMatrix(glm::value_ptr(mat));
 *
 * LL_VK_MATRIX_GUARD_CHECK=1: when a scope is destroyed the stack of its matrix must be as deep as the scope left it (an unbalanced push or pop inside the scope shows up
 * as a warning with the file and line of the constructor), and the stacks that are not empty at the end of a frame are reported once per new depth.
 */

#ifndef LL_LLRENDERMATRIX_H
#define LL_LLRENDERMATRIX_H

#include "stdtypes.h"
#include "glm/mat4x4.hpp"
#include "glm/vec3.hpp"

namespace LLRenderMatrix
{

enum class Kind : U8 { Modelview, Projection, Texture0, Texture1, Texture2, Texture3 };      // the order of LLRender::eMatrixMode

extern bool sGuardCheck;                                                                       // LL_VK_MATRIX_GUARD_CHECK=1

// ---- the operations (llrender.cpp), each on the named matrix ----
void push(Kind kind);
void pop(Kind kind);
void set(Kind kind, const glm::mat4& m);
void multiply(Kind kind, const glm::mat4& m);
void identity(Kind kind);
void translate(Kind kind, const glm::vec3& v);
void rotate(Kind kind, F32 degrees, const glm::vec3& axis);
void scale(Kind kind, const glm::vec3& v);
void ortho(Kind kind, F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar);
const glm::mat4& get(Kind kind);
U32 depth(Kind kind);                                                                          // the stack index (0: nothing pushed)
void warnUnbalanced(Kind kind, U32 expected_depth, U32 found_depth, const char* file, int line);
void checkFrameEnd();                                                                          // after_frame: reports stacks that are not empty (check mode)

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
    void translate(F32 x, F32 y, F32 z) { LLRenderMatrix::translate(K, glm::vec3(x, y, z)); }
    void translate(const glm::vec3& v) { LLRenderMatrix::translate(K, v); }
    void rotate(F32 degrees, const glm::vec3& axis) { LLRenderMatrix::rotate(K, degrees, axis); }
    void rotate(F32 degrees, F32 x, F32 y, F32 z) { LLRenderMatrix::rotate(K, degrees, glm::vec3(x, y, z)); }
    void scale(F32 x, F32 y, F32 z) { LLRenderMatrix::scale(K, glm::vec3(x, y, z)); }
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

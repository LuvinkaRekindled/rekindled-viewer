/**
 * @file llglstubs.cpp
 * @brief Stand-ins for the OpenGL entry points the viewer still names but never reaches (branch native-vk, LL_VK_NOGL).
 *
 * The viewer runs without any OpenGL context: the mirror's wrappers answer every GL call that matters, and the GL
 * start-up is replaced (see LLGLManager::initGLNoGL). What is left is code that still mentions the GL 1.1 functions
 * and the WGL calls directly (dead branches, legacy debug helpers). It used to be linked against opengl32.lib, which
 * loaded OPENGL32.dll and the driver's OpenGL ICD (nvoglv64.dll) into the process for nothing.
 *
 * These definitions replace the import: the compiler calls GL functions through the import-table pointers
 * (__imp_<name>), so defining those pointers here satisfies the linker without opengl32.lib. Calling one of them
 * returns 0 and logs the name once, which finds any caller that was missed.
 */

#include "linden_common.h"

#if LL_WINDOWS && defined(LL_VULKAN)

#include <mutex>
#include <set>
#include <string>

namespace
{
    void note_gl_stub(const char* name)
    {
        static std::mutex sMutex;
        static std::set<std::string> sSeen;
        std::lock_guard<std::mutex> lock(sMutex);
        if (sSeen.insert(name).second)
        {
            LL_WARNS("VkOwn") << "real OpenGL function called without OpenGL (stubbed, returns 0): " << name << LL_ENDL;
        }
    }
}

#define LL_GL_STUB(NAME)                                                                    \
    static size_t NAME##_stub() { note_gl_stub(#NAME); return 0; }                          \
    extern "C" void* __imp_##NAME = (void*)&NAME##_stub;

LL_GL_STUB(glBindTexture)
LL_GL_STUB(glBlendFunc)
LL_GL_STUB(glClear)
LL_GL_STUB(glClearColor)
LL_GL_STUB(glColorMask)
LL_GL_STUB(glCopyTexSubImage2D)
LL_GL_STUB(glCullFace)
LL_GL_STUB(glDeleteTextures)
LL_GL_STUB(glDepthFunc)
LL_GL_STUB(glDepthMask)
LL_GL_STUB(glDisable)
LL_GL_STUB(glDrawArrays)
LL_GL_STUB(glDrawBuffer)
LL_GL_STUB(glEnable)
LL_GL_STUB(glFinish)
LL_GL_STUB(glFlush)
LL_GL_STUB(glGenTextures)
LL_GL_STUB(glGetBooleanv)
LL_GL_STUB(glGetError)
LL_GL_STUB(glGetFloatv)
LL_GL_STUB(glGetIntegerv)
LL_GL_STUB(glGetString)
LL_GL_STUB(glGetTexImage)
LL_GL_STUB(glGetTexLevelParameteriv)
LL_GL_STUB(glHint)
LL_GL_STUB(glIsEnabled)
LL_GL_STUB(glLineWidth)
LL_GL_STUB(glMaterialfv)
LL_GL_STUB(glMateriali)
LL_GL_STUB(glPixelStorei)
LL_GL_STUB(glPointSize)
LL_GL_STUB(glPolygonMode)
LL_GL_STUB(glPolygonOffset)
LL_GL_STUB(glReadBuffer)
LL_GL_STUB(glReadPixels)
LL_GL_STUB(glScissor)
LL_GL_STUB(glTexImage2D)
LL_GL_STUB(glTexParameterf)
LL_GL_STUB(glTexParameteri)
LL_GL_STUB(glTexParameteriv)
LL_GL_STUB(glTexSubImage2D)
LL_GL_STUB(glViewport)
LL_GL_STUB(glAreTexturesResident)
LL_GL_STUB(glCopyTexImage2D)
LL_GL_STUB(glPopAttrib)
LL_GL_STUB(glPopClientAttrib)
LL_GL_STUB(glPushAttrib)
LL_GL_STUB(glPushClientAttrib)
LL_GL_STUB(glStencilFunc)
LL_GL_STUB(glStencilMask)
LL_GL_STUB(glStencilOp)
LL_GL_STUB(wglCreateContext)
LL_GL_STUB(wglDeleteContext)
LL_GL_STUB(wglGetCurrentDC)
LL_GL_STUB(wglGetProcAddress)
LL_GL_STUB(wglMakeCurrent)

// GLU is not an import-pointer symbol: the compiler calls it by name
extern "C" const unsigned char* gluErrorString(unsigned int)
{
    note_gl_stub("gluErrorString");
    return (const unsigned char*)"";
}

#endif // LL_WINDOWS && LL_VULKAN

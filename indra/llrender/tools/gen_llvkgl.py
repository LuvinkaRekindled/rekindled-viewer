# Generates indra/llrender/llvkgl.h, llvkgl_hooks.inc and llvkgl_async_dispatch.inc: wrappers that call the real GL function and then tell the Vulkan mirror.
# Run this after changing the FUNCS list below, or when a Firestorm update starts calling a GL function the mirror does not hook yet (see docs/vulkan/PORTING.md):
#     python indra/llrender/tools/gen_llvkgl.py
import os

LLRENDER = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))     # indra/llrender

# (return type, name, parameters)  -- the hook is LLVkGL::on_<name> with the same parameters
FUNCS = [
    # textures
    ('void', 'glGenTextures', 'GLsizei n, GLuint* textures'),
    ('void', 'glDeleteTextures', 'GLsizei n, const GLuint* textures'),
    ('void', 'glBindTexture', 'GLenum target, GLuint texture'),
    ('void', 'glActiveTexture', 'GLenum texture'),
    ('void', 'glTexImage2D', 'GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* pixels'),
    ('void', 'glTexImage3D', 'GLenum target, GLint level, GLint internalformat, GLsizei width, GLsizei height, GLsizei depth, GLint border, GLenum format, GLenum type, const void* pixels'),
    ('void', 'glTexSubImage2D', 'GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels'),
    ('void', 'glTexSubImage3D', 'GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels'),
    ('void', 'glCompressedTexImage2D', 'GLenum target, GLint level, GLenum internalformat, GLsizei width, GLsizei height, GLint border, GLsizei imageSize, const void* data'),
    ('void', 'glTexParameteri', 'GLenum target, GLenum pname, GLint param'),
    ('void', 'glTexParameterf', 'GLenum target, GLenum pname, GLfloat param'),
    ('void', 'glTexParameteriv', 'GLenum target, GLenum pname, const GLint* params'),
    ('void', 'glCopyTexSubImage2D', 'GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint x, GLint y, GLsizei width, GLsizei height'),
    ('void', 'glCopyTexSubImage3D', 'GLenum target, GLint level, GLint xoffset, GLint yoffset, GLint zoffset, GLint x, GLint y, GLsizei width, GLsizei height'),
    ('void', 'glCopyTexImage2D', 'GLenum target, GLint level, GLenum internalformat, GLint x, GLint y, GLsizei width, GLsizei height, GLint border'),
    ('void', 'glGenerateMipmap', 'GLenum target'),
    ('void', 'glPixelStorei', 'GLenum pname, GLint param'),
    # framebuffers and renderbuffers
    ('void', 'glGenFramebuffers', 'GLsizei n, GLuint* framebuffers'),
    ('void', 'glDeleteFramebuffers', 'GLsizei n, const GLuint* framebuffers'),
    ('void', 'glBindFramebuffer', 'GLenum target, GLuint framebuffer'),
    ('void', 'glFramebufferTexture2D', 'GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level'),
    ('void', 'glDrawBuffers', 'GLsizei n, const GLenum* bufs'),
    ('void', 'glDrawBuffer', 'GLenum buf'),
    ('void', 'glReadBuffer', 'GLenum src'),
    # buffers and vertex arrays
    ('void', 'glGenBuffers', 'GLsizei n, GLuint* buffers'),
    ('void', 'glDeleteBuffers', 'GLsizei n, const GLuint* buffers'),
    ('void', 'glBindBuffer', 'GLenum target, GLuint buffer'),
    ('void', 'glBufferData', 'GLenum target, GLsizeiptr size, const void* data, GLenum usage'),
    ('void', 'glBufferSubData', 'GLenum target, GLintptr offset, GLsizeiptr size, const void* data'),
    ('void', 'glBindBufferBase', 'GLenum target, GLuint index, GLuint buffer'),
    ('void', 'glGenVertexArrays', 'GLsizei n, GLuint* arrays'),
    ('void', 'glBindVertexArray', 'GLuint array'),
    ('void', 'glVertexAttribPointer', 'GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer'),
    ('void', 'glVertexAttribIPointer', 'GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer'),
    ('void', 'glEnableVertexAttribArray', 'GLuint index'),
    ('void', 'glDisableVertexAttribArray', 'GLuint index'),
    ('void', 'glVertexAttrib4f', 'GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w'),
    ('void', 'glVertexAttrib4fv', 'GLuint index, const GLfloat* v'),
    # draws
    ('void', 'glDrawArrays', 'GLenum mode, GLint first, GLsizei count'),
    ('void', 'glDrawRangeElements', 'GLenum mode, GLuint start, GLuint end, GLsizei count, GLenum type, const void* indices'),
    # state
    ('void', 'glEnable', 'GLenum cap'),
    ('void', 'glDisable', 'GLenum cap'),
    ('void', 'glBlendFunc', 'GLenum sfactor, GLenum dfactor'),
    ('void', 'glBlendFuncSeparate', 'GLenum sfactorRGB, GLenum dfactorRGB, GLenum sfactorAlpha, GLenum dfactorAlpha'),
    ('void', 'glDepthFunc', 'GLenum func'),
    ('void', 'glDepthMask', 'GLboolean flag'),
    ('void', 'glColorMask', 'GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha'),
    ('void', 'glCullFace', 'GLenum mode'),
    ('void', 'glPolygonOffset', 'GLfloat factor, GLfloat units'),
    ('void', 'glPolygonMode', 'GLenum face, GLenum mode'),
    ('void', 'glLineWidth', 'GLfloat width'),
    ('void', 'glPointSize', 'GLfloat size'),
    ('void', 'glStencilFunc', 'GLenum func, GLint ref, GLuint mask'),
    ('void', 'glStencilOp', 'GLenum sfail, GLenum dpfail, GLenum dppass'),
    ('void', 'glStencilMask', 'GLuint mask'),
    ('void', 'glViewport', 'GLint x, GLint y, GLsizei width, GLsizei height'),
    ('void', 'glScissor', 'GLint x, GLint y, GLsizei width, GLsizei height'),
    ('void', 'glClear', 'GLbitfield mask'),
    ('void', 'glClearColor', 'GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha'),
    # calls that return data or state: the hook checks (and later will supply) the answer
    ('void', 'glGetIntegerv', 'GLenum pname, GLint* params'),
    ('void', 'glGetFloatv', 'GLenum pname, GLfloat* params'),
    ('void', 'glGetBooleanv', 'GLenum pname, GLboolean* params'),
    ('GLboolean', 'glIsEnabled', 'GLenum cap'),
    ('void', 'glGetTexLevelParameteriv', 'GLenum target, GLint level, GLenum pname, GLint* params'),
    ('GLenum', 'glCheckFramebufferStatus', 'GLenum target'),
    # queries
    ('void', 'glGenQueries', 'GLsizei n, GLuint* ids'),
    ('void', 'glDeleteQueries', 'GLsizei n, const GLuint* ids'),
    ('void', 'glBeginQuery', 'GLenum target, GLuint id'),
    ('void', 'glEndQuery', 'GLenum target'),
    ('void', 'glQueryCounter', 'GLuint id, GLenum target'),
    ('void', 'glGetQueryObjectiv', 'GLuint id, GLenum pname, GLint* params'),
    ('void', 'glGetQueryObjectuiv', 'GLuint id, GLenum pname, GLuint* params'),
    ('void', 'glGetQueryObjectui64v', 'GLuint id, GLenum pname, GLuint64* params'),
    # readbacks (answered from the Vulkan images when the mirror owns the resources)
    ('void', 'glReadPixels', 'GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels'),
    ('void', 'glGetTexImage', 'GLenum target, GLint level, GLenum format, GLenum type, void* pixels'),
    ('void', 'glGetCompressedTexImage', 'GLenum target, GLint level, void* img'),
    # shader and program objects (answered by the mirror when it owns the resources)
    ('GLuint', 'glCreateShader', 'GLenum type'),
    ('void', 'glShaderSource', 'GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length'),
    ('void', 'glCompileShader', 'GLuint shader'),
    ('void', 'glGetShaderiv', 'GLuint shader, GLenum pname, GLint* params'),
    ('void', 'glGetShaderInfoLog', 'GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog'),
    ('void', 'glDeleteShader', 'GLuint shader'),
    ('GLboolean', 'glIsShader', 'GLuint shader'),
    ('GLuint', 'glCreateProgram', ''),
    ('void', 'glAttachShader', 'GLuint program, GLuint shader'),
    ('void', 'glDetachShader', 'GLuint program, GLuint shader'),
    ('void', 'glDeleteProgram', 'GLuint program'),
    ('GLboolean', 'glIsProgram', 'GLuint program'),
    ('void', 'glBindAttribLocation', 'GLuint program, GLuint index, const GLchar* name'),
    ('void', 'glLinkProgram', 'GLuint program'),
    ('void', 'glValidateProgram', 'GLuint program'),
    ('void', 'glGetProgramiv', 'GLuint program, GLenum pname, GLint* params'),
    ('void', 'glGetProgramInfoLog', 'GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog'),
    ('void', 'glGetAttachedShaders', 'GLuint program, GLsizei maxCount, GLsizei* count, GLuint* shaders'),
    ('void', 'glGetActiveUniform', 'GLuint program, GLuint index, GLsizei bufSize, GLsizei* length, GLint* size, GLenum* type, GLchar* name'),
    ('void', 'glGetActiveUniformsiv', 'GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params'),
    ('GLint', 'glGetUniformLocation', 'GLuint program, const GLchar* name'),
    ('GLint', 'glGetAttribLocation', 'GLuint program, const GLchar* name'),
    ('GLuint', 'glGetUniformBlockIndex', 'GLuint program, const GLchar* uniformBlockName'),
    ('void', 'glUniformBlockBinding', 'GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding'),
    ('void', 'glUseProgram', 'GLuint program'),
    ('void', 'glProgramParameteri', 'GLuint program, GLenum pname, GLint value'),
    ('void', 'glProgramBinary', 'GLuint program, GLenum binaryFormat, const void* binary, GLsizei length'),
    # sync objects
    ('GLsync', 'glFenceSync', 'GLenum condition, GLbitfield flags'),
    ('GLenum', 'glClientWaitSync', 'GLsync sync, GLbitfield flags, GLuint64 timeout'),
    ('void', 'glWaitSync', 'GLsync sync, GLbitfield flags, GLuint64 timeout'),
    ('void', 'glDeleteSync', 'GLsync sync'),
]

# (return type, name, parameters, value returned with LL_VK_NOGL: the viewer has no OpenGL context and these are not made)
PASS = [
    ('GLenum', 'glGetError', '', 'GLenum(0)'),
    ('const GLubyte*', 'glGetString', 'GLenum name', 'll_nogl_string(name)'),
    ('const GLubyte*', 'glGetStringi', 'GLenum name, GLuint index', '(const GLubyte*)""'),
    ('void', 'glFlush', '', ''),
    ('void', 'glFinish', '', ''),
    ('void', 'glHint', 'GLenum target, GLenum mode', ''),
    ('void', 'glGetBufferParameteriv', 'GLenum target, GLenum pname, GLint* params', ''),
    ('void', 'glObjectLabel', 'GLenum identifier, GLuint name, GLsizei length, const GLchar* label', ''),
    ('void', 'glClientActiveTexture', 'GLenum texture', ''),
    ('void', 'glGetUniformiv', 'GLuint program, GLint location, GLint* params', ''),
    ('void', 'glGetUniformfv', 'GLuint program, GLint location, GLfloat* params', ''),
    ('GLboolean', 'glAreTexturesResident', 'GLsizei n, const GLuint* textures, GLboolean* residences', 'GLboolean(GL_TRUE)'),
    ('void', 'glPushAttrib', 'GLbitfield mask', ''),
    ('void', 'glPopAttrib', '', ''),
    ('void', 'glPushClientAttrib', 'GLbitfield mask', ''),
    ('void', 'glPopClientAttrib', '', ''),
    ('void', 'glDebugMessageCallback', 'GLDEBUGPROC callback, const void* userParam', ''),
    ('void', 'glGetProgramBinary', 'GLuint program, GLsizei bufSize, GLsizei* length, GLenum* binaryFormat, void* binary', ''),
]

def arg_names(params):
    if not params.strip():
        return ''
    names = []
    for p in params.split(','):
        p = p.strip()
        names.append(p.split()[-1].lstrip('*'))
    return ', '.join(names)

out = []
out.append('''/**
 * @file llvkgl.h
 * @brief GENERATED by gen_llvkgl.py -- do not edit by hand.
 *
 * Vulkan mirror: wraps the OpenGL functions the viewer uses to draw. Each wrapper calls the real GL function
 * first (so OpenGL behaviour is unchanged) and then reports the call to the Vulkan mirror, which keeps a
 * translation of the GL state and replays draws into Vulkan (see llvkgl.cpp).
 *
 * Active only in llrender translation units of a build made with -DUSE_VULKAN=ON, and not in files that
 * define LL_VK_NO_INTERCEPT before including llglheaders.h.
 */

#if defined(LL_VULKAN) && !defined(LL_VK_NO_INTERCEPT) && !defined(LL_LLVKGL_H)
#define LL_LLVKGL_H

#include <mutex>
#include "llvkmirror.h"
#include "llvkasync.h"

namespace LLVkGL
{
    // true while the mirror is initialised and accepting calls
    extern bool gActive;
    // every mirror entry point runs under this lock: the viewer calls GL from several threads
    extern LLVkMirror::Mutex gMutex;

    // CPU time this thread spent inside real OpenGL calls (ticks of the performance counter); read by the benchmark
    inline thread_local long long gGlTicks = 0;
    struct GlTimer
    {
        LARGE_INTEGER mStart;
        GlTimer() { QueryPerformanceCounter(&mStart); }
        ~GlTimer() { LARGE_INTEGER e; QueryPerformanceCounter(&e); gGlTicks += e.QuadPart - mStart.QuadPart; }
    };
''')
GEN = {'glGenTextures': 0, 'glGenFramebuffers': 1, 'glGenBuffers': 2, 'glGenVertexArrays': 3, 'glGenQueries': 4}
ANSWER = {'glGetIntegerv', 'glGetFloatv', 'glGetBooleanv', 'glIsEnabled', 'glGetTexLevelParameteriv', 'glCheckFramebufferStatus',
          'glGetQueryObjectiv', 'glGetQueryObjectuiv', 'glGetQueryObjectui64v', 'glFenceSync', 'glClientWaitSync',
          'glReadPixels', 'glGetTexImage', 'glGetCompressedTexImage',
          'glCreateShader', 'glGetShaderiv', 'glGetShaderInfoLog', 'glIsShader', 'glCreateProgram', 'glIsProgram', 'glGetProgramiv',
          'glGetProgramInfoLog', 'glGetAttachedShaders', 'glGetActiveUniform', 'glGetActiveUniformsiv', 'glGetUniformLocation',
          'glGetAttribLocation', 'glGetUniformBlockIndex'}

out.append('    // true when the mirror owns the resources: sink calls are not made in real OpenGL, the answers come from the mirror (LL_VK_OWN)')
out.append('    extern bool gOwn;')
out.append('    // LL_VK_NOGL: there is no OpenGL context at all; calls that would reach real GL are answered here (see ll_nogl in llgl.cpp)')
out.append('    void own_gen(int kind, GLsizei n, GLuint* out);')
out.append('    // DLSS: runs the upscaler on four mirror-owned textures (colour, depth, motion vectors -> output); the gMutex must be held')
out.append('    void on_upscale(const LLVkMirror::UpscaleArgs& args);')
for ret, name, params in FUNCS:
    if ret == 'void':
        out.append('    void on_%s(%s);' % (name, params))
    else:
        out.append('    void on_%s(%s%s result);' % (name, params + ', ' if params else '', ret))
    if name in ANSWER:
        if ret == 'void':
            out.append('    bool own_%s(%s);' % (name, params))
        else:
            out.append('    bool own_%s(%s%s& out);' % (name, params + ', ' if params else '', ret))
out.append('}\n')
out.append('namespace LLVkGL')
out.append('{')
out.append('    // LL_VK_PHASES: calls and CPU time of every sink hook (the time includes waiting for the lock)')
out.append('    constexpr int HOOK_COUNT = %d;' % len(FUNCS))
out.append('    extern long long gHookStatTicks[2 * HOOK_COUNT];     // the second half: calls made by other threads')
out.append('    extern long long gHookStatCalls[2 * HOOK_COUNT];')
out.append('    const char* hook_name(int i);       // name of GL operation i (LLVkAsync::Op order), for the per-operation statistics of the worker')
out.append('    struct HookStat')
out.append('    {')
out.append('        int mId;')
out.append('        long long mStart = 0;')
out.append('        explicit HookStat(int id) : mId(LLVkMirror::gPhaseTiming ? id + (LLVkMirror::onMainThread() ? 0 : HOOK_COUNT) : -1) { if (mId >= 0) mStart = LLVkMirror::phaseNow(); }')
out.append('        ~HookStat() { if (mId >= 0) { gHookStatTicks[mId] += LLVkMirror::phaseNow() - mStart; ++gHookStatCalls[mId]; } }')
out.append('    };')
out.append('}\n')
# ---- commands for the worker thread (LL_VK_ASYNC) ----
SPECIAL = set(GEN) | {'glDeleteTextures', 'glDeleteFramebuffers', 'glDeleteBuffers', 'glDeleteQueries', 'glTexParameteriv', 'glDrawBuffers',
                      'glVertexAttrib4fv', 'glBufferData', 'glBufferSubData', 'glTexImage2D', 'glTexImage3D', 'glTexSubImage2D', 'glTexSubImage3D',
                      'glCompressedTexImage2D', 'glShaderSource', 'glBindAttribLocation', 'glProgramBinary', 'glPixelStorei', 'glBindBuffer',
                      'glBeginQuery', 'glEndQuery', 'glQueryCounter'}
def split_params(params):
    return [q.strip() for q in params.split(',')] if params.strip() else []
out.append('namespace LLVkAsync')
out.append('{')
out.append('    enum Op : unsigned short')
out.append('    {')
for ret, name, params in FUNCS:
    out.append('        OP_%s,' % name)
out.append('        OP_GL_COUNT')
out.append('    };')
disp = ['// generated by gen_llvkgl.py: the worker thread runs a queued GL call']
for ret, name, params in FUNCS:
    if name in ANSWER:
        continue
    if name in SPECIAL:
        out.append('    void enq_%s(%s);       // written by hand in llvkasync.cpp' % (name, params))
        continue
    plist = split_params(params)
    if plist:
        out.append('    struct A_%s { %s };' % (name, ' '.join(q + ';' for q in plist)))
        out.append('    inline void enq_%s(%s) { A_%s a = { %s }; LLVkAsync::push(OP_%s, &a, sizeof(a)); }' % (name, params, name, arg_names(params), name))
        disp.append('        case LLVkAsync::OP_%s: { const LLVkAsync::A_%s* a = (const LLVkAsync::A_%s*)p; LLVkGL::on_%s(%s); break; }' % (
            name, name, name, name, ', '.join('a->' + q.split()[-1].lstrip('*') for q in plist)))
    else:
        out.append('    inline void enq_%s() { LLVkAsync::push(OP_%s, nullptr, 0); }' % (name, name))
        disp.append('        case LLVkAsync::OP_%s: { LLVkGL::on_%s(); break; }' % (name, name))
out.append('}\n')
open(os.path.join(LLRENDER, 'llvkgl_async_dispatch.inc'), 'w', encoding='utf-8', newline='\n').write('\n'.join(disp) + '\n')
for idx, (ret, name, params) in enumerate(FUNCS):
    args = arg_names(params)
    lock = 'std::lock_guard<LLVkMirror::Mutex> lock(LLVkGL::gMutex);'
    phase = 'LLVkMirror::PhaseTimer p_(LLVkMirror::PH_ALL_HOOKS);'
    sink_phase = phase + ' LLVkGL::HookStat s_(%d);' % idx
    if name in GEN:
        body = 'if (LLVkAsync::gEnabled) { LLVkAsync::gen_enq(LLVkAsync::OP_%s, %d, %s); return; } %s LLVkGL::own_gen(%d, %s); LLVkGL::on_%s(%s);' % (
            name, GEN[name], args, lock, GEN[name], args, name, args)
        out.append('static inline void ll_vk_%s(%s) { %s }' % (name, params, body))
    elif name in ANSWER:
        stat = 'LLVkGL::HookStat s_(%d);' % idx
        if name in ('glGetQueryObjectiv', 'glGetQueryObjectuiv', 'glGetQueryObjectui64v'):
            kind = {'glGetQueryObjectiv': 0, 'glGetQueryObjectuiv': 1, 'glGetQueryObjectui64v': 2}[name]
            fast = 'if (LLVkAsync::gEnabled && LLVkAsync::query_get(%d, %s)) return%s; ' % (kind, args, '' if ret == 'void' else ' ' + ret + '()')
        else:
            fast = ''
        zero = 'if (params) *params = 0; ' if name in ('glGetIntegerv', 'glGetFloatv', 'glGetBooleanv', 'glGetTexLevelParameteriv') else ''   # an unanswered getter reads as 0, not as stack garbage
        if ret == 'void':
            body = zero + ('if (LLVkAsync::gEnabled) { %sLLVkAsync::syncCall([&]() { LLVkGL::own_%s(%s); }); return; } { %s %s LLVkGL::own_%s(%s); }' % (fast, name, args, stat, lock, name, args))
            out.append('static inline void ll_vk_%s(%s) { %s }' % (name, params, body))
        else:
            comma = args + ', ' if args else ''
            body = ('if (LLVkAsync::gEnabled) { %s%s o_ = %s(); LLVkAsync::syncCall([&]() { LLVkGL::own_%s(%so_); }); return o_; } '
                    '{ %s %s %s o_ = %s(); LLVkGL::own_%s(%so_); return o_; }') % (fast, ret, ret, name, comma, stat, lock, ret, ret, name, comma)
            out.append('static inline %s ll_vk_%s(%s) { %s }' % (ret, name, params, body))
    else:
        assert ret == 'void', name
        body = 'if (LLVkAsync::gEnabled) { LLVkAsync::enq_%s(%s); return; } %s %s LLVkGL::on_%s(%s);' % (name, args, sink_phase, lock, name, args)
        out.append('static inline void ll_vk_%s(%s) { %s }' % (name, params, body))
    out.append('#define %s ll_vk_%s' % (name, name))
for ret, name, params, nogl in PASS:
    args = arg_names(params)
    if ret == 'void':
        out.append('static inline void ll_vk_%s(%s) { %s; }' % (name, params, nogl or '(void)0'))
    else:
        out.append('static inline %s ll_vk_%s(%s) { return %s; }' % (ret, name, params, nogl))
    out.append('#define %s ll_vk_%s' % (name, name))
# the uniform uploads and program binds of real GL: the mirror gets uniforms through LLGLSLShader's own hooks, so these do nothing
for name in ['glUniform1f', 'glUniform2f', 'glUniform3f', 'glUniform4f', 'glUniform1i', 'glUniform2i', 'glUniform1fv', 'glUniform2fv', 'glUniform3fv',
             'glUniform4fv', 'glUniform1iv', 'glUniform4iv', 'glUniform4uiv', 'glUniformMatrix2fv', 'glUniformMatrix3fv', 'glUniformMatrix3x4fv',
             'glUniformMatrix4fv']:
    out.append('#define %s(...) ((void)0)' % name)
out.append('\n#endif // LL_VULKAN && !LL_VK_NO_INTERCEPT\n')

inc_lines = ['// generated by gen_llvkgl.py: storage and names for the per-hook statistics (LL_VK_PHASES)', 'namespace LLVkGL', '{',
             '    constexpr int HOOK_COUNT = %d;' % len(FUNCS), '    long long gHookStatTicks[2 * HOOK_COUNT] = {};', '    long long gHookStatCalls[2 * HOOK_COUNT] = {};',
             '    const char* const kHookNames[HOOK_COUNT] = { ' + ', '.join('"%s"' % n for r, n, p in FUNCS) + ' };', '}', '']
open(os.path.join(LLRENDER, 'llvkgl_hooks.inc'), 'w', encoding='utf-8', newline='\n').write('\n'.join(inc_lines))
path = os.path.join(LLRENDER, 'llvkgl.h')
open(path, 'w', encoding='utf-8', newline='\n').write('\n'.join(out))
print('wrote', path, len(FUNCS), 'functions')


/**
 * @file rkvkpasscensus.h
 * @brief S6.0 (doc/vulkan/S6-design.md): which state a render pass reads that an earlier pass left behind. Diagnostics only (LL_VK_PASSCENSUS=1), no behaviour change.
 *
 * Every state class the worker keeps across passes (render state block, program, program uniform class blocks, texture units, uniform buffer bindings, vertex binding)
 * is stamped with the pass and frame of the record that last wrote it. The first read of a class in a pass is counted by the age of its stamp:
 * written in this pass (own), earlier in this frame, in an earlier frame, or never. The report (every 600 presented frames) has one row per pass site and target.
 *
 * Compiled only with -DUSE_VULKAN=ON.
 */

#ifndef RK_RKVKPASSCENSUS_H
#define RK_RKVKPASSCENSUS_H

#if defined(RK_VULKAN)

#include "linden_common.h"

#include <string>

namespace RKVkPassCensus
{
    extern bool sOn;
    extern int sPoisonMask;     // LL_VK_PASSPOISON (default 127): which classes poison mode forgets: 1 program uniform blocks, 2 texture units, 4 uniform buffer bindings, 8 constant attributes, 16 vertex binding, 32 render state, 64 renderer program
    extern int sClose;          // S6.2 LL_VK_PASSCLOSE: 1 passes are closed (always), 2 plus the worker compares every announced class block with what it held, 3 plus the worker forgets the inherited state at every pass (poison)
    extern int sMode;           // 1: the viewer's render target binds are the passes, 2: the Vulkan render passes the renderer begins, 3: the GPU zone phases (needs --set FSGpuZoneTimers,1)
    extern U64 sPass;           // the census pass being played (0: before the first of the run)
    extern U64 sFrame;          // the census frame (counted by CMD_FRAME_BEGIN)

    enum Age { AGE_OWN, AGE_FRAME, AGE_OLDER, AGE_NEVER, AGE_COUNT };
    struct Stamp { U64 mPass = 0; U64 mFrame = 0; };
    inline Age ageOf(const Stamp& s)
    {
        if (!s.mPass) return AGE_NEVER;
        if (s.mPass == sPass) return AGE_OWN;
        return s.mFrame == sFrame ? AGE_FRAME : AGE_OLDER;
    }
    inline void stampNow(Stamp& s) { s.mPass = sPass; s.mFrame = sFrame; }

    // kept in every RKVk::Program: when each uniform class block was last written by the stream, and what this pass has written / read of it
    struct ProgState
    {
        Stamp mWrite[5];
        U32 mWriteBytes[5] = {};            // bytes written to the class in pass mWriteBytesPass
        U64 mWriteBytesPass[5] = {};
        U64 mReadPass[5] = {};              // the pass that first read the class (draw time)
    };

    // ---- producer (main thread): where a pass was bound; the id travels in the pass record (S6.1), the worker turns it back into a name for its logs ----
    // kind: 'b' a bind, 'r' the restore of the pass an inner one interrupted, 'f' the flush that goes back to the screen
    U32 internSite(const char* file, int line, char kind);
    std::string siteName(U32 id);

    // ---- worker ----
    void init();
    // a record of the stream is about to be played (op: RKVkAsync::Cmd; packet_kind: header kind for CMD_SHADOW_PACKETS)
    void frameBegin();
    void present();                         // a frame was presented: the report is due every 600
    void passBegin(S32 fbo, U32 site);              // mode 1: a pass record
    void renderPassBegin(const char* label);        // mode 2: the renderer began a Vulkan render pass
    void zone(const char* name, bool begin);        // mode 3
    void opDone(U16 op, U32 packet_kind, U64 ticks);
    void stateReceived();
    void stateRead();
    void programAnnounced();                // a program bind or a draw record named a program
    void packetProgram(const void* program, const char* name);      // a packet record that draws with the renderer's program
    void drawProgram(const void* program, const char* name);
    void binding(bool carried);             // first draw of a pass: the vertex binding came with it (true) or was inherited
    void unitSet(U32 unit);
    void unitRead(U32 unit);
    void uboSet(U32 index);
    void uboRead(U32 index);
    void uniformWrite(ProgState& ps, U32 cls, U32 bytes);
    void classRead(ProgState& ps, U32 cls, U32 bytes, const char* program_name);
    void announceCompare(const char* program, U32 cls, bool same, U32 bytes);       // mode 2
    void closeReport();
    void prepareSeen();                     // kind 21
    void geoDrawSeen();                     // kinds that read the prepare output: counts the ones from another frame
}

#endif // RK_VULKAN

#endif // RK_RKVKPASSCENSUS_H

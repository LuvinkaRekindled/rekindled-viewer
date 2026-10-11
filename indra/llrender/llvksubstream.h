/**
 * @file llvksubstream.h
 * @brief Recording draw work on another thread (F3), for code outside the render library (no LL_VULKAN needed to include this).
 *
 * A thread that calls begin() writes its render state, texture binds, uniforms and draws into a private buffer of records (its own render state context, its own pending
 * slots and uniform writes); the main thread calls splice() at the point of the frame where that work would have run, and the buffer joins the queue, in order. See
 * LLVkAsync::SubStream. The sub-stream starts from the render state the main producer has when it is constructed, so build it on the main thread where the work belongs.
 */

#ifndef LL_LLVKSUBSTREAM_H
#define LL_LLVKSUBSTREAM_H

class LLVkSubStream
{
public:
    // true when records can be written at all (the Vulkan worker runs)
    static bool usable();

    LLVkSubStream();                // main thread
    ~LLVkSubStream();
    void begin();                   // the thread that records
    void end();                     // the same thread
    void splice();                  // main thread: the buffer joins the queue here
    unsigned records() const;

private:
    LLVkSubStream(const LLVkSubStream&) = delete;
    LLVkSubStream& operator=(const LLVkSubStream&) = delete;
    void* mImpl;
};

#endif // LL_LLVKSUBSTREAM_H

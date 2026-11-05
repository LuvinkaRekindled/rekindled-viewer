/**
 * @file llmatrixtable.h
 * @brief The matrix table: the texture matrices of faces, allocated from a table instead of the heap.
 *
 * doc/vulkan/PLAN-clean-core.md step C3.4. A face that needs a texture transform (a texture entry with an offset, rotation or animation) owned `new LLMatrix4()`, freed with `delete` when the face dropped it,
 * and the draw infos kept the same raw pointer: a draw info that outlived the delete read freed memory. The matrices are rows of this table now: the face and the draw info keep the pointer they always
 * had (every use is unchanged), but a freed matrix waits in the table's quarantine (8 frames, llrowtable.h) and stays readable, and every matrix has a stable id (LLMatrixTable::id(ptr)) for the GPU side
 * (an instance and draw table that names its matrix by id instead of copying it per draw). Matrices are address-stable (rows never move). Rows are contiguous in chunks.
 *
 * The model matrices of the draw infos point into the instance table's transforms (llinstancetable.h), which are quarantined the same way since C1.
 */

#ifndef LL_LLMATRIXTABLE_H
#define LL_LLMATRIXTABLE_H

#include "llrowtable.h"
#include "m4math.h"
#include <cstddef>

struct LLMatrixRow
{
    const void* owner;      // the face the matrix belongs to (nullptr while free)
    U32 state;              // the canary of the table (0 while used and after release)
    U32 id;                 // this row's id
    LLMatrix4 matrix;
};

using LLMatrixRows = LLRowTable<LLMatrixRow, (1u << 19), 10>;

namespace LLMatrixTable
{
    // a matrix (identity, as `new LLMatrix4()` made it) for `owner`; never nullptr (the table is fatal when full: 524,288 matrices)
    inline LLMatrix4* alloc(const void* owner)
    {
        const U32 id = LLMatrixRows::acquire(owner);
        if (!id)
        {
            LL_ERRS() << "the matrix table is full (" << LLMatrixRows::kCapacity << " matrices)" << LL_ENDL;
        }
        LLMatrixRow& r = LLMatrixRows::row(id);
        r.id = id;
        r.matrix.setIdentity();
        return &r.matrix;
    }

    inline LLMatrixRow* rowOf(const LLMatrix4* m)
    {
        return reinterpret_cast<LLMatrixRow*>(const_cast<char*>(reinterpret_cast<const char*>(m)) - offsetof(LLMatrixRow, matrix));
    }

    // the stable id of a matrix of the table (0: nullptr)
    inline U32 id(const LLMatrix4* m) { return m ? rowOf(m)->id : 0; }

    // the matrix is given back; its row waits in the quarantine, a stale pointer to it still reads the matrix as it was
    inline void free(LLMatrix4* m)
    {
        if (m)
        {
            LLMatrixRows::release(rowOf(m)->id, 0);
        }
    }
}

#endif // LL_LLMATRIXTABLE_H

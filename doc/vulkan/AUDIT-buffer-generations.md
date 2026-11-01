# Audit: what the GPU side remembers about a buffer, and whether it checks the generation (2026-11-01)

Why: the resident pair crash (plan 8.31) and the ray scene's "bad range in a structure to build" warnings (8.33) had one shape: a buffer *name* (or a pointer to its entry) is remembered across frames, the buffer is respecified, deleted or recycled by the vertex buffer pool (its `mEpoch` changes; `sPacketBufferGeneration` moves when an entry goes), and the remembered range or pointer is used over contents it was not made for. This is the list of every such memory in the worker and the ray scene, what each checks, and what was done.

How a generation shows: `BufferInfo::mEpoch` (other contents altogether: respecified with another size, deleted, recycled), `mVersion` (every write), `mMoves` (same contents at another address); `sPacketBufferGeneration` (a buffer entry went: cached `Buf*` pointers are void). A remembered `(name, first, count)` is only good for the epoch it was noted under.

| memory | holds | checked by | verdict |
|---|---|---|---|
| resident pair (`ResPair`, resident fragments and the scene geometry table's `GeoFrag`) | the buffers' names, `Buf*` pointers, the addresses written into the entries | `res_pair_check`: generation, then epoch + moves; `res_pair_buffers` before any pointer is used (2026-11-01: a pointer to a gone entry was written through, the crash of 8.31) | fixed. The entries keep their `(first, count)` and only get new addresses when an epoch changes; counted (`VkResident`, `LL_VK_RES_EPOCH_LOG`): none seen in the laps, so no change of behaviour |
| ray scene group ranges (`Group::mRanges`) | `(vb, ib, first, count)` kept across frames | was: size only; now the buffer epochs of the latest note per range (`mRangeEpochs`), a range noted for an earlier epoch is left out of a build (`LL_VK_RT_EPOCH`), a stale unheld range leaves the group after 240 frames | fixed (8.33): bad range flags 100-130 a lap -> 0-14, 8 of 9 laps 0 |
| ray scene structure (`Blas`) | the epochs it was built for | `b.mVbEpoch != vi.mEpoch` drops it | ok (was already there) |
| ray scene alpha records / instance cache (`mCacheVb`, `mCacheIb`) | buffer addresses the records were made with | address change at the first note of a frame marks the group dirty | ok for addresses; the ranges they name are covered by the group fix |
| ray scene skinned draws (`SkinDraw`) | `(vb, ib, first, count, vertex range)` | noted again every frame (the palette is valid for one frame), sizes checked at the build | ok |
| packet record lookups (`cache_generation` in the packet builders) | `Buf*` found by name | `sPacketBufferGeneration` | ok |
| draw bindings (`BindingKey`, `RDraw`) | made at the draw from the current table; the index buffer by name looked up at the draw | none needed | ok |
| ray scene debug probe (`ProbeBlas`) | epochs kept | epochs | ok |
| viewer scene fragments (`LLSceneFragment`, served items) | `LLDrawInfo` pointers | `mSerial` against `group->mDrawMapSerial` | ok by design; not changed |

## Left for the next pass

- **Textures.** The same shape for texture names: ray scene alpha records and the pin table name textures by GL name (`mTextures`); a texture respecified under the same name is not tracked by an epoch the way buffers are. Look at `rtPinAcquire` and the alpha record's texture slots.
- **Sub-stream copies** (`llvkasync.h`): copies of state made for another producer thread.
- **The remaining NaN-only flags** (0-14 a lap, whole vertices, epochs matching): not written by the volume fill (`LL_VK_GEOMCHECK` found none in any lap). Candidates: another fill path (trees, grass, terrain, avatar bodies). The warning now names the bad slots.

## Rule for new caches

Anything that remembers a buffer by name or by pointer across frames records the buffer's epoch (and the cache generation for a pointer) when it is made, and checks both where it is used, with a counter and a log line (first 20, then every Nth) when the check fails. Do not repair silently: the safety nets (the builder's data check, the packet bounds) hid this for a long time.

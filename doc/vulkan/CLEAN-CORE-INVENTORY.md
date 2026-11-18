# Clean core: the ownership inventory, first pass (reading + a text census, 2026-11-05; nothing changed)

Step 0 of PLAN-clean-core.md: for the four render-side classes (LLDrawable, LLFace, LLDrawInfo, LLSpatialGroup), what each field IS (state, derived cache, copy of something the object owns, link), who touches it, and what the C1-C6 rows would look like. Method: read the headers (lldrawable.h, llface.h, llspatialpartition.h) and count uses over indra/newview by file category (render core: pipeline, llspatial*, lldrawpool*, llface, lldrawable; object classes: llvo*, llviewerobject, llviewerjoint*; UI / tools: llfloater*, llpanel*, lltool*, llselectmgr, llmanip*, llviewerwindow, fs*, ...). The census is TEXT matching: generic names (getParent, getRegion, getScale, getPositionAgent, isAvatar, setState on other classes) are inflated by other classes' members; the LLFace/LLDrawInfo/LLSpatialGroup specific names are reliable to within a few percent. A precise pass should be compiler-assisted (make a field private/const step by step and read the errors); this note says where to point it.

## 1. The surface the outside world sees

| Class | Used by | The part that is NOT render-internal |
|---|---|---|
| LLDrawInfo | render core 70-90% of every member (mVertexBuffer 102 of 132 uses, mModelMatrix 70 of 73, mCount 132 of 151) | mMaterial 18 uses in UI / tools (4 files: material editor / pathfinding console / GLTF list); everything else is render core or the geometry generators (llvovolume, llvopartgroup, llvograss: they build draw infos). **Can be deleted without a facade.** |
| LLSpatialGroup | render core (getSpatialPartition 76 of 83, mDrawMap 69 of 76, rebuildGeom/Mesh all render core) | lineSegmentIntersect 18 uses (picking: render core 8, object classes 6, UI 3); dirtyGeom 16 (object classes). The octree stays for CPU queries, so the group survives as a spatial index entry. |
| LLFace | render core + the geometry generators (llvovolume etc.): getTexture 109 (render 49, object 39, UI 8 in 5 files), getVertexBuffer 41, getIndicesCount 37 (object classes 31), getGeomCount 44 (37 in object classes), getDrawable 89 | UI / tools: getTexture 8 (5 files), setTexture 2, getViewerObject 6 (2 files), getXform 5, renderSelected 3 (render core). **Tools reach a handful of calls: a thin LLFace view over rows covers them.** |
| LLDrawable | everywhere: getFace 113 / getNumFaces 138 (UI / tools 14 + 29 calls in 7-9 files: the selection and texture tools walk faces), isState 130 / setState 91 / clearState 55 across all categories, getVObj 120 (render core 83), getSpatialGroup 63, getBounds 24 (render core) | The flag word (mState: IN_REBUILD_Q, ON_MOVE_LIST, ACTIVE, REBUILD_*, DEAD, BUILT, ...) is a protocol every module speaks: it stays as an inline accessor over a row. getFace/getNumFaces for the tools: a face range per instance. |

Reading: the UI / tools surface is small and mostly face iteration, texture get/set and the transform. The big surface is the geometry generators in the object classes (llvovolume, llvopartgroup, llvograss, llvotree, llvowater, llvosurfacepatch, llvosky) writing faces and draw infos, and render core reading them.

## 2. Fields by what they are

### LLDrawable (mXform, mParent, mState, mFaces, mVObjp, ...)
- **Link / identity:** mVObjp (counted pointer to the object), mParent (counted pointer, a copy of the object's parent relation), mSpatialBridge, mFaces (owner of the face list).
- **Copy of what the object owns:** mXform (world/local position, rotation, scale: authoritative in the object, the drawable's is the render copy updated by updateXform), mCurrentScale.
- **State:** mState (flag word, 28 bits), mRenderType.
- **Derived caches:** mRadius (bounds), mDistanceWRTCamera (per frame), mAttJointApplied[15] + mAttJointValid (the rigid attachment skip, ours).
- **Dead:** mGeneration (never read or written outside the class).
- Row candidate (C1): instance row = transform (position, rotation, scale), extents/radius, flag word, parent row index, object handle, face range (first, count). mDistanceWRTCamera is per-view and leaves the row.

### LLFace (the biggest)
- **Links:** mDrawablep (raw), mVObjp (counted), mTEOffset (which texture entry).
- **Where its geometry lives (state owned by the rebuild):** mVertexBuffer, mVertexBufferGLTF, mGeomIndex, mGeomCount, mIndicesIndex, mIndicesCount, mReferenceIndex, mRiggedIndex, mTextureIndex, mIndexInTex[ch]. Writers: 22 write sites of mVertexBuffer across pipeline (9), llvopartgroup (5), lldrawpool (4), llspatialpartition (3), the generators.
- **Bounds (derived from the geometry):** mExtents[2], mRiggedExtents[2] (on demand, may be stale), mCenterLocal, mCenterAgent (written by llvosurfacepatch 7, llvosky 4, ...), mTexExtents, mBoundingSphereRadius.
- **Texture priority caches (the texture system's state kept in the face):** mVSize, mPixelArea, mImportanceToCamera, mLastPixelAreaUpdate, mInFrustum, mLastTextureUpdate, mRecIdx[ch] (RKTexRecs), mDistance, mLastUpdateTime, mLastSkinTime, mLastMoveTime. Written from llviewertexturelist and rkviewertexturerecs on the main thread; the GPU mip feedback would replace most of them.
- **Counted references (the shared-write hazards):** mTexture[ch] (LLPointer<LLViewerTexture> x ch), mSkinInfo (const pointer), mAvatar (raw), plus the owned matrices mTextureMatrix / mSpecMapMatrix / mNormalMapMatrix (raw pointers into the object).
- **Legacy pool membership (delete with the pools):** mDrawPoolp, mPoolType, mDrawInfo (back-pointer to the draw info made from this face).
- **Other state:** mState, mFaceColor (+ USE_FACE_COLOR), mHasMedia / mIsMediaAllowed, mDrawOrderIndex, the Catznip texture toggle (mShowDiffTexture, mOrigDiffTexture).
- Row candidate (C2/C3): face row = instance index, TE index, geometry slice (arena offset, vertex count, index offset, index count), texture ids per channel (32-bit ids, not pointers), material handle, texture-transform matrix index, face colour, flags, extents. The priority caches move to a side table keyed by texture id.

### LLDrawInfo
- A **derived copy of a run of faces** (made by LLVolumeGeometryManager::registerFace and the part group): mVertexBuffer, mStart/mEnd/mCount/mOffset, mTexture, mSpecularMap, mNormalMap, three matrix pointers + mModelMatrix, mAvatar, mSkinInfo, mMaterial, mGLTFMaterial, mSpecColor, blend functions, mBump/mShiny/mFullbright/mHasGlow, mAlphaMaskCutoff, mEnvIntensity, mShaderMask, mExtents. Counted refs: mVertexBuffer, mTexture, mSpecularMap, mNormalMap, mAvatar (LLPointer), mSkinInfo, mMaterial, mGLTFMaterial, mTextureList (vector of counted pointers). Ours: mGeoServed, mParticleRows, mRtMaterial (atomic).
- After C2-C5 this is a record the GPU draw tables already hold (resident rows, the scene table): nothing needs the class.

### LLSpatialGroup
- **Octree node (stays as the CPU spatial index):** from LLOcclusionCullingGroup / LLViewerOctreeGroup (bounds, state, occlusion state, visibility stamps per camera id).
- **Derived draw data (delete with the draw infos):** mDrawMap (U32 type -> vector<LLPointer<LLDrawInfo>>), mVertexBuffer, mBufferMap, mDrawMapSerial, mSceneFragment, mGeoServedMask, mGeoServedTris, mGeoAllServed, mGeoHandle, mSceneSeenSerial/Frame, mChurn*.
- **Per-group caches written by whichever view culls it (shared-write hazards between views):** mDistance, mDepth, mLastUpdateDistance, mLastUpdateTime, mPixelArea, mRadius, mViewAngle, mLastUpdateViewAngle; the state bits (GEOM_DIRTY, ALPHA_DIRTY, IN_BUILD_Q1/2, FS_RELEASED, FS_WAKING, NEW_DRAWINFO, ...).
- **Alpha ordering for avatars:** mAvatarp, mRenderOrder. Others: mBridgeList, mReflectionProbe, mObjectBoxSize, mGeometryBytes, mSurfaceArea, mBuilt.

## 3. What blocks parallelism today, from the fields

1. Counted pointers on faces and draw infos (mTexture[ch], mSpecularMap, mNormalMap, mMaterial, mGLTFMaterial, mSkinInfo, mAvatar, mVertexBuffer): every copy of a draw info or a face writes shared counts. Made atomic in plan 8.8 levels 1+2 (no measurable cost), so this is no longer what stops parallel work; it remains the reason faces and draw infos cannot be plain rows. C2 (ids and handles) removes them. What stops a parallel group rebuild today is registerFace merging each face into the previous draw info of the pass (order dependence, plan 8.9).
2. Per-view caches stored on shared objects (the group's mDistance / mDepth / mLastUpdate*, the drawable's mDistanceWRTCamera, the face's mDistance): two views culling the same group write the same field. The View/ledger machinery (W1-W3) works around it; rows keyed by (view, entry) remove it.
3. Geometry placement written by the rebuild into the face while draw infos copy it (mGeomIndex, mIndicesIndex, mVertexBuffer): a face and its draw info can disagree mid-rebuild (the ordering problems of the R3b studies). One geometry slice per face, owned by the arena, removes the copy.
4. Priority caches in the face (mVSize, mPixelArea, mRecIdx, mLastTextureUpdate, mInFrustum): texture system state living on the render object, written on the main thread. Moves to the texture side.
5. The flag word on the drawable (mState): every module writes bits non-atomically from its own phase; a row with atomic bits (or per-owner bit sets in the ledger) makes the phases explicit.

## 4. First row layout (to be checked, C1-C3)

- **Instance row** (per LLDrawable): U32 object handle, U32 parent instance, 12 floats transform (or pos + rot + scale), 6 floats extents, F32 radius, U32 flag word (atomic), U32 first face, U16 face count, spatial group id.
- **Face row**: U32 instance, U8 TE index, geometry slice {U32 arena offset, U16 vertex count, U32 index offset, U32 index count}, U32 texture id x 4 channels, U32 material handle, U32 texture transform matrix id, packed face colour, U16 flags, 6 floats extents.
- **Group row**: node bounds, state bits, first instance (per partition array).
- Side tables: texture priority (by texture id), material handles (by id), matrices (by id), skin infos (by id).

## 5. What I would do first

1. **C2 on faces and draw infos only** (the smallest mechanical step: texture ids and a material handle on LLFace/LLDrawInfo, resolved at the few places that need the object; compare the draw info lists against the old ones). CORRECTION: this does NOT make the parallel group rebuild possible again: the counts are already atomic (plan 8.8-8.9, no cost) and what stops a parallel rebuild is registerFace's order dependence. C2 is pure structure (prerequisite for C3/C5), no measurable gain by itself. Size from the census: about 470 use sites in about 15 files (mTexture 93, mTextureList 57, mSpecularMap/mNormalMap 29 each, mMaterial 35, mGLTFMaterial 45, mAvatar 106, mSkinInfo 81), mostly render core and the geometry generators.
2. **C1 for drawables:** transform + extents + flags in rows, LLDrawable accessors inline over the row; the cull and sort jobs read rows; check = identical cull lists in order (the split checks already do this).
3. **Compiler-assisted census** before each step: make the target members private/const, let the compiler list every touching site, count them per file category, then change the sites in one pass.

## 6. C1 census (2026-11-05, text census, nothing changed)

The plan said "the view prep jobs (cull, sort) read rows". The census says that sentence belongs to the **group**, not the drawable.

**What the cull reads.** llvieweroctree.cpp touches no drawable field. The walk reads the octree group: bounds / object bounds / extents (4 x 2 LLVector4a, 128 bytes), the state word, the per-camera visible stamps (mVisible[cam]) and occlusion state (mOcclusionState[cam]), and the entry's extents. Per-view write fields on the group (mDistance, mDepth, mRadius, mViewAngle) are the shared-write hazard of section 3.2.

**Drawable surface (C1a).**
- mXform is the only public datum: 51 text sites (lldrawable 13+9 in the header, llviewerobject 12 which are mostly the object's own xform, llvoavatar 5, llviewerjointattachment 5, llface.h 3, the manipulators / select manager 4), plus getXform() 27 sites (llvoavatar 12). Small.
- Flags (mState), radius, scale, render type are already private behind accessors. isState / setState / clearState have 422 call sites in 55 files, but all go through five inline functions, so moving the flag word into a column is a change of five bodies.
- mDistanceWRTCamera: 29 sites in 11 files (the per-view datum; leaves the row).
- Consumers of drawable rows today: updateMove / the moved list (main thread), rebuild, attachments, and the model matrices the draw infos point at. Nothing in the cull or sort.

**Group surface (C1b).** getBounds 25, getObjectBounds 6, getExtents 10, getObjectExtents 6, mOcclusionState 19, isOcclusionState 44, mVisible[] 6, getOctreeNode 20, group mDistance/mDepth/mRadius around 200 hits mixed with other classes. A flat group column set (bounds, state, stamps by camera) is where a cull job gets contiguous reads.

**Conclusion.** C1a (drawable columns) is cheap to do and has no cull consumer; its consumers are the moved list and, later, the GPU instance buffer (C4: a moved object writes one row instead of re-uploading its draws' matrices). C1b (group columns) is what the cull and the sort read. Order proposed: C1b first, then C1a when C4 gives it a reader.

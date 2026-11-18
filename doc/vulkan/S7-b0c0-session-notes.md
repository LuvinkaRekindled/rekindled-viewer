<!-- Copied verbatim on 2026-10-23 from the stopped s7-measure session's notes. Context: its baseline runs were taken with the row-copy bug (3c142c98 fixed it: plaza 42 -> 68 fps), and its 09:48 exe (device lost) is on branch s7-census-wip. The census numbers below are exact counts per frame and still usable as ratios; do not trust its absolute frame times. -->
# S7 B0/C0 session notes (stopped by Adam mid-run; nothing committed)

## State
- Uncommitted in the tree: rkviscensus.h (new) + hooks in pipeline.cpp, llvieweroctree.cpp, llvovolume.cpp, llspatialpartition.h, rkhiz.h, rkvkbridge.h/.cpp, rkvkworker.cpp (Hi-Z batch carries table-item boxes; second dispatch against the previous pyramid). Built OK (exe 09:48, snapshot 20261023-094844). All behind LL_VK_VISCENSUS=1 except: Hi-Z results buffer is now 2x32768 slots (6.3 MB instead of 0.4 MB) and the test shader has mode/first push constants (mode 0 = old behaviour).
- No viewer running at the stop.

## Runs (London City, automated login via rt-run-shot3.ps1, LL_VK_PRESENT=immediate from the wrapper)
| run | exe | diagnostics | result |
|---|---|---|---|
| s7b-base1 | HEAD d8d9bd21 (08:38) | none | 40.0-45.9 fps steady (about 42, 23.5 ms), 85 avatars in range, 4 min, no crash |
| s7b-zones1 | same | FS_CPU_ZONES | 41.5-42.3 fps, loop 23.7-23.9 ms: display() 18.8-19.0, idle() 4.65-4.70; named zones in display() sum about 11 ms |
| s7b-census1 | new (09:48) | LL_VK_VISCENSUS=1 (laps + box test) | 42 fps steady (frame 23.2-24.0 ms), then VK_ERROR_DEVICE_LOST after about 2 min, in a Deferred Alpha Pull draw of the "uploads and copies" pass; fault page inside a 256 MB arena buffer |
| s7b-census2 | new | LL_VK_VISCENSUS=1, LL_VK_VISCENSUS_BOXES=0 | device lost after about 70 s, again a 256 MB buffer page fault, VRAM 10430 of 12840 MB |
| s7b-zones2 | new | FS_CPU_ZONES | killed by Adam, no result |

## Open issues (not solved)
- The baseline frame is about 42 fps / 23.5 ms with the GPU under about 70 % (Adam), while earlier today the same spot gave 56-60 fps (v0a/v0c, 69 avatars; 85 avatars now). Main thread waits about 8 ms inside display() in no named zone. Cause not found.
- Device lost 2 of 2 on the new exe with census on, 0 of 2 on the old exe; the new exe without any census switch was not run to completion, so whether my Hi-Z changes (buffer size, shader push constants, invalidate size) cause it is UNKNOWN. No earlier log in <work>\run-logs (last 60) has a device lost.
- Data from census1 (exact counts, windows 2-7 of 8): see agg.py output in the session; per frame about 5,800 table items, 14.9 M tris of which 13.7 M rigged; static opaque 3,317 items: frustum 55, hidden by the previous pyramid 967 (31 % of items, 6.8 % of tris with frustum); static alpha runs 1,127: 504 (45 %, 3.9 % tris); rigged items 0 dropped (260 of 11.7 M tris); all items: 1,526 (26 %), 88 k tris (0.59 %).

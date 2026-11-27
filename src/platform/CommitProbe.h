// SPDX-License-Identifier: GPL-3.0-or-later
// Dev-only commit probes (M3 Increment 6), compiled only into DEBUGTOOLS builds (`./dev build DEBUGTOOLS=1`; `./dev run
// --commitprobe <mode>` builds that variant by itself). A release build has neither the code nor the strings.
//
// Both modes run on a WORKER thread created with SDL_CreateThreadWithStackSize (the install worker's stack size, 64 KB unless
// commitprobe_stack says otherwise), never on the main thread, and only on the SD card root. Every log line starts with
// "commitprobe:". Both modes remove everything they created, also after a failure, and log "commitprobe: cleaned".
//
// MODE cap    (devstart: commitprobe=cap, commitprobe_files=N, commitprobe_stack=KB)
//   Measures a worst-case update on the real file system: heap in use, elapsed time per phase, stack high-water.
//   Layout, N = commitprobe_files (default 5000, 10..19500; 19000 sits just under both plan caps: 20000 ops, 40000 manifest paths):
//     old app dir  apps/osprobe: boot.dol, 50 shared folders s0..s49 each with `shipped` and ~N/50 user files (u<i>), and an old-only
//                  folder `gone/` with N files that the old manifest lists (installed/osprobe.json). Marker: apps/osprobe/.osprobe-marker.
//     staged tree  apps/OpenShop/staging/990001/root/apps/osprobe: boot.dol, s0..s49 with `shipped` plus ~N/50 new files each.
//   The update then needs ~N restoreFile ops (one per user file in the shared folders; `gone/` is ONE rename), a merged manifest of
//   ~2N paths and a journal of ~N ops: the plan, merge, both dry runs and the journal JSON are all near the caps at N=19000.
//   The commit runs with `onPhase` hooks that sample the heap at: before, walk, merge, dryrun-manifest, dryrun-journal, journal,
//   ops, done, after (the "peak" is the highest of those samples; there is no sampler thread, so a peak between two hooks is
//   missed). Building the synthetic trees takes 3N file creations (libfat: slow; it logs progress) and is NOT part of the numbers.
//   Refuses to run when apps/osprobe, staging/990001, installed/.journal(.bak) or installed/osprobe.json exists without the
//   probe's own marker (it removes its own leftover from an earlier cut run first).
// MODE flush  (commitprobe=flush)
//   Writes three files under sd:/apps/OpenShop/probe/ with distinctive ASCII markers "OSPROBE-<A|B|C>-<8 hex>":
//     A.txt  rec::writeRecord = writeFileAtomic with fsync before the rename (what saveJournal does),
//     B.txt  fopen/fwrite/fclose, NO fsync,
//     C.txt  written as C.tmp, fsynced, then renamed (rename of a fsynced file).
//   logs "commitprobe: flush ready ..." with names, sizes and markers, then sleeps 90 s (a line every 10 s) so the host can snapshot
//   the emulated SD image and scan it for the markers; then removes the three files and the folder.
#pragma once
#ifdef OPENSHOP_DEBUGTOOLS
#include <string>

namespace platform {
// Runs `mode` ("cap" or "flush") to the end; false (and a log line) when the mode is unknown or it could not run. `files` and
// `stackKb` < 0 mean the defaults.
bool commitProbe(const std::string& mode, int files, int stackKb);
} // namespace platform
#endif

// SPDX-License-Identifier: GPL-3.0-or-later
// Journal-driven recovery and staging cleanup (M3 Increment 6). No SDL. Runs synchronously at start-up, before any screen
// (App::init): RENAMES AND PROBES ONLY (user decision 2026-11-27: the bulk staging deletion does NOT run at start-up; it is
// Increment 7's Cleanup job).
//
// PER MOUNTED ROOT (every root OpenShop can see, whichever "Show apps on" selects: user decision 2026-11-25/26)
//   no journal            NOTHING happens. Staging dirs that are there (finished jobs, failed extractions, a start-phase job)
//                         are abandoned: a staging dir with no journal is abandoned by definition. They are left for the
//                         Cleanup job. "No journal" means clean ENOENT from lstat on ".journal" AND ".journal.bak" (and the
//                         read); anything else (EIO, a card that went away) leaves everything alone: LeftAlone.
//   start-phase journal   the job never reached the commit phase: nothing outside staging changed. Only the journal is removed;
//                         its staging is left for the Cleanup job.
//   commit-phase journal  ROLL FORWARD (the remaining ops, then the staged manifest is copied to installed/<slug>.json) when
//                         <staging>/manifest.json is intact (user decision 2026-11-27: it is written before the first rename,
//                         so a cut after the renames can always be finished). When it is not, or a forward op cannot be done,
//                         ROLL BACK (every op undone in reverse, probing the real state). Then the journal is removed; staging
//                         stays (it holds the parked old tree until the Cleanup job removes it). Every op probes, so recovery
//                         itself can be cut and re-run any number of times.
//   staging missing       a commit-phase journal whose staging folder is gone: DroppedNoStaging. The journal is dropped and
//                         nothing is moved (undoing without the staging would unmake a finished install), but the install state
//                         is unknown (the manifest may not have been written): logged as such.
//   Corrupt journal       (unreadable, bad JSON, over a cap, written for the other device) it is LEFT ALONE, staging too (it
//                         may hold the parked old app), and the problem is logged.
//   a failed recovery     the journal and staging stay for the next start; logged.
// A root that is not mounted is skipped without a word: its journal and staging wait for a start where it is present.
//
// cleanStaging (Increment 7's Cleanup job) only ever removes under <root>/apps/OpenShop/staging (FileUtil::removeTree's
// mustBeUnder). Every staging dir is removable once no journal names it (finished or abandoned); a staging dir with no journal is
// abandoned. It has its own guard (CleanGuard below), so it is safe even when called on a root whose recovery answered LeftAlone
// or Failed. A job id must not be reused while its staging dir exists (freshJobId in Commit.h).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/FileUtil.h"

namespace install {

struct RecoveryRoot {
	std::string id;   // "sd" | "usb"
	std::string root; // "sd:", "./sdcard", ...
};

enum class RecoveryOutcome {
	Nothing,            // no journal
	DroppedStartJournal, // a start-phase journal was removed (its staging is left for Cleanup)
	RolledForward,
	RolledBack,
	DroppedNoStaging,   // a commit-phase journal whose staging was gone: dropped, install state UNKNOWN
	LeftAlone,          // a corrupt, unreadable or foreign journal (or one that cannot be told from "absent"): nothing touched
	Failed,             // could not finish; journal and staging stay
};

struct RootRecovery {
	std::string id;
	RecoveryOutcome outcome = RecoveryOutcome::Nothing;
	std::string detail;
};

const char* describe(RecoveryOutcome o);

// The guard of the Cleanup job (M3 Increment 7; the re-review of 2026-11-27 asked for it). cleanStaging removes every staging entry
// EXCEPT:
//   - the job dir that a journal on this root names (a LeftAlone or Failed recovery keeps its journal, and the parked old app is
//     behind it). The journal is read ONCE per call with loadJournal; the dirs are only removable when it is cleanly Missing
//     (presenceOf says Absent for ".journal" and ".journal.bak"). A Corrupt, unreadable, foreign or unverifiable journal means
//     "cannot tell which dir it names": NOTHING is removed and the call answers false;
//   - the job ids in `activeJobIds` (a job that is queued or running right now).
struct CleanGuard {
	std::string device;                 // "sd" | "usb": the device `root` is (a journal for the other device is Corrupt)
	std::vector<int64_t> activeJobIds;  // staging dirs of live jobs
	std::function<bool()> stop;         // optional: polled between entries AND per entry inside each dir (removeTree's stop); true ends the
	                                    // run early (a cancelled Cleanup job): a dir may stay partly deleted, which the next run finishes
};
struct CleanStats {
	int removed = 0;
	int skippedJournal = 0; // the dir a journal names
	int skippedActive = 0;
	int failed = 0;
	bool journalUnclear = false; // the journal could not be read or told from "absent": nothing was removed
	bool stopped = false;
};

// Removes the entries of <root>/apps/OpenShop/staging that the guard allows (only ever under that folder: FileUtil::removeTree's
// mustBeUnder). True when nothing that should have gone is left: a folder that is cleanly absent counts, one that cannot be
// checked or listed, an unclear journal and a failed removal do not; a dir skipped by the guard is not a failure. A failed removal is
// only logged (the next start retries).
bool cleanStaging(const std::string& root, const std::vector<std::string>& removeRoots, const CleanGuard& guard, const FileOps& ops = {},
                  CleanStats* stats = nullptr);

// `roots` are the MOUNTED roots (the caller filters). Recovery removes nothing but the journal, so it needs no removeTree roots.
std::vector<RootRecovery> recover(const std::vector<RecoveryRoot>& roots, const FileOps& ops = {});

} // namespace install

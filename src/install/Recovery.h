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
// cleanStaging (for Increment 7's Cleanup job) only ever removes under <root>/apps/OpenShop/staging (FileUtil::removeTree's
// mustBeUnder). It must run with the same rules: every staging dir is removable once no journal names it (finished or
// abandoned); a staging dir with no journal is abandoned. A job id must not be reused while its staging dir exists
// (freshJobId in Commit.h).
#pragma once
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

// Removes every entry under <root>/apps/OpenShop/staging. True when none is left (a folder that is cleanly absent counts; one that
// cannot be checked or listed does not).
bool cleanStaging(const std::string& root, const std::vector<std::string>& removeRoots, const FileOps& ops = {});

// `roots` are the MOUNTED roots (the caller filters). Recovery removes nothing but the journal, so it needs no removeTree roots.
std::vector<RootRecovery> recover(const std::vector<RecoveryRoot>& roots, const FileOps& ops = {});

} // namespace install

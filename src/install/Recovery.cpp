// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Recovery.h"

#include <algorithm>

#include "core/Log.h"
#include "install/Commit.h"
#include "install/Journal.h"

namespace install {

const char* describe(RecoveryOutcome o) {
	switch (o) {
	case RecoveryOutcome::Nothing: return "nothing to recover";
	case RecoveryOutcome::DroppedStartJournal: return "dropped the journal of a job that never started committing";
	case RecoveryOutcome::RolledForward: return "finished the interrupted install";
	case RecoveryOutcome::RolledBack: return "undid the interrupted install";
	case RecoveryOutcome::DroppedNoStaging: return "dropped a journal whose staging folder was gone (install state unknown)";
	case RecoveryOutcome::LeftAlone: return "left a damaged, unreadable or foreign journal alone";
	case RecoveryOutcome::Failed: return "could not finish; left for the next start";
	}
	return "?";
}

bool cleanStaging(const std::string& root, const std::vector<std::string>& removeRoots, const CleanGuard& guard, const FileOps& ops,
                  CleanStats* stats) {
	CleanStats local;
	CleanStats& st = stats ? *stats : local;
	st = CleanStats{};
	const std::string base = stagingBase(root);
	const Presence bp = presenceOf(base, ops);
	if (bp == Presence::Absent) return true; // cleanly not there: nothing to remove
	if (bp == Presence::Error) {
		LOG("cleanup: cannot check %s; staging not cleaned", base.c_str());
		return false;
	}
	// The journal first: which dir does it name? Only a CLEAN "no journal" lets every other dir go.
	int64_t journalJob = -1;
	const JournalLoad jl = loadJournal(root, guard.device, ops);
	if (jl.status == LoadStatus::Corrupt) {
		st.journalUnclear = true;
		LOG("cleanup[%s]: the journal is unusable (%s); not touching any staging folder", guard.device.c_str(), jl.error.c_str());
		return false;
	}
	if (jl.status == LoadStatus::Missing) {
		const std::string jp = journalPath(root);
		if (presenceOf(jp, ops) != Presence::Absent || presenceOf(jp + ".bak", ops) != Presence::Absent) {
			st.journalUnclear = true;
			LOG("cleanup[%s]: cannot tell whether there is a journal; not touching any staging folder", guard.device.c_str());
			return false;
		}
	} else {
		journalJob = jl.journal.jobId;
	}
	std::vector<std::string> names;
	if (!listDir(base, names, ops)) {
		LOG("cleanup: cannot list %s; staging not cleaned", base.c_str());
		return false;
	}
	std::sort(names.begin(), names.end());
	bool all = true;
	for (const std::string& n : names) {
		if (guard.stop && guard.stop()) {
			st.stopped = true;
			return false;
		}
		// A job id is plain decimal digits; anything else cannot be named by a journal or by a live job.
		int64_t id = -1;
		if (!n.empty() && n.size() <= 18 && n.find_first_not_of("0123456789") == std::string::npos) id = std::stoll(n);
		if (id >= 0 && id == journalJob) {
			st.skippedJournal++;
			LOG("cleanup[%s]: staging/%s is named by the journal; left alone", guard.device.c_str(), n.c_str());
			continue;
		}
		if (id >= 0 && std::find(guard.activeJobIds.begin(), guard.activeJobIds.end(), id) != guard.activeJobIds.end()) {
			st.skippedActive++;
			continue;
		}
		// The stop poll also reaches into the dir: a cancelled Cleanup leaves it partly deleted, which the next run finishes.
		const RemoveResult r = removeTree(base + "/" + n, base, removeRoots, ops, guard.stop);
		if (r.stopped) {
			st.stopped = true;
			return false;
		}
		if (!r.ok) {
			LOG("cleanup: could not remove staging/%s: %s", n.c_str(), r.error.c_str());
			st.failed++;
			all = false;
		} else {
			st.removed++;
		}
	}
	return all;
}

namespace {
RootRecovery recoverRoot(const RecoveryRoot& rr, const FileOps& ops) {
	RootRecovery out;
	out.id = rr.id;
	auto leave = [&](const std::string& why) {
		out.outcome = RecoveryOutcome::LeftAlone;
		out.detail = why;
		LOG("recovery[%s]: %s; leaving the journal and the staging folders alone", rr.id.c_str(), why.c_str());
		return out;
	};
	const JournalLoad jl = loadJournal(rr.root, rr.id, ops);
	if (jl.status == LoadStatus::Corrupt) return leave("the journal is unusable (" + jl.error + ")");
	if (jl.status == LoadStatus::Missing) {
		// "No journal" is only believed when lstat says a clean ENOENT for the journal and its backup: a flaky card must not make
		// a journal (and the parked old app behind it) look like nothing.
		const std::string jp = journalPath(rr.root);
		if (presenceOf(jp, ops) != Presence::Absent || presenceOf(jp + ".bak", ops) != Presence::Absent)
			return leave("cannot tell whether there is a journal");
		return out; // Nothing: leftover staging is the Cleanup job's
	}

	const Journal& j = jl.journal;
	if (j.phase == JournalPhase::Commit) {
		const std::string stage = stagingAbs(rr.root, j.jobId);
		std::string err;
		const Presence sp = presenceOf(stage, ops);
		if (sp == Presence::Error) {
			out.outcome = RecoveryOutcome::Failed;
			out.detail = "cannot check the staging folder";
			LOG("recovery[%s]: cannot check staging/%lld; journal and staging stay for the next start", rr.id.c_str(), (long long)j.jobId);
			return out;
		}
		if (sp == Presence::Absent) {
			// Nothing is left to roll either way; undoing ops without their staging would move a finished install away.
			out.outcome = RecoveryOutcome::DroppedNoStaging;
			out.detail = "job " + std::to_string((long long)j.jobId) + " of '" + j.slug + "'";
			LOG("recovery[%s]: job %lld ('%s') has no staging folder: dropping its journal WITHOUT moving anything; whether the install "
			    "finished is unknown (its manifest may be missing or stale)",
			    rr.id.c_str(), (long long)j.jobId, j.slug.c_str());
		} else if (stagedManifestOk(rr.root, j, ops) && rollForward(rr.root, j, ops, err)) {
			out.outcome = RecoveryOutcome::RolledForward;
			LOG("recovery[%s]: finished the interrupted %s of '%s'", rr.id.c_str(), j.kind == JournalKind::Install ? "install" : "update",
			    j.slug.c_str());
		} else {
			if (!err.empty()) LOG("recovery[%s]: cannot roll forward (%s)", rr.id.c_str(), err.c_str());
			std::string berr;
			if (!rollBack(rr.root, j, ops, berr)) {
				out.outcome = RecoveryOutcome::Failed;
				out.detail = berr;
				LOG("recovery[%s]: cannot roll back either (%s); journal and staging stay for the next start", rr.id.c_str(), berr.c_str());
				return out;
			}
			out.outcome = RecoveryOutcome::RolledBack;
			LOG("recovery[%s]: undid the interrupted %s of '%s'", rr.id.c_str(), j.kind == JournalKind::Install ? "install" : "update",
			    j.slug.c_str());
		}
	} else {
		out.outcome = RecoveryOutcome::DroppedStartJournal;
		LOG("recovery[%s]: job %lld never reached the commit phase; dropping its journal (staging is left for cleanup)", rr.id.c_str(),
		    (long long)j.jobId);
	}
	if (!removeJournal(rr.root, ops)) {
		out.outcome = RecoveryOutcome::Failed;
		out.detail = "the journal could not be removed";
		LOG("recovery[%s]: the journal could not be removed; it stays for the next start", rr.id.c_str());
	}
	return out;
}
} // namespace

std::vector<RootRecovery> recover(const std::vector<RecoveryRoot>& roots, const FileOps& ops) {
	std::vector<RootRecovery> all;
	for (const RecoveryRoot& r : roots) all.push_back(recoverRoot(r, ops));
	return all;
}

} // namespace install

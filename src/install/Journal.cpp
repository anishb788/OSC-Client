// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/Journal.h"

#include <cstdlib>
#include <set>

#include "core/Log.h"
#include "core/NameRules.h"
#include "install/RecordIo.h"
#include "install/Rules.h"

namespace install {
namespace {
constexpr int kMaxNesting = 8;
constexpr size_t kMaxPathBytes = 1024;

struct OpName { OpType t; const char* n; };
constexpr OpName kOps[] = {
    {OpType::RenameDir, "renameDir"}, {OpType::ParkDir, "parkDir"},       {OpType::ParkFile, "parkFile"},
    {OpType::RestoreFile, "restoreFile"}, {OpType::WriteFile, "writeFile"}, {OpType::Mkdir, "mkdir"},
    {OpType::WriteManifest, "writeManifest"},
};

bool slugEq(std::string_view a, std::string_view b) { return foldKey(a) == foldKey(b); }

// The shape of one op alone. `other` must be exactly the generated string for this job and path.
std::string checkOp(const JournalOp& op, const Journal& j) {
	switch (op.type) {
	case OpType::RenameDir:
		if (!isAppDirPath(op.path)) return "op path is not an app dir";
		if (op.other != stagedRootPath(j.stagingDir, op.path)) return "op 'other' is not <staging>/root/<path>";
		break;
	case OpType::ParkDir:
		if (!isAppDirPath(op.path)) return "op path is not an app dir";
		if (op.other != stagedOldPath(j.stagingDir, op.path)) return "op 'other' is not <staging>/old/<path>";
		break;
	case OpType::ParkFile:
	case OpType::RestoreFile:
		if (!isTargetFilePath(op.path)) return "op path is not a valid file target";
		if (op.other != stagedOldPath(j.stagingDir, op.path)) return "op 'other' is not <staging>/old/<path>";
		break;
	case OpType::WriteFile:
		if (!isOutsideFilePath(op.path)) return "writeFile path is not a file outside apps/";
		if (op.other != stagedRootPath(j.stagingDir, op.path)) return "op 'other' is not <staging>/root/<path>";
		break;
	case OpType::Mkdir:
		if (!isTargetPath(op.path)) return "op path is not a valid target";
		if (!op.other.empty()) return "op has an unexpected 'other'";
		break;
	case OpType::WriteManifest:
		if (!slugEq(op.path, manifestRelPath(j.slug))) return "writeManifest path is not the manifest of the slug";
		if (op.other != stagedManifestPath(j.stagingDir)) return "writeManifest 'other' is not <staging>/manifest.json";
		break;
	}
	return {};
}

// Ordering and kind rules over the whole list (see Journal.h).
std::string checkConsistency(const Journal& j) {
	if (j.ops.empty()) return "a commit-phase journal has at least the writeManifest op";
	std::set<std::string> parkedDirs, renamedDirs, parkedFiles, laterRenames;
	for (const JournalOp& op : j.ops) if (op.type == OpType::RenameDir) laterRenames.insert(foldKeyUnicode(op.path));
	for (size_t i = 0; i < j.ops.size(); i++) {
		const JournalOp& op = j.ops[i];
		const std::string key = foldKeyUnicode(op.path);
		switch (op.type) {
		case OpType::WriteManifest:
			if (i + 1 != j.ops.size()) return "writeManifest must be the last op";
			break;
		case OpType::ParkDir:
			if (j.kind != JournalKind::Update) return "parkDir in an install journal";
			if (!laterRenames.count(key)) return "parkDir without a renameDir of the same path";
			parkedDirs.insert(key);
			break;
		case OpType::RenameDir:
			renamedDirs.insert(key);
			laterRenames.erase(key); // a second renameDir of one path would need its own park
			break;
		case OpType::RestoreFile: {
			if (j.kind != JournalKind::Update) return "restoreFile in an install journal";
			bool ok = false;
			for (const std::string& d : parkedDirs)
				if (renamedDirs.count(d) && pathStrictlyUnder(op.path, d)) ok = true;
			if (!ok) return "restoreFile not below an app dir that was parked and renamed earlier";
			break;
		}
		case OpType::ParkFile:
			parkedFiles.insert(key);
			break;
		case OpType::WriteFile:
			if (op.preexisting && !parkedFiles.count(key)) return "writeFile over an existing file without a parkFile before it";
			break;
		case OpType::Mkdir:
			break;
		}
	}
	size_t manifests = 0;
	for (const JournalOp& op : j.ops) if (op.type == OpType::WriteManifest) manifests++;
	if (manifests != 1) return "exactly one writeManifest is required";
	return {};
}
} // namespace

const char* opName(OpType t) {
	for (const OpName& o : kOps) if (o.t == t) return o.n;
	return "?";
}

bool isDirLevelOp(OpType t) { return t == OpType::RenameDir || t == OpType::ParkDir || t == OpType::WriteManifest; }
bool journalFlushDue(OpType t, int opsSinceFlush) { return isDirLevelOp(t) || opsSinceFlush >= kJournalFlushEvery; }

std::string stagedRootPath(const std::string& stagingDir, const std::string& path) { return stagingDir + "/root/" + path; }
std::string stagedOldPath(const std::string& stagingDir, const std::string& path) { return stagingDir + "/old/" + path; }
std::string stagedManifestPath(const std::string& stagingDir) { return stagingDir + "/manifest.json"; }

std::string stagingRelPath(int64_t jobId) { return "apps/OpenShop/staging/" + std::to_string((long long)jobId); }
std::string journalRelPath() { return "apps/OpenShop/installed/.journal"; }
std::string journalPath(const std::string& root) { return root + "/" + journalRelPath(); }

std::string validateJournal(const Journal& j) {
	if (j.formatVersion != kJournalFormat) return "unsupported formatVersion";
	if (j.jobId < 0) return "bad jobId";
	if (!isValidSlug(j.slug)) return "bad slug";
	if (j.device != "sd" && j.device != "usb") return "bad device";
	if (j.stagingDir != stagingRelPath(j.jobId)) return "stagingDir is not staging/<jobId>";
	if (j.phase == JournalPhase::Start) {
		if (!j.ops.empty() || j.step != -1) return "a start-phase journal has no ops";
		return {};
	}
	if (j.ops.size() > kMaxJournalOps) return "too many ops";
	if (j.step < -1 || j.step >= (int64_t)j.ops.size()) return "step out of range";
	for (const JournalOp& op : j.ops) {
		std::string e = checkOp(op, j);
		if (!e.empty()) return e;
	}
	return checkConsistency(j);
}

namespace {
std::string saveCheck(const Journal& j, std::string& json) {
	std::string bad = validateJournal(j);
	if (!bad.empty()) return bad;
	json = journalToJson(j);
	if (json.empty()) return "not serialisable (invalid UTF-8)";
	if (json.size() > kMaxJournalBytes) return std::to_string(json.size()) + " bytes is over the cap";
	const JournalParse back = parseJournal(json);
	if (!back.ok) return "would not load back: " + back.error;
	return {};
}
} // namespace

std::string journalSaveProblem(const Journal& j) {
	std::string json;
	return saveCheck(j, json);
}

std::string journalToJson(const Journal& j) {
	using rec::Json;
	bool badUtf8 = false;
	auto str = [&](const std::string& s) {
		json_t* v = rec::jsonStr(s);
		if (!v) badUtf8 = true;
		return v;
	};
	Json root(json_object());
	json_object_set_new(root.get(), "formatVersion", json_integer(kJournalFormat));
	json_object_set_new(root.get(), "phase", json_string(j.phase == JournalPhase::Start ? "start" : "commit"));
	json_object_set_new(root.get(), "jobId", json_integer(j.jobId));
	json_object_set_new(root.get(), "kind", json_string(j.kind == JournalKind::Install ? "install" : "update"));
	json_object_set_new(root.get(), "slug", str(j.slug));
	json_object_set_new(root.get(), "device", str(j.device));
	json_object_set_new(root.get(), "stagingDir", str(j.stagingDir));
	if (j.phase == JournalPhase::Commit) {
		json_t* ops = json_array();
		for (const JournalOp& op : j.ops) {
			json_t* o = json_object();
			json_object_set_new(o, "op", json_string(opName(op.type)));
			json_object_set_new(o, "path", str(op.path));
			if (!op.other.empty()) json_object_set_new(o, "other", str(op.other));
			if (op.type == OpType::WriteFile) json_object_set_new(o, "preexisting", json_boolean(op.preexisting));
			if (op.type == OpType::Mkdir) json_object_set_new(o, "created", json_boolean(op.created));
			json_array_append_new(ops, o);
		}
		json_object_set_new(root.get(), "ops", ops);
		json_object_set_new(root.get(), "step", json_integer(j.step));
	}
	if (badUtf8) return {};
	char* dump = json_dumps(root.get(), JSON_COMPACT | JSON_ENSURE_ASCII);
	std::string out = dump ? dump : "";
	std::free(dump);
	return out;
}

JournalParse parseJournal(std::string_view text) {
	JournalParse r;
	auto fail = [&](const std::string& why) {
		r.ok = false;
		r.journal = Journal{};
		r.error = why;
		return r;
	};
	if (text.size() > kMaxJournalBytes) return fail("too large");
	std::string err;
	rec::Json doc = rec::parseObject(text, kMaxNesting, kMaxJournalValues, err);
	if (!doc) return fail(err);
	json_t* o = doc.get();
	Journal& j = r.journal;

	int64_t fv = 0;
	if (!rec::getInt(o, "formatVersion", fv) || fv < 1 || fv > kJournalFormat) return fail("unsupported formatVersion");
	std::string phase, kind;
	if (!rec::getString(o, "phase", 16, phase) || (phase != "start" && phase != "commit")) return fail("bad phase");
	j.phase = phase == "start" ? JournalPhase::Start : JournalPhase::Commit;
	if (!rec::getInt(o, "jobId", j.jobId) || j.jobId < 0) return fail("bad jobId");
	if (!rec::getString(o, "kind", 16, kind) || (kind != "install" && kind != "update")) return fail("bad kind");
	j.kind = kind == "install" ? JournalKind::Install : JournalKind::Update;
	if (!rec::getString(o, "slug", kMaxSlugLength, j.slug)) return fail("bad slug");
	if (!rec::getString(o, "device", 8, j.device)) return fail("bad device");
	if (!rec::getString(o, "stagingDir", kMaxPathBytes, j.stagingDir)) return fail("bad stagingDir");
	if (j.phase == JournalPhase::Commit) {
		json_t* ops = rec::getArray(o, "ops", kMaxJournalOps);
		if (!ops) return fail("bad ops");
		int64_t step = 0;
		if (!rec::getInt(o, "step", step) || step < -1 || step >= (int64_t)json_array_size(ops)) return fail("bad step");
		j.step = (int)step;
		for (size_t i = 0; i < json_array_size(ops); i++) {
			json_t* e = json_array_get(ops, i);
			if (!json_is_object(e)) return fail("op is not an object");
			std::string name;
			if (!rec::getString(e, "op", 16, name)) return fail("op without a name");
			JournalOp op;
			bool known = false;
			for (const OpName& n : kOps) if (name == n.n) { op.type = n.t; known = true; }
			if (!known) return fail("unknown op");
			if (!rec::getString(e, "path", kMaxPathBytes, op.path)) return fail("op without a path");
			if (json_object_get(e, "other") && !rec::getString(e, "other", kMaxPathBytes, op.other)) return fail("bad op other");
			op.preexisting = false;
			if (op.type == OpType::WriteFile) {
				if (!rec::getBool(e, "preexisting", op.preexisting)) return fail("writeFile without a boolean preexisting");
			} else if (json_object_get(e, "preexisting") && !rec::getBool(e, "preexisting", op.preexisting)) {
				return fail("bad preexisting");
			}
			if (json_object_get(e, "created") && !rec::getBool(e, "created", op.created)) return fail("bad created");
			j.ops.push_back(std::move(op));
		}
	}
	const std::string bad = validateJournal(j);
	if (!bad.empty()) return fail(bad);
	r.ok = true;
	return r;
}

bool saveJournal(const std::string& root, const Journal& j, const FileOps& ops) {
	std::string json;
	const std::string bad = saveCheck(j, json);
	if (!bad.empty()) {
		LOG("journal: refused to save: %s", bad.c_str());
		return false;
	}
	return rec::writeRecord(journalPath(root), json, kMaxJournalBytes, ops);
}

JournalLoad loadJournal(const std::string& root, std::string_view device, const FileOps& ops) {
	JournalLoad r;
	const rec::Read rd = rec::readRecord(journalPath(root), kMaxJournalBytes, ops);
	if (rd.result == rec::ReadResult::Missing) return r;
	if (rd.result != rec::ReadResult::Ok) {
		r.status = LoadStatus::Corrupt;
		r.error = rd.result == rec::ReadResult::TooLarge ? "too large" : "unreadable";
		return r;
	}
	JournalParse p = parseJournal(rd.text);
	if (p.ok && p.journal.device != device) { p.ok = false; p.error = "written for another device"; }
	if (!p.ok) {
		r.status = LoadStatus::Corrupt;
		r.error = p.error;
		LOG("journal is corrupt: %s", p.error.c_str());
		return r;
	}
	r.status = LoadStatus::Ok;
	r.journal = std::move(p.journal);
	r.fromBackup = rd.fromBackup;
	return r;
}

bool removeJournal(const std::string& root, const FileOps& ops) { return rec::removeRecord(journalPath(root), ops); }

} // namespace install

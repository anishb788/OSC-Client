// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/JobQueue.h"

#include <algorithm>

namespace install {

const char* kindName(JobKind k) {
	switch (k) {
	case JobKind::Install: return "Install";
	case JobKind::Update: return "Update";
	case JobKind::Delete: return "Delete";
	case JobKind::Cleanup: return "Cleanup";
	case JobKind::Scan: return "Scan";
	case JobKind::Sleep: return "Sleep";
	}
	return "?";
}

const char* stateName(JobState s) {
	switch (s) {
	case JobState::Queued: return "Queued";
	case JobState::Downloading: return "Downloading";
	case JobState::Verifying: return "Verifying";
	case JobState::Installing: return "Installing";
	case JobState::AwaitingConfirmation: return "AwaitingConfirmation";
	case JobState::Finalizing: return "Finalizing";
	case JobState::Done: return "Done";
	case JobState::Failed: return "Failed";
	case JobState::Cancelled: return "Cancelled";
	}
	return "?";
}

const char* stageLabel(Stage s) {
	switch (s) {
	case Stage::Waiting: return "Starting";
	case Stage::Checking: return "Checking";
	case Stage::Downloading: return "Downloading";
	case Stage::Verifying: return "Checking the download";
	case Stage::Extracting: return "Unpacking";
	case Stage::Committing: return "Installing";
	case Stage::Finalizing: return "Finishing";
	case Stage::Cleaning: return "Cleaning up";
	case Stage::Sleeping: return "Working";
	}
	return "?";
}

JobState stateForStage(Stage s) {
	switch (s) {
	case Stage::Downloading: return JobState::Downloading;
	case Stage::Verifying: return JobState::Verifying;
	case Stage::Finalizing: return JobState::Finalizing;
	case Stage::Waiting:
	case Stage::Checking:
	case Stage::Extracting:
	case Stage::Committing:
	case Stage::Cleaning:
	case Stage::Sleeping: return JobState::Installing;
	}
	return JobState::Installing;
}

JobId JobQueue::add(JobKind kind, JobSpec spec) {
	JobEntry e;
	e.id = nextId_++;
	if (nextId_ == 0) nextId_ = 1; // wrap (never reached in practice)
	e.kind = kind;
	e.spec = std::move(spec);
	e.cell = std::make_shared<ProgressCell>();
	line_.push_back(std::move(e));
	return line_.back().id;
}

const JobEntry* JobQueue::running() const {
	for (const JobEntry& e : line_)
		if (e.running) return &e;
	return nullptr;
}

const JobEntry* JobQueue::nextToStart() const {
	if (line_.empty() || running()) return nullptr;
	const JobEntry& h = line_.front();
	return h.state == JobState::Queued ? &h : nullptr;
}

bool JobQueue::markStarted(JobId id) {
	const JobEntry* n = nextToStart();
	if (!n || n->id != id) return false;
	JobEntry& e = line_.front();
	e.running = true;
	e.state = stateForStage(Stage::Waiting);
	return true;
}

void JobQueue::syncRunning() {
	for (JobEntry& e : line_)
		if (e.running && e.state != JobState::Cancelled && e.state != JobState::AwaitingConfirmation) e.state = stateForStage(e.cell->stage());
}

void JobQueue::pushHistory(JobEntry e) {
	e.running = false;
	history_.push_front(std::move(e));
	while (history_.size() > kHistoryMax) history_.pop_back();
}

bool JobQueue::finish(JobId id, JobState outcome, const std::string& error) {
	auto it = std::find_if(line_.begin(), line_.end(), [id](const JobEntry& e) { return e.id == id; });
	if (it == line_.end()) return false;
	JobEntry e = std::move(*it);
	line_.erase(it);
	if (e.state == JobState::Cancelled) outcome = JobState::Cancelled; // cancelled while running: stays cancelled
	if (outcome != JobState::Done && outcome != JobState::Failed) outcome = JobState::Cancelled;
	e.state = outcome;
	e.error = outcome == JobState::Failed ? error : std::string();
	if (outcome != JobState::Cancelled && !isInternal(e.kind)) pushHistory(std::move(e));
	return true;
}

bool JobQueue::cancel(JobId id) {
	auto it = std::find_if(line_.begin(), line_.end(), [id](const JobEntry& e) { return e.id == id; });
	if (it == line_.end()) return false;
	if (!it->running) {
		line_.erase(it);
		return true;
	}
	if (it->state == JobState::Cancelled) return false;
	if (!it->cell->requestCancel()) return false; // committing, or its runner has already returned
	it->state = JobState::Cancelled;
	return true;
}

bool JobQueue::cancelAll() {
	bool stuck = false;
	for (auto it = line_.begin(); it != line_.end();) {
		if (!it->running) {
			it = line_.erase(it);
			continue;
		}
		if (it->state != JobState::Cancelled) {
			if (it->cell->requestCancel()) it->state = JobState::Cancelled;
			else stuck = true;
		}
		++it;
	}
	return stuck;
}

bool JobQueue::remove(JobId id) {
	auto it = std::find_if(line_.begin(), line_.end(), [id](const JobEntry& e) { return e.id == id; });
	if (it != line_.end()) {
		if (it->running) return false;
		line_.erase(it);
		return true;
	}
	auto h = std::find_if(history_.begin(), history_.end(), [id](const JobEntry& e) { return e.id == id; });
	if (h == history_.end()) return false;
	history_.erase(h);
	return true;
}

JobId JobQueue::retry(JobId id, JobSpec spec) {
	auto h = std::find_if(history_.begin(), history_.end(), [id](const JobEntry& e) { return e.id == id; });
	if (h == history_.end() || h->state != JobState::Failed) return 0;
	const JobKind kind = h->kind;
	history_.erase(h);
	return add(kind, std::move(spec));
}

void JobQueue::clearFinished() { history_.clear(); }

const JobEntry* JobQueue::find(JobId id) const {
	for (const JobEntry& e : line_)
		if (e.id == id) return &e;
	for (const JobEntry& e : history_)
		if (e.id == id) return &e;
	return nullptr;
}

size_t JobQueue::userActiveCount() const {
	size_t n = 0;
	for (const JobEntry& e : line_)
		if (!isInternal(e.kind)) n++;
	return n;
}

int JobQueue::position(JobId id) const {
	int i = 0;
	for (const JobEntry& e : line_) {
		if (isInternal(e.kind)) {
			if (e.id == id) return -1;
			continue;
		}
		if (e.id == id) return i;
		i++;
	}
	return -1;
}

JobId JobQueue::nextQueued() const {
	for (const JobEntry& e : line_)
		if (!e.running && e.state == JobState::Queued && !isInternal(e.kind)) return e.id;
	return 0;
}

bool JobQueue::busy() const {
	for (const JobEntry& e : line_)
		if (e.kind != JobKind::Cleanup) return true;
	return false;
}

} // namespace install

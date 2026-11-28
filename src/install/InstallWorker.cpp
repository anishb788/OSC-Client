// SPDX-License-Identifier: GPL-3.0-or-later
#include "install/InstallWorker.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <exception>

#include <SDL_mutex.h>
#include <SDL_thread.h>
#include <SDL_version.h>

#include "core/Log.h"
#include "core/StackProbe.h"

namespace install {

namespace {
struct Lock {
	explicit Lock(SDL_mutex* m) : m_(m) { SDL_LockMutex(m_); }
	~Lock() { SDL_UnlockMutex(m_); }
	Lock(const Lock&) = delete;
	Lock& operator=(const Lock&) = delete;
	SDL_mutex* m_;
};
constexpr size_t kPaintSkip = 2048;          // below the thread entry: left alone (the paint helper's own frames)
constexpr size_t kPaintMax = 40 * 1024;      // painted at most this much (well inside a 64 KB stack)
constexpr size_t kPaintHeadroom = 12 * 1024; // and never closer than this to the end of the requested stack
} // namespace

struct InstallWorker::State {
	SDL_mutex* mutex = SDL_CreateMutex();
	SDL_cond* cond = SDL_CreateCond();
	std::deque<Task> tasks;
	bool stopping = false;
	std::atomic<bool> exited{false};
	bool measure = false, lowPriority = true;
	size_t stackBytes = 0;
	size_t highWater = 0; // written by the thread before `exited` is set
	~State() {
		if (cond) SDL_DestroyCond(cond);
		if (mutex) SDL_DestroyMutex(mutex);
	}
};

int InstallWorker::threadMain(void* arg) {
	std::shared_ptr<State> st = *static_cast<std::shared_ptr<State>*>(arg);
	delete static_cast<std::shared_ptr<State>*>(arg);
	if (st->lowPriority) SDL_SetThreadPriority(SDL_THREAD_PRIORITY_LOW); // a no-op on the Wii; see the header
	unsigned char marker = 0;
	unsigned char* paintBase = nullptr;
	size_t paintSize = 0;
	if (st->measure && st->stackBytes > kPaintSkip + kPaintHeadroom + 4096) {
		paintSize = st->stackBytes - kPaintSkip - kPaintHeadroom;
		if (paintSize > kPaintMax) paintSize = kPaintMax;
		paintBase = reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(&marker) - kPaintSkip - paintSize);
		stackprobe::paint(paintBase, paintSize);
	}
	for (;;) {
		Task t;
		{
			Lock l(st->mutex);
			while (st->tasks.empty() && !st->stopping) SDL_CondWait(st->cond, st->mutex);
			if (st->stopping) break; // tasks that never started are dropped
			t = std::move(st->tasks.front());
			st->tasks.pop_front();
		}
		try {
			t();
		} catch (const std::exception& e) {
			LOG("install worker: a task threw: %s", e.what());
		} catch (...) {
			LOG("install worker: a task threw");
		}
	}
	if (paintBase) st->highWater = stackprobe::highWater(paintBase, paintSize) + kPaintSkip;
	{
		Lock l(st->mutex);
		st->tasks.clear(); // destroy never-run tasks here, not in whoever drops the state last
	}
	st->exited.store(true, std::memory_order_release);
	return 0;
}

InstallWorker::InstallWorker() = default;

InstallWorker::~InstallWorker() {
	if (thread_) {
		requestStop();
		join();
	}
}

bool InstallWorker::start(const Options& o) {
	if (thread_) return false;
	auto st = std::make_shared<State>();
	if (!st->mutex || !st->cond) return false;
	st->measure = o.measureStack;
	st->lowPriority = o.lowPriority;
	st->stackBytes = o.stackBytes;
	auto* handoff = new std::shared_ptr<State>(st);
	SDL_Thread* t = nullptr;
#if SDL_VERSION_ATLEAST(2, 0, 9)
	t = SDL_CreateThreadWithStackSize(&InstallWorker::threadMain, "install-worker", o.stackBytes, handoff);
#else
	// SDL < 2.0.9 has no SDL_CreateThreadWithStackSize, and the default stack (8 KB on the Wii) is too small for extraction and the
	// commit engine (about 6.5 KB measured, libzip more). Refuse to start rather than run on a stack that may overflow: Jobs then
	// refuses every job. (The devkitPro Wii SDL2 port is 2.28.5, checked in the devkitppc image, so this is a guard for a toolchain
	// downgrade, not a path that runs.)
	LOG("install worker: REFUSING TO START: this SDL (%d.%d.%d) has no SDL_CreateThreadWithStackSize; installs are unavailable",
	    SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
	delete handoff;
	return false;
#endif
	if (!t) {
		LOG("install worker: SDL_CreateThread failed: %s", SDL_GetError());
		delete handoff;
		return false;
	}
	st_ = std::move(st);
	thread_ = t;
	stackBytes_ = o.stackBytes;
	highWater_ = 0;
	return true;
}

bool InstallWorker::submit(Task t) {
	if (!thread_ || !t) return false;
	Lock l(st_->mutex);
	if (st_->stopping) return false;
	st_->tasks.push_back(std::move(t));
	SDL_CondBroadcast(st_->cond);
	return true;
}

void InstallWorker::requestStop() {
	if (!st_) return;
	Lock l(st_->mutex);
	st_->stopping = true;
	SDL_CondBroadcast(st_->cond);
}

bool InstallWorker::exited() const { return !st_ || st_->exited.load(std::memory_order_acquire); }

void InstallWorker::join() {
	if (!thread_) return;
	SDL_WaitThread(thread_, nullptr);
	thread_ = nullptr;
	highWater_ = st_->highWater;
}

void InstallWorker::detach() {
	if (!thread_) return;
	SDL_DetachThread(thread_);
	thread_ = nullptr;
}

} // namespace install

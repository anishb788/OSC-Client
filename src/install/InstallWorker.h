// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// The install (disk) worker: ONE thread that runs disk tasks in order (M3 Increment 7). Everything that touches the card for an
// install runs here: verify, extract, commit, delete, scan, cleanup. The main thread never does that (Threading rule). Network
// transfers stay on M2's net::Worker; this thread opens no sockets.
//
// Created with SDL_CreateThreadWithStackSize and an explicit stack (64 KB; the Wii's default is 8 KB and the commit probe showed
// the commit engine needs about 6.5 KB, extraction with libzip more). SDL_SetThreadPriority(LOW) is requested but is a no-op on
// the Wii (SDL2's LWP backend; Platform.cpp's notes), so the thread is only as polite as its tasks: they yield between chunks.
//
// Plain tasks, no job knowledge: install::Jobs wraps each job in one. The shared state is reference counted, so a worker that
// has to be abandoned at exit (a commit that does not finish within the budget) can keep running on its own copy.
#include <cstddef>
#include <functional>
#include <memory>

struct SDL_Thread;

namespace install {

class InstallWorker {
public:
	using Task = std::function<void()>;
	struct Options {
		size_t stackBytes = 64 * 1024;
		bool lowPriority = true;
		bool measureStack = false; // perf=1: paint the stack below the thread entry and report how deep the tasks went
	};
	InstallWorker();
	~InstallWorker(); // stops and joins (waits for the running task) if still running

	InstallWorker(const InstallWorker&) = delete;
	InstallWorker& operator=(const InstallWorker&) = delete;

	// False (logged loudly) when the thread cannot be created, and always on SDL < 2.0.9, which has no SDL_CreateThreadWithStackSize:
	// the default stack is too small for the tasks, so Jobs then refuses every job (the Wii's SDL2 port is 2.28.5).
	bool start(const Options& opts);
	// False when not running or stopping. Runs after every task that was submitted earlier.
	bool submit(Task t);
	// No task that has not started is run any more; the one running finishes. Does not wait.
	void requestStop();
	// True once the thread has left its loop (join() will not block long).
	bool exited() const;
	// Joins the thread (blocks until it is done) and fills stackHighWater(). Call after requestStop().
	void join();
	// Gives up on the thread (it ends by itself, on its own copy of the state).
	void detach();
	bool running() const { return thread_ != nullptr; }
	// Bytes of stack the tasks used below the thread entry (a lower bound; 0 = not measured). Valid after join().
	size_t stackHighWater() const { return highWater_; }
	size_t stackBytes() const { return stackBytes_; }

private:
	struct State;
	static int threadMain(void* arg);
	std::shared_ptr<State> st_;
	SDL_Thread* thread_ = nullptr;
	size_t highWater_ = 0, stackBytes_ = 0;
};

} // namespace install

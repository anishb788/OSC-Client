#include "platform/Platform.h"

#include <SDL.h>

#include <dirent.h>
#include <malloc.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <vector>

#include "core/FileUtil.h"
#include "core/Log.h"
#include "core/PathRoot.h"
#include "core/StackProbe.h"

#ifdef __wii__
#include <fat.h>
#include <ogc/conf.h>
#include <ogc/system.h>
#include <ogc/usbstorage.h>
#include <wiiuse/wpad.h>
#else
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

namespace platform {
namespace {
std::string gDataDir;
uint32_t gPrevHeld = 0;
float gRepeatTimer = 0;
float gLastPx = -1, gLastPy = -1;

constexpr float kRepeatDelay = 0.38f;
constexpr float kRepeatRate = 0.085f;

// Turns raw held state into pressed / auto-repeat edges. `downEvents` are buttons
// that went down during this poll even if already released again (quick clicks).
void finishFrame(Input& in, uint32_t held, float dt, uint32_t downEvents = 0) {
	in.held = held;
	in.pressed = (held & ~gPrevHeld) | downEvents;
	in.repeat = in.pressed;
	uint32_t dirs = held & BTN_DIRS;
	if (in.pressed & BTN_DIRS) {
		gRepeatTimer = kRepeatDelay;
	} else if (dirs) {
		gRepeatTimer -= dt;
		if (gRepeatTimer <= 0) {
			in.repeat |= dirs;
			gRepeatTimer += kRepeatRate;
		}
	}
	gPrevHeld = held;

	in.pointerMoved = in.pointerValid && (in.px != gLastPx || in.py != gLastPy);
	gLastPx = in.px;
	gLastPy = in.py;
}
} // namespace

const std::string& dataDir() { return gDataDir; }

bool writeFile(const std::string& path, const std::string& data) {
	// Atomic: a tmp file, then a commit that survives libfat's no-rename-over-existing rule (see FileUtil.h).
	if (writeFileAtomic(path, data)) return true;
	LOG("writeFile: failed for %s", path.c_str());
	return false;
}

bool readFile(const std::string& path, std::string& out, size_t maxBytes) {
	ReadStatus rs = readFileCapped(path, out, maxBytes);
	if (rs == ReadStatus::TooLarge) LOG("readFile: %s is larger than %zu bytes, not read", path.c_str(), maxBytes);
	return rs == ReadStatus::Ok;
}


// ---------------------------------------------------------------------------------------------------------------------
// Storage roots, mounting, heap and stack checks (M3 Increment 2, the M7 hardware questions).
//
// RECORDED RESULTS (Dolphin and hardware; the epic's Risks section repeats them):
//  - Mount state before and after mount (Dolphin, SD only, 2026-11-26): sd: already mounted before any mount call
//    (writable, statvfs 0 ms); usb: absent, one fatMountSimple attempt failed in 0 ms, then shut down
//  - Mount state, HBC launch (hardware, 2026-11-26): from SD, sd: is already mounted before any mount call and usb:
//    isn't (one fatMountSimple attempt, 0 ms, with no stick in). From USB, usb: is already mounted and sd: isn't;
//    with no SD card inserted, fatInitDefault() returned 1 in 24 ms (it mounts whatever it finds) and sd: stayed
//    unmounted, as expected
//  - argv[0] of a launch from USB (hardware, 2026-11-26): usb:/apps/OpenShop/boot.dol, so dataDir() = usb:/apps/OpenShop
//  - Heap: does newlib malloc grow into MEM2? (Dolphin heapprobe=1, 2026-11-26): YES. Usable on top of the app's
//    ~5.5 MB: about 12 MB MEM1 + 53 MB MEM2 (65 x 1 MB, stopped when malloc failed); with trimming disabled sbrk(0)
//    stayed at its peak after the free and no _free_r fault followed. The heap
//    continues in MEM2 after MEM1 runs out, with a gap in the address space between them (see disableHeapTrim).
//  - Install-worker stack (Dolphin, 2026-11-26): a 64 KB request ran a 16 x ~1 KB workload that reached 17568 bytes
//    below the entry sp inside the assumed bounds, with no fault or heap damage afterwards (the Wii has no fault for
//    overflowing a malloc'd stack, so the probe measures inside the requested size instead of proving the size)
//    Found statically, without running anything (devkitPro image, libSDL2.a and libsysbase.a disassembled): the Wii
//    SDL2 port builds threads on pthreads. SDL_CreateThreadWithStackSize calls pthread_attr_setstacksize, which
//    libsysbase passes on to __syscall_thread_create (libogc LWP); with no explicit size it passes 0 (libogc's
//    default, 8 KB). SDL_SYS_SetThreadPriority is "li r3,0; blr": SDL_SetThreadPriority is accepted but does nothing,
//    so every SDL thread keeps the LWP priority libogc gives new threads. libogc offers no call to read a
//    priority back, so the dev probe only logs what SDL_SetThreadPriority returns.
//  - USB mounting, found statically in the devkitPro image (libfat.a disassembled): fatInitDefault() is
//    dvmInit(true, default cache), which probes the SD card AND, because libfat's g_dvmOgcUsbMount defaults to 1, the
//    USB storage (mount name "usb", __io_usbstorage). So after fatInitDefault() a second fatMountSimple("usb") is
//    redundant; it is only made when fatInitDefault() was not called (the SD card was already mounted by the loader).
//  - HBC listing of apps/OpenShop/{installed,staging} on USB (hardware, 2026-11-26): HBC lists nothing for either
// ---------------------------------------------------------------------------------------------------------------------
namespace {
std::vector<StorageRoot> gRoots;

bool dirOpens(const std::string& root) {
	DIR* d = opendir((root + "/").c_str());
	if (!d) return false;
	closedir(d);
	return true;
}

// Can a file be created, written and closed (a full card fails at fclose) on this root? One file directly in the root,
// created and deleted again. Never creates a directory, so a root is not touched beyond that file. Only called for
// mounted roots.
bool probeWritable(const std::string& root) {
	const std::string f = root + "/.openshop_probe";
	std::remove(f.c_str()); // a leftover from an interrupted run
	std::FILE* fp = std::fopen(f.c_str(), "wb");
	if (!fp) return false;
	bool ok = std::fputc('x', fp) != EOF;
	ok = std::fclose(fp) == 0 && ok;
	ok = std::remove(f.c_str()) == 0 && ok;
	return ok;
}

// Free space in MB, -1 if unknown.
long long rootFreeMB(const std::string& root) {
	const std::optional<uint64_t> b = freeSpace(root);
	return b ? (long long)(*b / (1024ull * 1024ull)) : -1;
}

// The roots with their current state. Wii: sd and usb, always both. PC: sd, and usb only if its folder exists.
std::vector<StorageRoot> probeRoots() {
	std::vector<StorageRoot> r;
#ifdef __wii__
	r.push_back({"sd", "SD card", "sd:", false, false});
	r.push_back({"usb", "USB drive", "usb:", false, false});
#else
	const char* sd = std::getenv("OPENSHOP_SD");
	const char* usb = std::getenv("OPENSHOP_USB");
	r.push_back({"sd", "SD card", sd && *sd ? sd : "./sdcard", false, false});
	StorageRoot u{"usb", "USB drive", usb && *usb ? usb : "./usbdrive", false, false};
	if (dirOpens(u.root)) r.push_back(u);
#endif
	for (StorageRoot& s : r) {
		s.mounted = dirOpens(s.root);
		s.writable = s.mounted && probeWritable(s.root);
	}
	return r;
}

// Free space is read once per root (statvfs can be slow on a sleeping USB drive) and reused for the after-mount line.
void logRoots(const char* when, const std::vector<StorageRoot>& roots, std::map<std::string, long long>& freeMB) {
	for (const StorageRoot& s : roots) {
		long long mb = -1;
		if (s.mounted) {
			auto it = freeMB.find(s.id);
			if (it == freeMB.end()) {
				const uint32_t t0 = SDL_GetTicks();
				mb = rootFreeMB(s.root);
				freeMB[s.id] = mb;
				LOG("storage: statvfs(%s) took %u ms", s.root.c_str(), (unsigned)(SDL_GetTicks() - t0));
			} else {
				mb = it->second;
			}
		}
		char free[32];
		if (mb >= 0) std::snprintf(free, sizeof free, "%lld", mb);
		else std::snprintf(free, sizeof free, "n/a");
		LOG("storage %s: id=%s label=\"%s\" root=%s mounted=%d writable=%d free=%s MB", when, s.id.c_str(), s.label.c_str(),
		    s.root.c_str(), s.mounted ? 1 : 0, s.writable ? 1 : 0, free);
	}
}

[[maybe_unused]] const StorageRoot* findRoot(const std::vector<StorageRoot>& roots, const char* id) {
	for (const StorageRoot& s : roots) if (s.id == id) return &s;
	return nullptr;
}

// Logs the state, mounts what is missing (Wii only), logs again and remembers the result.
void mountStorage() {
	std::map<std::string, long long> freeMB;
	std::vector<StorageRoot> roots = probeRoots();
	logRoots("before mount", roots, freeMB);
#ifdef __wii__
	bool initDefaultRan = false;
	const StorageRoot* sd = findRoot(roots, "sd");
	if (!sd || !sd->mounted) {
		const uint32_t t0 = SDL_GetTicks();
		const bool ok = fatInitDefault();
		initDefaultRan = true;
		LOG("storage: fatInitDefault() -> %d in %u ms", ok ? 1 : 0, (unsigned)(SDL_GetTicks() - t0));
		roots = probeRoots();
	}
	const StorageRoot* usb = findRoot(roots, "usb");
	if (usb && !usb->mounted) {
		if (initDefaultRan) {
			// libfat's fatInitDefault() already probed USB (see the RECORDED RESULTS above), so that was the attempt.
			LOG("storage: usb: not mounted after fatInitDefault(), which already tried it; not trying again");
		} else {
			// ONE attempt at startup, no retry loop: a drive that is not ready now is treated as absent for this session.
			// A later increment (2b, when the user picks USB as the apps device) may retry once at that moment.
			const uint32_t t0 = SDL_GetTicks();
			const bool ok = fatMountSimple("usb", &__io_usbstorage);
			LOG("storage: fatMountSimple(usb) -> %d in %u ms", ok ? 1 : 0, (unsigned)(SDL_GetTicks() - t0));
			if (!ok) {
				__io_usbstorage.shutdown(); // release the half-started USB interface so a later retry starts clean
				LOG("storage: usb shut down after the failed mount");
			}
		}
		roots = probeRoots();
	}
#else
	LOG("storage: PC, nothing to mount (OPENSHOP_SD / OPENSHOP_USB name the folders)");
#endif
	logRoots("after mount", roots, freeMB);
	gRoots = roots;
}

// ----- heap -----

#ifdef __wii__
// Called first thing in init(). Newlib returns the top of the heap to the system (malloc_trim / sbrk with a negative
// size) when free() leaves more than M_TRIM_THRESHOLD unused at the top. On the Wii the heap grows MEM1 -> MEM2: when
// MEM1 (arena1) runs out libogc's sbrk continues at 0x90000000, leaving a hole in the address space (MEM1 ended near
// 0x8175xxxx, MEM2 starts at 0x90000000). Observed in Dolphin (heapprobe, 65 MB reached, all freed): the trim moved the
// break from 0x93517000 back to 0x90001000 and every free() afterwards faulted in newlib _free_r at the bin unlink
// (stw r6,12(r7) with r7 == NULL, "Invalid write to 0x0000000c"): the bins were corrupt. Without the trim the same
// run is clean. So trimming is switched off: the heap only ever grows, which is what a program that frees large buffers
// (install buffers beyond ~12 MB of MEM1) needs.
// Newlib (devkitPPC libc.a, _mallopt_r disassembled): option M_TRIM_THRESHOLD is -1 in <malloc.h>, the call stores the
// value in __malloc_trim_threshold and returns 1; _free_r compares it UNSIGNED (cmplw) against the size of the top
// chunk, so passing -1 (0xFFFFFFFF) is the maximum and trimming never happens. M_TOP_PAD is left alone (it only sets how
// much extra sbrk asks for).
void disableHeapTrim() {
	const int rc = mallopt(M_TRIM_THRESHOLD, -1);
	LOG("heap: trimming disabled (M_TRIM_THRESHOLD=-1, i.e. 0xFFFFFFFF unsigned) -> mallopt returned %d (1 = accepted)", rc);
}
#endif

struct HeapNumbers { size_t arena, used, free, keep, mmapped; };

HeapNumbers heapNumbers() {
#ifdef __wii__
	struct mallinfo m = mallinfo();
#else
	struct mallinfo2 m = mallinfo2();
#endif
	return {(size_t)m.arena, (size_t)m.uordblks, (size_t)m.fordblks, (size_t)m.keepcost, (size_t)m.hblkhd};
}

#ifdef __wii__
constexpr uintptr_t kMem2Base = 0x90000000u;
uintptr_t gMem1HeapEnd = 0; // where the MEM1 part of the heap ended (highest break seen below MEM2), 0 = unknown

// The address-space hole between the MEM1 and MEM2 parts of the heap, 0 while the heap is still in MEM1. mallinfo counts
// the sbrk range, hole included.
size_t memGap() {
	if ((uintptr_t)sbrk(0) < kMem2Base) return 0;
	const uintptr_t end1 = gMem1HeapEnd ? gMem1HeapEnd : (uintptr_t)SYS_GetArena1Lo();
	return end1 < kMem2Base ? (size_t)(kMem2Base - end1) : 0;
}
#endif

// ----- stack -----

constexpr size_t kProbeStack = 64 * 1024;
constexpr size_t kProbeSkip = 2048;       // bytes below the probe's entry that are left alone (the paint helper's own frames)
constexpr size_t kProbePaint = 24 * 1024; // the painted region; skip + paint is well under the requested size
constexpr int kProbeBurnLevels = 16;      // ~1 KB each, so ~16 KB of the 24 KB painted

struct StackProbeResult {
	size_t requested = 0, painted = 0, highWater = 0;
	uintptr_t marker = 0;            // address of a local at thread entry: about the stack pointer there
	uintptr_t paintLo = 0;           // lowest address painted
	uintptr_t boundLo = 0, boundHi = 0; // the stack bounds used for the report
	bool boundsReal = false;         // true: read from pthread (PC); false: assumed (Wii)
	int prioRc[4] = {0, 0, 0, 0};
	int niceAfter[4] = {0, 0, 0, 0};
};

// Uses about 1 KB of stack per level.
__attribute__((noinline)) unsigned burnStack(int levels) {
	volatile unsigned char buf[1024];
	for (size_t i = 0; i < sizeof buf; i += 16) buf[i] = (unsigned char)(i + levels);
	if (levels <= 0) return buf[0];
	return burnStack(levels - 1) + buf[16];
}

int stackProbeThread(void* arg) {
	StackProbeResult* r = static_cast<StackProbeResult*>(arg);
	unsigned char marker = 0;
	r->marker = (uintptr_t)&marker;
#ifndef __wii__
	pthread_attr_t at;
	if (pthread_getattr_np(pthread_self(), &at) == 0) {
		void* addr = nullptr;
		size_t sz = 0;
		if (pthread_attr_getstack(&at, &addr, &sz) == 0) {
			r->boundLo = (uintptr_t)addr;
			r->boundHi = (uintptr_t)addr + sz;
			r->boundsReal = true;
		}
		pthread_attr_destroy(&at);
	}
#else
	// Assumed, not observable: libogc's __syscall_thread_create does malloc(stack_size + 1256) with the TCB at the start of
	// the chunk and the stack top at its end, so the stack is [chunk+1256, chunk+1256+size). The chunk address is not
	// visible from inside the thread, so the top is taken as the stack pointer at entry (the start-up frames above it are
	// small), which makes the bounds [top-size, top) a slightly optimistic guess.
	r->boundHi = r->marker;
	r->boundLo = r->marker - r->requested;
#endif
	// Paint only a region that lies inside the stack by construction: from kProbeSkip below the entry down for kProbePaint
	// bytes, i.e. at most skip + paint = 26 KB below the entry of a stack that was asked to be 64 KB.
	r->painted = kProbePaint;
	const uintptr_t top = r->marker - kProbeSkip;
	unsigned char* base = (unsigned char*)(top - r->painted);
	r->paintLo = (uintptr_t)base;
	stackprobe::paint(base, r->painted);
	const unsigned sink = burnStack(kProbeBurnLevels);
	r->highWater = stackprobe::highWater(base, r->painted);
	if (sink == 0xFFFFFFFFu) r->highWater = 0; // never true; keeps burnStack's result alive
	// The priority mapping: what SDL_SetThreadPriority returns, and on the PC the nice value that results.
	const SDL_ThreadPriority prios[4] = {SDL_THREAD_PRIORITY_LOW, SDL_THREAD_PRIORITY_NORMAL, SDL_THREAD_PRIORITY_HIGH,
	                                     SDL_THREAD_PRIORITY_TIME_CRITICAL};
	for (int i = 0; i < 4; i++) {
		r->prioRc[i] = SDL_SetThreadPriority(prios[i]);
#ifndef __wii__
		r->niceAfter[i] = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
#endif
	}
	return 0;
}
} // namespace

const std::vector<StorageRoot>& storageRoots() { return gRoots; }

RemoveResult removeTree(const std::string& path, const std::string& mustBeUnder) {
	std::vector<std::string> roots;
	for (const StorageRoot& s : gRoots) roots.push_back(s.root);
	return ::removeTree(path, mustBeUnder, roots);
}

std::string rootOf(const std::string& path) {
	std::vector<std::string> roots;
	for (const StorageRoot& s : gRoots) roots.push_back(s.root);
	std::string cwd;
#ifndef __wii__
	char buf[4096];
	if (getcwd(buf, sizeof buf)) cwd = buf;
#endif
	const int i = pathroot::indexOf(roots, path, cwd);
	return i < 0 ? std::string() : gRoots[(size_t)i].root;
}

std::string heapSummary() {
	static size_t highUsed = 0;
	const HeapNumbers h = heapNumbers();
	char buf[320];
#ifdef __wii__
	const size_t gap = memGap(); // mallinfo counts the hole between MEM1 and MEM2 once the heap has crossed over
	const size_t arena = h.arena > gap ? h.arena - gap : h.arena;
	const size_t used = h.used > gap ? h.used - gap : h.used;
	highUsed = std::max(highUsed, used);
	const unsigned a1 = (unsigned)((uintptr_t)SYS_GetArena1Hi() - (uintptr_t)SYS_GetArena1Lo());
	const unsigned a2 = (unsigned)((uintptr_t)SYS_GetArena2Hi() - (uintptr_t)SYS_GetArena2Lo());
	std::snprintf(buf, sizeof buf, "heap: malloc arena %zu KB, in use %zu KB (high-water at 120-frame samples %zu KB; MEM1/MEM2 gap %zu KB removed), free SYS arena1 (MEM1) %u KB, arena2 (MEM2) %u KB",
	              arena / 1024, used / 1024, highUsed / 1024, gap / 1024, a1 / 1024, a2 / 1024);
#else
	highUsed = std::max(highUsed, h.used);
	long rssKb = -1;
	if (std::FILE* f = std::fopen("/proc/self/statm", "r")) {
		long total = 0, resident = 0;
		if (std::fscanf(f, "%ld %ld", &total, &resident) == 2) rssKb = resident * (sysconf(_SC_PAGESIZE) / 1024);
		std::fclose(f);
	}
	std::snprintf(buf, sizeof buf, "heap: rss %ld KB, malloc arena %zu KB, in use %zu KB (high-water at 120-frame samples %zu KB)", rssKb,
	              h.arena / 1024, h.used / 1024, highUsed / 1024);
#endif
	return buf;
}

namespace {
void logHeapState(const char* when) {
	const HeapNumbers h = heapNumbers();
#ifdef __wii__
	LOG("heapprobe %s: mallinfo arena=%zu uordblks=%zu fordblks=%zu keepcost=%zu; sbrk(0)=%p; arena1 lo=%p hi=%p; arena2 lo=%p hi=%p",
	    when, h.arena, h.used, h.free, h.keep, sbrk(0), SYS_GetArena1Lo(), SYS_GetArena1Hi(), SYS_GetArena2Lo(), SYS_GetArena2Hi());
#else
	LOG("heapprobe %s: mallinfo2 arena=%zu uordblks=%zu fordblks=%zu keepcost=%zu hblkhd=%zu", when, h.arena, h.used, h.free,
	    h.keep, h.mmapped);
#endif
}
} // namespace

void heapProbe() {
	constexpr size_t kBlock = 1024 * 1024;
	constexpr int kCapMB = 80;
	LOG("heapprobe: start, 1 MB blocks until malloc fails or %d MB", kCapMB);
	logHeapState("before");
#ifdef __wii__
	void* const brkBefore = sbrk(0);
	uintptr_t mem1Brk = 0; // highest break seen below MEM2 (where the MEM1 part of the heap ends)
	uintptr_t brkSeen = 0;
#endif
	std::vector<void*> blocks;
	// Per region: block count, lowest and highest address. Region 0 = below 0x90000000 (MEM1 on the Wii), 1 = at or above.
	int count[2] = {0, 0};
	uintptr_t lo[2] = {UINTPTR_MAX, UINTPTR_MAX}, hi[2] = {0, 0};
	bool capped = true;
	for (int i = 0; i < kCapMB; i++) {
		void* p = std::malloc(kBlock);
		if (!p) { capped = false; break; }
		static_cast<unsigned char*>(p)[0] = 1; // touch it so it really is backed
		const uintptr_t a = (uintptr_t)p;
#ifdef __wii__
		const int reg = a >= 0x90000000u ? 1 : 0;
#else
		const int reg = 0; // the PC has no MEM1/MEM2 split
#endif
		count[reg]++;
		lo[reg] = std::min(lo[reg], a);
		hi[reg] = std::max(hi[reg], a + kBlock);
		blocks.push_back(p);
#ifdef __wii__
		const uintptr_t b = (uintptr_t)sbrk(0);
		brkSeen = b;
		if (b < kMem2Base) mem1Brk = std::max(mem1Brk, b);
#endif
	}
	logHeapState("peak");
	size_t gap = 0;
#ifdef __wii__
	if (mem1Brk) gMem1HeapEnd = mem1Brk;
	gap = memGap();
#endif
	const HeapNumbers pk = heapNumbers();
	LOG("heapprobe: obtained %zu MB in %zu blocks of 1 MB (sum of the block sizes)", blocks.size(), blocks.size());
	for (int reg = 0; reg < 2; reg++) {
		if (count[reg] == 0) continue;
#ifdef __wii__
		const char* name = reg ? "MEM2" : "MEM1";
#else
		const char* name = "all";
#endif
		LOG("heapprobe: %s: %d blocks, lowest %p, highest end %p", name, count[reg], (void*)lo[reg], (void*)hi[reg]);
	}
	LOG("heapprobe: MEM1 -> MEM2 gap %zu KB; at peak mallinfo uordblks %zu KB - gap = %zu KB real use (the gap is address space, not memory)",
	    gap / 1024, pk.used / 1024, (pk.used > gap ? pk.used - gap : pk.used) / 1024);
	const char* why = capped ? "the 80 MB cap was reached (malloc never failed)" : "malloc failed";
#ifdef __wii__
	LOG("heapprobe RESULT: usable heap ~ %d MB MEM1 + %d MB MEM2 (on top of what the app already held), stopped because %s",
	    count[0], count[1], why);
#else
	LOG("heapprobe RESULT: usable heap ~ %d MB (PC: no MEM1/MEM2 split), stopped because %s", count[0] + count[1], why);
#endif
	for (void* p : blocks) std::free(p);
	logHeapState("after free");
#ifdef __wii__
	void* const brkAfter = sbrk(0);
	LOG("heapprobe: sbrk(0) before %p, at peak %p, after free %p: %s", brkBefore, (void*)brkSeen, brkAfter,
	    (uintptr_t)brkAfter == brkSeen ? "not trimmed (expected, trimming is disabled)"
	                                   : "TRIMMED (unexpected: mallopt did not take effect; free() may fault next)");
#endif
}

void stackProbe() {
	StackProbeResult r;
	r.requested = kProbeStack;
	LOG("stackprobe: requesting %zu bytes via SDL_CreateThreadWithStackSize", r.requested);
	SDL_Thread* t = SDL_CreateThreadWithStackSize(stackProbeThread, "stackprobe", r.requested, &r);
	if (!t) {
		LOG("stackprobe RESULT: SDL_CreateThreadWithStackSize failed: %s", SDL_GetError());
		return;
	}
	SDL_WaitThread(t, nullptr);
	static const char* names[4] = {"LOW", "NORMAL", "HIGH", "TIME_CRITICAL"};
	for (int i = 0; i < 4; i++) {
#ifdef __wii__
		LOG("stackprobe priority: SDL %s -> SDL_SetThreadPriority returned %d (Wii SDL2: a no-op, the LWP priority is not changed)",
		    names[i], r.prioRc[i]);
#else
		LOG("stackprobe priority: SDL %s -> SDL_SetThreadPriority returned %d, nice value now %d", names[i], r.prioRc[i], r.niceAfter[i]);
#endif
	}
#ifdef __wii__
	LOG("stackprobe: bounds ASSUMED (libogc malloc(size+1256): TCB first, stack = chunk+1256 .. chunk+1256+size; the chunk is not visible "
	    "from the thread, so the top is taken as the entry sp): %p .. %p", (void*)r.boundLo, (void*)r.boundHi);
#else
	LOG("stackprobe: bounds from pthread_getattr_np: %p .. %p (%zu bytes)", (void*)r.boundLo, (void*)r.boundHi,
	    (size_t)(r.boundHi - r.boundLo));
#endif
	const uintptr_t lowestWritten = r.paintLo + (r.painted - r.highWater);
	LOG("stackprobe RESULT: requested %zu bytes; sp at entry %p; painted %zu bytes %p .. %p (from %zu bytes below the entry); "
	    "workload %d levels of ~1 KB wrote down to %p = %zu bytes below the entry, high-water %zu bytes of the painted region; "
	    "lowest write is %zu bytes above the %s stack bottom",
	    r.requested, (void*)r.marker, r.painted, (void*)r.paintLo, (void*)(r.paintLo + r.painted), kProbeSkip, kProbeBurnLevels,
	    (void*)lowestWritten, (size_t)(r.marker - lowestWritten), r.highWater,
	    lowestWritten >= r.boundLo ? (size_t)(lowestWritten - r.boundLo) : (size_t)0, r.boundsReal ? "real" : "assumed");
}

#ifdef __wii__

void init(int argc, char** argv) {
	disableHeapTrim(); // before any large allocation
	LOG("argv[0] raw: %s (argc=%d)", argc > 0 && argv && argv[0] ? argv[0] : "(none)", argc);
	mountStorage();
	// HBC passes the full path of boot.dol, e.g. "sd:/apps/OpenShop/boot.dol". A launch from USB shows up here.
	if (argc > 0 && argv[0]) {
		std::string p = argv[0];
		size_t slash = p.rfind('/');
		if (p.find(":/") != std::string::npos && slash != std::string::npos) gDataDir = p.substr(0, slash);
	}
	if (gDataDir.empty()) gDataDir = "sd:/apps/OpenShop";
	LOG("dataDir() = %s (derived from argv[0], argc=%d)", gDataDir.c_str(), argc);
}

bool widescreen() { return CONF_GetAspectRatio() == CONF_ASPECT_16_9; }

const char* exitLabel() { return "Exit"; }

const char* musicToggleHint() { return "Press 1 to toggle music"; }

void poll(Input& in, float dt, int layoutW) {
	in = Input{};
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: in.quit = true; break;
		case SDL_TEXTINPUT: in.text += e.text.text; break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_BACKSPACE) in.backspaces++;
			if (e.key.keysym.sym == SDLK_RETURN) in.enterKey = true;
			break;
		}
	}

	// SDL already pumps WPAD while processing events; read the result directly so
	// A/B keep their Wii meaning (SDL maps B to left click).
	WPADData* d = WPAD_Data(0);
	uint32_t h = d ? d->btns_h : 0;
	uint32_t held = in.enterKey ? (uint32_t)BTN_CONFIRM : 0u;
	auto map = [&](uint32_t wpad, uint32_t btn) { if (h & wpad) held |= btn; };
	map(WPAD_BUTTON_A, BTN_CONFIRM);
	map(WPAD_BUTTON_B, BTN_BACK);
	map(WPAD_BUTTON_UP, BTN_UP);
	map(WPAD_BUTTON_DOWN, BTN_DOWN);
	map(WPAD_BUTTON_LEFT, BTN_LEFT);
	map(WPAD_BUTTON_RIGHT, BTN_RIGHT);
	map(WPAD_BUTTON_PLUS, BTN_PLUS);
	map(WPAD_BUTTON_MINUS, BTN_MINUS);
	map(WPAD_BUTTON_HOME, BTN_HOME);
	map(WPAD_BUTTON_1, BTN_ONE);
	map(WPAD_CLASSIC_BUTTON_A, BTN_CONFIRM);
	map(WPAD_CLASSIC_BUTTON_B, BTN_BACK);
	map(WPAD_CLASSIC_BUTTON_UP, BTN_UP);
	map(WPAD_CLASSIC_BUTTON_DOWN, BTN_DOWN);
	map(WPAD_CLASSIC_BUTTON_LEFT, BTN_LEFT);
	map(WPAD_CLASSIC_BUTTON_RIGHT, BTN_RIGHT);
	map(WPAD_CLASSIC_BUTTON_PLUS, BTN_PLUS);
	map(WPAD_CLASSIC_BUTTON_MINUS, BTN_MINUS);
	map(WPAD_CLASSIC_BUTTON_HOME, BTN_HOME);
	map(WPAD_CLASSIC_BUTTON_FULL_R, BTN_PLUS);
	map(WPAD_CLASSIC_BUTTON_FULL_L, BTN_MINUS);

	if (d && d->ir.valid) {
		in.pointerValid = true;
		in.px = d->ir.x * layoutW / 640.0f;
		in.py = d->ir.y;
	}
	finishFrame(in, held, dt);
}

#else // PC

namespace {
uint32_t gKeysHeld = 0, gMouseHeld = 0;
bool gMouseInside = false;
float gMx = -1, gMy = -1;

uint32_t keyToBtn(SDL_Keycode k) {
	switch (k) {
	case SDLK_RETURN: case SDLK_KP_ENTER: return BTN_CONFIRM;
	case SDLK_ESCAPE: return BTN_BACK;
	case SDLK_UP: return BTN_UP;
	case SDLK_DOWN: return BTN_DOWN;
	case SDLK_LEFT: return BTN_LEFT;
	case SDLK_RIGHT: return BTN_RIGHT;
	case SDLK_PAGEDOWN: return BTN_PLUS;
	case SDLK_PAGEUP: return BTN_MINUS;
	case SDLK_F1: return BTN_HOME;
	case SDLK_F2: return BTN_ONE;
	default: return 0;
	}
}
} // namespace

void init(int argc, char** argv) {
	const char* env = SDL_getenv("OPENSHOP_DATA");
	gDataDir = env ? env : "sdcard/apps/OpenShop";
	LOG("argv[0] raw: %s (argc=%d)", argc > 0 && argv && argv[0] ? argv[0] : "(none)", argc);
	mountStorage();
	LOG("dataDir() = %s (OPENSHOP_DATA or the default)", gDataDir.c_str());
}

bool widescreen() {
	const char* env = SDL_getenv("OPENSHOP_WIDE");
	return env && env[0] == '1';
}

const char* exitLabel() { return "Quit"; }

const char* musicToggleHint() { return "Press F2 to toggle music"; }

void poll(Input& in, float dt, int layoutW) {
	(void)layoutW;
	in = Input{};
	uint32_t downs = 0;
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: in.quit = true; break;
		case SDL_TEXTINPUT: in.text += e.text.text; break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_BACKSPACE) in.backspaces++;
			if (e.key.keysym.sym == SDLK_RETURN || e.key.keysym.sym == SDLK_KP_ENTER) in.enterKey = true;
			if (!e.key.repeat) {
				gKeysHeld |= keyToBtn(e.key.keysym.sym);
				downs |= keyToBtn(e.key.keysym.sym);
			}
			break;
		case SDL_KEYUP: gKeysHeld &= ~keyToBtn(e.key.keysym.sym); break;
		case SDL_MOUSEMOTION:
			gMx = (float)e.motion.x; gMy = (float)e.motion.y;
			gMouseInside = true;
			break;
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			uint32_t b = e.button.button == SDL_BUTTON_LEFT ? (uint32_t)BTN_CONFIRM
			           : e.button.button == SDL_BUTTON_RIGHT ? (uint32_t)BTN_BACK : 0u;
			gMx = (float)e.button.x; gMy = (float)e.button.y;
			if (e.type == SDL_MOUSEBUTTONDOWN) { gMouseHeld |= b; downs |= b; }
			else gMouseHeld &= ~b;
			break;
		}
		case SDL_MOUSEWHEEL: in.wheel += e.wheel.y > 0 ? 1 : e.wheel.y < 0 ? -1 : 0; break;
		case SDL_WINDOWEVENT:
			if (e.window.event == SDL_WINDOWEVENT_LEAVE) gMouseInside = false;
			if (e.window.event == SDL_WINDOWEVENT_ENTER) gMouseInside = true;
			if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) gKeysHeld = gMouseHeld = 0;
			break;
		}
	}
	in.pointerValid = gMouseInside && gMx >= 0;
	in.px = gMx;
	in.py = gMy;
	finishFrame(in, gKeysHeld | gMouseHeld, dt, downs);
}

#endif

} // namespace platform

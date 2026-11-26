#pragma once
#include <string>
#include <vector>

#include "core/FileUtil.h"
#include "core/Input.h"

namespace platform {
// One storage device. `root` is the prefix every path on it starts with: "sd:" / "usb:" on the Wii, the OPENSHOP_SD /
// OPENSHOP_USB folders on the PC (defaults ./sdcard and ./usbdrive). App folders are always root + "/apps/" + dir.
struct StorageRoot {
	std::string id;    // "sd" or "usb"
	std::string label; // "SD card" or "USB drive"
	std::string root;
	bool mounted = false;  // the device's root directory can be opened
	bool writable = false; // a file <root>/.openshop_probe could be created, written, closed and deleted (no folder is ever created)
};

// Call after SDL_Init. Logs argv[0] and the derived dataDir(), mounts what is not mounted yet (Wii) and logs every root
// before and after.
void init(int argc, char** argv);
// The roots as they were after init (Wii: sd and usb, always both; PC: sd, and usb only if its folder exists). A root
// that is not writable is listed but is never an install target.
const std::vector<StorageRoot>& storageRoots();
// The root (as listed in storageRoots) that `path` lies on, or "" if none. Whole path components only, "." / ".." / trailing
// slashes resolved lexically, relative and absolute PC paths compared as absolute (rules in core/PathRoot.h).
// "sd:foo" counts as on "sd:" on purpose.
std::string rootOf(const std::string& path);
// FileUtil's removeTree with the roots taken from storageRoots(), so a caller cannot hand it a bad roots vector. Same
// guards and result (see core/FileUtil.h).
RemoveResult removeTree(const std::string& path, const std::string& mustBeUnder);
// One line for the perf=1 log, every 120 frames: malloc arena, bytes in use and their high-water, and where the free
// heap is (Wii: MEM1 and MEM2 arenas, with the MEM1->MEM2 address gap taken out; PC: the process RSS).
std::string heapSummary();
// Dev-only (heapprobe=1): grabs 1 MB blocks until malloc fails or 80 MB, logs the blocks per region (MEM1/MEM2: count,
// lowest, highest), the MEM1->MEM2 gap, sbrk(0) before / at peak / after, and a RESULT line; then frees them again.
void heapProbe();
// Dev-only (stackprobe=1): runs a 64 KB-stack SDL thread that works well inside that size, logs the stack bounds (real on
// PC, assumed on the Wii), the painted-stack high-water and what SDL_SetThreadPriority does. It does not prove the size.
void stackProbe();
// Pumps SDL events and fills `in` for this frame. layoutW is the logical width.
void poll(Input& in, float dt, int layoutW);
bool widescreen();
// Directory holding user files (bgm.mp3, cache, settings).
// Wii: the app's folder on SD/USB, e.g. sd:/apps/OpenShop. PC: ./sdcard/apps/OpenShop.
const std::string& dataDir();
const char* exitLabel();
// The music key as the hint on Home names it: "Press 1 to toggle music" (Wii) / "Press F2 to toggle music" (PC).
const char* musicToggleHint();
// Writes `data` to `path` atomically: "<path>.tmp" first, then it replaces the target (libfat cannot rename over an
// existing file, so the old one is parked as .bak meanwhile). Logs failures; returns false on error.
bool writeFile(const std::string& path, const std::string& data);
// Reads a whole file into `out` (one fread). Returns false, with `out` empty, if it is missing, unreadable or larger
// than `maxBytes` (the default suits small text files; callers reading bigger data pass their own bound).
bool readFile(const std::string& path, std::string& out, size_t maxBytes = 1u << 20);
} // namespace platform

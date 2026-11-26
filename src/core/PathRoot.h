// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Which storage root a path lies on. Pure string work (no filesystem access, no SDL), so the host tests cover it and
// platform::rootOf only supplies the roots and the current directory.
//
// Rules:
//  - Matching is by whole path components: "./sdcard2/x" is not on "./sdcard".
//  - "." and empty components and trailing slashes are ignored. ".." is resolved lexically (symlinks are NOT followed:
//    ./sdcard may be a symlink, and the roots are compared as written), so "sdcard/../x" is not on "sdcard".
//  - A path that starts with "<device>:" (Wii: "sd:", "usb:") is a device path. ".." never climbs above the device
//    root. "sd:foo" is DELIBERATELY treated like "sd:/foo": devkitPro resolves it against the current directory of that
//    device, so it is on that device, and every caller builds paths as root + "/" + ... anyway.
//  - Any other path is a plain one. When `cwd` is given (PC: an absolute directory) a relative path and relative roots
//    are made absolute with it first, so "./sdcard" and "/home/x/sdcard" are the same place. With an empty `cwd`
//    relative paths stay relative (leading ".." kept).
//  - Empty input matches nothing.
#include <cstddef>
#include <string>
#include <vector>

namespace pathroot {

struct Parts {
	std::string device;            // "sd:" or "" (not a device path)
	bool absolute = false;         // plain paths only: started with '/'
	bool empty = true;             // the input was empty
	std::vector<std::string> comps;
};

inline void splitInto(const std::string& s, std::vector<std::string>& out) {
	size_t i = 0;
	while (i <= s.size()) {
		size_t j = s.find('/', i);
		if (j == std::string::npos) j = s.size();
		out.push_back(s.substr(i, j - i));
		i = j + 1;
	}
}

inline Parts parse(const std::string& path, const std::string& cwd) {
	Parts p;
	if (path.empty()) return p;
	p.empty = false;
	std::string rest = path;
	const size_t colon = path.find(':'), slash = path.find('/');
	if (colon != std::string::npos && colon > 0 && (slash == std::string::npos || colon < slash)) {
		p.device = path.substr(0, colon + 1);
		rest = path.substr(colon + 1);
	} else {
		p.absolute = path[0] == '/';
	}
	std::vector<std::string> raw;
	if (p.device.empty() && !p.absolute && !cwd.empty()) { // relative plain path: start from the current directory
		splitInto(cwd, raw);
		p.absolute = true;
	}
	splitInto(rest, raw);
	const bool anchored = !p.device.empty() || p.absolute;
	for (const std::string& c : raw) {
		if (c.empty() || c == ".") continue;
		if (c == "..") {
			if (!p.comps.empty() && p.comps.back() != "..") p.comps.pop_back();
			else if (!anchored) p.comps.push_back(c);
			continue;
		}
		p.comps.push_back(c);
	}
	return p;
}

// Does `path` lie on (or equal) `root`?
inline bool under(const Parts& root, const Parts& path) {
	if (root.empty || path.empty || root.device != path.device) return false;
	if (root.device.empty() && root.absolute != path.absolute) return false;
	if (root.comps.size() > path.comps.size()) return false;
	for (size_t i = 0; i < root.comps.size(); i++)
		if (root.comps[i] != path.comps[i]) return false;
	return true;
}

// Index of the longest root that `path` lies on, or -1.
inline int indexOf(const std::vector<std::string>& roots, const std::string& path, const std::string& cwd) {
	const Parts p = parse(path, cwd);
	int best = -1;
	size_t bestLen = 0;
	for (size_t i = 0; i < roots.size(); i++) {
		const Parts r = parse(roots[i], cwd);
		if (!under(r, p)) continue;
		if (best < 0 || r.comps.size() > bestLen) { best = (int)i; bestLen = r.comps.size(); }
	}
	return best;
}

} // namespace pathroot

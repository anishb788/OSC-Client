#include "catalog/Catalog.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>

namespace {
std::string lower(std::string_view s) {
	std::string out(s);
	for (char& c : out) c = (char)std::tolower((unsigned char)c);
	return out;
}

std::vector<std::string> words(std::string_view q) {
	std::vector<std::string> out;
	std::string cur;
	for (char c : q) {
		if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == '\'' || c == '.' || c == '-' || c == '+') {
			cur += (char)std::tolower((unsigned char)c);
		} else if (!cur.empty()) {
			out.push_back(std::move(cur));
			cur.clear();
		}
	}
	if (!cur.empty()) out.push_back(std::move(cur));
	return out;
}

bool startsWordAt(const std::string& hay, size_t pos) {
	return pos == 0 || !std::isalnum((unsigned char)hay[pos - 1]);
}

// Letters of `needle` appear in order in `hay` (e.g. "usbgx" in "usb loader gx").
bool subsequence(std::string_view needle, std::string_view hay) {
	size_t i = 0;
	for (char c : hay) if (i < needle.size() && c == needle[i]) i++;
	return i == needle.size();
}

int scoreWord(const std::string& w, const std::string& name, const std::string& slug,
              const std::string& author, const std::string& desc) {
	int best = 0;
	if (name == w) best = 100;
	size_t p = name.find(w);
	if (p != std::string::npos) best = std::max(best, startsWordAt(name, p) ? (p == 0 ? 80 : 60) : 40);
	if (slug.find(w) != std::string::npos) best = std::max(best, 35);
	if (author.find(w) != std::string::npos) best = std::max(best, 30);
	if (w.size() >= 3 && subsequence(w, name)) best = std::max(best, 20);
	if (best == 0 && w.size() >= 2 && desc.find(w) != std::string::npos) best = 10;
	return best;
}
} // namespace

bool AppEntry::hasFlag(std::string_view f) const {
	return std::find(flags.begin(), flags.end(), f) != flags.end();
}

const AppEntry* Catalog::find(std::string_view slug) const {
	for (auto& a : apps) if (a.slug == slug) return &a;
	return nullptr;
}

const AppEntry* Catalog::featured() const {
	if (const AppEntry* a = find(featuredSlug)) return a;
	return apps.empty() ? nullptr : &apps.front();
}

const Category* Catalog::category(std::string_view name) const {
	for (auto& c : categories) if (c.name == name) return &c;
	return nullptr;
}

std::string Catalog::categoryDisplay(std::string_view name) const {
	const Category* c = category(name);
	return c ? c->display : std::string(name);
}

std::vector<const AppEntry*> Catalog::inCategory(std::string_view cat) const {
	std::vector<const AppEntry*> out;
	for (auto& a : apps) if (cat.empty() || a.category == cat) out.push_back(&a);
	return out;
}

int Catalog::count(std::string_view cat) const {
	int n = 0;
	for (auto& a : apps) if (cat.empty() || a.category == cat) n++;
	return n;
}

void Catalog::sort(std::vector<const AppEntry*>& list, SortMode mode) {
	switch (mode) {
	case SortMode::Popular:
		std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->downloads > b->downloads; });
		break;
	case SortMode::Newest:
		std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->releaseDate > b->releaseDate; });
		break;
	case SortMode::Name:
		std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return lower(a->name) < lower(b->name); });
		break;
	case SortMode::Relevance:
		break; // already in relevance order from search()
	}
}

std::vector<const AppEntry*> Catalog::search(std::string_view query) const {
	std::vector<std::string> ws = words(query);
	std::vector<std::pair<int, const AppEntry*>> scored;
	if (ws.empty()) return {};

	for (auto& a : apps) {
		std::string name = lower(a.name), slug = lower(a.slug), author = lower(a.author);
		std::string desc = lower(a.shortDesc) + " " + lower(a.longDesc);
		int total = 0;
		for (auto& w : ws) {
			int s = scoreWord(w, name, slug, author, desc);
			if (s == 0) { total = 0; break; }
			total += s;
		}
		if (total > 0) scored.emplace_back(total, &a);
	}
	std::stable_sort(scored.begin(), scored.end(), [](auto& x, auto& y) {
		if (x.first != y.first) return x.first > y.first;
		return x.second->downloads > y.second->downloads;
	});
	std::vector<const AppEntry*> out;
	for (auto& [s, a] : scored) out.push_back(a);
	return out;
}

std::string formatSize(uint64_t bytes) {
	char buf[32];
	if (bytes >= 1024ull * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", bytes / (1024.0 * 1024.0));
	else if (bytes >= 1024) std::snprintf(buf, sizeof buf, "%.0f KB", bytes / 1024.0);
	else std::snprintf(buf, sizeof buf, "%u B", (unsigned)bytes);
	return buf;
}

std::string formatDate(int64_t t) {
	if (t <= 0) return "Unknown";
	std::time_t tt = (std::time_t)t;
	std::tm* tm = std::gmtime(&tt);
	if (!tm) return "Unknown";
	static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
	char buf[32];
	std::snprintf(buf, sizeof buf, "%s %d, %d", months[tm->tm_mon], tm->tm_mday, tm->tm_year + 1900);
	return buf;
}

std::string formatCount(uint32_t n) {
	std::string s = std::to_string(n);
	for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
	return s;
}

std::string peripheralName(std::string_view id) {
	static const std::pair<std::string_view, std::string_view> names[] = {
		{"wii_remote", "Wii Remote"}, {"nunchuk", "Nunchuk"}, {"classic_controller", "Classic Controller"},
		{"gamecube_controller", "GameCube Controller"}, {"usb_keyboard", "USB Keyboard"},
		{"wii_zapper", "Wii Zapper"}, {"sdhc", "SDHC card"},
	};
	for (auto& [k, v] : names) if (k == id) return std::string(v);
	return std::string(id);
}

std::string platformName(std::string_view id) {
	if (id == "wii") return "Wii";
	if (id == "vwii") return "vWii";
	if (id == "wii_mini") return "Wii mini";
	return std::string(id);
}

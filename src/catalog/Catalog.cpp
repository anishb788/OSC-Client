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

// Fuzzy (subsequence) matching only for words this long: 3-letter words match almost every long name.
constexpr size_t kMinFuzzy = 4;

// `w` is already lowercase; the app's search fields were lowercased once by finalize().
int scoreWord(const std::string& w, const AppEntry& a) {
	int best = 0;
	const std::string& name = a.nameL;
	if (name == w) best = 100;
	size_t p = name.find(w);
	if (p != std::string::npos) best = std::max(best, startsWordAt(name, p) ? (p == 0 ? 80 : 60) : 40);
	if (a.slugL.find(w) != std::string::npos) best = std::max(best, 35);
	if (a.authorL.find(w) != std::string::npos) best = std::max(best, 30);
	if (w.size() >= 2 && a.descL.find(w) != std::string::npos) best = std::max(best, 10);
	// Fuzzy hits rank below every real substring hit.
	if (best == 0 && w.size() >= kMinFuzzy && subsequence(w, name)) best = 5;
	return best;
}
} // namespace

bool AppEntry::hasFlag(std::string_view f) const {
	return std::find(flags.begin(), flags.end(), f) != flags.end();
}

std::string sanitizeText(std::string_view s, bool singleLine) {
	std::string out;
	out.reserve(s.size());
	for (char ch : s) {
		unsigned char c = (unsigned char)ch;
		if (c == '\r') continue; // "\r\n" keeps its "\n"; a lone "\r" is dropped
		if (c == '\n') out += singleLine ? ' ' : '\n';
		else if (c == '\t') out += ' ';
		else if (c < 0x20 || c == 0x7F) continue;
		else out += (char)c;
	}
	return out;
}

bool isValidSlug(std::string_view slug) {
	if (slug.empty() || slug[0] == '.') return false;
	for (char c : slug) {
		if (!(std::isalnum((unsigned char)c) && (unsigned char)c < 0x80) && c != '.' && c != '_' && c != '-') return false;
	}
	return true;
}

void Catalog::finalize() {
	auto one = [](std::string& s) { s = sanitizeText(s, true); };
	auto many = [&](std::vector<std::string>& v) { for (auto& s : v) one(s); };
	for (auto& c : categories) { one(c.name); one(c.display); one(c.singular); one(c.plural); }
	for (auto& a : apps) {
		one(a.slug); one(a.name); one(a.author); one(a.category); one(a.shortDesc); one(a.version);
		a.longDesc = sanitizeText(a.longDesc, false);
		many(a.authors); many(a.contributors);
		one(a.iconUrl); one(a.archiveUrl); one(a.archiveMd5); one(a.binaryUrl); one(a.binaryMd5); one(a.metaUrl);
		many(a.peripherals); many(a.platforms); many(a.flags); many(a.subdirectories);
		a.nameL = lower(a.name);
		a.slugL = lower(a.slug);
		a.authorL = lower(a.author);
		a.descL = lower(a.shortDesc) + " " + lower(a.longDesc);
	}
	one(featuredSlug);

	slugIndex.clear();
	categoryCounts.clear();
	featuredIdx = (size_t)-1;
	size_t newest = (size_t)-1;
	for (size_t i = 0; i < apps.size(); i++) {
		slugIndex.emplace(apps[i].slug, i); // first duplicate wins
		categoryCounts[apps[i].category]++;
		if (newest == (size_t)-1 || apps[i].releaseDate > apps[newest].releaseDate) newest = i;
	}
	auto it = slugIndex.find(featuredSlug);
	featuredIdx = it != slugIndex.end() ? it->second : newest; // fall back to the newest app by releaseDate
}

const AppEntry* Catalog::find(std::string_view slug) const {
	auto it = slugIndex.find(slug);
	return it == slugIndex.end() ? nullptr : &apps[it->second];
}

const AppEntry* Catalog::featured() const {
	return featuredIdx < apps.size() ? &apps[featuredIdx] : nullptr;
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
	if (cat.empty()) return (int)apps.size();
	auto it = categoryCounts.find(cat);
	return it == categoryCounts.end() ? 0 : it->second;
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
		std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->nameL < b->nameL; });
		break;
	case SortMode::Relevance:
		break; // relevance order only exists as search() output; callers keep that list as their base
	}
}

std::vector<const AppEntry*> Catalog::search(std::string_view query) const {
	std::vector<std::string> ws = words(query);
	std::vector<std::pair<int, const AppEntry*>> scored;
	if (ws.empty()) return {};

	for (auto& a : apps) {
		int total = 0;
		for (auto& w : ws) {
			int s = scoreWord(w, a);
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

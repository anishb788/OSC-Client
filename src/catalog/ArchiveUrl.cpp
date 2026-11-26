// SPDX-License-Identifier: GPL-3.0-or-later
// Archive URL rebuild and download allowlist (pure; no jansson, so every test that links Catalog.cpp can link this).
#include <string>
#include <string_view>

#include "catalog/OscApi.h"
#include "core/Strings.h"

namespace oscapi {

namespace {

constexpr std::string_view kHost = "hbb1.oscwii.org";
constexpr size_t kMaxFileName = 128;

// ASCII-only on purpose: <cctype> follows the C locale, which a platform or a library may change.
constexpr char lowerChar(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }
constexpr bool isAsciiAlnum(unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

bool ieq(std::string_view a, std::string_view b) {
	if (a.size() != b.size()) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (lowerChar(a[i]) != lowerChar(b[i])) return false;
	}
	return true;
}

// Untrusted file segment: 1-128 bytes of [A-Za-z0-9._-], no leading dot, ending in .zip, not a device name.
bool validArchiveFile(std::string_view f) {
	if (f.empty() || f.size() > kMaxFileName || f[0] == '.') return false;
	for (char ch : f) {
		unsigned char c = static_cast<unsigned char>(ch);
		if (!isAsciiAlnum(c) && ch != '.' && ch != '_' && ch != '-') return false;
	}
	if (f.size() < 5 || !ieq(f.substr(f.size() - 4), ".zip")) return false;
	return !isReservedDeviceName(f);
}

} // namespace

std::string rebuildArchiveUrl(std::string_view slug, std::string_view apiUrl) {
	if (!isValidSlug(slug)) return std::string();
	std::string_view rest = apiUrl;
	bool https = false;
	if (rest.size() >= 7 && ieq(rest.substr(0, 7), "http://")) rest.remove_prefix(7);
	else if (rest.size() >= 8 && ieq(rest.substr(0, 8), "https://")) { rest.remove_prefix(8); https = true; }
	else return std::string();
	size_t slash = rest.find('/');
	if (slash == std::string_view::npos) return std::string();
	std::string_view authority = rest.substr(0, slash), path = rest.substr(slash);
	// No userinfo; a port only if it is the scheme's default.
	if (authority.find('@') != std::string_view::npos) return std::string();
	size_t colon = authority.find(':');
	if (colon != std::string_view::npos) {
		std::string_view port = authority.substr(colon + 1);
		if (port != (https ? "443" : "80")) return std::string();
		authority = authority.substr(0, colon);
	}
	if (!ieq(authority, kHost)) return std::string();
	// No query or fragment (they are also outside the file's character set).
	if (path.find_first_of("?#") != std::string_view::npos) return std::string();
	std::string prefix = "/api/contents/";
	if (path.substr(0, prefix.size()) != prefix) return std::string();
	path.remove_prefix(prefix.size());
	if (path.substr(0, slug.size()) != slug || path.size() <= slug.size() || path[slug.size()] != '/') return std::string();
	std::string_view file = path.substr(slug.size() + 1);
	if (!validArchiveFile(file)) return std::string();
	return std::string(kContentsUrl) + "/" + std::string(slug) + "/" + std::string(file);
}

namespace {
#if defined(OPENSHOP_DEVHOOKS) && OPENSHOP_DEVHOOKS
std::string g_devBase; // dev builds only: the local test server's contents URL
#endif

bool allowedUnder(std::string_view url, std::string_view base) {
	if (url.substr(0, base.size()) != base || url.size() <= base.size() + 1 || url[base.size()] != '/') return false;
	std::string_view rest = url.substr(base.size() + 1);
	size_t slash = rest.find('/');
	if (slash == std::string_view::npos) return false;
	return isValidSlug(rest.substr(0, slash)) && validArchiveFile(rest.substr(slash + 1));
}
} // namespace

bool isAllowedDownloadUrl(std::string_view url) {
	if (allowedUnder(url, kContentsUrl)) return true;
#if defined(OPENSHOP_DEVHOOKS) && OPENSHOP_DEVHOOKS
	if (!g_devBase.empty() && allowedUnder(url, g_devBase)) return true;
#endif
	return false;
}

#if defined(OPENSHOP_DEVHOOKS) && OPENSHOP_DEVHOOKS
void allowDevDownloadBase(std::string_view contentsBase) { g_devBase = std::string(contentsBase); }
#endif

} // namespace oscapi

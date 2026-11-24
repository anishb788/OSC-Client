#pragma once

#define OPENSHOP_VERSION "0.2.0"

// User-Agent sent with every HTTP request (Open Shop Channel asks clients to identify themselves).
// Keep hbc/meta.xml's <version> in sync with OPENSHOP_VERSION (`./dev` copies it into the package).
inline constexpr const char* userAgent() {
#ifdef __wii__
	return "OpenShop/" OPENSHOP_VERSION " (Nintendo Wii; Open Shop Channel client)";
#else
	return "OpenShop/" OPENSHOP_VERSION " (Linux; Open Shop Channel client)";
#endif
}
